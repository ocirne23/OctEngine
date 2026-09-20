export module RendererVK:GIProbePipeline;

import Core;
import Core.glm;

import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :GraphicsPipeline;
import :RenderPass;
import :DescriptorSet;
import :Sampler;
import :Layout;

// Diffuse GI probe system over a single persistent, world-space CASCADED CLIPMAP volume. GI_NUM_CASCADES
// nested toroidal probe grids (centred on the SCENE FOCUS - u_sceneFocus: the game's player, else the camera -
// with doubling spacing) store SH-L1 irradiance at absolute lattice
// positions; toroidal addressing carries irradiance forward in place with no hash table, copy, or ping-pong.
// Three compute passes per frame:
//   1. TLAS-instance pass : writes the per-instance VkAccelerationStructureInstanceKHR array on the GPU.
//   2. Sky-map pass       : bakes skyRadiance into a small lat-long image the trace samples on MISS rays
//                           (one 8x8-workgroup dispatch instead of an atmosphere march per miss).
//   3. Trace pass         : ray-queries the TLAS per probe, shades hits (reusing the light grid + sun), and
//                           temporally blends into each probe's SH-L1 (full replace for probes that just
//                           scrolled into the clipmap). The probe set + window is derived from the scene focus.
//                           A wave (4x4x4 probe block) traces every max(1, "GI/Update Interval Mult" x its
//                           priority factor) frames - close blocks cancel the multiplier, down to every frame -
//                           with the alpha scaled by the interval (same wall-time convergence); fresh probes
//                           always trace. See giWaveUpdateInterval in gi_probe.inc.glsl.
// The TLAS is built by the AccelerationStructure object between passes 1 and 3 (orchestrated by the Renderer).
export class GIProbePipeline final
{
public:
    ~GIProbePipeline();
    // maxTextures = fixed device-limit cap baked into the trace layout; numTextureDescriptors = live
    // variable count the trace sets are allocated with. Both owned by the Renderer.
    void initialize(uint32 maxTlasInstances, uint32 maxTextures, uint32 numTextureDescriptors);
    void reloadShaders(uint32 maxTextures);
    // The "GI" grid-shape tweaks (RendererVKLayout::g_giGrid: cascades, probes per axis, focus Y offset).
    // They are shader #defines in EVERY pipeline that samples the probes, so onGridChanged must: wait for
    // the GPU, call resizeGrid(), reload ALL shaders (Renderer::reloadShaders) and re-record.
    void registerGridTweaks(const oc::function<void()>& onGridChanged);
    // Re-allocates the persistent SH clipmap buffer for the current g_giGrid and schedules the one-time
    // clear (nothing is preserved - the toroidal slots mean something else now). GPU must be idle.
    void resizeGrid();
    // Grows the per-frame TLAS instance buffers (GPU scratch, nothing preserved; GPU must be idle).
    void resizeTlasInstanceBuffers(uint32 maxTlasInstances);
    // Re-allocates the trace descriptor sets with a grown live texture count (GPU must be idle).
    void resizeTextureDescriptors(uint32 numTextureDescriptors);

    // One-time clear of BOTH ping-pong grid tables (so the first frames' prev lookups are empty).
    void recordClearPersistent(CommandBuffer& commandBuffer);
    bool needsClear() const { return !m_cleared; }
    void markCleared() { m_cleared = true; }
    void doClear() { m_cleared = false; }

    struct TlasInstanceParams
    {
        Buffer& renderNodeTransforms;
        Buffer& meshInstances;   // InMeshInstance (cull input) buffer
        Buffer& instanceOffsets;
        Buffer& blasAddresses;   // mesh idx -> uint64 BLAS device address
        Buffer& rtMeshAlias;     // mesh idx -> RT mesh idx (LOD chains share one BLAS; packed into sbtOffset)
        Buffer& materialInfos;   // MATERIAL_FLAG_NO_RAYTRACING -> instance mask 0
        Buffer& nodePassMasks;   // nodes without PASS_GI|PASS_SHADOW -> instance mask 0
        Buffer& ubo;             // u_giTlasNumInstances (live count), u_giTrace1.w (range bound), u_sceneFocus (its center)
        uint32 capacity;         // the instance buffer's slot count: the dispatch covers all of it (tail written inactive)
    };
    // Cached (recorded once per invalidation): the live instance count and the range bound ride the UBO.
    void recordTlasInstances(CommandBuffer& commandBuffer, uint32 frameIdx, TlasInstanceParams& params);

    // Bakes this frame's sky into the sky map: layer 0 = skyRadiance (GI miss rays, the forward pass's
    // skyRadiance(up) ambient), layer 1 = the mirror sky (ocean / terrain-film reflection rays: 12-step
    // march, saturation curve, no ground term). Self-contained barriers: last frame's compute + fragment
    // reads of the single image -> this write -> this frame's compute + fragment reads. Recorded on EVERY
    // frame (ahead of the RT toggle) because the forward pass samples it.
    void recordSkyMap(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo);
    vk::ImageView getSkyMapView() const { return m_skyMapView; }
    vk::Sampler getSkyMapSampler() const { return m_skyMapSampler; }

