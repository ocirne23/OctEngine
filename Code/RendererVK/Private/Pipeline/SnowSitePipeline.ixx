export module RendererVK:SnowSitePipeline;

import Core;
import Core.glm;
import :VK;
import :Allocator;
import :ComputePipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;
import :PushBlock;

// THE SNOW SITE MAP (Snow.md): per texel, straight down from the sky against THIS frame's TLAS (snow_site.cs.glsl) - the top
// SOLID surface's world Y and the share of the snow the foliage above it lets through (the tree cards alpha-tested, so a
// crown shelters its own outline, not its card's square). The snow rays cull RT_CULL_SNOW: no movers (a passing unit is no
// roof) and no bark. A world-snapped TOROIDAL window of SNOW_SITE_RES^2 texels of SNOW_SITE_TEXEL around the scene focus,
// on a fixed tick: only the texels that scrolled in since the last tick and one row slice in SNOW_SITE_SLICES are traced
// (the whole window on the first tick or after a jump). Two images, R32F + RG8 (6 MB), made once and OWNED by the
// TextureManager (adoptImage): every reader samples them through the bindless array, so they sit in
// SHADER_READ_ONLY_OPTIMAL between the ticks (GENERAL only for the trace). A third, the CLASS (R8UI, 1 MB, ours, always
// GENERAL: compute only): what the top is (the terrain, an object, a rock) and the INVALIDATION bits - recordInvalidate
// marks a rectangle STALE (the caps over it go at once) and RETRACE (its texels are traced on the next tick that sees
// them, not only on their row's slice).
// The trace pipeline is built at the first activation (it needs ray queries); the invalidation's with the images.
export class SnowSitePipeline final
{
public:
    SnowSitePipeline() = default;
    ~SnowSitePipeline();
    SnowSitePipeline(const SnowSitePipeline&) = delete;

    // This frame's window (advance, the begin-frame job): the UBO and the record read the same one.
    struct Tick
    {
        bool active = false;      // the map is live this frame (u_snow_sitePresent)
        bool tracing = false;     // the record traces (a tick)
        bool full = false;        // every texel (the first tick, a jump)
        glm::ivec2 origin{ 0 };   // the window after this frame's trace (lattice min corner): the scene readers
        glm::ivec2 prevOrigin{ 0 }; // the last traced window: a texel outside it scrolled in
        glm::ivec2 readOrigin{ 0 }; // the window the map holds BEFORE this frame's trace: the pre-scene readers (the culls, the wetness)
        uint32 slice = 0;         // the round-robin row slice this tick refreshes
        float topY = 0.0f;        // the rays start here (world Y) ...
        float range = 1.0f;       // ... and run this far down
    };
    // wanted: the snow is on and RT is on. realDeltaSec: the wall clock (the shelters move with the world, not the sim).
    Tick advance(bool wanted, float realDeltaSec, const glm::vec3& focus);

    // Once (initSharedBuffers): cleared to open sky, in SHADER_READ_ONLY_OPTIMAL, adopted into the bindless array; the class
    // image and the invalidation's pipeline.
    void createImages();
    uint16 topTexture() const { return m_topTex; }
    uint16 passTexture() const { return m_passTex; }
    vk::ImageView topView() const { return m_topView; }   // the passes without the bindless array bind these (grass cull, wetness)
    vk::ImageView passView() const { return m_passView; }
    vk::ImageView classView() const { return m_classView; } // R8UI storage, GENERAL (the caps' prepare)
    vk::Sampler sampler() const { return m_textureSampler.getSampler(); } // linear, repeat (the toroidal wrap)
    // The window the map holds (the last RECORDED trace) and whether it holds one: the caps read only this.
    glm::ivec2 committedOrigin() const { return m_origin; }
    bool hasTrace() const { return m_traced; }

