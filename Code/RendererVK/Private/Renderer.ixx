export module RendererVK:Renderer;

import Core;
import Core.glm;
import Core.Rect;
import Core.Sphere;
import Core.Transform;
import Core.Camera;
import Core.VrSession;
import Threading;

import :Layout;
import :Instance;
import :Device;
import :GpuProfiler;
import :OpenXRSession;
import :Allocator;
import :Surface;
import :SwapChain;
import :RenderPass;
import :Framebuffers;
import :CommandBuffer;
import :Buffer;
import :MeshDataManager;
import :DescriptorSet;
import :IndirectCullComputePipeline;
import :SkinningComputePipeline;
import :StaticMeshGraphicsPipeline;
import :LightGridComputePipeline;
import :ShadowMap;
import :ShadowCullComputePipeline;
import :ShadowMapGraphicsPipeline;
import :AccelerationStructure;
import :GIProbePipeline;
import :GBuffer;
import :GBufferPipeline;
import :RTAOPipeline;
import :OceanSimulationPipeline;
import :VolumetricFogPipeline;
import :BakedWorldMap;
import :SceneColor;
import :DebugLinePipeline;
import :ParticlePipeline;
import :DecalPipeline;
import :ForceFieldPipeline;
import :TaaPipeline;
import :CompositePipeline;
import :EyeAdaptationPipeline;
import :GraphicsPipeline;
import :Light;
import :GpuCrashTracker;
import :Settings;
import :RenderNode;

import Core.fwd;

static_assert(oc::size(ForceFieldParams{}.teamColors) == RendererVKLayout::MAX_FORCE_TEAMS,
    "ForceFieldParams::teamColors must cover MAX_FORCE_TEAMS (Settings.ixx doesn't import :Layout)");

export enum class EValidation { ENABLED, DISABLED };
export enum class EVr { ENABLED, DISABLED };

// Recycles contiguous slot ranges freed by destroyed ObjectContainers (mesh infos, materials, instance
// offsets, skinning jobs, ...). Ranges stay sorted and coalesced; allocation is best-fit so small
// requests don't shred the large holes. All quantities are element counts, not bytes.
export struct IndexRangeFreeList
{
    struct Range { uint32 base; uint32 count; };

    uint32 allocate(uint32 count)
    {
        int32 best = -1;
        for (int32 i = 0; i < (int32)m_ranges.size(); ++i)
            if (m_ranges[i].count >= count && (best < 0 || m_ranges[i].count < m_ranges[best].count))
                best = i;
        if (best < 0)
            return UINT32_MAX;
        const uint32 base = m_ranges[best].base;
        m_ranges[best].base += count;
        m_ranges[best].count -= count;
        if (m_ranges[best].count == 0)
            m_ranges.erase(m_ranges.begin() + best);
        return base;
    }

    void release(uint32 base, uint32 count)
    {
        if (count == 0)
            return;
        auto it = oc::lower_bound(m_ranges.begin(), m_ranges.end(), base,
            [](const Range& range, uint32 b) { return range.base < b; });
        it = m_ranges.insert(it, Range{ base, count });
        if (auto next = it + 1; next != m_ranges.end() && it->base + it->count == next->base)
        {
            it->count += next->count;
            m_ranges.erase(next);
        }
        if (it != m_ranges.begin() && (it - 1)->base + (it - 1)->count == it->base)
        {
            (it - 1)->count += it->count;
            m_ranges.erase(it);
        }
    }

private:
    oc::vector<Range> m_ranges; // sorted by base, no two adjacent
};

// One mesh LOD chain: global mesh indices per level ([0] = full resolution) sharing one set of local
// bounds. Registered by ObjectContainer at load, referenced by RenderNode::m_lodInstances; selection
// happens per instance on the GPU (mesh_lod.inc.glsl in the cull shaders), fed by the GpuMeshLodGroup
// mirror this registers.
export struct MeshLodGroup
{
    uint16 meshIdx[RendererVKLayout::MAX_MESH_LODS] = {};
    uint8 numLods = 0;
    glm::vec3 center = glm::vec3(0.0f); // LOD0 local bounds
    float radius = 0.0f;
    // Geometric deviation of each level from LOD0 in mesh-local units (meshopt simplify error x mesh
    // extents), 0 for level 0. Drives screen-space-error selection: a level is usable when its error
    // projects below "LOD/Max error (px)". All-zero (authored chains without error data) falls back to
    // the projected-size metric.
    float errors[RendererVKLayout::MAX_MESH_LODS] = {};
    uint32 lastUseFrame = UINT32_MAX; // frame stamp for the once-per-frame chain-warmth noteUse
};

export class Renderer final
{
public:

    Renderer() {}
    ~Renderer();

    bool initialize(Window& window, EValidation validation, EVr vr = EVr::DISABLED);
    // False in headless server mode (the global exists — static init_seg — but initialize never ran).
    // Anything that may run headless and would touch GPU/mapped state must gate on this.
    bool isInitialized() const { return m_initialized; }

    // Blocks until this frame slot's previous GPU submission has retired (the slot's fence) — the
    // vsync/GPU throttle of the whole loop. Call it FIRST THING in the frame, before input is
    // sampled: nothing else may write the slot's host-visible per-frame buffers before it, and
    // waiting at the top keeps input -> sim -> present tight instead of letting the sampled input
    // go stale during the stall. beginFrame asserts it ran; present() re-arms it.
    // Returns true once the slot's fence is signaled (or was already waited this frame), false on
    // timeout. The default is the blocking wait; Time::beginFrame passes a bounded timeout to wake
    // a lead before the predicted frame start and kick the window thread's event pump, then calls
    // again blocking. A device error recreates the swapchain and reports signaled.
    bool waitFrameSlot(uint64 timeoutNs = UINT64_MAX);
    // viewportRect is the editor's viewport sub-rect within the swapchain (ignored in VR, which renders
    // full-extent); a change to it re-records the command buffers.
    const Frustum& beginFrame(const Camera& camera, const Rect& viewportRect); // [Concurrency: SERIAL-OWNER of the render chain]
    // passMask (RendererVKLayout::PASS_* bits) selects which culled passes may draw/trace the node
    // this frame: main view, sun shadows, ray tracing (GI/RTAO/RT shadows).
    // [Concurrency: LOCK-FREE - callable from any job between beginFrame and present; on an
    // overflow frame the tail that no longer fits is dropped for one frame and capacity regrows
    // at the next beginFrame]
    void renderNode(const RenderNode& node, uint32 passMask = RendererVKLayout::PASS_ALL);
    void addLightInfo(const RendererVKLayout::LightInfo& light);   // [Concurrency: LOCK-FREE]
    void addFogVolume(const RendererVKLayout::FogVolumeInfo& fogVolume); // [Concurrency: LOCK-FREE]
    void addPointLight(const PointLight& light);                   // [Concurrency: LOCK-FREE]
    void addAreaLight(const AreaLight& areaLight);                 // [Concurrency: LOCK-FREE]
    void addSpotLight(const SpotLight& spotLight);                 // [Concurrency: LOCK-FREE]
    // World-space debug overlay line for this frame (physics collider wireframes etc.). color is
    // packed RGBA8 with R in the low byte. Callable any time between two present() calls.
    // [Concurrency: LOCK-FREE - per-worker staging, merged in present]
    void addDebugLine(const glm::vec3& a, const glm::vec3& b, uint32 color)
    {
        oc::vector<DebugLinePipeline::LineVertex>& verts = m_debugLineVerts.local();
        verts.push_back({ a, color });
        verts.push_back({ b, color });
    }
    void setSunLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    void present();