    struct TraceParams
    {
        Buffer& ubo;
        Buffer& lightInfos;
        Buffer& lightGrid;
        Buffer& lightTable;
        Buffer& vertexBuffer;
        Buffer& indexBuffer;
        Buffer& meshInfos;
        Buffer& meshInstances;   // InMeshInstance buffer (instanceCustomIndex indexes into this)
        Buffer& materialInfos;
        vk::AccelerationStructureKHR tlas;
        vk::ImageView shadowMapView;
        vk::Sampler shadowMapSampler;
    };
    // Cached (recorded once per invalidation): the frame index, the previous focus and the tweaks ride the
    // UBO (u_frameIndex, u_giTrace0/1 - see getTraceParams0 / getTlasRange).
    void recordTrace(CommandBuffer& commandBuffer, uint32 frameIdx, TraceParams& params);
    // Rewrites one slot of the trace set's texture array (binding 13) with a new or streamed texture's view.
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);
    // u_giTrace0: x = rays per probe, y = temporal alpha of THIS frame, z = max ray distance, w = update interval multiplier.
    // "GI/Temporal Alpha" is the per-frame blend AT 60 FPS; y is that rate compounded over this frame's wall
    // delta, so the field converges in the same WALL time at any frame rate. Per frame and uncorrected, 0.01
    // is a 1.7 s time constant at 60 fps but 0.4 s at 240 fps, and the blend's noise wanders 4x faster - it
    // reads as flicker. The delta is clamped so a hitch frame cannot replace the history.
    glm::vec4 getTraceParams0(float wallDeltaSec) const
    {
        const float frames60 = 60.0f * glm::clamp(wallDeltaSec, 0.001f, 0.1f);
        const float frameAlpha = 1.0f - powf(1.0f - glm::clamp(m_giTemporalAlpha, 0.0f, 1.0f), frames60);
        return glm::vec4((float)oc::max(m_giRaysPerProbe, 1), frameAlpha, m_giMaxRayDist, oc::max(m_giUpdateIntervalMult, 1.0f));
    }
    float getTlasRange() const { return m_tlasRange; }
    // u_giPriorityDist / Falloff / FrustumWeight: x = nominal-rate distance from the focus (m), y = distance falloff exponent, z = frustum weight.
    glm::vec3 getPriorityParams() const { return glm::vec3(m_giPriorityDist, oc::max(m_giPriorityFalloff, 0.0f), m_giPriorityFrustumWeight); }

    // Debug visualization: a sphere impostor at every clipmap probe, drawn into the main color pass - the SH
    // evaluated per pixel (x "GI/Strength") in the irradiance mode, a shaded flat colour in the other modes.
    // initializeDebug must be called after the main render pass exists.
    void initializeDebug(vk::RenderPass renderPass);
    void reloadDebugShaders(vk::RenderPass renderPass);
    void recordDebugDraw(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, float radius, uint32 mode);

    Buffer& getTlasInstanceBuffer(uint32 frameIdx) { return m_tlasInstanceBuffer[frameIdx]; }
    // Persistent GI clipmap SH volume (consumed by the main pass's fragment shader).
    Buffer& getGiGridDataBuffer() { return m_giGridData; }
    float getStrength() const { return m_giStrength; }
    // x = Chebyshev variance floor (fraction of probe spacing), y = unused, z = probe weight floor,
    // w = mean scale. Uploaded to the frame UBO (u_giVisParams) for every probe-sampling shader.
    glm::vec4 getVisibilityParams() const { return glm::vec4(m_visVarianceFloor, 0.0f, m_visWeightFloor, m_visMeanScale); }

