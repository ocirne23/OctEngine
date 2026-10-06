export module RendererVK:RainOcclusionPipeline;

import Core;
import Core.glm;
import :VK;
import :Allocator;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// The weather volume's RAIN OCCLUSION MAP (the particle sim's shelter test): per texel, two short ray queries straight
// DOWN through the volume's top-down orthographic box (u_weather_rainOcclusionViewProj) against THIS frame's TLAS - the solid
// surface's depth, and the foliage (alpha-masked instances) above it: its top depth + the fraction of rain its layers
// let through - packed into ONE uint (rain_occlusion.cs.glsl). No cull, no instance buffers, ONE image (not one per
// frame slot: the trace's opening barrier waits for every earlier read on the queue, the previous frame's sim too):
// RAIN_OCCLUSION_RESOLUTION^2 x 4 B = 1 MB in all.
// ONLY WHILE ACTIVE (a rain / snow volume asks for the map, Renderer::present): the compute pipeline is built on the
// first activation and the full-size image exists only while active; inactive, a 1x1 placeholder (open sky) the
// particle sim's descriptor can name.
export class RainOcclusionPipeline final
{
public:
    RainOcclusionPipeline() = default;
    ~RainOcclusionPipeline();
    RainOcclusionPipeline(const RainOcclusionPipeline&) = delete;

    void initialize(); // the sampler + the placeholders only
    void reloadShaders();
    // Allocates (true) or frees (false) the map. The GPU must be idle; the caller re-records the particle sim's set.
    void setActive(bool active);
    bool isActive() const { return m_active; }
    // Into the primary, after the TLAS build's barrier: the trace, then a barrier to the particle sim's sampled read.
    // viewProj = the frame UBO's u_weather_rainOcclusionViewProj (its inverse rides the push constants); layerBlock = the
    // rain one foliage layer stops (0..1).
    void record(vk::CommandBuffer cmd, uint32 frameIdx, vk::AccelerationStructureKHR tlas, const glm::mat4& viewProj, float layerBlock);

    // Always in GENERAL (the particle sim samples it there, nearest, bounds-checked itself).
    vk::ImageView getView() const { return m_view; }
    vk::Sampler getSampler() const { return m_sampler; }

private:
    void buildLayout(ComputePipelineLayout& layout);
    void createImages(uint32 resolution);
    void destroyImages();

    bool m_active = false;
    bool m_pipelineBuilt = false;
    ComputePipeline m_pipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets; // per slot: the TLAS handle is per slot
    vk::Image m_image;
    VmaAllocation m_memory = nullptr;
    vk::ImageView m_view;
    vk::Sampler m_sampler;
};
