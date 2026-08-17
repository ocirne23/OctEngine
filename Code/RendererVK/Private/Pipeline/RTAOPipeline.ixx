export module RendererVK:RTAOPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;
import :Settings;

// Screen-space ray-traced ambient occlusion with a temporal-reprojection + spatial denoise.
// Three half-resolution compute passes per frame, all R16F (images kept in GENERAL, no layout churn):
//   1. trace    : ray-query the GI TLAS from the G-buffer -> raw (noisy) AO.
//   2. temporal : reproject last frame's accumulated AO via prevMvp, reject on disocclusion, blend ->
//                 accumulated AO (this frame's becomes next frame's history; ping-ponged like the GI grid).
//   3. spatial  : depth/normal-aware bilateral blur of the accumulated AO -> final AO (sampled by the
//                 forward pass and upsampled with a linear sampler).
export class RTAOPipeline final
{
public:
    RTAOPipeline() = default;
    ~RTAOPipeline();
    RTAOPipeline(const RTAOPipeline&) = delete;

    // viewCount > 1 (VR) keeps a separate AO image set + history per eye (record() takes the eye index).
    // maxTextures = fixed device-limit cap baked into the trace layout; numTextureDescriptors = live count
    // for the variable-count texture array (used by the alpha-masked candidate test).
    void initialize(const RTAOParams* pParams, uint32 fullWidth, uint32 fullHeight, uint32 maxTextures, uint32 numTextureDescriptors, uint32 viewCount = 1);
    void recreateImages(uint32 fullWidth, uint32 fullHeight);
    void resizeTextureDescriptors(uint32 numTextureDescriptors);
    void reloadShaders();

    struct RecordParams
    {
        Buffer& ubo;
        vk::ImageView gbufferNormalView;     // this frame
        vk::ImageView gbufferDepthView;      // this frame
        vk::ImageView prevGbufferDepthView;  // previous frame (disocclusion)
        vk::Sampler   gbufferSampler;
        vk::AccelerationStructureKHR tlas;
        // Geometry + materials for the alpha-masked candidate test (only consulted when RTAOParams::alphaTest).
        Buffer& vertexBuffer;
        Buffer& indexBuffer;
        Buffer& meshInfos;
        Buffer& meshInstances;
        Buffer& materialInfos;
    };
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params);
    // Rewrites one slot of the trace sets' texture array (binding 10, all eyes) with a streamed texture's current view.
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);

    // Final denoised AO (half-res) for an eye; sampled by the forward pass with getAOSampler() (linear upsample).
    // With the blur disabled (blurRadius <= 0) the spatial pass is skipped, so the accumulated AO is the output.
    vk::ImageView getAOView(uint32 frameIdx, uint32 eye) const
    {
        const ImageSet& out = (m_pParams && m_pParams->blurRadius > 0) ? m_final : m_accum;
        return out.view[slot(frameIdx, eye)];
    }
    vk::Sampler   getAOSampler() const { return m_aoSampler; }
    uint32 getWidth() const  { return m_width; }
    uint32 getHeight() const { return m_height; }

private:
    static constexpr uint32 MAX_VIEWS = 2;
    static constexpr uint32 SLOTS = RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS;
    static uint32 slot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }

    struct ImageSet
    {
        oc::array<vk::Image, SLOTS> image{};
        oc::array<VmaAllocation, SLOTS> memory{};
        oc::array<vk::ImageView, SLOTS> view{};
        oc::array<bool, SLOTS> initialized{};
    };

    void buildTraceLayout(ComputePipelineLayout& layout, uint32 maxTextures);
    void buildTemporalLayout(ComputePipelineLayout& layout);
    void buildSpatialLayout(ComputePipelineLayout& layout);
    void createImageSet(ImageSet& set);
    void destroyImageSet(ImageSet& set);
    void transitionToGeneral(vk::CommandBuffer cmd, ImageSet& set, uint32 slotIdx, bool forWrite);

    uint32 m_viewCount = 1;
    ComputePipeline m_tracePipeline;
    ComputePipeline m_temporalPipeline;
    ComputePipeline m_spatialPipeline;
    oc::array<DescriptorSet, SLOTS> m_traceSets;
    oc::array<DescriptorSet, SLOTS> m_temporalSets;
    oc::array<DescriptorSet, SLOTS> m_spatialSets;

    const RTAOParams* m_pParams = nullptr;
    uint32 m_maxTextures = 0; // fixed device-limit cap baked into the trace layout
    Sampler m_textureSampler; // repeat + full-mip sampler for the alpha-mask texture array

    uint32 m_width = 0;  // half-res AO dimensions
    uint32 m_height = 0;
    ImageSet m_raw;
    ImageSet m_accum;
    ImageSet m_final;
    vk::Sampler m_aoSampler;
};
