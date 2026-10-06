export module RendererVK:EyeAdaptationPipeline;

import Core;
import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;
import :RenderParams;
import :PushFields;

// Automatic exposure ("eye adaptation"). Two compute passes per frame over the TAA-resolved scene colour:
//   1. histogram : 256-bin log-luminance histogram of the viewport region (eyeadapt_histogram.cs.glsl);
//                  from the same read it also writes BLOOM level 0 (BloomPipeline) while bloom is on.
//   2. reduce    : weighted-average luminance -> target exposure, smoothed over time into a persistent
//                  exposure buffer (eyeadapt_reduce.cs.glsl).
// The composite pass reads getExposureBuffer() as its auto-exposure multiplier. The exposure buffer persists
// across frames (the adaptation state); the cross-frame read/write hazard is harmless (slow, smoothed value).
export class EyeAdaptationPipeline final
{
public:
    void initialize();
    void reloadShaders();

    // The histogram's lockable push values (bloom threshold / knee, the exposure rule): registered before initialize;
    // the Renderer bakes the list and calls reloadShaders + re-records after a change.
    void registerPushFields();
    PushFieldList& pushFields() { return m_pushFields; }

    // Geometry that depends on the viewport (baked into the command buffer; recorded once, re-recorded on
    // resize like the other passes).
    struct RecordParams
    {
        vk::ImageView resolvedView; // TAA-resolved scene colour for this frame
        // GENERAL for TAA's storage image, SHADER_READ_ONLY when TAA is off and this samples SceneColor
        vk::ImageLayout resolvedLayout = vk::ImageLayout::eGeneral;
        vk::Sampler   sampler;
        glm::ivec2 viewportMin;     // viewport rect within the resolved image
        glm::ivec2 viewportSize;
        // Bloom level 0 (BloomPipeline, GENERAL): the histogram pass writes it from the same read when `bloom`.
        // Always bound.
        vk::ImageView bloomLevel0View;
        bool bloom = false;
        float bloomThreshold = 0.0f; // exposed units, 0 = off (see eyeadapt_histogram.cs.glsl bloomThreshold)
        float bloomKnee = 0.5f;
        float exposureEV = 0.0f;     // the composite's exposure rule, for the threshold
        bool  autoExposure = true;
    };
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);

    // The runtime-tunable adaptation values (from PostParams) + delta time are written to a mapped buffer
    // each frame so the once-recorded command buffer always reads the live values (no per-frame re-record).
    void updateParams(uint32 frameIdx, const PostParams& post, float deltaSeconds);

    // Persistent { float avgLuminance; float exposure; } consumed by the composite pass.
    Buffer& getExposureBuffer() { return m_adaptBuffer; }

private:
    void buildHistogramLayout(ComputePipelineLayout& layout);
    void buildReduceLayout(ComputePipelineLayout& layout);

    // Geometry push constants (baked at record time).
    struct HistogramPC
    {
        glm::ivec2 vpMin;
        glm::ivec2 vpSize;
        int32 bloom; // 1 = also write bloom level 0
        float bloomThreshold;
        float bloomKnee;
        float manualExposure;
        int32 autoExposure;
    };
    struct ReducePC
    {
        uint32 pixelCount;
    };

    // Mapped per-frame UBO contents (matches the GpuParams block in both eye-adapt shaders).
    struct GpuParams
    {
        float minLogLum;
        float invLogLumRange;
        float logLumRange;
        float dt;
        float tau;
        float key;
        float minExposure;
        float maxExposure;
    };

    PushFieldList m_pushFields;
    ComputePipeline m_histogramPipeline;
    ComputePipeline m_reducePipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_histogramSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_reduceSets;

    Buffer m_histogramBuffer; // 256 * uint32 bins (cleared each frame)
    Buffer m_adaptBuffer;     // persistent { float avgLum; float exposure; }
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_paramsBuffer; // mapped GpuParams, updated per frame
    oc::array<oc::span<GpuParams>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedParams;
};
