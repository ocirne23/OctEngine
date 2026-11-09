export module RendererVK:TreeVoxelFarPipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// THE FAR VOXEL TREES ("Trees/Voxel far trees"; Docs/Context/RendererVK/Scene.md): the world tree records of the chunks
// outside the near set, expanded on the GPU into their variants' VOXEL boxes (tree_voxel_far.cs.glsl: cull, prefix,
// scatter), and drawn by StaticMeshGraphicsPipeline's far voxel pipeline - one indexed indirect draw per voxel mesh.
// Per frame slot: the CPU's chunk list and type tables (host-visible), the GPU's appended + sorted records and draws.
export class TreeVoxelFarPipeline final
{
public:
    static constexpr uint32 MAX_BUCKETS = 64;        // distinct voxel meshes (tree_voxel_far.cs.glsl FAR_MAX_BUCKETS)
    static constexpr uint32 MAX_CHUNKS = 32768;      // listed record chunks per frame (a 20 km ring of 256 m chunks: ~19600)
    static constexpr uint32 MAX_TYPES = 4096;        // the world set's types
    // Drawn trees per frame: ~200 per 256 m chunk, a quarter of the ring in view - ~1M at 20 km (262144 overflowed at ~9 km).
    // Past it the rest are dropped: the chunks are listed nearest first, so it is the farthest that go.
    static constexpr uint32 MAX_INSTANCES = 1u << 20;

    // MIRRORED in tree_voxel_far.cs.glsl.
    struct ChunkGpu { glm::ivec2 coord{ 0 }; uint32 first = 0; uint32 count = 0; };
    struct FarTypeGpu
    {
        uint32 bucket = UINT32_MAX; // its voxel mesh's bucket; UINT32_MAX = no voxel tier
        uint32 materialMesh = 0;    // meshIdx | materialIdx << 16
        float wind = 1.0f;          // x the trunk bend
        float centreY = 0.0f;       // the box centre's height at scale 1 (m)
        float radius = 0.0f;        // the box's half diagonal at scale 1 (m)
        uint32 shadowBucket = UINT32_MAX; // the far shadow casters: its billboard mesh's bucket; UINT32_MAX = casts nothing
        uint32 shadowTex = 0xFFFF;        // ... and the billboard's alpha-mask texture
        float pad0 = 0.0f;
    };
    struct BucketGpu { uint32 indexCount = 0; uint32 firstIndex = 0; int32 vertexOffset = 0; uint32 pad = 0; };
    // 32 B: the yaw only (a tree turns about up), the bucket and the index in it in one word (bucket << 26 | index).
    struct InstanceGpu { glm::vec4 posScale; float yaw; uint32 materialMesh; float wind; uint32 bucketLocal; };
    static_assert(sizeof(InstanceGpu) == 32);

    struct RecordParams
    {
        Buffer& ubo;
        vk::DeviceAddress records = 0; // the record pool (its grounds too)
        vk::DeviceAddress map = 0;     // this slot's chunk map
        uint32 mapSize = 0;
        vk::DeviceAddress types = 0;   // the record types
        uint32 numTypes = 0;
        uint32 worldSeed = 1;
        float chunkSize = 256.0f;
        float endDistance = 0.0f;
    };
    struct Draw
    {
        Buffer* instances = nullptr; // InstanceGpu, sorted by bucket (instance rate)
        Buffer* commands = nullptr;  // VkDrawIndexedIndirectCommand per bucket (MAX_BUCKETS)
    };

    // THE FAR SHADOW CASTERS: the same chunks' trees as BILLBOARDS in the sun cascades (FAR_SHADOW: the cascade test of the
    // shadow cull instead of the view's), drawn by ShadowMapGraphicsPipeline's far billboard pipeline. Their own buffers.
    static constexpr uint32 MAX_SHADOW_INSTANCES = 1u << 19;

    void initialize();
    void reloadShaders();
    // Main thread, before this slot records: its chunk list and tables (the types and buckets change only with the set,
    // but are written per slot so no frame in flight sees a rewrite). shadowChunks / shadowBuckets: the far shadow casters'.
    void setFrame(uint32 frameIdx, oc::span<const ChunkGpu> chunks, oc::span<const FarTypeGpu> types, oc::span<const BucketGpu> buckets,
        oc::span<const ChunkGpu> shadowChunks, oc::span<const BucketGpu> shadowBuckets);
    // The three passes, then the barrier to the draws. In the primary (the counts change per frame).
    void record(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params);
    // Off: the draws emptied.
    void recordClear(vk::CommandBuffer cmd, uint32 frameIdx);
    Draw getDraw(uint32 frameIdx) { return Draw{ &m_view.sorted[frameIdx], &m_view.commands[frameIdx] }; }
    // The far shadow casters the same way (before the shadow draw); off: emptied.
    void recordShadow(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params);
    void recordShadowClear(vk::CommandBuffer cmd, uint32 frameIdx);
    Draw getShadowDraw(uint32 frameIdx) { return Draw{ &m_shadow.sorted[frameIdx], &m_shadow.commands[frameIdx] }; }

private:
    void buildLayout(ComputePipelineLayout& layout, uint32 pass, bool shadow = false);
    // One list's per-slot buffers (the view's or the shadow casters'). state: [0] count, [1 + b] bucket counts, [1 + 64 + b]
    // bases.
    struct Lists
    {
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> chunks;  // host-visible
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> buckets; // host-visible
        oc::array<oc::span<uint8>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> chunksMapped, bucketsMapped;
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> appended;
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> state;
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> commands;
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> sorted;
        oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> numChunks{};
        oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> numBuckets{};
    };
    void initLists(Lists& lists, uint32 maxInstances, const char* name);
    void writeList(Lists& lists, uint32 frameIdx, oc::span<const ChunkGpu> chunks, oc::span<const BucketGpu> buckets);
    void runPasses(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params, const ComputePipeline& cull, Lists& lists,
        uint32 maxInstances, vk::PipelineStageFlags2 drawStages);
    void clearList(vk::CommandBuffer cmd, Lists& lists, uint32 frameIdx);

    ComputePipeline m_cull, m_shadowCull, m_prefix, m_scatter;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets; // the UBO alone (binding 0)
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_types;       // host-visible, both lists'
    oc::array<oc::span<uint8>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_typesMapped;
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_numTypes{};
    Lists m_view;   // the voxel boxes in the view
    Lists m_shadow; // the billboards in the cascades
};