    // A rectangle of lattice texels (min corner, size) and what to set: SNOW_SITE_STALE and / or SNOW_SITE_RETRACE.
    struct Invalidation
    {
        glm::ivec2 min{ 0 };
        glm::ivec2 size{ 0 };
        uint32 bits = 0;
    };
    // Into the primary BEFORE the trace (every frame that has any; the window clips them): ORs the bits into the class.
    void recordInvalidate(vk::CommandBuffer cmd, oc::span<const Invalidation> rects);

    // Builds the pipeline once (main, present: it compiles a shader): the bindless layout cap and the live descriptor count.
    void ensureBuilt(uint32 layoutCap, uint32 descriptorCount);
    bool isBuilt() const { return m_built; }
    void reloadShaders();
    void registerPushFields(); // the lockable foliage block
    PushBlock& pushBlock() { return m_block; }
    void resizeTextureDescriptors(uint32 count);
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);

    struct RecordParams
    {
        vk::AccelerationStructureKHR tlas;
        uint64 terrainRay = 0;   // this slot's terrain ray table
        vk::Buffer vertices;     // the alpha test's geometry and materials (rt_shadow.inc.glsl)
        vk::Buffer indices;
        vk::Buffer meshInfos;
        vk::Buffer instances;
        vk::Buffer materials;
        vk::Buffer ubo;          // that slot's frame UBO (the frustum)
    };
    // Into the primary after the TLAS build's barrier, on a tracing tick only: to GENERAL, the trace, back to read-only.
    // Commits the tick's window (the next ticks scroll from it).
    void record(vk::CommandBuffer cmd, uint32 frameIdx, const Tick& tick, const RecordParams& params);

private:
    void buildLayout(ComputePipelineLayout& layout, uint32 maxTextures);
    void fillTextureDescriptors();
    void transition(vk::CommandBuffer cmd, bool toWrite);

    // The tick state: m_origin is the window of the last RECORDED trace (record commits it), m_traced whether it is valid.
    bool m_traced = false;
    float m_accum = 0.0f;
    glm::ivec2 m_origin{ 0 };
    uint32 m_slice = 0;

    bool m_built = false;
    uint32 m_maxTextures = 0;
    // snow_site.cs.glsl's push block (served as push.generated.glsl).
    struct SitePush
    {
        PushGroup g;
        PushValue<glm::ivec2> origin = g("origin");
        PushValue<glm::ivec2> prevOrigin = g("prevOrigin");
        PushValue<uint32> slice = g("slice");
        PushValue<uint32> full = g("full");
        PushValue<float> topY = g("topY");
        PushValue<float> range = g("range");
        PushValue<float> layerBlock = g("layerBlock"); // lockable: the snow one foliage layer stops (0..1)
        PushValue<uint64> terrainRay = g("terrainRay", "TerrainRayTable");
    };
    PushBlock m_block;
    SitePush m_push{ { m_block } };
    ComputePipeline m_pipeline;
    // snow_site_invalidate.cs.glsl: one dispatch per rectangle.
    void buildInvalidateLayout(ComputePipelineLayout& layout);
    struct InvalidatePush
    {
        PushGroup g;
        PushValue<glm::ivec2> min = g("min");
        PushValue<glm::ivec2> size = g("size");
        PushValue<uint32> bits = g("bits");
    };
    PushBlock m_invalidateBlock;
    InvalidatePush m_invalidatePush{ { m_invalidateBlock } };
    ComputePipeline m_invalidatePipeline;
    DescriptorSet m_invalidateSet; // the class image only (it never changes)
    vk::Image m_classImage;
    VmaAllocation m_classMemory = nullptr;
    vk::ImageView m_classView;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets; // per slot: the TLAS and the instance buffer are per slot
    Sampler m_textureSampler; // the alpha test's (the bindless array)
    // Owned by the TextureManager (adoptImage); kept for the barriers and the storage descriptors.
    vk::Image m_topImage;
    vk::Image m_passImage;
    vk::ImageView m_topView;
    vk::ImageView m_passView;
    uint16 m_topTex = 0;
    uint16 m_passTex = 0;
};