    // ---- GPU particles + projected decals (driven by the Particle library) ----
    // Emitter slot management is main-thread (the Particle system's serial update); addDecal is
    // lock-free like addLightInfo. An emitter slot's GPU config is re-uploaded every frame, so
    // updateParticleEmitter edits retroactively drive already-spawned particles.
    // Returns UINT32_MAX when all MAX_PARTICLE_EMITTERS slots are taken.
    uint32 createParticleEmitter(const RendererVKLayout::ParticleEmitterGpu& desc);
    void updateParticleEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc);
    // Queues count spawns from the slot this frame (clamped to MAX_PARTICLE_SPAWNS_PER_FRAME total).
    void emitParticles(uint32 slot, uint32 count);
    // Flags the slot's live particles for retirement; the slot recycles once the kill has drained.
    void destroyParticleEmitter(uint32 slot);
    void resetParticles() { m_particleResetPending = true; }
    void addDecal(const RendererVKLayout::DecalInfo& decal); // [Concurrency: LOCK-FREE]
    // Loads a standalone texture (path relative to Assets/) into the bindless array for particle
    // emitters / decals to reference (ParticleEmitterGpu::texFlags.x, DecalInfo::params.x).
    uint16 loadEffectTexture(const char* filePath, bool sRGB = true);
    // A material whose diffuse is a 1x1 solid-color texture — the per-entity TINT path: pass the
    // returned index to RenderNode::setMaterialOverride. Cached per quantized color (repeat calls
    // with the same color are free); main thread.
    uint16 createSolidColorMaterial(const glm::vec3& color);

    // ---- Forcefield bubbles (driven by the Force library) ----
    // Emitter slot management is main-thread (the Force system's serial update is the only writer).
    // Every live slot's GPU config is re-uploaded (compacted) each frame, so updateForceEmitter is
    // the per-frame transform/param push. Returns UINT32_MAX when all MAX_FORCE_EMITTERS are taken.
    uint32 createForceEmitter(const RendererVKLayout::ForceEmitterGpu& desc);
    void updateForceEmitter(uint32 slot, const RendererVKLayout::ForceEmitterGpu& desc);
    // Deactivates the slot; it recycles after the in-flight frames have drained (the per-emitter
    // force readback is slot-indexed, so a slot must not be re-issued while stale results can land).
    void destroyForceEmitter(uint32 slot);
    // Persistent gameplay point-query slots (stable indices across the ~2-frame readback latency —
    // deliberately NOT a lock-free per-frame push). Same main-thread + retirement contract as the
    // emitter slots. Returns UINT32_MAX when all MAX_FORCE_QUERIES slots are taken.
    uint32 createForceQuerySlot();
    void setForceQuery(uint32 slot, const glm::vec3& pos);
    void destroyForceQuerySlot(uint32 slot);
    // GPU readbacks, slot-indexed, ~2 frames old; valid to read between beginFrame and present.
    // Force: xyz = applied force (opposing-field pressure integral), w = mean opposing pressure.
    glm::vec4 getForceEmitterReadback(uint32 slot) const;
    RendererVKLayout::ForceQueryResult getForceQueryReadback(uint32 slot) const;
    // Field/shading params: the Force library owns the tweaks and pushes the struct every frame
    // (setOceanParams pattern). Everything lands in the per-frame UBO (live) except useGrid, which
    // rebuilds the force pipelines (FORCE_GRID define) like the ocean hit-lighting toggle.
    void setForceFieldParams(const ForceFieldParams& params);

    void setAmbientLight(const glm::vec3& color, float intensity) { m_skyParams.ambientColor = color; m_skyParams.ambientIntensity = intensity; }
    void setSkyRadiance(const glm::vec3& color, float intensity) { m_skyParams.skyRadianceColor = color; m_skyParams.skyRadianceIntensity = intensity; }
    void setSkyParams(const SkyParams& sky) { m_skyParams = sky; }
    const SkyParams& getSkyParams() const { return m_skyParams; }
    void setFogParams(const FogParams& fog) { m_fogParams = fog; }
    // Live terrain state for the shaders: meshRadius = radius (m, radial from the camera XZ) inside which
    // streamed terrain chunks are guaranteed resident — the fence for the ocean's land cull (0 = no
    // terrain mesh up, cull disabled); seaLevel feeds the terrain shader's coloring. Set per frame.
    // lapseRate = the generator's temperature change per WORLD metre above sea level (<= 0). The terrain
    // data map bakes the SEA-LEVEL temperature baseline; this is the other half, and the shaders multiply
    // it by the height THEY shade to get a temperature (terrainTemperatureAt). One value for the world, so
    // no consumer can disagree with another about it; 0 = temperature does not vary with height.
    void setTerrainParams(float meshRadius, float seaLevel, float lapseRate = 0.0f)
    {
        m_terrainParams = glm::vec4(meshRadius, glm::min(lapseRate, 0.0f), seaLevel, 0.0f);
    }
    // The streamed-mesh coverage radius above (0 = no terrain mesh up). CPU-side ocean culling must
    // fence on the SAME value its vertex shaders do, or it deletes water the VS would have drawn.
    float getTerrainMeshRadius() const { return m_terrainParams.x; }
    // One terrain splat material: BC-compressed .dds paths (relative to Assets/). Which climate it covers
    // is NOT here — see setTerrainSplatClimate; textures are heavy and change ~never, the climate boxes
    // are two dozen floats and track live tweaks.
    struct TerrainSplatMaterial
    {
        oc::string diffuseDds;
        oc::string normalDds;
        oc::string armDds; // packed AO (R) / roughness (G) / metalness (B); sampled linear
    };
    // Layout of a registered set, in the order the shader composites it bottom-up. The beach and snow
    // entries are OVERLAYS, not materials the climate blend can pick: beach paints over the waterline
    // whatever the climate, and snow paints over everything else (ground AND rock) where it is cold
    // enough and the slope is shallow enough to hold it.
    struct TerrainSplatCounts
    {
        uint32 numGround = 0;
        uint32 numRock = 0;
        bool hasBeach = false;
        bool hasSnow = false;
    };
    // Registers the terrain texture set: mats must be laid out [numGround][numRock][beach?][snow?] to
    // match. Call once (or again to replace — old materials stay allocated, textures are freed; cheap
    // enough for a config-tweak refresh, not a per-frame path). Textures mip-stream like cooked scene
    // textures.
    void setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts);
    // The CLIMATE BOX each ground/rock entry covers, parallel to the registered mats (trailing beach/snow
    // entries are not climate-selected; their boxes are ignored). Per box, in the generator's normalized
    // space: xy = (t01 min, t01 max), zw = (h01 min, h01 max). Weight is 1 inside the box and
    // Gaussian-decays outside it, so an entry that does not care about an axis leaves it full width (0..1)
    // instead of having to claim a fictional value on it.
    // Cheap — push it per frame. The boxes are authored in real climate units and converted through the
    // generator's live precipitation scale, so a tweak change must reach the shader without dragging a
    // texture re-upload behind it.
    void setTerrainSplatClimate(oc::span<const glm::vec4> boxes);
    // Terrain splat shaping (UBO terrainTexParams0..4); tweak-backed, owned by TerrainStreamer
    // (Terrain/Textures category) and pushed here every frame.
    struct TerrainTexTweaks
    {
        float uvScaleGround = 0.20f;  // 1/m: ~5 m texture repeat on flat ground
        float uvScaleRock = 0.08f;    // 1/m: rock features read larger on cliffs
        float uvScaleSnow = 0.12f;    // 1/m
        float climateBlend = 0.09f;   // Gaussian sigma OUTSIDE a climate box, in (t01,h01) units
        // Slope here is 1 - N.y, so these read as angles: 0.30 = 45 deg, 0.55 = 63 deg. Soil genuinely
        // stops holding around 45, which is also about the steepest the diffusion model's 30 m/px field
        // reaches — a threshold set for a sharper procedural field simply never fires on it.
        float slopeRockStart = 0.30f;
        float slopeRockFull = 0.55f;
        float cragStart = 12.0f;
        float cragFull = 50.0f;
        float beachBand = 2.5f;
        // Snow cover. It is a layer ON TOP of the ground/rock, not a climate entry competing with them:
        // real snow buries soil and bedrock alike and slides off anything steep, which is what makes a
        // cold mountain read as white with bare rock on its faces. It sheds EARLIER than rock appears
        // (0.18 = 35 deg vs 0.30 = 45), so a steepening slope loses its snow first and only then goes to
        // bedrock, rather than flipping between the two at one shared angle.
        // Mean annual temperature below freezing is NOT permanent snow — Siberia averages -10 C and is
        // forest. Permanent cover needs roughly -8 C and colder, so these sit well below 0: measured on the
        // model's own climate, a -2 C snow line covers 15.8% of land and a +2 C one covers 25.4%.
        float snowTempFull = -11.0f;  // C at/below which cover is complete
        float snowTempNone = -3.0f;   // C at/above which there is none
        float snowSlopeStart = 0.18f; // slope where it starts sliding off (~35 deg)
        float snowSlopeFull = 0.45f;  // slope where none remains (~57 deg)
        float snowAridity = 0.10f;    // humidity at/below which cold ground stays bare (polar desert)
        // Crag wander. The crag test measures the surface against the generator's macro altitude, which for
        // V3 is a 7.68 km surface — nearly flat across one mountain — so crag ~= height - constant and the
        // rock boundary traces an elevation contour right across a range. This wanders it. Scaled by the
        // local relief in the shader, so it can only modulate relief that exists and never rocks a plain.
        // TerrainStreamer scales these by V3's world scale, like the crag thresholds.
        float cragWanderAmp = 150.0f;      // metres at the model's true scale; 0 = off
        float cragWanderWavelength = 2000.0f; // metres at the model's true scale
    };
    void setTerrainTextureParams(const TerrainTexTweaks& params) { m_terrainTexTweaks = params; }
    // Deepest current ocean wave trough below the calm water level (m, >= 0; the OceanGenerator estimates
    // it from its displacement readback). Sizes the waterline band inside which the fog scatter samples
    // the live FFT wave height for the underwater fog boundary (fogParams7.y).
    void setOceanWaveTrough(float meters) { m_oceanWaveTrough = glm::max(meters, 0.0f); }
    // Flipping OceanParams::hitLighting rebuilds the ocean fragment variant (GPU idle + shader reload).
    void setOceanParams(const OceanParams& ocean);
    // Replaces the fog terrain map (FOG_TERRAIN_CASCADES layers of FOG_TERRAIN_RES^2 RGBA texel quads,
    // interleaved floats: R = raw world-space terrain height, G = water surface level, B = regional fog
    // thickness [0,1], A = spare; near cascade first) centered on centerXZ; cascade i covers
    // cascadeWorldSizes[i] meters (far cascade = same texel count over a larger range, so long-range
    // data costs no extra memory). The height fog base rises by Fog/Terrain Follow x the local terrain
    // height (clamped up to seaLevel, so fog rests on the water), Fog/Region strength modulates density
    // by the fog channel, and the ocean reads the same cascades for its water depth/level (shoaling,
    // surf, swash, land cull). Staged ping-pong (BakedWorldMap): active next frame together with its
    // UBO center/ranges; no GPU sync, no command-buffer re-record.
    void setFogTerrainHeightMap(oc::span<const float> heightTexels, const glm::vec2& centerXZ, const glm::vec2& cascadeWorldSizes, float seaLevel) { m_fogTerrainMap.upload(heightTexels, centerXZ, cascadeWorldSizes, seaLevel, m_swapChain.getCurrentFrameIndex()); }
    void clearFogTerrainHeightMap() { m_fogTerrainMap.clear(); } // fog reverts to the flat height base
    // This frame slot's ocean displacement readback: RGBA16F texels (Dx, h, Dz, dDxz), outRes^2 per
    // cascade, cascades packed consecutively. Safe to read between beginFrame (fence waited) and
    // present (slot resubmits) — copy it out inside that window (Procedural::OceanGenerator does, for
    // the buoyancy water-height queries); contents are ~2 frames old and only refresh while the ocean
    // simulates.
    oc::span<const uint16> getOceanDisplacementReadback(uint32& outRes) const
    {
        outRes = OceanSimulationPipeline::READBACK_RES;
        return m_oceanSimPipeline.getDisplacementReadback(m_swapChain.getCurrentFrameIndex());
    }
    void setPostParams(const PostParams& post) { m_postParams = post; setHaveToRecordCommandBuffers(); }
    // The UI's snapshotted ImGui draw data (an ImDrawData*, opaque here); present() records the
    // ImGui pass from it. Null until the first UI::render - the pass is skipped.
    void setImGuiDrawData(const void* drawData) { m_imguiDrawData = drawData; }

    uint32 getNumMeshInstances() const { return m_meshInstanceCounter; }
    uint32 getNumMeshTypes() const { return m_meshInfoCounter; }
    uint32 getNumMaterials() const { return m_materialInfoCounter; }
    uint32 getCurrentFrameIndex() const { return m_swapChain.getCurrentFrameIndex(); }

    void reloadShaders();

    // Sun cascade matrices computed by beginFrame, for CPU-side shadow-caster queries.
    // Zero cascades when RT sun shadows replace the cascade maps.
    uint32 getNumSunCascades() const { return m_numSunCascades; }
    const glm::mat4* getSunCascadeViewProj() const { return m_sunCascadeViewProj; }

    // The culling view-projection beginFrame built (VR: head-centred two-eye union), for CPU-side
    // occlusion rasterization against the same view the GPU cull uses.
    const glm::mat4& getCenterViewProj() const { return m_centerViewProj; }

    bool isVrEnabled() const { return Globals::openXR.isEnabled(); }
    bool isVSyncEnabled() const { return m_vsyncEnabled; } // FIFO present: frames land on whole refresh periods (Time's stable-dt snap relies on it)
    bool isVrStageSpace() const { return Globals::openXR.isStageSpace(); }
    IVrSession* getVrSession() { return Globals::openXR.isEnabled() ? &Globals::openXR : nullptr; }

    void toggleGiProbeDebug() { m_giProbeDebugEnabled = !m_giProbeDebugEnabled; }
    void cycleGiProbeDebugMode() { m_giProbeDebugMode ^= 1u; setHaveToRecordCommandBuffers(); } // 0 = irradiance, 1 = cellSize/LOD

    void setWindowMinimized(bool minimized);
    void recreateWindowSurface(Window& window);

    Stats getStats();

    // Read by Entity's World to build scene-cache cook options (LOD generation params are baked into
    // cooked files, so they participate in the cache's options hash).
    const MeshLodParams& getLodParams() const { return m_lodParams; }

    // Mesh streaming (MeshStreamer): rewrite one MeshInfo in the CPU backing store + GPU buffer.
    // Streamed-out meshes keep their bounds but draw zero indices, so the cull's DGC draws, the shadow
    // pass, and the TLAS-instance writer all become no-ops for them without any re-record.
    void setMeshStreamedOut(uint16 meshInfoIdx);
    void setMeshStreamedIn(uint16 meshInfoIdx, int32 vertexOffset, uint32 firstIndex, uint32 indexCount);

