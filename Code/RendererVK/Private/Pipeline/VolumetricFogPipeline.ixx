export module RendererVK:VolumetricFogPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :GraphicsPipeline;
import :DescriptorSet;
import :Layout;

// Froxel-based volumetric fog (camera-frustum-aligned 3D grid, VOL_FROXEL_* texels, exponential Z slices):
//   1. scatter   : per froxel, media density (global height fog + noise + local fog volumes) and in-scattered
//                  light (sun via TLAS ray / cascade tap, GI probe ambient, light-grid lights), temporally
//                  blended against last frame's reprojected grid (ping-ponged across frames in flight).
//   2. integrate : front-to-back accumulation along Z -> in-scatter + transmittance at every slice.
//   3. apply     : fullscreen blend in the scene-color pass (before TAA), sampling the integrated volume at
//                  each pixel's G-buffer depth: out = inScatter + sceneColor * transmittance. Everything
//                  past the volume's far plane is added analytically instead of with more slices, so the
//                  volume's range is a near-field quality knob and not a view distance (vol_apply.fs.glsl).
// The grid resolution is fixed (independent of the window size), so no swapchain-recreate handling is needed.
export class VolumetricFogPipeline final
{
public:
    VolumetricFogPipeline() = default;
    ~VolumetricFogPipeline();
    VolumetricFogPipeline(const VolumetricFogPipeline&) = delete;

    void initialize();
    // viewCount > 1 (VR): one apply descriptor set per eye so the per-eye inline draws don't clobber each other.
    void initializeApply(vk::RenderPass renderPass, uint32 viewCount = 1);
    void reloadShaders(vk::RenderPass renderPass);

    struct RecordParams
    {
        Buffer& ubo;
        Buffer& lightInfosBuffer;
        Buffer& lightGridsBuffer;
        Buffer& lightTableBuffer;
        Buffer& fogVolumesBuffer;
        Buffer& giGridDataBuffer;
        vk::ImageView shadowMapView;
        vk::Sampler   shadowMapSampler;
        vk::ImageView oceanMapsView;   // FFT displacement maps: the scatter pass samples the live wave
        vk::Sampler   oceanMapsSampler; // height around the waterline (underwater fog boundary)
        vk::AccelerationStructureKHR tlas;
    };
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);

    struct ApplyParams
    {
        Buffer& ubo;
        Buffer& giGridDataBuffer; // far field ambient (virtual sky probe)
        vk::ImageView gbufferDepthView;
        // DEPTH_STENCIL_READ_ONLY while depth-prepass reuse binds this image as the scene pass depth.
        vk::ImageLayout gbufferDepthLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::Sampler   gbufferSampler;
    };
    // Records the fullscreen apply draw; the caller has begun a command buffer inside the scene-color
    // render pass and set the viewport/scissor. eye selects the per-eye depth/projection (0 = desktop/left).
    void recordApply(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const ApplyParams& params);

    // Points ALL of this slot's sets at the fog terrain height map (Renderer's BakedWorldMap): the scatter
    // set's binding 10 and every eye's apply set binding 3, both UPDATE_AFTER_BIND and refreshed every
    // frame, so a re-bake's ping-pong flip never re-records the cached fog CBs. The scatter pass raises the
    // height-fog base by a fraction of the local terrain height (Fog/Terrain Follow), so fog pools in
    // valleys and clears the peaks; the far field marches the same ground past the volume. Covers the whole
    // pipeline in one call — keep it that way, or a set added later gets missed.
    void updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler);

private:
    struct ImageSet
    {
        oc::array<vk::Image, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> image;
        oc::array<VmaAllocation, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> memory{};
        oc::array<vk::ImageView, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> view;
    };

    void buildScatterLayout(ComputePipelineLayout& layout);
    void buildIntegrateLayout(ComputePipelineLayout& layout);
    void buildApplyLayout(GraphicsPipelineLayout& layout);
    void createImageSet(ImageSet& set);
    void destroyImageSet(ImageSet& set);

    ComputePipeline m_scatterPipeline;
    ComputePipeline m_integratePipeline;
    GraphicsPipeline m_applyPipeline;
    static constexpr uint32 MAX_VIEWS = 2;
    static uint32 applySlot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }
    uint32 m_applyViewCount = 1;

    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_scatterSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_integrateSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS> m_applySets;

    ImageSet m_scatter;    // per-froxel in-scatter/extinction; previous frame's image is the temporal history
    ImageSet m_integrated; // accumulated in-scatter + transmittance per slice
    vk::Sampler m_sampler;
};
