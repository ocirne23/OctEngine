export module RendererVK:MotionBlurPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// Motion blur, after TAA (desktop only): three compute passes over TAA's resolved colour, into one image the
// composite tonemaps instead (eye adaptation keeps reading the unblurred colour).
//  1. motion_blur_tiles    - per pixel the blur velocity (px over the exposure) + view distance, per
//                            MOTION_BLUR_TILE^2 tile the longest velocity;
//  2. motion_blur_neighbor - per tile the longest velocity of its 3x3 tiles;
//  3. motion_blur_gather   - McGuire's reconstruction filter along that velocity.
// The images are used within the frame only, so there is one set (not one per frame slot): the record starts
// with a barrier against last frame's reads. All of them stay in GENERAL.
export class MotionBlurPipeline final
{
public:
    MotionBlurPipeline() = default;
    ~MotionBlurPipeline();
    MotionBlurPipeline(const MotionBlurPipeline&) = delete;

    void initialize(uint32 width, uint32 height);
    void recreateImages(uint32 width, uint32 height);
    void reloadShaders();

    struct RecordParams
    {
        Buffer& ubo;
        vk::ImageView colorView;        // TAA's resolved colour, or the scene colour with TAA off
        vk::ImageLayout colorLayout;
        vk::ImageView sceneDepthView;   // this frame's depth (SCENE_DEPTH_SAMPLED_LAYOUT)
        vk::ImageView motionView;       // this frame's motion target (SHADER_READ_ONLY)
        float shutter;
        float maxRadius;
        float cameraScale;
        uint32 samples;
    };
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);

    // The blurred colour (GENERAL), for the composite, with getSampler() (linear).
    vk::ImageView getOutputView() const { return m_out.view; }
    vk::Sampler   getSampler() const { return m_sampler; }

private:
    struct Image
    {
        vk::Image image;
        VmaAllocation memory = nullptr;
        vk::ImageView view;
    };

    void createImage(Image& image, uint32 width, uint32 height, vk::Format format, const char* name);
    void destroyImage(Image& image);
    void destroyImages();
    void buildLayouts(ComputePipelineLayout& tiles, ComputePipelineLayout& neighbor, ComputePipelineLayout& gather);

    ComputePipeline m_tilesPipeline;
    ComputePipeline m_neighborPipeline;
    ComputePipeline m_gatherPipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tilesSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_neighborSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_gatherSets;

    uint32 m_width = 0;
    uint32 m_height = 0;
    uint32 m_tilesX = 0;
    uint32 m_tilesY = 0;
    Image m_velocity;    // full res: xy = velocity (px), z = view distance
    Image m_tileMax;     // per tile
    Image m_neighborMax; // per tile
    Image m_out;         // full res: the blurred colour
    vk::Sampler m_sampler;
};