private:

    Renderer(const Renderer&) = delete;
    Renderer(const Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(const Renderer&&) = delete;

    // Applied by beginFrame from its viewportRect argument, which is the only way to set it. VR renders
    // full-extent (no editor panel sub-rect), so the rect is ignored there.
    // An EMPTY rect is ignored (keeps the previous one): the UI widget pass runs a frame behind, so
    // frame 0 arrives with the default Rect() — the swapchain-sized rect from initialize() stands
    // in, instead of a 0/0 aspect reaching glm::perspective (asserted in Debug).
    void setViewportRect(const Rect& rect) { if (Globals::openXR.isEnabled()) return; if (rect.getSize().x <= 0 || rect.getSize().y <= 0) return; if (rect != m_viewportRect) { m_viewportRect = rect; setHaveToRecordCommandBuffers(); } }

    CommandBuffer& getCurrentCommandBuffer() { return m_perFrameData[m_swapChain.getCurrentFrameIndex()].primaryCommandBuffer; }

    void recordCommandBuffers();
    // Rewrites swapped streamed-texture slots in this frame slot's bindless texture arrays (all
    // consuming pipelines). Called from recordCommandBuffers, where the slot's fence has been waited.
    void applyPendingTextureDescriptorWrites(uint32 frameIdx);
    // Texture-streaming priority pass: reports the node's projected screen size to the TextureStreamer
    // for every material texture its instances sample (called from renderNode/renderNodeThreadSafe).
    void noteTextureUse(const RenderNode& node, uint32 passMask);
    struct PerFrameData;
    // Per-frame CPU side of GPU LOD selection: stamps the node's chains as used (keeps every level's
    // mesh set warm in the mesh streamer) and publishes the node's state-slot bias for the cull shader.
    void noteLodChainUse(const RenderNode& node, uint32 startIdx, PerFrameData& frameData);
    void recordSkinning(uint32 frameIdx);
    void recordOceanSim(uint32 frameIdx);
    void recordIndirectCull(uint32 frameIdx);
    void recordLightGrid(uint32 frameIdx);
    void recordShadowCull(uint32 frameIdx);
    void recordShadowDraw(uint32 frameIdx);
    void recordStaticMesh(uint32 frameIdx);
    void recordStaticMeshInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordGBuffer(uint32 frameIdx);
    void recordGBufferInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    // Depth-prepass reuse: transitions this eye's G-buffer depth SHADER_READ_ONLY <-> DEPTH_READ_ONLY
    // around the scene pass (the reuse render pass performs no transitions itself — its dependency array
    // must stay identical to the main pass's for render-pass compatibility).
    void recordReuseDepthBarrier(vk::CommandBuffer cb, vk::Image gbufferDepth, uint32 eyeIndex, bool toAttachment);
    void recordAOInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordFogApplyInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordTaaInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordGiProbeDebug(uint32 frameIdx);
    void recordDebugLines(uint32 frameIdx);
    void recordParticleSim(uint32 frameIdx);
    void recordParticles(uint32 frameIdx);
    void recordParticlesInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordDecals(uint32 frameIdx);
    void recordDecalsInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordForceField(uint32 frameIdx);
    void recordForceFieldInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordForceCompute(uint32 frameIdx);
    void checkForceGridCapacity();
    void recordAO(uint32 frameIdx);
    void recordVolumetricFog(uint32 frameIdx);
    void recordFogApply(uint32 frameIdx);
    void recordTaa(uint32 frameIdx);
    void recordEyeAdaptation(uint32 frameIdx);
    void recordComposite(uint32 frameIdx);
    // Re-allocates the variable-count texture-array descriptors when the live texture count outgrew them
    // (TextureManager::getGeneration bumped). Runs at beginFrame AND again in present() before recording,
    // because containers loaded after beginFrame (terrain streaming, mid-frame spawns) upload textures
    // that this frame's record would otherwise write past the descriptor capacity.
    void syncTextureDescriptorCapacity();
    void createEyeCompositeTargets();
    void destroyEyeCompositeTargets();
    bool recordGlobalIllum(uint32 frameIdx); // returns true if the ray-tracing TLAS handle changed this frame
    void setHaveToRecordCommandBuffers();
    void recreateSwapchain();
    void createLightGridBuffers();
    void initImgui(Window& window);

    friend class ObjectContainer;
    void addObjectContainer(ObjectContainer* pObjectContainer);
    // Container teardown (~ObjectContainer): returns every renderer resource the container (and its
    // parked skinned bundles) allocated to the free lists. All RenderNodes spawned from the container
    // must have been destroyed first. CPU-side slots recycle immediately; vk objects that in-flight
    // frames may still reference (texture images, static BLASes) are retired and destroyed after the
    // GPU drain queued for them (m_pendingTextureFrees / the AS retire queue) - freed mega-buffer/slot
    // ranges are safe to recycle immediately since any future upload into them drains the GPU itself
    // (see uploadToSharedBuffer).
    void removeObjectContainer(ObjectContainer* pObjectContainer);
    // Neutralizes MeshInfo slots (zero indexCount = draws/TLAS writes no-op, like streamed-out meshes),
    // retires their BLASes/aliases and returns the range to the free list.
    void freeMeshInfoRange(uint32 baseMeshInfoIdx, uint32 count);
    // Full teardown of a PARKED bundle (container destruction): frees the per-instance MeshInfo range,
    // output vertex regions, palette region, skinning-job slots and LOD groups its spawn allocated.
    void destroySkinnedBundle(uint32 bundleHandle);
    // Destroys textures queued by removeObjectContainer. Caller guarantees the GPU is idle; the freed
    // slots' bindless entries rewrite to the fallback when each frame slot next records.
    void processPendingTextureFrees();
    // Contents-only upload into a buffer that's shared (not per-frame-in-flight) and read full-range every
    // frame by GI/RTAO compute and the draw passes (m_meshInfosBuffer, m_materialInfosBuffer,
    // m_instanceOffsetsBuffer). See StagingManager::ensureDrainedForSharedWrite for why a per-frame-gated
    // drain here - rather than deferring to a later explicit flush - is what's actually required.
    void uploadToSharedBuffer(Buffer& buffer, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset);

    uint32 addRenderNodeTransform(const Transform& transform);
    // vertexCounts: exact per-MeshInfo vertex count (parallel to meshInfos) — the BLAS builder needs a
    // tight maxVertex per mesh and offsets are no longer monotonic with the mesh streamer's free-list.
    // skinnedOutputs: MeshInfos pointing at per-instance skinned output regions — excluded from the
    // one-time static BLAS builds (and thus compaction): their regions are uninitialized until the
    // skinning compute runs, and their addresses are owned per frame slot by the skinned BLAS rebuild.
    uint32 addMeshInfos(const oc::vector<RendererVKLayout::MeshInfo>& meshInfos, oc::span<const uint32> vertexCounts, bool skinnedOutputs = false);
    uint16 getRtMeshAlias(uint16 meshIdx) const { return (uint16)m_accelStructure.getMeshAlias(meshIdx); }
    uint32 addMaterialInfos(const oc::vector<RendererVKLayout::MaterialInfo>& materialInfos);
    uint32 addMeshInstanceOffsets(const oc::vector<RendererVKLayout::MeshInstanceOffset>& meshInstanceOffsets);
    uint32 addMeshLodGroup(const MeshLodGroup& group)
    {
        uint32 groupIdx;
        if (!m_freeMeshLodGroups.empty())
        {
            groupIdx = m_freeMeshLodGroups.back();
            m_freeMeshLodGroups.pop_back();
            m_meshLodGroups[groupIdx] = group;
        }
        else
        {
            m_meshLodGroups.push_back(group);
            groupIdx = (uint32)m_meshLodGroups.size() - 1;
            if ((uint32)m_meshLodGroups.size() > m_maxMeshLodGroups)
                growMeshLodGroupCapacity((uint32)m_meshLodGroups.size());
        }
        // One shared BLAS per chain: every level aliases the RT level's geometry (rays don't need
        // per-level fidelity), so only that level's BLAS is ever built.
        const uint8 rtLevel = (uint8)oc::clamp(m_rtParams.blasLodLevel, 0, (int)group.numLods - 1);
        for (uint8 k = 0; k < group.numLods; ++k)
            m_accelStructure.setMeshAlias(group.meshIdx[k], group.meshIdx[rtLevel]);
        // GPU LOD selection: publish the group and point every member mesh at it.
        uploadMeshLodGroup(groupIdx);
        for (uint8 k = 0; k < group.numLods; ++k)
            setMeshLodGroupIdx(group.meshIdx[k], groupIdx);
        return groupIdx;
    }
    // Frees a LOD group: detaches its member meshes from GPU selection, then recycles the slot.
    void freeMeshLodGroup(uint32 groupIdx)
    {
        const MeshLodGroup& group = m_meshLodGroups[groupIdx];
        for (uint8 k = 0; k < group.numLods; ++k)
            setMeshLodGroupIdx(group.meshIdx[k], UINT32_MAX);
        m_freeMeshLodGroups.push_back(groupIdx);
    }
    void uploadMeshLodGroup(uint32 groupIdx);
    void setMeshLodGroupIdx(uint16 meshIdx, uint32 groupIdx);
    // Per-instance LOD hysteresis state slots (GPU), one contiguous range per RenderNode with LOD chains.
    uint32 allocateLodStateRange(uint32 count);
    void growLodStateCapacity(uint32 needed);
    void growMeshLodGroupCapacity(uint32 needed);
    uint32 addSkinnedMeshSources(const oc::vector<RendererVKLayout::SkinnedMeshSource>& sources);
    const RendererVKLayout::SkinnedMeshSource& getSkinnedMeshSource(uint32 idx) const { return m_skinnedMeshSources[idx]; }

    // Everything one spawnSkinnedNode allocated (per-instance MeshInfo range, output vertex regions,
    // palette region, contiguous SkinningJob/SkinnedBlasBuild range), recycled as a unit: destroying the
    // node parks its bundle (jobs/BLAS deactivated IN PLACE — the per-frame skinned BLAS slots are
    // positional and sized once, so entries never move) on a per-container free list, and the next
    // spawnSkinnedNode of the same container reuses it wholesale with zero GPU uploads or re-records.
    struct SkinnedInstanceBundle
    {
        uint32 sourceKey;    // owning container's base SkinnedMeshSource index (free-list key)
        uint32 baseMeshIdx;  // first MeshInfo of the per-instance range (level 0s; LOD levels follow)
        uint32 paletteHandle;
        uint32 firstJob;     // first entry of the contiguous SkinningJob/SkinnedBlasBuild range
        uint32 numMeshes;
        oc::vector<uint32> lodGroupForMesh; // per mesh: MeshLodGroup idx (UINT32_MAX = no chain)
        oc::vector<uint32> blasIndexCounts; // per mesh: the skinned BLAS's index count (its RT level's,
                                             // restored on unpark — src.indexCount would be level 0's)
    };
    uint32 acquireSkinnedBundle(uint32 sourceKey); // reactivates + returns a parked bundle, UINT32_MAX if none free
    uint32 registerSkinnedBundle(const SkinnedInstanceBundle& bundle);
    void releaseSkinnedBundle(uint32 bundleHandle);
    const SkinnedInstanceBundle& getSkinnedBundle(uint32 handle) const { return m_skinnedBundles[handle]; }

    friend class RenderNode;
    void freeRenderNode(RenderNode& node);
    inline Transform& getRenderNodeTransform(uint32 idx) { return m_renderNodeTransforms[idx]; }

    friend class AnimatorComponent;
    uint32 allocateSkinningPalette(uint32 boneCount);
    void setSkinningPalette(uint32 paletteHandle, oc::span<const glm::mat4> palette);
    // A bundle's SkinningJob/SkinnedBlasBuild entries must be one contiguous range (the per-frame
    // skinned BLAS slots are positional), so the range is allocated as a block — from the free list
    // (destroyed containers) when one fits, appended otherwise — and filled per mesh afterwards.
    uint32 allocateSkinningJobRange(uint32 count);
    void setSkinnedInstance(uint32 jobIdx, uint32 baseVertexOffset, uint32 skinVertexOffset, uint32 outVertexOffset, uint32 vertexCount, uint32 paletteHandle, uint32 meshIdx, uint32 firstIndex, uint32 indexCount);

    void waitForGpuAndFlushStaging();
    void growRenderNodeCapacity(uint32 needed);
    void growMeshInstanceCapacity(uint32 needed);
    void growUniqueMeshCapacity(uint32 needed);
    void growMaterialCapacity(uint32 needed);
    void growInstanceOffsetCapacity(uint32 needed);
    void growLightGridBuffers(size_t neededGridBytes, uint32 neededTableEntries);
    void checkLightGridCapacity();

private:

    Instance m_instance;
    Device m_device;
    Surface m_surface;
    SwapChain m_swapChain;
    GpuProfiler m_gpuProfiler;
    RenderPass m_renderPass;
    Framebuffers m_framebuffers;
	GpuCrashTracker m_gpuCrashTracker;

    IndirectCullComputePipeline m_indirectCullComputePipeline;
    SkinningComputePipeline m_skinningComputePipeline;
    LightGridComputePipeline m_lightGridComputePipeline;
    StaticMeshGraphicsPipeline m_staticMeshGraphicsPipeline;
    GBufferPipeline m_gbufferPipeline;
    RTAOPipeline m_rtaoPipeline;
    OceanSimulationPipeline m_oceanSimPipeline;
    VolumetricFogPipeline m_volumetricFogPipeline;
    // CPU-baked terrain height snapshots around the camera (raw surface height, world meters). The shore
    // map drives the ocean's shoaling/surf/waterline; the fog terrain map (2 cascades) drives the fog's
    // terrain-following height base AND is the ocean's coarse fallback outside the shore map's range.
    BakedWorldMap m_fogTerrainMap;
    // Last fog-terrain-map flip generation written into each frame slot's descriptor sets (0 = never).
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_fogTerrainDescGen{};
    TaaPipeline m_taaPipeline;
    ShadowCullComputePipeline m_shadowCullComputePipeline;
    ShadowMapGraphicsPipeline m_shadowMapGraphicsPipeline;
    CompositePipeline m_compositePipeline;
    EyeAdaptationPipeline m_eyeAdaptationPipeline;
    AccelerationStructure m_accelStructure;
    GIProbePipeline m_giProbePipeline;
    DebugLinePipeline m_debugLinePipeline;
    ParticlePipeline m_particlePipeline;
    DecalPipeline m_decalPipeline;
    ForceFieldPipeline m_forceFieldPipeline;
    // CPU emitter slot table, re-uploaded per frame; retired slots keep their KILL flag alive until the
    // sim has drained their particles, then recycle.
    oc::vector<RendererVKLayout::ParticleEmitterGpu> m_particleEmitters;
    oc::vector<uint32> m_freeParticleEmitterSlots;
    oc::vector<oc::pair<uint32, uint32>> m_retiredParticleEmitters; // slot, retire frame
    oc::vector<oc::pair<uint16, uint16>> m_particleSpawnRequests;   // emitter slot, count
    bool m_particlesEnabled = true;
    bool m_particleCollision = true;
    float m_particleTimeScale = 1.0f;
    bool m_particleLogStats = false; // "Particles/Log stats": prints GPU alive/dead counts ~once a second
    // Pool init/reset request. Cleared only AFTER a frame that carried reset=1 AND executed the sim was
    // actually submitted: a reset consumed by a frame that never runs (acquire failure -> early return,
    // no mesh instances so the sim CB is skipped) would leave the dead stack empty forever, silently
    // dropping every spawn. True at startup: the first simulating frame initializes the pool.
    bool m_particleResetPending = true;
    bool m_decalsEnabled = true;
    // CPU force-emitter slot table (Force library), compact-uploaded per frame; destroyed slots stay
    // reserved (FORCE_FLAG_ACTIVE cleared) until the in-flight frames drain, then recycle — the
    // per-emitter force readback is slot-indexed and must never pair a new emitter with stale results.
    oc::vector<RendererVKLayout::ForceEmitterGpu> m_forceEmitters;
    oc::vector<uint32> m_freeForceEmitterSlots;
    oc::vector<oc::pair<uint32, uint32>> m_retiredForceEmitters; // slot, retire frame
    oc::vector<RendererVKLayout::ForceQueryGpu> m_forceQueries;   // persistent query slots (same contract)
    oc::vector<uint32> m_freeForceQuerySlots;
    oc::vector<oc::pair<uint32, uint32>> m_retiredForceQuerySlots;
    ForceFieldParams m_forceFieldParams;
    PerWorker<oc::vector<DebugLinePipeline::LineVertex>> m_debugLineVerts; // per-worker CPU staging, merged in present()
    oc::vector<DebugLinePipeline::LineVertex> m_debugLineMergedVerts;

    SkyParams m_skyParams;
    ShadowParams m_shadowParams;
    FogParams m_fogParams;
    OceanParams m_oceanParams;
    glm::vec4 m_terrainParams{ 0.0f }; // see setTerrainParams; x = 0 disables the ocean land cull
    // Terrain splat materials (setTerrainSplatMaterials): contiguous material range + owned textures.
    int32  m_terrainSplatBaseMaterial = -1; // -1 = no set registered (shader uses flat-color fallback)
    TerrainSplatCounts m_terrainSplatCounts;
    oc::vector<uint16> m_terrainSplatTextures; // for the per-frame streaming noteUse + replacement frees
    glm::vec4 m_terrainSplatClimate[RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS]{};
    TerrainTexTweaks m_terrainTexTweaks; // see setTerrainTextureParams
    float m_oceanWaveTrough = 0.0f;  // see setOceanWaveTrough; 0 while the ocean is disabled
    PostParams m_postParams;
    RTParams m_rtParams;
    RTAOParams m_rtaoParams;
    TAAParams m_taaParams;

    glm::vec3 m_cameraPos = glm::vec3(0.0f);
    glm::vec3 m_giPrevCameraPos = glm::vec3(0.0f); // last frame's camera; drives GI clipmap probe freshness
    float m_mipPixelScale = 0.0f; // viewportHeight / tan(fovY/2): projected diameter px = radius * scale / dist
    MeshLodParams m_lodParams;

    glm::mat4 m_sunCascadeViewProj[RendererVKLayout::NUM_SHADOW_CASCADES];
    uint32 m_numSunCascades = 0;
    glm::mat4 m_centerViewProj = glm::mat4(1.0f);

    bool   m_giProbeDebugEnabled = false;
    uint32 m_giProbeDebugMode = 0;
    float  m_giProbeDebugRadius = 0.12f;

    glm::ivec2 m_windowSize;
    const void* m_imguiDrawData = nullptr; // see setImGuiDrawData
    Rect m_viewportRect = Rect();
    bool m_initialized = false;
    bool m_frameSlotWaited = false; // waitFrameSlot() ran for the current slot (cleared by present)
    bool m_windowMinimized = false;
    bool m_vsyncEnabled = true; // "Time/VSync" tweak (Saved; --no-vsync overrides it): FIFO vs Immediate, applied by a swapchain recreate
    bool m_wireframe = false; // "Renderer/Wireframe" tweak: forward scene variants rasterize as lines
    bool m_depthPrepassReuse = true; // "Depth prepass reuse" tweak: forward pass reuses the G-buffer prepass depth for early-Z
    glm::vec2 m_prevTaaJitter{ 0.0f }; // last frame's TAA jitter (ubo.taaJitter.zw): consumers of the PREV depth image compensate with it
    uint32 m_sceneViewCount = 1; // 2 in VR: SceneColor + forward pass are multiview (one layer per eye)

    // VR: per-eye LDR composite targets
    oc::array<vk::Image, 2> m_eyeColorImage{};
    oc::array<VmaAllocation, 2> m_eyeColorMem{};
    oc::array<vk::ImageView, 2> m_eyeColorView{};
    oc::array<vk::Framebuffer, 2> m_eyeFramebuffer{};
    oc::array<DescriptorSet, 2> m_vrCompositeDescriptorSet;
    vk::Image m_eyeDepthImage;
    VmaAllocation m_eyeDepthMem = nullptr;
    vk::ImageView m_eyeDepthView;

    uint32 m_maxRenderNodes = RendererVKLayout::INITIAL_RENDER_NODES;
    uint32 m_maxUniqueMeshes = RendererVKLayout::INITIAL_UNIQUE_MESHES;
    uint32 m_maxUniqueMaterials = RendererVKLayout::INITIAL_UNIQUE_MATERIALS;
    uint32 m_maxInstanceOffsets = RendererVKLayout::INITIAL_INSTANCE_OFFSETS;
    uint32 m_maxInstanceData = RendererVKLayout::INITIAL_INSTANCE_DATA;
    uint32 m_maxTextures = RendererVKLayout::INITIAL_TEXTURES;
    uint32 m_maxGiTlasInstances = RendererVKLayout::GI_INITIAL_TLAS_INSTANCES;

    size_t m_lightGridBufferSize = RendererVKLayout::INITIAL_LIGHT_GRID_BUFFER_SIZE;
    uint32 m_lightTableEntries = RendererVKLayout::INITIAL_LIGHT_TABLE_NUM_ENTRIES;

    uint32 m_numTextureDescriptors = RendererVKLayout::INITIAL_TEXTURES;
    uint32 m_meshDataGeneration = 0; // last seen MeshDataManager::getGeneration(); change -> re-record
    uint32 m_textureGeneration = 0;  // last seen TextureManager::getGeneration(); change -> rebuild texture-array pipelines

    oc::vector<Transform>& m_renderNodeTransforms = Globals::renderNodeTransforms;

    // Skinned mesh state. Palette regions and skinning jobs are persistent (set up at spawn); palette
    // CONTENTS are refreshed each frame via setSkinningPalette and uploaded in present().
    struct SkinningPaletteRegion { uint32 offset; uint32 boneCount; };
    oc::vector<SkinningPaletteRegion> m_skinningPaletteRegions;
    oc::vector<glm::mat4> m_skinningPalettes;   // CPU staging (concatenated per region), uploaded each frame
    oc::vector<RendererVKLayout::SkinningJob> m_skinningJobs; // one per skinned mesh instance; uploaded each frame
    oc::vector<AccelerationStructure::SkinnedBlasBuild> m_skinnedBlasBuilds; // parallel to m_skinningJobs; per-frame BLAS rebuild
    oc::vector<RendererVKLayout::SkinnedMeshSource> m_skinnedMeshSources; // per-container skinned-mesh source data (CPU-only)
    uint32 m_maxSkinningPaletteEntries = RendererVKLayout::INITIAL_SKINNING_PALETTE;
    uint32 m_maxSkinningJobs = RendererVKLayout::INITIAL_SKINNING_JOBS;
    void growSkinningPaletteCapacity(uint32 needed);
    void growSkinningJobCapacity(uint32 needed);

    oc::vector<ObjectContainer*> m_objectContainers;
    oc::vector<uint32> m_numInstancesPerMesh;
    oc::vector<uint32> m_meshVertexCounts;    // exact per-MeshInfo vertex count (BLAS maxVertex)
    oc::vector<uint8> m_meshIsSkinnedOutput;  // per MeshInfo: skinned output region (no static BLAS build)
    oc::vector<uint32> m_pendingBlasRebuilds; // re-streamed meshes awaiting a BLAS rebuild in recordGlobalIllum
    oc::vector<MeshLodGroup> m_meshLodGroups;
    oc::vector<uint32> m_meshToLodGroup; // per MeshInfo: owning MeshLodGroup (UINT32_MAX = no chain); mirrors m_meshLodGroupIdxBuffer
    oc::array<uint32, RendererVKLayout::MAX_MESH_LODS> m_lodInstanceCounts{}; // stats snapshot of the GPU cull's per-level picks
    oc::vector<uint32> m_freeRenderNodeIndexes;
    oc::vector<SkinnedInstanceBundle> m_skinnedBundles;
    oc::unordered_map<uint32, oc::vector<uint32>> m_freeSkinnedBundles; // sourceKey -> parked bundle handles

    // Slot recycling for destroyed ObjectContainers: the shared info/offset buffers and CPU arrays are
    // append-only (holes are never compacted), freed ranges are reused by later containers instead.
    IndexRangeFreeList m_freeMeshInfoSlots;
    IndexRangeFreeList m_freeMaterialSlots;
    IndexRangeFreeList m_freeInstanceOffsetSlots;
    IndexRangeFreeList m_freeSkinnedSourceSlots;
    IndexRangeFreeList m_freeSkinningJobSlots;      // freed slots stay inert (vertexCount/indexCount 0)
    oc::vector<uint32> m_freeMeshLodGroups;
    oc::vector<uint32> m_freeSkinningPaletteHandles; // regions reused on exact boneCount match
    oc::vector<uint32> m_freeSkinnedBundleSlots;
    oc::vector<uint16> m_pendingTextureFrees; // images possibly still sampled in flight; freed in present() after the GPU drain

    uint32 m_frameCounter = 0; // monotonic; rotates the GI probe ray/taa samples set each frame
    uint32 m_meshInfoCounter = 0;
    uint32 m_materialInfoCounter = 0;
    uint32 m_instanceOffsetCounter = 0;
    uint32 m_meshInstanceCounter = 0;
    uint32 m_lightCounter = 0;
    uint32 m_fogVolumeCounter = 0;
    uint32 m_decalCounter = 0;
    uint32 m_blasBuiltCount = 0;
    uint32 m_pendingMaxInstanceData = 0;
    uint32 m_instanceOverflowStart = UINT32_MAX; // smallest failed claim this frame; present clamps the counter to it

    Buffer m_meshInfosBuffer;
    Buffer m_materialInfosBuffer;
    oc::unordered_map<uint32, uint16> m_solidColorMaterials; // packed RGB8 -> material idx (tint cache)
    Buffer m_instanceOffsetsBuffer;

    // GPU LOD selection (shared, device-local): per-mesh group index, packed group data, and the
    // per-instance hysteresis state the main cull reads/writes (advisory only — the shader clamps
    // whatever it reads into the frame's valid band, so cross-frame races and garbage are benign).
    Buffer m_meshLodGroupIdxBuffer;
    Buffer m_meshLodGroupsBuffer;
    Buffer m_lodLevelStateBuffer;
    IndexRangeFreeList m_freeLodStateSlots;
    uint32 m_lodStateCounter = 0;
    uint32 m_maxLodStateSlots = RendererVKLayout::INITIAL_LOD_STATE_SLOTS;
    uint32 m_maxMeshLodGroups = RendererVKLayout::INITIAL_MESH_LOD_GROUPS;

    struct PerFrameData
    {
        SceneColor sceneColor;
        GBuffer gbuffer;
        ShadowMap shadowMap;

        // Per-eye in VR
        oc::array<DescriptorSet, 2> staticMeshPipelineDescriptorSet;
        oc::array<DescriptorSet, 2> gbufferDescriptorSet;
        DescriptorSet compositeDescriptorSet;
        DescriptorSet indirectCullPipelineDescriptorSet;
        DescriptorSet skinningDescriptorSet;
        DescriptorSet lightGridPipelineDescriptorSet;
        DescriptorSet shadowCullDescriptorSet;
        DescriptorSet shadowDrawDescriptorSet;

        CommandBuffer primaryCommandBuffer;
        CommandBuffer staticMeshCommandBuffer;
        CommandBuffer gbufferCommandBuffer;
        CommandBuffer aoCommandBuffer;
        CommandBuffer indirectCullCommandBuffer;
        CommandBuffer skinningCommandBuffer;
        CommandBuffer oceanSimCommandBuffer;
        CommandBuffer lightGridCommandBuffer;
        CommandBuffer imguiCommandBuffer;
        CommandBuffer shadowCullCommandBuffer;
        CommandBuffer shadowDrawCommandBuffer;
        CommandBuffer globalIllumCommandBuffer;
        CommandBuffer volumetricFogCommandBuffer;
        CommandBuffer fogApplyCommandBuffer;
        CommandBuffer giProbeDebugCommandBuffer;
        CommandBuffer debugLineCommandBuffer;
        CommandBuffer particleSimCommandBuffer;
        CommandBuffer particleCommandBuffer;
        CommandBuffer decalCommandBuffer;
        CommandBuffer forceFieldCommandBuffer;
        CommandBuffer forceComputeCommandBuffer;
        CommandBuffer taaCommandBuffer;
        CommandBuffer eyeAdaptCommandBuffer;
        CommandBuffer compositeCommandBuffer;

        bool updated = false;
        Buffer ubo;
        Buffer inRenderNodeTransformsBuffer;
        Buffer inNodePassMasksBuffer; // uint per render node: PASS_* bits, written at push time
        Buffer inNodeLodStateBiasBuffer; // int per render node: LOD state slot bias (stateBase - startIdx), written at push time
        Buffer inMeshInstancesBuffer;
        Buffer inFirstInstancesBuffer;
        Buffer lodStatsBuffer; // per-level LOD pick counts, written by the cull, read back for stats
        oc::span<uint32> mappedLodStats;
        // This frame's unique-mesh count, read on the GPU by the DGC executes (sequenceCountAddress) and
        // the G-buffer's drawIndexedIndirectCount, so registering new meshes never re-records.
        Buffer meshCountBuffer;
        oc::span<uint32> mappedMeshCount;

        Buffer lightInfosBuffer;
        Buffer lightGridsBuffer;
        Buffer lightTableBuffer;

        RendererVKLayout::Ubo* mappedUniformBuffer = nullptr;
        oc::span<RendererVKLayout::RenderNodeTransform> mappedRenderNodeTransforms;
        oc::span<uint32> mappedNodePassMasks;
        oc::span<int32> mappedNodeLodStateBias;
        oc::span<RendererVKLayout::InMeshInstance> mappedMeshInstances;
        oc::span<uint32> mappedFirstInstances;

        oc::span<RendererVKLayout::LightInfo> mappedLightInfos;

        Buffer fogVolumesBuffer;
        oc::span<RendererVKLayout::FogVolumes> mappedFogVolumes; // single element: count header + array
    };
    oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_RENDERER)
    Renderer rendererVK;
} // namespace Globals