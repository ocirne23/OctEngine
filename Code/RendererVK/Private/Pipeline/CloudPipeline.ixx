export module RendererVK:CloudPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :GraphicsPipeline;
import :DescriptorSet;
import :Layout;

// Volumetric clouds (clouds.inc.glsl holds the density model). Three passes per eye:
//   1. march    : half res, every pixel every frame - the view ray through the cloud shell up to the
//                 farthest scene surface of each 2x2 block -> in-scatter + transmittance + log2 distances.
//   2. temporal : reprojects last frame's accumulation (weighted cloud distance + the wind), rejects on a
//                 distance change, clamps to the 3x3 neighbourhood, blends. Ping-ponged per frame slot.
//   3. apply    : a fullscreen draw in the scene-colour pass before the fog apply: depth-aware upsample,
//                 out = inScatter + scene * transmittance.
// The noise textures (base / detail / weather / curl) are generated on the GPU at initialize and on every
// shader reload (cloud_noise.cs.glsl). Every image stays in GENERAL for life.
// THE CLOUD SHADOW MAP (Beer shadow map, cloud_shadow.cs.glsl / cloud_shadow.inc.glsl): two sun-aligned
// cascades in one 2-layer array, written before GI in the primary. ONE image, not per frame slot: the far
// cascade is refreshed only every few frames and must survive between its updates. It exists (cleared to
// "no cloud") even while the clouds are off, so every consumer can keep it bound.
export class CloudPipeline final
{
public:
    static constexpr uint32 SHADOW_RESOLUTION = 1024;
    static constexpr uint32 SHADOW_CASCADES = 2;

    CloudPipeline() = default;
    ~CloudPipeline();
    CloudPipeline(const CloudPipeline&) = delete;

    // viewCount > 1 (VR): separate half-res images + history per eye.
    void initialize(uint32 fullWidth, uint32 fullHeight, vk::RenderPass sceneRenderPass, uint32 viewCount);
    void recreateImages(uint32 fullWidth, uint32 fullHeight);
    void reloadShaders(vk::RenderPass sceneRenderPass); // the caller has waited for the GPU

    struct RecordParams
    {
        Buffer& ubo;
        vk::ImageView sceneDepthView; // SCENE_DEPTH_SAMPLED_LAYOUT: this frame's, after the opaque stages
        vk::Sampler   sceneDepthSampler;
        vk::ImageView skyMapView;     // GI's sky bake (GENERAL): the ambient
        vk::Sampler   skyMapSampler;
    };
    // March + temporal for one eye (compute, no render pass).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params);

    struct ApplyParams
    {
        Buffer& ubo;
        vk::ImageView sceneDepthView;
        vk::Sampler   sceneDepthSampler;
    };
    // The fullscreen apply draw; the caller is inside the scene-colour render pass with the viewport set.
    void recordApply(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const ApplyParams& params);

    // The shadow map cascades in cascadeMask (bit c = cascade c), recorded straight into the primary. Per cascade
    // split (0 = every texel, 1 = one of each 2x2, 2 = one of each 4x4) and the phase = which texel of the pattern.
    void recordShadow(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, uint32 cascadeMask,
        const oc::array<uint32, SHADOW_CASCADES>& split, const oc::array<uint32, SHADOW_CASCADES>& phase);
    // The clouds of the GI sky map (cloud_sky.cs.glsl), recorded straight into the primary after the shadow map
    // and before GI's sky-map bake. skyMap = the sky map itself (last frame's clear layer: the ambient).
    void recordSky(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, vk::ImageView skyMapView, vk::Sampler skyMapSampler);
    vk::ImageView getSkyCloudsView() const { return m_skyClouds.view; } // GENERAL; the sky map's texel grid
    vk::ImageView getShadowView() const { return m_shadow.view; } // sampler2DArray, GENERAL
    vk::Sampler getShadowSampler() const { return m_linearSampler; }
    // The accumulated clouds (GENERAL): the fog apply composites them when the fog is on.
    vk::ImageView getAccumColorView(uint32 frameIdx, uint32 eye) const { return m_accumColor.view[slot(frameIdx, eye)]; }
    vk::ImageView getAccumDepthView(uint32 frameIdx, uint32 eye) const { return m_accumDepth.view[slot(frameIdx, eye)]; }
    vk::Sampler getLinearSampler() const { return m_linearSampler; }

private:
    static constexpr uint32 MAX_VIEWS = 2;
    static constexpr uint32 SLOTS = RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS;
    static uint32 slot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }

    struct ImageSet
    {
        oc::array<vk::Image, SLOTS> image{};
        oc::array<VmaAllocation, SLOTS> memory{};
        oc::array<vk::ImageView, SLOTS> view{};
    };
    struct NoiseTexture
    {
        vk::Image image;
        VmaAllocation memory{};
        vk::ImageView view;        // all mips (sampled)
        vk::ImageView storageView; // mip 0 (the generator's output)
        uint32 size = 0;
        uint32 mipLevels = 1;
        bool is3D = false;
    };

    struct ShadowMapImage
    {
        vk::Image image;
        VmaAllocation memory{};
        vk::ImageView view;
    };

    void buildNoiseLayout(ComputePipelineLayout& layout, bool is3D);
    void buildShadowLayout(ComputePipelineLayout& layout);
    void buildSkyLayout(ComputePipelineLayout& layout);
    void createShadowMap();
    void createSkyClouds();
    void buildMarchLayout(ComputePipelineLayout& layout);
    void buildTemporalLayout(ComputePipelineLayout& layout);
    void buildApplyLayout(GraphicsPipelineLayout& layout);
    void createNoiseTexture(NoiseTexture& tex, uint32 size, bool is3D, const char* debugName);
    void destroyNoiseTexture(NoiseTexture& tex);
    void generateNoise();
    void createImageSet(ImageSet& set, const char* debugName);
    void destroyImageSet(ImageSet& set);
    oc::array<vk::DescriptorImageInfo, 4> noiseInfos() const;

    uint32 m_viewCount = 1;
    uint32 m_width = 0; // half-res
    uint32 m_height = 0;

    ComputePipeline m_noise3DPipeline;
    ComputePipeline m_noise2DPipeline;
    ComputePipeline m_shadowPipeline;
    ComputePipeline m_marchPipeline;
    ComputePipeline m_temporalPipeline;
    GraphicsPipeline m_applyPipeline;
    oc::array<DescriptorSet, 4> m_noiseSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_shadowSets;
    ShadowMapImage m_shadow;
    ComputePipeline m_skyPipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_skySets;
    ShadowMapImage m_skyClouds; // one lat-long RGBA16F image (in-scatter, transmittance), GENERAL for life
    oc::array<DescriptorSet, SLOTS> m_marchSets;
    oc::array<DescriptorSet, SLOTS> m_temporalSets;
    oc::array<DescriptorSet, SLOTS> m_applySets;

    NoiseTexture m_base;
    NoiseTexture m_detail;
    NoiseTexture m_weather;
    NoiseTexture m_curl;
    ImageSet m_marchColor;
    ImageSet m_marchDepth;
    ImageSet m_accumColor;
    ImageSet m_accumDepth;
    vk::Sampler m_noiseSampler;  // linear, repeat, all mips
    vk::Sampler m_linearSampler; // linear, clamp (the half-res images)
};
