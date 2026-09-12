export module RendererVK:TerrainWetnessPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;

// Terrain wetness clipmap: ONE persistent R16F image (two layers = ping/pong), TERRAIN_WET_RES^2 texels,
// stored toroidally around the scene focus (terrain_wetness.inc.glsl has the addressing; the same scheme
// as the GI probe clipmap). Per frame, terrain_wetness.cs.glsl reads last frame's layer (optionally
// through a 3x3 diffusion tent - a shader define), decays it and wets the ground the live ocean surface
// covers (the swash tongue, permanently submerged seabed) at a rate-limited wet-in, plus a uniform rain
// term, into the other layer; the TERRAIN fragment shader samples the written layer to darken albedo and
// drop roughness. GENERAL layout for its whole life, cleared once at init. Entirely UBO-driven
// (u_terrainWetParams*): records once, every parameter is live.
export class TerrainWetnessPipeline final
{
public:
    TerrainWetnessPipeline() = default;
    ~TerrainWetnessPipeline();
    TerrainWetnessPipeline(const TerrainWetnessPipeline&) = delete;

    // onDefinesChanged: fired when a baked-define tweak (Diffusion / Diffusion spread) changes - the
    // Renderer idles the GPU, calls reloadShaders and re-records (the light grid's pattern).
    void initialize(oc::function<void()> onDefinesChanged);
    void reloadShaders();

    struct RecordParams
    {
        Buffer* ubo = nullptr;                // that frame slot's main UBO
        vk::ImageView terrainView;            // baked terrain-data cascades (height / water level / climate)
        vk::Sampler terrainSampler;
        vk::ImageView oceanMapsView;          // FFT displacement maps (the live wave surface)
        vk::Sampler oceanMapsSampler;
    };
    // Records the per-frame decay + injection dispatch. Runs after the ocean sim (this frame's maps) and
    // before the scene forward pass (which samples the result).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);

    // Points the terrain-data binding (UPDATE_AFTER_BIND) at the active ping-pong image; refreshed per
    // frame by the Renderer so a CPU re-bake swaps images without re-recording the cached CB.
    void updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler);

    // The wetness image (GENERAL layout) for the terrain fragment shader's binding.
    vk::ImageView getView() const { return m_view; }
    vk::Sampler getSampler() const { return m_sampler.getSampler(); }

private:
    static constexpr uint32 RES = RendererVKLayout::TERRAIN_WET_RES;

    void buildLayout(ComputePipelineLayout& layout);
    void createImage();
    void destroyImage();

    ComputePipeline m_pipeline;
    // Per frame slot: the set binds that frame's UBO, and cmdUpdateDescriptorSets is immediate, so the
    // other slot's cached CB must keep its own set.
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets;

    vk::Image m_image{};
    VmaAllocation m_memory{};
    vk::ImageView m_view{};
    Sampler m_sampler; // the reader uses texelFetch; the sampler only satisfies the combined binding

    // "Terrain/Wetness" Diffusion toggle, BAKED as the WET_DIFFUSION define on the compute shader (the
    // tent reads per texel are not worth a uniform branch); a change reloads it. The spread RATE is
    // UBO-driven (Renderer::TerrainWetTweaks::diffusionRate, packed per frame with dt), so it is live
    // and framerate independent - a define could not carry the frame delta.
    bool m_diffusion = true;
};
