export module RendererVK:EyeAdaptationPipeline;

import Core;
import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;
import :Settings;

// Automatic exposure ("eye adaptation"). Two compute passes per frame over the TAA-resolved scene colour:
//   1. histogram : 256-bin log-luminance histogram of the viewport region (eyeadapt_histogram.cs.glsl).
//   2. reduce    : weighted-average luminance -> target exposure, smoothed over time into a persistent
//                  exposure buffer (eyeadapt_reduce.cs.glsl).
// The composite pass reads getExposureBuffer() as its auto-exposure multiplier. The exposure buffer persists
// across frames (the adaptation state); the cross-frame read/write hazard is harmless (slow, smoothed value).
export class EyeAdaptationPipeline final
{
public:
    void initialize();
    void reloadShaders();

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

    ComputePipeline m_histogramPipeline;
    ComputePipeline m_reducePipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_histogramSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_reduceSets;

    Buffer m_histogramBuffer; // 256 * uint32 bins (cleared each frame)
    Buffer m_adaptBuffer;     // persistent { float avgLum; float exposure; }
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_paramsBuffer; // mapped GpuParams, updated per frame
    oc::array<oc::span<GpuParams>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedParams;
};
