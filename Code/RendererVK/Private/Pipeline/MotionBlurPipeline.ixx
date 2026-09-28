export module RendererVK:MotionBlurPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// Motion blur (desktop only), FUSED into the passes around it - it has no full-res pass of its own:
//  * TAA (taa.cs.glsl) already reads the depth and the motion target: it writes the per-pixel blur velocity and
//    the longest velocity of each 8 x 8 SUB-tile (its workgroup). With TAA off, motion_blur_tiles does the same.
//  * record(): motion_blur_neighbor - per MOTION_BLUR_TILE tile, the longest velocity of its 3 x 3 tile
//    neighbourhood, read from the sub-tiles (tiny).
//  * The COMPOSITE runs the gather (McGuire's reconstruction filter, motion_blur.inc.glsl) for the pixels of
//    tiles whose neighbourhood moves, and reads the resolved colour once elsewhere.
// Used within the frame only: one set of images for every frame slot, GENERAL for life. Their writers order
// themselves after last frame's reads (TAA's own opening barrier, or record's).
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
        vk::ImageView sceneDepthView;   // this frame's depth (SCENE_DEPTH_SAMPLED_LAYOUT)
        vk::ImageView motionView;       // this frame's motion target (SHADER_READ_ONLY)
        bool velocityPass;              // TAA is off: write the velocity + sub-tiles here
        float shutter;
        float maxRadius;
        float cameraScale;
    };
    // After TAA: [the velocity pass] + the neighbour pass; ends with a barrier to the composite's reads.
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);

    // TAA writes these two (storage, GENERAL).
    vk::ImageView getVelocityView() const { return m_velocity.view; }
    vk::ImageView getSubTileView() const { return m_subTiles.view; }
    // The composite reads these (texelFetch) with getSampler().
    vk::ImageView getNeighborMaxView() const { return m_neighborMax.view; }
    vk::Sampler   getSampler() const { return m_sampler; }
    // The blur may reach at most one tile, so the 3 x 3 neighbourhood holds every velocity that reaches a pixel.
    static float clampMaxRadius(float maxRadius) { return oc::min(maxRadius, (float)RendererVKLayout::MOTION_BLUR_TILE); }

private:
    struct Image
    {
        vk::Image image;
        VmaAllocation memory = nullptr;
        vk::ImageView view;
    };

    void createImage(Image& image, uint32 width, uint32 height, const char* name);
    void destroyImage(Image& image);
    void destroyImages();
    void buildLayouts(ComputePipelineLayout& tiles, ComputePipelineLayout& neighbor);

    ComputePipeline m_tilesPipeline;
    ComputePipeline m_neighborPipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tilesSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_neighborSets;

    uint32 m_width = 0;
    uint32 m_height = 0;
    uint32 m_subTilesX = 0;
    uint32 m_subTilesY = 0;
    uint32 m_tilesX = 0;
    uint32 m_tilesY = 0;
    Image m_velocity;    // full res, RG16F: px over the exposure
    Image m_subTiles;    // per 8 x 8, RG16F
    Image m_neighborMax; // per tile, RG16F
    vk::Sampler m_sampler;
};
