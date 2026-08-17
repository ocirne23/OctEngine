export module RendererVK:OceanSimulationPipeline;

import Core;
import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;

// GPU FFT ocean simulation (Tessendorf 2001, "Simulating Ocean Water"; spectrum + spreading from Horvath
// 2015, "Empirical directional wave spectra for computer graphics"). Per frame, entirely UBO-driven so it
// records once and every parameter is live:
//   1. spectrum : per cascade, generate the TMA initial spectrum h0(k) (JONSWAP x Kitaigorodskii
//                 finite-depth attenuation, Hasselmann directional spreading; deterministic per-texel
//                 gaussians) and time-evolve it with the finite-depth dispersion w^2 = g k tanh(k D).
//                 Eight real output signals (h, Dx, Dz, dh/dx, dh/dz, dDx/dx, dDz/dz, dDx/dz) pack as
//                 4 Hermitian spectra = 2 complex pairs per RGBA32F layer, 2 layers per cascade.
//   2. ifft     : radix-2 Stockham inverse FFT in shared memory, one dispatch per axis (rows, then
//                 columns), ping-ponging between two spectrum array images.
//   3. assemble : undo the (-1)^(x+z) spectrum centering, unpack the 8 signals and write the output maps:
//                 layer c              = displacement (Dx, h, Dz, dDx/dz)
//                 layer   CASCADES + c = gradients    (dh/dx, dh/dz, dDx/dx, dDz/dz)
//                 layer 2*CASCADES + c = slope second moments (LEAN, Olano & Baker 2010 / Bruneton 2010:
//                                        mip-averaged squares recover the filtered-out slope variance,
//                                        which the water shader adds as microfacet roughness)
//                 Choppiness lambda is NOT baked in - the samplers apply it, so it stays live.
//   4. foam     : temporal whitecap COVERAGE accumulation (persistent mask over cascade 0's patch):
//                 inject on total-Jacobian folding (mip-filtered to the mask's texel), decay per frame;
//                 stashed into moments[cascade 0].w for mipped sampling. The mask stores WHERE whitecaps
//                 are; the water shader erodes it with world-anchored noise for crisp edge detail.
//   5. mip chain: blit-downsample the maps so distant water samples prefiltered slopes (no shimmer).
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

    // Records the whole per-frame simulation. ubo = that frame slot's main UBO (time + ocean params).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo);

    // Output maps: 2D array, layers [0, CASCADES) = displacement, [CASCADES, 2*CASCADES) = gradients,
    // full mip chain, SHADER_READ_ONLY between frames. Sampled by the G-buffer/forward vertex shaders
    // (displacement) and the ocean fragment shader (gradients).
    vk::ImageView getMapsView() const { return m_mapsView; }
    vk::Sampler getMapsSampler() const { return m_mapsSampler.getSampler(); }

    // CPU displacement readback (buoyancy): each simulated frame copies the displacement layers' mip
    // READBACK_MIP into that frame slot's host-visible buffer — a coarse tile is all physics needs (the
    // swell moves bodies; sub-texel chop doesn't). Read the CURRENT slot only after beginFrame's fence
    // wait and only until that slot resubmits (Procedural::OceanGenerator copies it out right away);
    // contents are frame N-2's ocean, imperceptible for bobbing. Texels are RGBA16F (Dx, h, Dz, dDxz),
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
    // 3 packed-complex-pair layers per cascade: 2 carry the 8 geometry signals, the 3rd carries the
    // vertical acceleration d2h/dt2 = -w^2 h~ (+ dh/dt in its imaginary half) for the Longuet-Higgins
    // breaking-crest foam criterion (downward crest acceleration > fraction of g).
    static constexpr uint32 SPECTRUM_LAYERS = CASCADES * 3;
    static constexpr uint32 MAPS_LAYERS = CASCADES * 3;     // displacement + gradients + moments per cascade

    void buildSpectrumLayout(ComputePipelineLayout& layout);
    void buildFftLayout(ComputePipelineLayout& layout);
    void buildAssembleLayout(ComputePipelineLayout& layout);
    void buildFoamLayout(ComputePipelineLayout& layout);
    void createImages();
    void destroyImages();

    ComputePipeline m_spectrumPipeline;
    ComputePipeline m_fftPipeline;
    ComputePipeline m_assemblePipeline;
    ComputePipeline m_foamPipeline;

    // All sets are per frame slot: the spectrum binds that frame's UBO, and the others must not be
    // host-updated (cmdUpdateDescriptorSets is immediate) while the other slot's cached CB is in flight.
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_spectrumSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_fftHorizontalSets; // ping -> pong
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_fftVerticalSets;   // pong -> ping
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_assembleSets;      // ping -> maps mip 0
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_foamSets;          // maps mip 0 + foam accum

    // Ping/pong complex spectra (RGBA32F = 2 complex values), kept in GENERAL for their whole life.
    vk::Image m_spectrumImage[2]{};
    VmaAllocation m_spectrumMemory[2]{};
    vk::ImageView m_spectrumView[2]{};

    // Output displacement/gradient/moments maps (RGBA16F, mipped). SHADER_READ_ONLY between frames.
    vk::Image m_mapsImage{};
    VmaAllocation m_mapsMemory{};
    vk::ImageView m_mapsView{};     // all mips (sampled)
    vk::ImageView m_mapsMip0View{}; // mip 0 (assemble/foam storage access)
    uint32 m_mapsMipLevels = 0;

    // Persistent foam coverage mask (R16F, cascade 0's patch; GENERAL, cleared once at init).
    vk::Image m_foamImage{};
    VmaAllocation m_foamMemory{};
    vk::ImageView m_foamView{};

    // Displacement readback (see getDisplacementReadback). Host-visible + coherent, persistently mapped.
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_readbackBuffers;
    oc::array<oc::span<uint8>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_readbackMapped;

    Sampler m_mapsSampler; // repeat + trilinear + aniso (the default engine sampler)
};
