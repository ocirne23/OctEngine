export module RendererVK:IndirectCullComputePipeline;

import Core;
import Core.Transform;

import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :Layout;
import :DescriptorSet;
import :DrawCompactPipeline;

export class IndirectCullComputePipeline final
{
public:
    IndirectCullComputePipeline();
    ~IndirectCullComputePipeline();
    IndirectCullComputePipeline(const IndirectCullComputePipeline&) = delete;

    struct RecordParams
    {
        DescriptorSet& descriptorSet;
        Buffer& ubo;                          // 0
        Buffer& inRenderNodeTransformsBuffer; // 1
        Buffer& inMeshInstancesBuffer;        // 2
        Buffer& inMeshInstanceOffsetsBuffer;  // 3
        Buffer& inMeshInfoBuffer;             // 4
        Buffer& inFirstInstancesBuffer;       // 5
        Buffer& inNodeStatesBuffer;           // 10 - per node: InstanceStream::NodeState (stamped pass mask, LOD state bias)
        Buffer& inMeshLodGroupIdxBuffer;      // 11 - per mesh: LOD group index (GPU LOD selection)
        Buffer& inMeshLodGroupsBuffer;        // 12
        Buffer& lodLevelStateBuffer;          // 13 - per-instance hysteresis state (RW)
        Buffer& outLodStatsBuffer;            // 15 - per-level pick counts (stats readback)
        Buffer& meshCountBuffer;              // [0] = the registered mesh count: the slots the compaction walks
        Buffer& inPrevRenderNodeTransformsBuffer; // 18 - last frame's node transforms (InstanceStream, motion vectors)
        Buffer& inPrevNodeStatesBuffer;           // 19 - ... and its node states (the stamped pass masks)
        Buffer& treePiecesBuffer;                 // 20 - the baked tree records' pieces (static; tree_cull.inc.glsl)
        Buffer& treeTypesBuffer;                  // 21 - ... and their types
        Buffer& treeListBuffer;                   // 22 - ... and this frame's visible trees (piece | pass bits << 28)
    };

    void initialize(uint32 maxMeshInstances, uint32 maxUniqueMeshes);
    void reloadShaders();
    // TERRAIN_TESS_ROUTE (0/1): the tessellated terrain's routing, baked. Takes effect on the next reloadShaders.
    void setTerrainTess(bool enabled) { m_terrainTess = enabled; }
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& recordParams);
    void update(uint32 frameIdx, uint32 numThreads); // the cull threads: u_present_treeThreads (Renderer::fillTreeCullUbo)
    // Capacity growth (caller must have the GPU idle and re-record command buffers afterwards).
    void resizeInstanceBuffers(uint32 maxMeshInstances);
    void resizeCommandBuffers(uint32 maxUniqueMeshes);

    Buffer& getIndirectCommandBuffer(uint32 idx)   { return m_perFrameData[idx].outIndirectCommandBuffer; }
    Buffer& getTransparentIndirectCommandBuffer(uint32 idx) { return m_perFrameData[idx].outTransparentIndirectCommandBuffer; }
    Buffer& getTerrainTessCommandBuffer(uint32 idx) { return m_perFrameData[idx].outTerrainTessCommandBuffer; }
    Buffer& getTerrainFilmCommandBuffer(uint32 idx) { return m_perFrameData[idx].outTerrainFilmCommandBuffer; }
    Buffer& getTreeVoxelCommandBuffer(uint32 idx) { return m_perFrameData[idx].outTreeVoxelCommandBuffer; }
    // The compacted lists' draw counts: [0] opaque, [1] transparent, [2] terrain tess ground, [3] terrain film,
    // [4] the trees' voxel tier.
    static constexpr uint32 NUM_DRAW_LISTS = 5;
    Buffer& getDrawCountBuffer(uint32 idx) { return m_perFrameData[idx].drawCountBuffer; }
    // Per draw slot (gl_InstanceIndex). ONE copy, not one per frame slot: the GPU writes it (the shadow cull, then the main cull)
    // and reads it (the draws' vertex, tessellation and fragment stages) inside one frame, and the frames run in order on one
    // queue. RendererState::recordPrimaryPreScene's barrier holds the next frame's writes back until those reads are done.
    Buffer& getOutMeshInstancesBuffer() { return m_outMeshInstancesBuffer; }
    // Dispatch-size buffer { numInstances, 1, 1 }; reused by the shadow cull which dispatches the
    // same instance set against each cascade's frustum.
    Buffer& getDispatchIndirectBuffer(uint32 idx) { return m_perFrameData[idx].inIndirectCommandBuffer; }

    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_computePipeline.getDescriptorSetLayout(); }

private:

    void buildComputeLayout(ComputePipelineLayout& layout);

    ComputePipeline m_computePipeline;
    DrawCompactPipeline m_compact;
    bool m_terrainTess = true; // TERRAIN_TESS_ROUTE; set from "Terrain/Tessellation/Enabled" before the build

    struct PerFrameData
    {
        Buffer inIndirectCommandBuffer;
        Buffer outIndirectCommandBuffer;         // 8 - opaque
        Buffer outTransparentIndirectCommandBuffer; // 9 - transparent
        Buffer outTerrainTessCommandBuffer;         // 16 - tessellated terrain ground (plain indirect draws)
        Buffer outTerrainFilmCommandBuffer;         // 17 - terrain film (plain indirect draws, never tessellated)
        Buffer outTreeVoxelCommandBuffer;           // 23 - the trees' voxel tier (plain indirect draws: it writes FragDepth)
        Buffer drawCountBuffer;                     // the lists' counts after compaction (NUM_DRAW_LISTS)

        oc::span<vk::DispatchIndirectCommand> mappedIndirectCommands;
    };
    oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
    Buffer m_outMeshInstancesBuffer; // 6 - per draw slot (getOutMeshInstancesBuffer)
};