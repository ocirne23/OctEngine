export module RendererVK:ShadowCullComputePipeline;

import Core;

import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :Layout;
import :DescriptorSet;
import :DrawCompactPipeline;

export class ShadowCullComputePipeline final
{
public:
    ShadowCullComputePipeline();
    ~ShadowCullComputePipeline();
    ShadowCullComputePipeline(const ShadowCullComputePipeline&) = delete;

    struct RecordParams
    {
        DescriptorSet& descriptorSet;
        Buffer& ubo;                          // 0 - the cascade planes, the scene focus, the LOD params
        Buffer& dispatchIndirectBuffer;       // dispatch size { numInstances, 1, 1 }
        Buffer& inRenderNodeTransformsBuffer; // 1
        Buffer& inMeshInstancesBuffer;        // 2
        Buffer& inMeshInstanceOffsetsBuffer;  // 3
        Buffer& inMeshInfoBuffer;             // 4
        Buffer& inFirstInstancesBuffer;       // 5
        // 6, 7, 8: the MAIN cull's out buffers, borrowed: the shadow cull + draw run before the main cull writes them.
        Buffer& outMeshInstancesBuffer;       // 6 - OutShadowMeshInstance at the main cull's (larger) stride's capacity
        Buffer& outMeshInstanceIndexesBuffer; // 7
        Buffer& outIndirectCommandBuffer;     // 8 - its opaque list
        Buffer& inMaterialInfoBuffer;         // 9 - resolves the alpha-mask texture per caster
        Buffer& inNodePassMasksBuffer;        // 10
        Buffer& inMeshLodGroupIdxBuffer;      // 11 - per mesh: LOD group index (GPU LOD selection)
        Buffer& inMeshLodGroupsBuffer;        // 12
        Buffer& meshCountBuffer;              // [0] = the registered mesh count: the slots the compaction walks
        Buffer& treePiecesBuffer;             // 13 - the baked tree records' pieces (static; tree_cull.inc.glsl)
        Buffer& treeTypesBuffer;              // 14 - ... and their types
        Buffer& treeListBuffer;               // 15 - ... and this frame's visible trees (piece | pass bits << 28)
    };

    void initialize();
    void reloadShaders();
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params);

    Buffer& getDrawCountBuffer(uint32 frameIdx) { return m_perFrameData[frameIdx].drawCountBuffer; } // the compacted list's count

    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_computePipeline.getDescriptorSetLayout(); }

private:

    void buildComputeLayout(ComputePipelineLayout& layout);

    ComputePipeline m_computePipeline;
    DrawCompactPipeline m_compact;

    struct PerFrameData
    {
        Buffer drawCountBuffer; // the compacted list's count
    };
    oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
};
