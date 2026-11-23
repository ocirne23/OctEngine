export module RendererVK:SnowDeformPipeline;

import Core;
import Core.glm;
import :VK;
import :Allocator;
import :Buffer;
import :ComputePipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;
import :PushBlock;

// THE SNOW DEFORMATION (Snow.md "The deformation"): two R32UI TOROIDAL windows around the scene focus - FINE
// (SNOW_DEFORM_FINE_TEXEL, footprints) and COARSE (SNOW_DEFORM_COARSE_TEXEL, trails and craters) - each texel the offset
// from the UNDISTURBED snow surface (hi16) and the snowfall when it was stamped (lo16: the readers refill a track by the
// snow that fell since). The images are MUTABLE: the stamp writes them as R32UI (CAS), every reader samples an R16G16
// view through the bindless array (hardware bilinear; the TextureManager owns the image and that view, adoptImage). They
// rest in SHADER_READ_ONLY_OPTIMAL, GENERAL only for the passes. 8 MB.
// Per frame, inline in the primary BEFORE the grass / clutter culls and the scene (snow_deform.cs.glsl):
//   the MAINTENANCE pass (a window that stepped: the scrolled-in texels go back to undisturbed), then
//   the STAMPS: one workgroup per deformer (Renderer::addSnowDeformer: the frame's lock-free list, by device address)
//   over its footprint in both windows.
// The windows move in steps (SNOW_DEFORM_*_STEP texels), so the maintenance runs on a step frame only.
export class SnowDeformPipeline final
{
public:
    SnowDeformPipeline() = default;
    ~SnowDeformPipeline();
    SnowDeformPipeline(const SnowDeformPipeline&) = delete;

    struct Tick
    {
        bool active = false;          // the maps are live (u_snow_deformPresent; the stamps go in)
        bool maintain = false;        // a window stepped, a clear, an activation or a rebase: the maintenance pass runs
        bool fresh = false;           // ... over every texel (nothing is held: no fine seed from the coarse map)
        bool rebase = false;          // ... and the refilled tracks go back to undisturbed (the refill clock nears its wrap)
        glm::ivec2 fineOrigin{ 0 };   // the windows after this frame (lattice min corners) ...
        glm::ivec2 coarseOrigin{ 0 };
        glm::ivec2 finePrev{ 0 };     // ... and the ones the maps hold now (a texel outside it is cleared)
        glm::ivec2 coarsePrev{ 0 };
        glm::vec2 centre{ 0.0f };     // the focus the windows were placed around (xz): the stamps' reach test
        double fall = 0.0;            // the snowfall total (m) this tick saw: the rebase's mark
    };
    // wanted: snow lies, its depth and its tracks are on. clear: empty the maps ("Weather/Snow/Clear snow"; held until a
    // record runs it). fallMetres: the refill clock (the snowfall total).
    Tick advance(bool wanted, bool clear, const glm::vec3& focus, double fallMetres);

    void initialize(); // the pipeline (initPipelines)
    void createImages(); // once (initSharedBuffers): undisturbed, in SHADER_READ_ONLY_OPTIMAL, adopted into the bindless array
    void reloadShaders();
    uint16 fineTexture() const { return m_fine.tex; }
    uint16 coarseTexture() const { return m_coarse.tex; }
    vk::ImageView coarseSampledView() const { return m_coarse.sampledView; } // R16G16 (the caps' prepare binds it directly)

    struct RecordParams
    {
        vk::Buffer ubo;                 // that slot's frame UBO
        vk::ImageView terrainView;      // the terrain-data cascades (the ground, the climate)
        vk::Sampler terrainSampler;
        vk::ImageView siteTopView;      // the snow site map (the top surface, the catch)
        vk::ImageView sitePassView;
        vk::Sampler siteSampler;
        uint64 deformers = 0;           // this slot's stamp list (device address) ...
        uint32 deformerCount = 0;       // ... and its settled count
    };
    // Into the primary (both), on an active tick: to GENERAL, the maintenance (when due), the stamps, back to read-only.
    // Commits the tick's windows.
    void record(vk::CommandBuffer cmd, uint32 frameIdx, const Tick& tick, const RecordParams& params);

private:
    struct Map
    {
        vk::Image image;
        VmaAllocation memory = nullptr;
        vk::ImageView storageView; // R32UI (the stamp's CAS); the pipeline's
        vk::ImageView sampledView; // R16G16_UNORM (the readers); the TextureManager's, with the image
        uint16 tex = 0;
    };
    void buildLayout(ComputePipelineLayout& layout);
    void makeMap(Map& map, const char* name);
    void transition(vk::CommandBuffer cmd, bool toWrite);

    bool m_traced = false;       // the maps hold the committed windows
    bool m_clearPending = false; // a clear no record has run yet (a frame that records nothing keeps it)
    glm::ivec2 m_fineOrigin{ 0 };
    glm::ivec2 m_coarseOrigin{ 0 };
    double m_rebasedAt = 0.0;    // the snowfall total at the last committed fresh start or rebase

    struct DeformPush
    {
        PushGroup g;
        PushValue<uint64> deformers = g("deformers", "SnowDeformerList");
        PushValue<glm::ivec2> fineOrigin = g("fineOrigin");
        PushValue<glm::ivec2> finePrev = g("finePrev");
        PushValue<glm::ivec2> coarseOrigin = g("coarseOrigin");
        PushValue<glm::ivec2> coarsePrev = g("coarsePrev");
        PushValue<uint32> count = g("count");
        PushValue<uint32> mode = g("mode");   // 0 = the maintenance, 1 = the stamps
        PushValue<uint32> flags = g("flags"); // the maintenance's: 1 = seed the fine map from the coarse, 2 = rebase
    };
    PushBlock m_block;
    DeformPush m_push{ { m_block } };
    ComputePipeline m_pipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets; // per slot: the UBO, the ping-pong terrain map
    Map m_fine;
    Map m_coarse;
};
