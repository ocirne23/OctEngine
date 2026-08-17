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
// the shared vertex mega-buffer, using a bone-matrix palette. The per-instance jobs live in a per-frame
// SSBO and a single CPU-written indirect dispatch covers all of them (x = groups of the largest job,
// y = job count), so spawning/despawning skinned instances never invalidates the recorded command buffer.
// Output is in the standard MeshVertex format (model space), so the existing cull / DGC / G-buffer /
// shadow paths consume it unchanged.
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
    // Copies this frame's concatenated bone palettes + job list into the mapped GPU buffers and writes
    // the indirect dispatch dimensions.
    void update(uint32 frameIdx, oc::span<const glm::mat4> palettes, oc::span<const RendererVKLayout::SkinningJob> jobs);
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
        Buffer jobBuffer;     // 3 - SkinningJob[] (host-visible, mapped)
        oc::span<RendererVKLayout::SkinningJob> mappedJobs;
        Buffer dispatchArgsBuffer; // vk::DispatchIndirectCommand (host-visible, mapped)
        oc::span<vk::DispatchIndirectCommand> mappedDispatchArgs;
    };
    oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
};
