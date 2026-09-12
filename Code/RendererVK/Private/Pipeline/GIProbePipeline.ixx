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
//                           "GI/Update interval": a probe traces every N frames (per workgroup, alpha scaled
//                           by N - same wall-time convergence, 1/N of the rays); fresh probes always trace.
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
    // onDefineChanged: g_giGrid values that are shader defines but change no resource (the Chebyshev
    // power) - reload every shader, no resize, no clipmap clear.
    void registerGridTweaks(const oc::function<void()>& onGridChanged, const oc::function<void()>& onDefineChanged);
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
        glm::vec3 viewPos;       // TLAS range bound center (the scene focus: Renderer::sceneFocusOrCamera)
        uint32 numInstances;
    };
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
        uint32 frameIndex;       // free-running frame counter (RNG seed)
        glm::vec3 prevViewPos;   // last frame's scene focus (previous clipmap window, drives probe freshness)
    };
    void recordTrace(CommandBuffer& commandBuffer, uint32 frameIdx, TraceParams& params);
    // Rewrites one slot of the trace set's texture array (binding 13) with a streamed texture's current view.
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);

    // Debug visualization: instanced cubes at every clipmap probe, drawn into the main color pass.
    // initializeDebug must be called after the main render pass exists.
    void initializeDebug(vk::RenderPass renderPass);
    void reloadDebugShaders(vk::RenderPass renderPass);
    // Depth-prepass reuse: the scene pass depth is read-only, so the debug spheres may not write depth
    // (they lose self-sorting; debug-only). Rebuild via reloadDebugShaders after flipping.
    void setDebugDepthReadOnly(bool readOnly) { m_debugDepthReadOnly = readOnly; }
    void recordDebugDraw(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, float radius, uint32 mode);

    Buffer& getTlasInstanceBuffer(uint32 frameIdx) { return m_tlasInstanceBuffer[frameIdx]; }
    // Persistent GI clipmap SH volume (consumed by the main pass's fragment shader).
    Buffer& getGiGridDataBuffer() { return m_giGridData; }
    float getStrength() const { return m_giStrength; }
    // x = Chebyshev variance floor (fraction of probe spacing), y = Chebyshev power, z = probe weight
    // floor, w = mean scale. Uploaded to the frame UBO (u_giVisParams) for every probe-sampling shader.
    glm::vec4 getVisibilityParams() const { return glm::vec4(m_visVarianceFloor, 0.0f /* y unused: the power is the GI_VIS_CHEB_POWER define */, m_visWeightFloor, m_visMeanScale); }

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
    int m_giUpdateInterval = 8;        // a probe traces every N frames (alpha scaled by N; fresh probes always trace)
    float m_giTemporalAlpha = 0.005f;  // per-frame blend toward freshly traced irradiance
    float m_giMaxRayDist = 8.0f;       // gather ray max distance (world units)
    float m_giStrength = 1.0f;         // multiplier on the sampled probe irradiance at shading time
    float m_tlasRange = 4096.0f;        // TLAS instance range bound around the camera (origin distance)

    // SH-L1 depth visibility (Chebyshev) lookup tuning. Higher variance floor / lower power = softer,
    // temporally stabler occlusion edges (the L1 depth estimate wobbles with the per-frame ray jitter);
    // lower floor / higher power = sharper leak blocking.
    float m_visVarianceFloor = 0.3f;   // min std-dev as a fraction of the cascade's probe spacing
    // (the Chebyshev exponent lives in RendererVKLayout::g_giGrid.visChebPower - a shader define)
    float m_visWeightFloor = 0.01f;    // occluded probes keep this much weight (0 = hard cutoff)
    float m_visMeanScale = 2.5f;      // scales the reconstructed mean distance before the Chebyshev test:
                                       // > 1 widens each probe's visible footprint (more overlap/smoothing),
                                       // countering the L1 blur's distance underestimate at grazing angles

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
    oc::array<DescriptorSetUpdateInfo, 8> m_tlasUpdates;   // bindings 0..7 of the TLAS-instance set
    oc::array<DescriptorSetUpdateInfo, 2> m_skyUpdates;    // the sky-map set: UBO + storage image
    oc::vector<DescriptorSetUpdateInfo> m_traceUpdates;    // the trace set's fixed bindings (see recordTrace for the index map)
    DescriptorSetUpdateInfo m_traceTexUpdate;              // binding 13: the whole texture array (written separately; may be empty)

    bool m_cleared = false;
    bool m_debugDepthReadOnly = true; // scene pass depth is read-only under depth-prepass reuse
};
