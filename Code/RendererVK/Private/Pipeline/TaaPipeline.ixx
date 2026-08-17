export module RendererVK:TaaPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// Temporal anti-aliasing resolve. A single full-resolution compute pass per frame reprojects last frame's
// resolved colour (history) into the current frame via the depth buffer + u_prevMvp, clamps it to the local
// neighbourhood, and blends. The output doubles as the displayed image and next frame's history, so the
// resolved images are ping-ponged per frame-in-flight (like the RTAO accumulation / GI grid). Images stay in
// GENERAL (storage written, then sampled by the composite pass), matching the RTAO convention.
export class TaaPipeline final
{
public:
    TaaPipeline() = default;
    ~TaaPipeline();
    TaaPipeline(const TaaPipeline&) = delete;

    // viewCount > 1 (VR) keeps a separate resolved image + history per eye (record() takes the eye index).
    void initialize(uint32 width, uint32 height, uint32 viewCount = 1);
    void recreateImages(uint32 width, uint32 height);
    void reloadShaders();

    struct RecordParams
    {
        Buffer& ubo;
        vk::ImageView currentColorView;      // this frame's scene colour (SHADER_READ_ONLY)
        vk::Sampler   currentColorSampler;
        vk::ImageView gbufferDepthView;      // this frame's camera depth (SHADER_READ_ONLY)
        vk::ImageView prevGbufferDepthView;  // last frame's camera depth (SHADER_READ_ONLY)
        vk::ImageView gbufferNormalView;     // this frame's normals; .a = ocean flag (SHADER_READ_ONLY)
        vk::Sampler   gbufferSampler;
        float feedback;                      // history weight; 0 disables accumulation
        float oceanFeedback;                 // history weight cap on ocean pixels (waves animate without motion vectors)
    };
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params);

    // Resolved colour for this frame/eye; sampled by the composite pass with getSampler() (linear).
    vk::ImageView getResolvedView(uint32 frameIdx, uint32 eye) const { return m_resolved.view[slot(frameIdx, eye)]; }
    vk::Sampler   getSampler() const { return m_sampler; }
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

    void buildLayout(ComputePipelineLayout& layout);
    void createImageSet(ImageSet& set);
    void destroyImageSet(ImageSet& set);
    void transitionToGeneral(vk::CommandBuffer cmd, ImageSet& set, uint32 slotIdx, bool forWrite);

    uint32 m_viewCount = 1;
    ComputePipeline m_pipeline;
    oc::array<DescriptorSet, SLOTS> m_sets;

    uint32 m_width = 0;
    uint32 m_height = 0;
    ImageSet m_resolved;
    vk::Sampler m_sampler;
};
