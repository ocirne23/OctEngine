export module RendererVK:SkinningComputePipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :Layout;
import :DescriptorSet;

// GPU skinning: deforms each skinned mesh instance's base vertices into a per-instance output region of
// the shared vertex mega-buffer, using a bone-matrix palette. The jobs of the instances pushed this frame
// live packed in a per-frame SSBO, and a single CPU-written indirect dispatch covers all of them (each
// job's own workgroups, from its firstGroup on), so spawning/despawning skinned instances never
// invalidates the recorded command buffer. Output is in the standard MeshVertex format (model space), so
// the existing cull / DGC / shadow paths consume it unchanged.
export class SkinningComputePipeline final
{
public:
    SkinningComputePipeline();
    ~SkinningComputePipeline();
    SkinningComputePipeline(const SkinningComputePipeline&) = delete;

    struct RecordParams
    {
        DescriptorSet& descriptorSet;
        Buffer& vertexBuffer;   // 0 - shared MeshVertex mega-buffer (read base region, write output region)
        Buffer& skinningBuffer; // 1 - SkinningVertex influences
    };

    void initialize(uint32 maxPaletteEntries, uint32 maxJobs);
    void reloadShaders();
    // This frame slot's mapped palettes and jobs (fence waited): SkinnedMeshRegistry::writeFrame fills them, then
    // finishUpload flushes them, writes the job count and the indirect dispatch dimensions.
    oc::span<glm::mat4> getMappedPalettes(uint32 frameIdx) { return m_perFrameData[frameIdx].mappedPalettes; }
    oc::span<RendererVKLayout::SkinningJob> getMappedJobs(uint32 frameIdx) { return m_perFrameData[frameIdx].mappedJobs; }
    void finishUpload(uint32 frameIdx, uint32 paletteEnd, uint32 numJobs, uint32 numGroups);
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params);
    void resizePaletteBuffer(uint32 maxPaletteEntries);
    void resizeJobBuffer(uint32 maxJobs);

    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_computePipeline.getDescriptorSetLayout(); }

private:

    void buildComputeLayout(ComputePipelineLayout& layout);

    ComputePipeline m_computePipeline;
    uint32 m_maxPaletteEntries = 0;
    uint32 m_maxJobs = 0;

    struct PerFrameData
    {
        Buffer paletteBuffer; // 2 - bone matrices (host-visible, mapped); concatenated per skinned instance
        oc::span<glm::mat4> mappedPalettes;
        Buffer jobBuffer;     // 3 - the job count (SKINNING_JOB_HEADER_BYTES), then SkinningJob[] (host-visible, mapped)
        uint32* mappedJobCount = nullptr;
        oc::span<RendererVKLayout::SkinningJob> mappedJobs;
        Buffer dispatchArgsBuffer; // vk::DispatchIndirectCommand (host-visible, mapped)
        oc::span<vk::DispatchIndirectCommand> mappedDispatchArgs;
    };
    oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
};
