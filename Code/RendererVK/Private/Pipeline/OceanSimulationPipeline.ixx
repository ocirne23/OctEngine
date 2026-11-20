export module RendererVK:OceanSimulationPipeline;

import Core;
import Core.glm;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;
import :RenderParams;
import Settings.fwd;

// GPU FFT ocean simulation (Tessendorf 2001, "Simulating Ocean Water"; spectrum + spreading from Horvath
// 2015, "Empirical directional wave spectra for computer graphics"). Per frame, entirely UBO-driven so it
// records once and every parameter is live:
//   1. spectrum : per cascade, generate the TMA initial spectrum h0(k) (JONSWAP x Kitaigorodskii
//                 finite-depth attenuation, Hasselmann directional spreading; deterministic per-texel
//                 gaussians) and time-evolve it with the finite-depth dispersion w^2 = g k tanh(k D).
//                 Eight real output signals (h, Dx, Dz, dh/dx, dh/dz, dDx/dx, dDz/dz, dDx/dz) pack as
//                 4 Hermitian spectra = 2 complex pairs per RGBA16F layer, 2 layers per cascade.
//                 A cascade keeps k < half its Nyquist, so only the (N/2)^2 block |m| < N/4 is non-zero:
//                 the pass runs on that block and only that block is stored.
//   2. ifft     : radix-2 in-place inverse FFT in shared memory, one dispatch per axis: the ROWS of the
//                 block (N/2 lines; the other rows are zero and stay zero) into an N x N/2 image, then the
//                 COLUMNS, which write the output maps (mip 0) themselves:
//                 layer c              = displacement (Dx, h, Dz, -)
//                 layer   CASCADES + c = slopes (dh/dx, dh/dz) + their second moments (LEAN, Olano &
//                                        Baker 2010 / Bruneton 2010: mip-averaged squares recover the
//                                        filtered-out slope variance, which the water shader adds as
//                                        microfacet roughness)
//                 layer 2*CASCADES + c = the fold Jacobian's terms (dDx/dx, dDz/dz, dDx/dz) + the vertical
//                                        acceleration d2h/dt2 = g (dDx/dx + dDz/dz) (the breaking-crest
//                                        foam criterion, Longuet-Higgins; exact in deep water)
//                 One layer per use: the geometry, a normal, a fold test each read ONE layer per cascade.
//                 Choppiness lambda is NOT baked in - the samplers apply it, so it stays live.
//   3. foam     : the WORLD-SPACE foam field (ocean_foam_field.inc.glsl): OCEAN_FOAM_LEVELS camera-centred
//                 clipmap levels in drifted rest coordinates, one foam amount per texel -
//                 inject on breaking, decay per frame, scroll with the camera. Written into the maps' last
//                 layers (layer 3 * CASCADES + level) so every maps sampler reads it, mipped.
//   4. mip chain: blit-downsample the maps so distant water samples prefiltered slopes (no shimmer).
// OCEAN_CASCADES band-split patches (each keeps a disjoint wavenumber range) break up tiling; the raster
// passes sum all cascades (ocean_wave.inc.glsl).
export class OceanSimulationPipeline final
{
public:
    OceanSimulationPipeline() = default;
    ~OceanSimulationPipeline();
    OceanSimulationPipeline(const OceanSimulationPipeline&) = delete;

