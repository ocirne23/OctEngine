export module RendererVK:BloomPipeline;

import Core;
import Core.glm;
import :VK;
import :Allocator;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// Bloom ("Post/Bloom"): a mip chain of the scene colour, blurred wider at each level, summed back up and mixed
// into the composite (energy-conserving: no threshold). Desktop only.
//  * Level 0 (half res) is written by the eye-adaptation HISTOGRAM pass, which already reads every pixel of
//    the resolved colour (eyeadapt_histogram.cs.glsl, Karis-averaged 2 x 2 quads): bloom never reads the
//    full-res image itself.
//  * record(): the downsample chain (bloom_downsample, 13 taps) to the smallest level, then the upsample chain
//    (bloom_upsample, tent + add, in place) back to level 0, which then holds the sum of every level.
//  * The COMPOSITE mixes level 0 in, before the exposure: no full-res bloom pass at all.
// Each level uses only the texels its viewport region maps to (from the origin), so the image is sized once
// for the full render target and the editor viewport can change without a re-create. One image for every
// frame slot (the passes run within the frame); B10G11R11: bloom is positive, and half the bytes of RGBA16F.
export class BloomPipeline final
{
public:
    static constexpr uint32 MAX_LEVELS = 7;

    BloomPipeline() = default;
    ~BloomPipeline();
    BloomPipeline(const BloomPipeline&) = delete;

    void initialize(uint32 width, uint32 height);
    void recreateImages(uint32 width, uint32 height);
    void reloadShaders();

    // After the eye-adaptation pass wrote level 0: the chain over `levels` levels (clamped to the image's) of
    // the viewport (px). radius (0..1) weighs the levels: level k counts 2^(k (2 radius - 1)) - 0.5 = equal,
    // higher = the wide levels count more (a wide glow, no near-sharp haze).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, glm::ivec2 viewportSize, uint32 levels, float radius);
    // 1 / the sum of the level weights: level 0 holds the weighted sum (the composite's scale).
    float getNormalize(uint32 levels, float radius) const;

    // Level 0: written by the histogram pass (storage) and read by the composite (sampled), GENERAL for life.
    vk::ImageView getLevel0View() const { return m_levelViews[0]; }
    vk::Sampler   getSampler() const { return m_sampler; }
    uint32 clampLevels(uint32 levels) const { return oc::clamp(levels, 1u, m_levelCount); }
    // Full-frame uv -> level 0's viewport region (the composite's u_bloomUv). viewportMin / Size in px.
    glm::vec4 getUvTransform(glm::ivec2 viewportMin, glm::ivec2 viewportSize) const;

private:
    void buildLayouts(ComputePipelineLayout& down, ComputePipelineLayout& up);
    void destroyImages();
    glm::ivec2 levelSize(uint32 level) const;
    static float levelWeight(uint32 level, float radius);

    ComputePipeline m_downPipeline;
    ComputePipeline m_upPipeline;
    // One set per step and frame slot: [slot][level] - the downsample into level + 1, the upsample into level.
    oc::array<oc::array<DescriptorSet, MAX_LEVELS - 1>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_downSets;
    oc::array<oc::array<DescriptorSet, MAX_LEVELS - 1>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_upSets;

    uint32 m_width = 0;  // the full render target
    uint32 m_height = 0;
    uint32 m_levelCount = 0; // mip levels of the image (<= MAX_LEVELS)
    vk::Image m_image;
    VmaAllocation m_memory = nullptr;
    oc::array<vk::ImageView, MAX_LEVELS> m_levelViews{};
    vk::Sampler m_sampler;
};
