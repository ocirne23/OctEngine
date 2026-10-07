export module RendererVK:ClutterPipeline;

import Core;
import Core.glm;
import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :GraphicsPipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;

// GROUND CLUTTER (Docs/GroundClutterPlan.md): its compute half, its buffers and its near-shadow casters (the objects
// draw in StaticMeshGraphicsPipeline: they need the lit core's descriptor set). The model is in clutter.inc.glsl /
// clutter_cull.cs.glsl: a grid of patches around the camera, per patch and type a ranked list of candidates, kept by
// the type's density there.
//
// Per frame: CULL (a workgroup per patch: the kept records + their bucket keys + the bucket counts) -> PREFIX (one
// workgroup: the buckets' ranges + one indexed draw per bucket) -> SCATTER (the records into their buckets' ranges, the
// draws' instance-rate attributes). The buckets: CLUTTER_FLOWER_LODS for the flowers (a fixed draw of that many
// commands), then one per (mesh, LOD) - their draw count is written by the prefix pass.
//
// The ASSETS (setAssets: the types, the rigid meshes) live in device-local buffers and change only on a load (GPU idle).
// The CLUTTER FRAME (ClutterFrameGpu: the patch grid, the counts, the flower index ranges, the FOREST FLOOR MAP) is
// host-visible per frame slot, written in present (Renderer::uploadClutterFrame).
export class ClutterPipeline final
{
public:
    ClutterPipeline() = default;
    ~ClutterPipeline() = default;
    ClutterPipeline(const ClutterPipeline&) = delete;

    // maxTextures = the bindless layout cap (the cull reads the splat height maps: the tessellated relief),
    // numTextureDescriptors = the live count the sets are allocated with.
    void initialize(uint32 maxTextures, uint32 numTextureDescriptors);
    void resizeTextureDescriptors(uint32 numTextureDescriptors); // GPU idle (BindlessTextures' capacity change)
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);
    // The near grass cascade's casters: against the sun ShadowMap's extra layer pass.
    void initializeNearShadow(vk::RenderPass nearShadowRenderPass);
    void reloadShaders();

    // The types and the rigid meshes (every mesh's levels in one vertex / index buffer). The caller idles the GPU and
    // re-records (the draws bind the buffers). Empty types = no clutter.
    void setAssets(oc::span<const RendererVKLayout::ClutterTypeGpu> types, oc::span<const RendererVKLayout::ClutterMeshGpu> meshes,
        oc::span<const RendererVKLayout::ClutterVertexGpu> vertices, oc::span<const uint32> indices);
    uint32 numTypes() const { return m_numTypes; }
    uint32 numMeshes() const { return m_numMeshes; }
    // The flower index buffer's range per LOD (x first index, y index count): the clutter frame's header.
    const oc::array<glm::uvec4, RendererVKLayout::CLUTTER_FLOWER_LODS>& flowerLods() const { return m_flowerLods; }

    struct RecordParams
    {
        Buffer* ubo = nullptr;          // that frame slot's main UBO
        vk::ImageView terrainView;      // the baked terrain-data cascades (climate, water level)
        vk::Sampler terrainSampler;
        Buffer* vertexBuffer = nullptr; // the vertex mega-buffer (the terrain chunks' vertices)
        Buffer* groundTable = nullptr;  // that slot's GrassFrameGpu (the terrain chunks under the camera)
    };
    // Cull + prefix + scatter, and the barrier to the draws (indirect + vertex attribute reads).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);
    // A frame without the cull: no draws (the recorded draws read the counts and the flower commands).
    void recordClear(vk::CommandBuffer primary, uint32 frameIdx);
    // The near grass cascade's casters into its extra-layer pass (inside its secondary; the viewport is set).
    void recordNearShadow(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, Buffer& vertexBuffer);
    // Points the terrain-data binding (UPDATE_AFTER_BIND) at the active ping-pong image.
    void updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler);

    // This slot's clutter frame (present, after the slot's fence; the floor map part is written only when it changed).
    RendererVKLayout::ClutterFrameGpu& frame(uint32 frameIdx) { return *m_mappedFrames[frameIdx]; }
    void flushFrame(uint32 frameIdx, bool withFloor);
    Buffer& frameBuffer(uint32 frameIdx) { return m_frames[frameIdx]; }

    struct Draw
    {
        Buffer* instances = nullptr;     // the sorted ClutterInstanceGpu records (instance-rate)
        Buffer* commands = nullptr;      // VkDrawIndexedIndirectCommand per bucket: the flowers' first
        Buffer* counts = nullptr;        // [1] = the rigid draws' count
        Buffer* vertices = nullptr;      // the rigid meshes (ClutterVertexGpu)
        Buffer* indices = nullptr;
        Buffer* flowerIndices = nullptr; // the flower topology per LOD (vertex ids)
        bool rigid = false;              // any rigid mesh
    };
    Draw getDraw(uint32 frameIdx);

private:
    void buildLayouts(ComputePipelineLayout& cull, ComputePipelineLayout& prefix, ComputePipelineLayout& scatter);
    void buildNearShadowLayout(GraphicsPipelineLayout& layout, bool flowers);
    void buildFlowerIndices();

    ComputePipeline m_cullPipeline;
    ComputePipeline m_prefixPipeline;
    ComputePipeline m_scatterPipeline;
    GraphicsPipeline m_nearShadowPipeline;       // rigid
    GraphicsPipeline m_nearShadowFlowerPipeline;
    vk::RenderPass m_nearShadowRenderPass;
    bool m_nearShadowBuilt = false;
    bool m_nearShadowFlowerBuilt = false;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_cullSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_prefixSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_scatterSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_nearShadowSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_nearShadowFlowerSets;

    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_frames;
    oc::array<RendererVKLayout::ClutterFrameGpu*, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedFrames{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_flat;     // the cull's records, in kept order
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_keys;     // bucket << CLUTTER_LOCAL_BITS | local index
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sorted;   // the records by bucket (instance-rate)
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_counts;   // [0] kept, [1] rigid draws
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bucketCounts;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bucketBase;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_commands;

    Buffer m_types;
    Buffer m_meshes;
    Buffer m_vertices;
    Buffer m_indices;
    Buffer m_flowerIndices;
    oc::array<glm::uvec4, RendererVKLayout::CLUTTER_FLOWER_LODS> m_flowerLods{}; // x first index, y count
    uint32 m_numTypes = 0;
    uint32 m_numMeshes = 0;
    uint32 m_maxTextures = 1;
    Sampler m_textureSampler; // the cull's bindless array (explicit-gradient height taps)
};