    void initialize();
    void reloadShaders();
    // ---- The CPU mirror of the ocean, pushed in by Procedural::OceanGenerator ----
    // The spectrum params drive BOTH this compute simulation (the TMA spectrum is re-evaluated every
    // frame, so all of it is live) and the surface shading, through the frame UBO the Renderer builds.
    void setOceanParams(const OceanParams& params) { m_oceanParams = params; }
    const OceanParams& getOceanParams() const { return m_oceanParams; }
    bool isOceanEnabled() const { return m_oceanParams.enabled; }
    // Deepest current wave trough below the calm water level (m, >= 0; the generator estimates it from
    // its displacement readback). Sizes the waterline band inside which the fog scatter samples the live
    // FFT wave height for the underwater fog boundary.
    void setWaveTrough(float meters) { m_waveTrough = glm::max(meters, 0.0f); }
    float getWaveTrough() const { return m_waveTrough; }
    // Worst-case distance the ocean's vertex shader moves a clipmap vertex off its authored lattice
    // position. The per-instance frustum cull adds it to the ocean sectors' bounding spheres, which were
    // built from the UNDISPLACED mesh: the choppy horizontal displacement grows with "Choppiness", and
    // without this it culls sectors whose crests are still on screen - gaps along the screen edges.
    void setDisplacementExtent(float meters) { m_displacementExtent = glm::max(meters, 0.0f); }
    float getDisplacementExtent() const { return m_displacementExtent; }
    // The LIVE water surface world Y under the camera (the generator's CPU wave-height mirror, ~2 frames
    // of latency), or none: the particle draw's camera-side gate reads it from the UBO instead of
    // sampling the waves per particle.
    void setCameraWaterSurface(float worldY) { m_cameraWaterSurface = worldY; m_cameraWaterSurfaceValid = true; }
    void clearCameraWaterSurface() { m_cameraWaterSurfaceValid = false; }
    float getCameraWaterSurface() const { return m_cameraWaterSurface; }
    bool hasCameraWaterSurface() const { return m_cameraWaterSurfaceValid; }
    // The "Ocean/Spray *" settings plus the emitter slot the Particle system publishes (UINT32_MAX = off,
    // Renderer::setOceanSprayEmitter). Everything rides the frame UBO (oceanSpray0/1/2), which the
    // Renderer builds from getSprayParams() - the spray step itself reads it there.
    const OceanSprayParams& getSprayParams() const;
    void setSprayEmitter(uint32 slot) { m_sprayEmitter = slot; }
    uint32 getSprayEmitter() const { return m_sprayEmitter; }

    // The spray step's inputs (ocean_spray.cs.glsl, the last step of record: the particle GPU spawn
    // path's producer buffers plus the terrain-data cascades for the shore test).
    struct SprayParams
    {
        Buffer* particleCounters = nullptr;
        Buffer* particleRequests = nullptr;
        vk::ImageView terrainView;
        vk::Sampler terrainSampler;
    };
    // The foam field's per-frame CPU state into the UBO (oceanFoamField / 1 / Levels): the drift integrated
    // over dt along the swell's travel, each level's origin snapped around the camera and the whole texels it
    // moved since the last call (the compute's read offset). Once per built frame, in frame order - the
    // shift is relative to the previous call, which the other ping/pong slot was simulated with.
    void advanceFoamField(const glm::vec3& cameraPos, float dt);
    // Its results (u_ocean_foamLevels / foamDrift): a level's origin (xy, m) and the whole texels it moved (zw).
    const glm::vec4& getFoamLevel(uint32 level) const { return m_foamLevels[level]; }
    glm::vec2 getFoamDrift() const { return m_foamDrift; }
    // Records the whole per-frame simulation. ubo = that frame slot's main UBO (time + ocean params).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, const SprayParams& spray);
    // Points the spray step's terrain-data binding (UPDATE_AFTER_BIND) at the active ping-pong image;
    // refreshed per frame by the Renderer so a CPU re-bake swaps images without re-recording the CB.
    void updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler);

    // Output maps: 2D array (the layers above), full mip chain, SHADER_READ_ONLY between frames. Sampled by
    // the forward ocean vertex shader (displacement), the ocean fragment shader (slopes, fold) and every
    // other water reader.
    vk::ImageView getMapsView() const { return m_mapsView; }
    vk::Sampler getMapsSampler() const { return m_mapsSampler.getSampler(); }

    // CPU displacement readback (buoyancy): each simulated frame copies the displacement layers' mip
    // READBACK_MIP into that frame slot's host-visible buffer - a coarse tile is all physics needs (the
    // swell moves bodies; sub-texel chop doesn't). Read the CURRENT slot only after beginFrame's fence
    // wait and only until that slot resubmits (Procedural::OceanGenerator copies it out right away);
    // contents are frame N-2's ocean, imperceptible for bobbing. Texels are RGBA16F (Dx, h, Dz, -),
    // READBACK_RES^2 per cascade, cascades packed consecutively.
    static constexpr uint32 READBACK_MIP = 2;
    static constexpr uint32 READBACK_RES = RendererVKLayout::OCEAN_FFT_SIZE >> READBACK_MIP;
    oc::span<const uint16> getDisplacementReadback(uint32 frameIdx) const
    {
        const oc::span<uint8> mapped = m_readbackMapped[frameIdx];
        return { reinterpret_cast<const uint16*>(mapped.data()), mapped.size() / sizeof(uint16) };
    }