private:
    void buildTlasInstanceLayout(ComputePipelineLayout& layout);
    void buildSkyMapLayout(ComputePipelineLayout& layout);
    void buildTraceLayout(ComputePipelineLayout& layout, uint32 maxTextures);
    void buildDebugLayout(GraphicsPipelineLayout& layout);
    void createSkyMap();

    ComputePipeline m_tlasInstancePipeline;
    ComputePipeline m_skyMapPipeline;
    ComputePipeline m_tracePipeline;
    GraphicsPipeline m_debugPipeline;
    vk::RenderPass m_debugRenderPass;

    // Sky map (gi_sky_map.cs.glsl): a small lat-long RGBA16F 2-layer array (0 = skyRadiance, 1 = mirror
    // sky), GENERAL layout for life, rewritten every frame. Single-buffered under recordSkyMap's barriers.
    // 256x128: reflections look along the horizon band, where the sunset gradient needs ~1.4 deg rows.
    static constexpr uint32 SKY_MAP_WIDTH = 256, SKY_MAP_HEIGHT = 128, SKY_MAP_LAYERS = 2;
    vk::Image m_skyMapImage;
    VmaAllocation m_skyMapMemory{};
    vk::ImageView m_skyMapView;
    vk::Sampler m_skyMapSampler; // linear, U repeat (azimuth wraps), V clamp (poles)

    // GI probe trace tuning (runtime-tweakable; consumed by GIProbePipeline::recordTrace).
    int m_giRaysPerProbe = 17;         // gather rays per probe per visit
    float m_giUpdateIntervalMult = 16.0f; // global factor of a wave's update interval: frames = max(1, this x the priority factor),
                                         // so it is the interval AT "GI/Priority Distance" and close blocks cancel it (fresh probes always trace)
    float m_giTemporalAlpha = 0.025f;  // blend toward freshly traced irradiance per frame AT 60 FPS (rescaled by the wall delta, see getTraceParams0)
    float m_giMaxRayDist = 8.0f;       // gather ray max distance (world units)
    float m_giStrength = 1.0f;         // multiplier on the sampled probe irradiance at shading time
    float m_tlasRange = 4096.0f;        // TLAS instance range bound around the camera (origin distance)

    // Update priority (gi_probe.inc.glsl giWavePriority, one factor of giWaveUpdateInterval): a wave's interval is multiplied by
    // (focus distance / priorityDist) ^ falloff / viewBoost - NO bounds: a close wave's factor < 1 cancels the
    // interval multiplier, and the far field has no cap. viewBoost = frustumWeight for a wave IN the view
    // frustum (its interval divides by it), fading to 1 over priorityDist metres outside it.
    // Defaults (first-person scene, focus = camera, interval mult 16, falloff 3, weight 5): in view the
    // interval is 16 x (d / 10)^3 / 5 frames - every frame within ~8.5 m, 3 at 10 m, 25 at 20 m, 400 at
    // 50 m; out of view, 5x that. A steep curve: all the rays go to what is near the focus.
    float m_giPriorityDist = 8.0f;         // focus distance (m) of the nominal rate (factor 1) for a wave OUT of view; the falloff curve pivots here
    float m_giPriorityFalloff = 1.5f;       // exponent on (distance / priorityDist): 1 = linear, 2 = quadratic (far field all but stops), 0.5 = gentle, 0 = no distance term
    float m_giPriorityFrustumWeight = 5.0f; // a wave IN the view frustum has its interval divided by this (1 = the frustum is ignored)

    // SH-L1 depth visibility (Chebyshev) lookup tuning. Three knobs, each with its own job: the mean scale
    // moves the occlusion THRESHOLD, the variance floor is the MINIMUM edge softness (the measured variance
    // widens it where the depth really spreads - sideways past a wall), the weight floor is the leak level /
    // the all-occluded fallback. The exponent is fixed (GI_VIS_CHEB_POWER = 2 in gi_probe.inc.glsl): near the
    // threshold it only rescales the floor (weight ~ 1 - p (delta / sigma)^2), and the weight floor cuts the
    // tail it shapes. An additive mean bias was tried and removed: the same effect as the scale or the floor.
    float m_visVarianceFloor = 0.35f;  // min std-dev as a fraction of the cascade's probe spacing: covers the L1 mean's error
                                       // toward a wall (0.15 .. 0.4 spacings); below ~0.25 the ray jitter moves the edge (flicker)
    float m_visWeightFloor = 0.01f;    // occluded probes keep this much weight (0 = hard cutoff, noisy when all 8 are occluded)
    float m_visMeanScale = 1.2f;       // scales the reconstructed depth (mean AND, by its square, the second moment, so the
                                       // variance stays consistent) before the Chebyshev test: > 1 widens each probe's visible
                                       // footprint. A wall at distance m reads as scale x m, so points up to (scale - 1) x m
                                       // BEHIND it keep full weight: the leak depth

    // Single persistent GI clipmap SH volume: irradiance carries forward in place (toroidal addressing),
    // so there is no prev/cur ping-pong. Read across frames by the fragment shader and read+written by the
    // trace compute; GI is low-frequency so the cross-frame hazard is tolerated (slightly stale reads).
    Buffer m_giGridData;

    // Per-frame instance buffer: written each frame by the TLAS-instance compute and consumed by that
    // frame's TLAS build; double-buffered for the same cross-frame-hazard reason as the TLAS itself.
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tlasInstanceBuffer;

    Sampler m_textureSampler;

    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tlasInstanceSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_skyMapSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_traceSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_debugSets;

    // PERSISTENT descriptor-update scratch (built once by buildUpdateScratch): the per-frame records only
    // patch buffer/image handles into these, so "Record GI" allocates nothing after its first frame - the
    // old per-call DescriptorSetUpdateInfo temporaries cost ~20 heap vectors plus a texture-count-sized
    // one every frame. The texture list keeps its capacity across frames (clear + push_back).
    void buildUpdateScratch();
    bool m_updateScratchBuilt = false;
    oc::array<DescriptorSetUpdateInfo, 9> m_tlasUpdates;   // bindings 0..7 of the TLAS-instance set + the UBO (8)
    oc::array<DescriptorSetUpdateInfo, 2> m_skyUpdates;    // the sky-map set: UBO + storage image
    oc::vector<DescriptorSetUpdateInfo> m_traceUpdates;    // the trace set's fixed bindings (see recordTrace for the index map)
    // Writes every live texture view into the trace sets' array (binding 13) - at (re)allocation only.
    void fillTextureDescriptors();

    bool m_cleared = false;
};
