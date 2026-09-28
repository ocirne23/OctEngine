export module RendererVK:DlssPipeline;

import Core;
import Core.glm;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;
import :Streamline;

// DLSS Super Resolution's own images and passes around Streamline::evaluateDlss (Renderer::recordDlssEvaluate):
//  * the full motion vectors (dlss_mvec.cs.glsl - the scene's motion target carries object motion only);
//    render-size RG16F;
//  * the bias-current-colour mask, from the same pass: the ocean's waves have no motion vectors, so DLSS
//    trusts the current frame there ("Ocean current bias" - TAA's ocean feedback cap); render-size R16F;
//  * the OUTPUT image, used when the viewport does not start at (0, 0): SL never enables NGX's output subrects,
//    so DLSS may only write at the origin - it writes here, and recordOutputCopy moves the result to the
//    viewport's place in TAA's resolved image. Swapchain-size RGBA16F. A viewport at (0, 0) needs neither.
// Both are used within the frame only: one of each for every frame slot, GENERAL for life. Their writers order
// themselves after last frame's reads (the opening barriers of record / evaluate).
export class DlssPipeline final
{
public:
    DlssPipeline() = default;
    ~DlssPipeline();
    DlssPipeline(const DlssPipeline&) = delete;

    void initialize(uint32 renderWidth, uint32 renderHeight, uint32 outputWidth, uint32 outputHeight);
    void recreateImages(uint32 width, uint32 height);             // the render-size motion vectors
    void recreateOutputImage(uint32 width, uint32 height);        // the swapchain-size output
    void reloadShaders();

    struct RecordParams
    {
        Buffer& ubo;
        vk::ImageView sceneDepthView;   // this frame's depth (SCENE_DEPTH_SAMPLED_LAYOUT)
        vk::ImageView motionView;       // this frame's motion target (SHADER_READ_ONLY)
        vk::ImageView sceneColorView;   // this frame's scene colour (SHADER_READ_ONLY): .a = 0 = ocean
        glm::ivec2 renderOrigin;        // the render rect
        glm::ivec2 renderSize;
        float oceanBias;                // the mask value on ocean pixels
        // The fused motion blur (DLAA only; MotionBlurPipeline): this pass writes its velocity + sub-tiles, as TAA
        // does. Always bound.
        vk::ImageView mbVelocityView;
        vk::ImageView mbSubTileView;
        bool  mbEnabled = false;
        float mbShutter = 0.0f;
        float mbMaxRadius = 0.0f;       // already clamped (MotionBlurPipeline::clampMaxRadius)
        float mbCameraScale = 0.0f;
    };
    // The motion vectors + the bias mask [+ the motion blur velocity]; ends with a barrier to the compute reads
    // of the upscale and the motion blur's neighbour pass. Cached.
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);

    Streamline::Image getMotionImage() const;
    Streamline::Image getBiasImage() const;
    Streamline::Image getOutputImage() const;
    // Before the evaluate: last frame's copy read of the output -> DLSS's write.
    void beginOutputWrite(vk::CommandBuffer cmd) const;
    // After the evaluate: the output's (0, 0, size) -> dst at dstOffset (GENERAL; the caller orders dst).
    void recordOutputCopy(vk::CommandBuffer cmd, vk::Image dst, glm::ivec2 dstOffset, glm::uvec2 size) const;

private:
    struct Image
    {
        vk::Image image;
        VmaAllocation memory = nullptr;
        vk::ImageView view;
        uint32 width = 0;
        uint32 height = 0;
    };

    void buildLayout(ComputePipelineLayout& layout);
    void createImage(Image& image, uint32 width, uint32 height, vk::Format format, vk::ImageUsageFlags usage, const char* name);
    void destroyImage(Image& image);

    ComputePipeline m_pipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets;
    Image m_motion;
    Image m_bias;
    Image m_output;
    vk::Sampler m_sampler;
};