private:
    static constexpr uint32 N = RendererVKLayout::OCEAN_FFT_SIZE;
    static constexpr uint32 CASCADES = RendererVKLayout::OCEAN_CASCADES;
    // 2 packed-complex-pair layers per cascade: the 8 signals.
    static constexpr uint32 SPECTRUM_LAYERS = CASCADES * 2;
    static constexpr uint32 FOAM_LEVELS = RendererVKLayout::OCEAN_FOAM_LEVELS;
    // displacement + slopes + fold per cascade, then the foam field's levels
    static constexpr uint32 MAPS_LAYERS = CASCADES * 3 + FOAM_LEVELS;

    void buildSpectrumLayout(ComputePipelineLayout& layout);
    void buildFftLayout(ComputePipelineLayout& layout, bool columns);
    void buildFoamLayout(ComputePipelineLayout& layout);
    void buildSprayLayout(ComputePipelineLayout& layout);
    void createImages();
    void destroyImages();

    ComputePipeline m_spectrumPipeline;
    ComputePipeline m_fftRowsPipeline;
    ComputePipeline m_fftColumnsPipeline; // OCEAN_FFT_COLUMNS: writes the maps
    ComputePipeline m_foamPipeline;
    ComputePipeline m_sprayPipeline; // breaking-crest spray -> particle spawn requests
    uint32 m_sprayEmitter = UINT32_MAX;
    OceanParams m_oceanParams;
    float m_waveTrough = 0.0f;          // 0 while the ocean is disabled
    float m_displacementExtent = 0.0f;  // idem
    float m_cameraWaterSurface = 0.0f;
    bool m_cameraWaterSurfaceValid = false;

    // All sets are per frame slot: the spectrum binds that frame's UBO, and the others must not be
    // host-updated (cmdUpdateDescriptorSets is immediate) while the other slot's cached CB is in flight.
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_spectrumSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_fftRowsSets;    // spectrum block -> row result
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_fftColumnsSets; // row result -> maps mip 0
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_foamSets;       // maps mip 0 + foam accum
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_spraySets;      // maps + terrain + particle buffers

    // The complex spectra (RGBA16F = 2 complex values), kept in GENERAL for their whole life: [0] = the
    // spectrum's non-zero block (N/2 x N/2), [1] = the row FFT's result (N x N/2).
    vk::Image m_spectrumImage[2]{};
    VmaAllocation m_spectrumMemory[2]{};
    vk::ImageView m_spectrumView[2]{};

    // Output displacement / slope / fold maps (RGBA16F, mipped). SHADER_READ_ONLY between frames.
    vk::Image m_mapsImage{};
    VmaAllocation m_mapsMemory{};
    vk::ImageView m_mapsView{};     // all mips (sampled)
    vk::ImageView m_mapsMip0View{}; // mip 0 (column FFT / foam storage access)
    uint32 m_mapsMipLevels = 0;

    // The foam field's ping/pong state (R16F, layer = level * 2 + slot; GENERAL, cleared once at init).
    vk::Image m_foamImage{};
    VmaAllocation m_foamMemory{};
    vk::ImageView m_foamView{};
    // advanceFoamField's state: the accumulated drift (m), each level's origin in whole texels, and the
    // texel the origins were addressed with (a change re-addresses everything: the field starts over).
    glm::vec2 m_foamDrift = glm::vec2(0.0f);
    oc::array<glm::vec2, FOAM_LEVELS> m_foamOriginCells{};
    oc::array<glm::vec4, FOAM_LEVELS> m_foamLevels{};
    float m_foamTexel = 0.0f;
    bool m_foamValid = false; // false after a frame with the ocean off (no simulation ran: stale state)

    // Displacement readback (see getDisplacementReadback). Host-visible + coherent, persistently mapped.
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_readbackBuffers;
    oc::array<oc::span<uint8>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_readbackMapped;

    Sampler m_mapsSampler; // repeat + trilinear + aniso (the default engine sampler)
};
