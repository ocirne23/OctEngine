export module RendererVK:GIProbePipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :GraphicsPipeline;
import :RenderPass;
import :DescriptorSet;
import :Sampler;
import :Layout;

// Diffuse GI probe system over a single persistent, world-space CASCADED CLIPMAP volume. GI_NUM_CASCADES
// nested toroidal probe grids (camera-centered, doubling spacing) store SH-L1 irradiance at absolute lattice
// positions; toroidal addressing carries irradiance forward in place with no hash table, copy, or ping-pong.
// Two compute passes per frame:
//   1. TLAS-instance pass : writes the per-instance VkAccelerationStructureInstanceKHR array on the GPU.
//   2. Trace pass         : ray-queries the TLAS per probe, shades hits (reusing the light grid + sun), and
//                           temporally blends into each probe's SH-L1 (full replace for probes that just
//                           scrolled into the clipmap). The probe set + window is derived from the camera.
// The TLAS is built by the AccelerationStructure object between the two passes (orchestrated by the Renderer).
export class GIProbePipeline final
{
public:
    // maxTextures = fixed device-limit cap baked into the trace layout; numTextureDescriptors = live
    // variable count the trace sets are allocated with. Both owned by the Renderer.
    void initialize(uint32 maxTlasInstances, uint32 maxTextures, uint32 numTextureDescriptors);
    void reloadShaders(uint32 maxTextures);
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
        glm::vec3 viewPos;       // TLAS range bound center (camera)
        uint32 numInstances;
    };
    void recordTlasInstances(CommandBuffer& commandBuffer, uint32 frameIdx, TlasInstanceParams& params);

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
        glm::vec3 prevViewPos;   // last frame's camera (previous clipmap window, drives probe freshness)
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
    glm::vec4 getVisibilityParams() const { return glm::vec4(m_visVarianceFloor, m_visChebPower, m_visWeightFloor, m_visMeanScale); }

private:
    void buildTlasInstanceLayout(ComputePipelineLayout& layout);
    void buildTraceLayout(ComputePipelineLayout& layout, uint32 maxTextures);
    void buildDebugLayout(GraphicsPipelineLayout& layout);

    ComputePipeline m_tlasInstancePipeline;
    ComputePipeline m_tracePipeline;
    GraphicsPipeline m_debugPipeline;
    vk::RenderPass m_debugRenderPass;

    // GI probe trace tuning (runtime-tweakable; consumed by GIProbePipeline::recordTrace).
    int m_giRaysPerProbe = 17;         // gather rays per probe per frame
    float m_giTemporalAlpha = 0.005f;  // per-frame blend toward freshly traced irradiance
    float m_giMaxRayDist = 8.0f;       // gather ray max distance (world units)
    float m_giStrength = 1.0f;         // multiplier on the sampled probe irradiance at shading time
    float m_tlasRange = 4096.0f;        // TLAS instance range bound around the camera (origin distance)

    // SH-L1 depth visibility (Chebyshev) lookup tuning. Higher variance floor / lower power = softer,
    // temporally stabler occlusion edges (the L1 depth estimate wobbles with the per-frame ray jitter);
    // lower floor / higher power = sharper leak blocking.
    float m_visVarianceFloor = 0.3f;   // min std-dev as a fraction of the cascade's probe spacing
    float m_visChebPower = 2.0f;       // exponent on the Chebyshev weight (DDGI uses 3)
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
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_traceSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_debugSets;

    bool m_cleared = false;
    bool m_debugDepthReadOnly = true; // scene pass depth is read-only under depth-prepass reuse
};
