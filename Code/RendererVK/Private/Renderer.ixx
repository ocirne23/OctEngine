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
import :RTAOPipeline;
import :OceanSimulationPipeline;
import :TerrainWetnessPipeline;
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
import :SlotTable;
import :MeshLodRegistry;
import :SkinnedMeshRegistry;
import :SharedTable;
import :InstanceStream;
import :FrameSubmission;
import :VrEyeTargets;
import :TerrainResources;
import :ForceFieldState;
import :BindlessTextures;
import :ParticleState;
import :RayTracingScene;

import Core.fwd;

static_assert(oc::size(ForceFieldParams{}.teamColors) == RendererVKLayout::MAX_FORCE_TEAMS,
    "ForceFieldParams::teamColors must cover MAX_FORCE_TEAMS (Settings.ixx doesn't import :Layout)");

export enum class EValidation { ENABLED, DISABLED };
export enum class EVr { ENABLED, DISABLED };


export class Renderer final
{
public:

    Renderer() {}
    ~Renderer();

    bool initialize(Window& window, EValidation validation, EVr vr = EVr::DISABLED);
    // False in headless server mode; anything that may run headless must gate on it.
    bool isInitialized() const { return m_initialized; }

    // Waits this frame slot's fence - the vsync/GPU throttle of the whole loop. Call it FIRST THING,
    // before input: nothing else may write the slot's host-visible buffers, and waiting at the top
    // keeps the sampled input fresh. False = timed out (Time::beginFrame polls, then blocks).
    bool waitFrameSlot(uint64 timeoutNs = UINT64_MAX);
    // viewportRect: the editor's sub-rect of the swapchain (ignored in VR); a change re-records.
    // [Concurrency: SERIAL OWNER of the render chain - ONE call in flight, and its outputs are valid
    // only after the join. Nothing may touch renderer frame state in between.]
    const Frustum& beginFrame(const Camera& camera, const Rect& viewportRect);
    // beginFrame as a job. VR defers instead: the join runs it on main, because xrWaitFrame owns VR
    // pacing and would pin a worker.
    void kickBeginFrameJob(const Camera& camera, const Rect& viewportRect);
    void joinBeginFrameJob();
    // Call after the frame's LAST light add and the force update.
    void kickGridBuilds();
    // The spatial cull's view, computable BEFORE beginFrame. VR serves LAST frame's head view (the
    // pose arrives inside beginFrame), so it lags a frame and is invalid on the first one.
    CullView getCullView(const Camera& camera, const Rect& viewportRect);
    // Desktop only (asserts !VR): bit-identical to the frustum beginFrame will build, so the cull can
    // start while beginFrame runs. Publishes m_centerViewProj, so getCenterViewProj() serves it too.
    Frustum computeCullFrustum(const Camera& camera, const Rect& viewportRect);
    // passMask (PASS_* bits): which culled passes may draw/trace the node this frame.
    // [Concurrency: LOCK-FREE between beginFrame and present. An overflow frame drops the tail that
    // no longer fits; capacity regrows at the next beginFrame.]
    void renderNode(const RenderNode& node, uint32 passMask = RendererVKLayout::PASS_ALL);
    void addLightInfo(const RendererVKLayout::LightInfo& light);   // [Concurrency: LOCK-FREE]
    void addFogVolume(const RendererVKLayout::FogVolumeInfo& fogVolume); // [Concurrency: LOCK-FREE]
    void addPointLight(const PointLight& light);                   // [Concurrency: LOCK-FREE]
    void addAreaLight(const AreaLight& areaLight);                 // [Concurrency: LOCK-FREE]
    void addSpotLight(const SpotLight& spotLight);                 // [Concurrency: LOCK-FREE]
    // One frame's world-space overlay line; color is packed RGBA8, R in the low byte.
    // [Concurrency: LOCK-FREE - per-worker staging, merged in present]
    void addDebugLine(const glm::vec3& a, const glm::vec3& b, uint32 color)
    {
        oc::vector<DebugLinePipeline::LineVertex>& verts = m_debugLineVerts.local();
        const ThreadLocalScope tlsPin;
        verts.push_back({ a, color });
        verts.push_back({ b, color });
    }
    void setSunLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    // The world point every distance-based falloff measures from (u_sceneFocus). Set: the sun cascades
    // become nested SPHERES around it and the RTAO fades are metres from it. Cleared = the camera.
    void setSceneFocus(const glm::vec3& worldPos) { m_sceneFocus = worldPos; m_sceneFocusEnabled = true; }
    void clearSceneFocus() { m_sceneFocusEnabled = false; }
    // The GI clipmap window and the TLAS range bound centre on this too. Valid after beginFrame.
    glm::vec3 sceneFocusOrCamera() const { return m_sceneFocusEnabled ? m_sceneFocus : m_cameraPos; }
    // The live "Shadows" tweaks: a pushed preset shows up in the panel and stays editable, so save the
    // old block if you mean to restore it.
    const ShadowParams& shadowParams() const { return m_shadowParams; }
    void setShadowParams(const ShadowParams& params) { m_shadowParams = params; }
    // Metres from sceneFocusOrCamera: a PASS_GI push farther out never reaches the TLAS, so drop it.
    float giTlasRange() const { return m_giProbePipeline.getTlasRange(); }
    void present();

    // ---- GPU particles + projected decals (driven by the Particle library) ----
    // Slot management is main-thread. The slot's config re-uploads every frame, so an update drives
    // already-spawned particles too. UINT32_MAX = all MAX_PARTICLE_EMITTERS slots taken.
    uint32 createParticleEmitter(const RendererVKLayout::ParticleEmitterGpu& desc);
    void updateParticleEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc);
    // Queues count spawns from the slot this frame (clamped to MAX_PARTICLE_SPAWNS_PER_FRAME total).
    void emitParticles(uint32 slot, uint32 count);
    // Flags the slot's live particles for retirement; the slot recycles once the kill has drained.
    void destroyParticleEmitter(uint32 slot);
    void resetParticles() { m_particles.requestReset(); }
    // The world box the NEXT frame's shelter depth pass covers (PARTICLE_FLAG_OCCLUDE). Main thread,
    // once per frame; the request expires each present, and no request = no pass and no shelter test.
    void setRainOcclusionVolume(const glm::vec3& center, const glm::vec3& halfExtents);
    // Valid after the begin-frame join.
    const glm::vec3& cameraPos() const { return m_cameraPos; }
    // The emitter ocean_spray.cs.glsl spawns into; UINT32_MAX = no spray. Main thread, after the join.
    void setOceanSprayEmitter(uint32 slot) { m_oceanSimPipeline.setSprayEmitter(slot); }
    void addDecal(const RendererVKLayout::DecalInfo& decal); // [Concurrency: LOCK-FREE]
    // Into the bindless array, for ParticleEmitterGpu::texFlags.x / DecalInfo::params.x to reference.
    uint16 loadEffectTexture(const char* filePath, bool sRGB = true);
    // The per-entity TINT path: feed the result to RenderNode::setMaterialOverride. Cached per
    // quantized colour; main thread.
    uint16 createSolidColorMaterial(const glm::vec3& color);

    // ---- Forcefield bubbles (driven by the Force library) ----
    // Main-thread, and every live slot re-uploads each frame, so update is the per-frame push.
    // UINT32_MAX = all MAX_FORCE_EMITTERS slots taken.
    uint32 createForceEmitter(const RendererVKLayout::ForceEmitterGpu& desc);
    void updateForceEmitter(uint32 slot, const RendererVKLayout::ForceEmitterGpu& desc);
    // Deactivates the slot; it recycles once the in-flight frames drain, because the readback below
    // is slot-indexed and must never pair a new emitter with stale results.
    void destroyForceEmitter(uint32 slot);
    // Persistent point-query slots - indices must stay stable across the readback latency, so this is
    // deliberately not a per-frame push. Same contract as the emitter slots.
    uint32 createForceQuerySlot();
    void setForceQuery(uint32 slot, const glm::vec3& pos);
    // This frame's chunk set, pushed with the emitters; read back ~2 frames later.
    void setForceBakeChunks(oc::span<const glm::ivec4> chunks, float sampleY) { m_force.setBakeChunks(chunks, sampleY); }
    // The baked field + the chunk list it was evaluated for (~2 frames old).
    RendererVKLayout::ForceBakeReadback getForceBakeReadback() const;
    void destroyForceQuerySlot(uint32 slot);
    // Slot-indexed, ~2 frames old, readable between beginFrame and present.
    // xyz = applied force (opposing-field pressure integral), w = mean opposing pressure.
    glm::vec4 getForceEmitterReadback(uint32 slot) const;
    RendererVKLayout::ForceQueryResult getForceQueryReadback(uint32 slot) const;
    // Pushed every frame; all live except useGrid, which rebuilds the force pipelines (GPU stall).
    void setForceFieldParams(const ForceFieldParams& params);

    void setAmbientLight(const glm::vec3& color, float intensity) { m_skyParams.ambientColor = color; m_skyParams.ambientIntensity = intensity; }
    void setSkyRadiance(const glm::vec3& color, float intensity) { m_skyParams.skyRadianceColor = color; m_skyParams.skyRadianceIntensity = intensity; }
    void setSkyParams(const SkyParams& sky) { m_skyParams = sky; }
    const SkyParams& getSkyParams() const { return m_skyParams; }
    void setFogParams(const FogParams& fog) { m_fogParams = fog; }
    // Per frame. meshRadius: m from the camera XZ inside which streamed chunks are resident, the fence
    // for the ocean's land cull (0 = no terrain, cull off). lapseRate: temperature change per world
    // metre above sea level (<= 0) - ONE value for the world, so no two consumers can disagree.
    void setTerrainParams(float meshRadius, float seaLevel, float lapseRate = 0.0f) { m_terrain.setParams(meshRadius, lapseRate, seaLevel); }
    // CPU ocean culling must fence on the SAME value its vertex shaders do, or it deletes water the
    // VS would have drawn.
    float getTerrainMeshRadius() const { return m_terrain.getMeshRadius(); }
    // Defined with the terrain state; re-spelled here for the call sites.
    using TerrainSplatMaterial = ::TerrainSplatMaterial;
    using TerrainSplatCounts = ::TerrainSplatCounts;
    using TerrainTexTweaks = ::TerrainTexTweaks;
    using TerrainWetTweaks = ::TerrainWetTweaks;
    // mats must be laid out [numGround][numRock][beach?][snow?] to match counts. Re-registering frees
    // the old textures but leaks the old material slots - a config refresh, not a per-frame path.
    void setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts);
    // One climate box per ground/rock entry, parallel to the mats (beach/snow are overlays, ignored).
    // xy = (t01 min, max), zw = (h01 min, max); weight is 1 inside and Gaussian-decays outside, so an
    // entry that does not care about an axis leaves it full width. Cheap - push it per frame.
    void setTerrainSplatClimate(oc::span<const glm::vec4> boxes);
    void setTerrainTextureParams(const TerrainTexTweaks& params) { m_terrain.setTexTweaks(params); }
    void setTerrainWetParams(const TerrainWetTweaks& params) { m_terrain.setWetTweaks(params); }
    // Deepest wave trough below the calm water level (m, >= 0). Sizes the waterline band inside which
    // the fog scatter samples the live wave height for the underwater boundary.
    void setOceanWaveTrough(float meters) { m_oceanSimPipeline.setWaveTrough(meters); }
    // Worst-case distance the ocean VS moves a vertex off its lattice position. The cull pads the ocean
    // sectors' UNDISPLACED bounding spheres by it; without that, crests still on screen get culled.
    void setOceanDisplacementExtent(float meters) { m_oceanSimPipeline.setDisplacementExtent(meters); }
    // The live water surface Y under the camera (~2 frames of latency), or none: the Underwater /
    // AboveWater particle gate reads it instead of sampling the waves per particle.
    void setCameraWaterSurface(float worldY) { m_oceanSimPipeline.setCameraWaterSurface(worldY); }
    void clearCameraWaterSurface() { m_oceanSimPipeline.clearCameraWaterSurface(); }
    // Flipping OceanParams::hitLighting rebuilds the ocean fragment variant (GPU idle + shader reload).
    void setOceanParams(const OceanParams& ocean);
    // FOG_TERRAIN_CASCADES layers of FOG_TERRAIN_RES^2 RGBA float quads, near cascade first:
    // R = terrain height, G = water surface level, B = regional fog thickness [0,1], A = spare.
    // Cascade i covers cascadeWorldSizes[i] m. Both the height fog base and the ocean's water
    // depth/level read it. Staged ping-pong: live next frame, no GPU sync and no re-record.
    void setFogTerrainHeightMap(oc::span<const float> heightTexels, const glm::vec2& centerXZ, const glm::vec2& cascadeWorldSizes, float seaLevel) { m_terrain.getHeightMap().upload(heightTexels, centerXZ, cascadeWorldSizes, seaLevel, m_swapChain.getCurrentFrameIndex()); }
    void clearFogTerrainHeightMap() { m_terrain.getHeightMap().clear(); } // fog reverts to the flat height base
    // RGBA16F (Dx, h, Dz, dDxz), outRes^2 per cascade, cascades packed consecutively. COPY IT OUT
    // between beginFrame and present - the slot resubmits after that. ~2 frames old.
    oc::span<const uint16> getOceanDisplacementReadback(uint32& outRes) const
    {
        outRes = OceanSimulationPipeline::READBACK_RES;
        return m_oceanSimPipeline.getDisplacementReadback(m_swapChain.getCurrentFrameIndex());
    }
    void setPostParams(const PostParams& post) { m_postParams = post; setHaveToRecordCommandBuffers(); }
    // An ImDrawData*, opaque here. Called from the widget-pass JOB, so it only stores PENDING:
    // present must not see a snapshot before updateImGuiTextures promoted it, or a fast pass draws a
    // frame early with its atlas still an unserviced create request.
    void setImGuiDrawData(const void* drawData) { m_imguiPendingDrawData.store(drawData, oc::memory_order_release); }
    // MAIN THREAD, between the widget pass's join and the next UI::update. Uploads the queued
    // font-atlas changes, which present() would otherwise do while the widget pass mutates the atlas.
    void updateImGuiTextures();

    uint32 getNumMeshInstances() const { return m_instances.getInstanceCount(); }
    uint32 getNumMeshTypes() const { return m_meshInfos.count(); }
    uint32 getNumMaterials() const { return m_materials.count(); }
    uint32 getCurrentFrameIndex() const { return m_swapChain.getCurrentFrameIndex(); }

    void reloadShaders();

    // Built by beginFrame, for CPU shadow-caster queries. Zero while RT sun shadows replace them.
    uint32 getNumSunCascades() const { return m_numSunCascades; }
    const glm::mat4* getSunCascadeViewProj() const { return m_sunCascadeViewProj; }

    // What the GPU cull used (VR: the head-centred two-eye union), for CPU occlusion rasterization.
    const glm::mat4& getCenterViewProj() const { return m_centerViewProj; }

    bool isVrEnabled() const { return Globals::openXR.isEnabled(); }
    bool isVSyncEnabled() const { return m_vsyncEnabled; } // FIFO present: frames land on whole refresh periods (Time's stable-dt snap relies on it)
    bool isVrStageSpace() const { return Globals::openXR.isStageSpace(); }
    IVrSession* getVrSession() { return Globals::openXR.isEnabled() ? &Globals::openXR : nullptr; }

    // The testbed keys (P / O) and the "GI/Debug probes" tweaks drive the same state.
    void toggleGiProbeDebug() { m_giProbePipeline.toggleDebug(); }
    void cycleGiProbeDebugMode() { m_giProbePipeline.cycleDebugMode(); setHaveToRecordCommandBuffers(); }

    void setWindowMinimized(bool minimized);
    void recreateWindowSurface(Window& window);

    Stats getStats();

    // Read by Entity's World to build scene-cache cook options (LOD generation params are baked into
    // cooked files, so they participate in the cache's options hash).
    const MeshLodParams& getLodParams() const { return m_lodParams; }

    // Mesh streaming (MeshStreamer). A streamed-out mesh keeps its bounds but draws zero indices, so
    // the cull's DGC draws, the shadow pass and the TLAS-instance writer all no-op for it - no re-record.
    void setMeshStreamedOut(uint16 meshInfoIdx);
    void setMeshStreamedIn(uint16 meshInfoIdx, int32 vertexOffset, uint32 firstIndex, uint32 indexCount);

private:

    Renderer(const Renderer&) = delete;
    Renderer(const Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(const Renderer&&) = delete;

    // An EMPTY rect keeps the previous one: the UI widget pass runs a frame behind, so frame 0 arrives
    // with the default Rect() and a 0/0 aspect would reach glm::perspective.
    void setViewportRect(const Rect& rect) { if (Globals::openXR.isEnabled()) return; if (rect.getSize().x <= 0 || rect.getSize().y <= 0) return; if (rect != m_viewportRect) { m_viewportRect = rect; setHaveToRecordCommandBuffers(); } }

    CommandBuffer& getCurrentCommandBuffer() { return m_perFrameData[m_swapChain.getCurrentFrameIndex()].primaryCommandBuffer; }

    void recordCommandBuffers();
    // recordCommandBuffers' parts (see the frame order in CONTEXT.md):
    void recordSceneSecondaries(uint32 frameIdx);                          // every cached secondary (invalidation frames only)
    void recordPrimaryPreScene(uint32 frameIdx, vk::CommandBuffer primary); // skinning .. shadow draw (shared by both view modes)
    void recordPrimaryVR(uint32 frameIdx, CommandBuffer& primary);          // GI + fog, then the per-eye chain inline, eye adaptation, the eye composites
    void recordPrimaryDesktop(uint32 frameIdx, vk::CommandBuffer primary);  // GI, fog, scene opaque, RTAO, force passes, scene forward, TAA, eye adaptation
    void executeScoped(vk::CommandBuffer primary, const char* scope, vk::CommandBuffer secondary);
    void initBindlessTextures(); // wires the six consumers of the bindless arrays into m_textures
    // Reports the node's projected screen size to the TextureStreamer, per material texture it samples.
    void noteTextureUse(const RenderNode& node, uint32 passMask);
    struct PerFrameData;
    // Keeps every level of the node's chains warm in the mesh streamer, and publishes its state-slot
    // bias for the cull shader.
    void noteLodChainUse(const RenderNode& node, uint32 startIdx, InstanceStream::FrameSlot& instances);

    // beginFrame helpers, in call order; they run wherever beginFrame does.
    glm::mat4 computeCenterViewProj(const Camera& camera) const; // pure: projection (VR: both eyes) * view
    void applyVrHeadPose(const Camera& cameraIn, Camera& camera, glm::quat& vrBaseOrientation);
    void checkFrameCapacities();
    void snapshotLodStats(PerFrameData& frameData);
    void buildFrameUbo(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation, PerFrameData& frameData);
    void buildUboViews(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation);
    void buildUboSky();
    void buildUboSunShadow(const Camera& camera);
    void buildUboRainOcclusion();
    void buildUboFog();
    void buildUboOcean();
    void buildUboForce();
    void buildUboTerrain();

    // Each caller ends its own cb.
    vk::CommandBuffer beginComputeSecondary(CommandBuffer& cb);              // outside any render pass
    vk::CommandBuffer beginScenePassSecondary(uint32 frameIdx, CommandBuffer& cb); // continues the scene-colour pass
    // The viewport/scissor every full-res scene stage sets: y-flipped over m_viewportRect, scissored
    // to the whole swapchain extent.
    void setFullViewport(vk::CommandBuffer vkCb) const;

    void recordSkinning(uint32 frameIdx);
    void recordOceanSim(uint32 frameIdx);
    void recordTerrainWetness(uint32 frameIdx);
    void recordIndirectCull(uint32 frameIdx);
    void recordLightGrid(uint32 frameIdx);
    void recordShadowCull(uint32 frameIdx);
    void recordShadowDraw(uint32 frameIdx);
    void recordRainOcclusionCull(uint32 frameIdx);
    void recordRainOcclusionDraw(uint32 frameIdx);
    void recordStaticMesh(uint32 frameIdx);
    void recordStaticMeshInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    // The scene depth's ONE layout switch per frame and eye. The stage render passes do no depth
    // transition of their own - their dependency arrays must stay identical for compatibility.
    void recordSceneDepthToSampled(vk::CommandBuffer cb, vk::Image sceneDepth, uint32 eyeIndex);
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
    // Two secondaries so the GPU profiler scopes the proxy ray-march and the union blend separately.
    void recordForceShells(uint32 frameIdx);
    void recordForceUnion(uint32 frameIdx);
    void recordForceFieldInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex,
        ForceFieldPipeline::EDrawPart part = ForceFieldPipeline::EDrawPart::Both);
    // VR records both parts in one go; a default argument cannot ride a member-function pointer.
    void recordForceFieldBothInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex)
    { recordForceFieldInto(cb, frameIdx, eyeIndex, ForceFieldPipeline::EDrawPart::Both); }
    void recordForceCompute(uint32 frameIdx);
    void recordForceMarch(uint32 frameIdx);
    void recordAO(uint32 frameIdx);
    void recordVolumetricFog(uint32 frameIdx);
    void recordFogApply(uint32 frameIdx);
    void recordTaa(uint32 frameIdx);
    void recordEyeAdaptation(uint32 frameIdx);
    void recordComposite(uint32 frameIdx);

    // ---- THE scene stage table ----
    // recordSceneSecondaries, recordPrimaryDesktop and recordPrimaryVR all read it, so a stage is
    // added, re-ordered or re-gated in exactly ONE place. Table order IS draw order.
    struct SceneStage
    {
        const char* name;
        bool opaque;        // runs with the depth-WRITING group, before the read-only switch
        bool enabled;       // this frame's gate: desktop executes it, VR records it inline
        bool gateRecording; // ALSO gate the cached record. Debug lines only: its vertex buffers may not
                            // exist yet. Others must record unconditionally - their enable tweaks force
                            // no re-record, so a skipped one leaves a stale secondary.
        CommandBuffer* cb;                                              // the cached secondary (desktop)
        void (Renderer::*recordCached)(uint32);                         // fills cb
        void (Renderer::*recordInline)(CommandBuffer&, uint32, uint32); // VR, per eye; null = desktop only
    };
    oc::array<SceneStage, 8> buildSceneStages(uint32 frameIdx);
    void recreateVrEyeTargets(); // VR: the per-eye LDR composite targets follow the swapchain
    void recordGlobalIllumPrep(uint32 frameIdx); // per frame: one-shot BLAS builds, compaction, the skinned rebuild, the one-time clear
    void recordGlobalIllum(uint32 frameIdx);     // cached: sky map, TLAS instances + build, probe trace
    void setHaveToRecordCommandBuffers();
    void recreateSwapchain();
    void initImgui(Window& window);
    // initialize(), in call order; none may be called on its own.
    void registerTweaks();                                                  // every tweak the Renderer owns
    bool initDeviceAndSwapchain(Window& window, EValidation validation, EVr vr); // instance/device/XR/surface/swapchain + the global managers
    void initPipelines();                                                   // the scene targets and every pipeline over them
    void initPerFrameResources();                                           // per-slot descriptor sets, command buffers and mapped buffers
    void initSharedBuffers();                                               // the frame-independent scene buffers + the fallback textures

    friend class ObjectContainer;
    void addObjectContainer(ObjectContainer* pObjectContainer);
    // ~ObjectContainer. Every RenderNode it spawned must be destroyed first. CPU slots recycle at once;
    // vk objects an in-flight frame may still sample (textures, static BLASes) retire to a drain queue.
    void removeObjectContainer(ObjectContainer* pObjectContainer);
    // Zeroes the slots' indexCount, so draws and TLAS writes no-op like a streamed-out mesh, then
    // retires their BLASes and frees the range.
    void freeMeshInfoRange(uint32 baseMeshInfoIdx, uint32 count);
    // Full teardown of a PARKED bundle: frees everything its spawn allocated.
    void destroySkinnedBundle(uint32 bundleHandle);

    // PARALLEL ENTITY SPAWNING: one coarse mutex over every allocator the spawn/despawn path reaches.
    // RECURSIVE because ObjectContainer::spawnNodeForIdx holds it across its whole body while calling
    // the locked leaves below. The parallel entity PASS never takes it and stays lock-free.
    std::recursive_mutex m_spawnMutex;

    uint32 addRenderNodeTransform(const Transform& transform);
    // vertexCounts: exact per-MeshInfo vertex count (the BLAS builder needs a tight maxVertex).
    // skinnedOutputs: per-instance skinned output regions, which never build a static BLAS.
    uint32 addMeshInfos(const oc::vector<RendererVKLayout::MeshInfo>& meshInfos, oc::span<const uint32> vertexCounts, bool skinnedOutputs = false);
    uint16 getRtMeshAlias(uint16 meshIdx) const { return (uint16)m_rt.accel().getMeshAlias(meshIdx); }
    uint32 addMaterialInfos(const oc::vector<RendererVKLayout::MaterialInfo>& materialInfos);
    uint32 addMeshInstanceOffsets(const oc::vector<RendererVKLayout::MeshInstanceOffset>& meshInstanceOffsets);
    // m_meshLods owns the selection half; only the RT aliases are the Renderer's.
    uint32 addMeshLodGroup(const MeshLodGroup& group)
    {
        // One shared BLAS per chain: rays don't need per-level fidelity.
        const uint8 rtLevel = (uint8)oc::clamp(m_rtParams.blasLodLevel, 0, (int)group.numLods - 1);
        for (uint8 k = 0; k < group.numLods; ++k)
            m_rt.accel().setMeshAlias(group.meshIdx[k], group.meshIdx[rtLevel]);
        return m_meshLods.addGroup(group);
    }
    void freeMeshLodGroup(uint32 groupIdx) { m_meshLods.freeGroup(groupIdx); }
    // Per-instance LOD hysteresis state slots (GPU), one contiguous range per RenderNode with LOD chains.
    uint32 allocateLodStateRange(uint32 count)
    {
        const std::lock_guard lock(m_spawnMutex); // parallel entity spawning
        return m_meshLods.allocateStateRange(count);
    }
    // m_skinned owns skinning; these are the spawn-path spellings, with the mutex taken where needed.
    using SkinnedInstanceBundle = SkinnedMeshRegistry::Bundle;
    uint32 addSkinnedMeshSources(const oc::vector<RendererVKLayout::SkinnedMeshSource>& sources)
    {
        return m_skinned.addSources(sources);
    }
    const RendererVKLayout::SkinnedMeshSource& getSkinnedMeshSource(uint32 idx) const { return m_skinned.getSource(idx); }
    uint32 acquireSkinnedBundle(uint32 sourceKey) // reactivates + returns a parked bundle, UINT32_MAX if none free
    {
        const std::lock_guard lock(m_spawnMutex); // parallel entity spawning
        return m_skinned.acquireBundle(sourceKey);
    }
    uint32 registerSkinnedBundle(const SkinnedInstanceBundle& bundle)
    {
        const std::lock_guard lock(m_spawnMutex);
        return m_skinned.registerBundle(bundle);
    }
    void releaseSkinnedBundle(uint32 bundleHandle)
    {
        const std::lock_guard lock(m_spawnMutex);
        m_skinned.parkBundle(bundleHandle);
    }
    const SkinnedInstanceBundle& getSkinnedBundle(uint32 handle) const { return m_skinned.getBundle(handle); }

    friend class RenderNode;
    void freeRenderNode(RenderNode& node);
    inline Transform& getRenderNodeTransform(uint32 idx) { return m_instances.getTransform(idx); }

    friend class AnimatorComponent;
    uint32 allocateSkinningPalette(uint32 boneCount)
    {
        const std::lock_guard lock(m_spawnMutex); // parallel entity spawning
        return m_skinned.allocatePalette(boneCount);
    }
    void setSkinningPalette(const RenderNode& node, oc::span<const glm::mat4> palette);
    // One contiguous block per bundle; fill it per mesh afterwards with setSkinnedInstance.
    uint32 allocateSkinningJobRange(uint32 count)
    {
        const std::lock_guard lock(m_spawnMutex);
        return m_skinned.allocateJobRange(count);
    }
    void setSkinnedInstance(uint32 jobIdx, uint32 baseVertexOffset, uint32 skinVertexOffset, uint32 outVertexOffset, uint32 vertexCount, uint32 paletteHandle, uint32 meshIdx, uint32 firstIndex, uint32 indexCount)
    {
        m_skinned.setInstance(jobIdx, baseVertexOffset, skinVertexOffset, outVertexOffset, vertexCount, paletteHandle, meshIdx, firstIndex, indexCount);
    }

    void waitForGpuAndFlushStaging();
    // SharedTable's onGrown hook: everything besides the mesh-info buffer that the capacity feeds.
    void onUniqueMeshCapacityGrown(uint32 maxUniqueMeshes);

private:

    Instance m_instance;
    Device m_device;
    Surface m_surface;
    SwapChain m_swapChain;
    GpuProfiler m_gpuProfiler;
    JobCounter m_gpuCollectCounter; // the in-flight timestamp-collect job (beginFrame kicks -> recordCommandBuffers joins)
    // The job reads these, so they only change while no job is in flight.
    Camera m_beginFrameJobCamera;
    Rect m_beginFrameJobRect;
    JobCounter m_beginFrameJobCounter;
    bool m_beginFrameDeferred = false; // VR: kick stored, join runs beginFrame synchronously
    // Both grid jobs share it; what each measured lives in the object that owns that grid.
    JobCounter m_gridJobCounter;
    bool m_gridBuildsKicked = false;
    void joinGridBuilds(uint32 frameIdx, PerFrameData& frameData);
    // VR one-frame-latent cull view (see getCullView); written at the end of beginFrame, VR only.
    Camera m_lastCullCamera;
    bool m_hasCullView = false;
    RenderPass m_renderPass;
    Framebuffers m_framebuffers;
	GpuCrashTracker m_gpuCrashTracker;

    IndirectCullComputePipeline m_indirectCullComputePipeline;
    SkinningComputePipeline m_skinningComputePipeline;
    LightGridComputePipeline m_lightGridComputePipeline;
    StaticMeshGraphicsPipeline m_staticMeshGraphicsPipeline;
    RTAOPipeline m_rtaoPipeline;
    OceanSimulationPipeline m_oceanSimPipeline;
    TerrainWetnessPipeline m_terrainWetnessPipeline;
    VolumetricFogPipeline m_volumetricFogPipeline;
    TaaPipeline m_taaPipeline;
    ShadowCullComputePipeline m_shadowCullComputePipeline;
    ShadowMapGraphicsPipeline m_shadowMapGraphicsPipeline;
    // RAIN_OCCLUSION variants of the two shadow pipelines, over PerFrameData::rainOcclusionMap.
    ShadowCullComputePipeline m_rainCullComputePipeline;
    ShadowMapGraphicsPipeline m_rainMapGraphicsPipeline;
    CompositePipeline m_compositePipeline;
    EyeAdaptationPipeline m_eyeAdaptationPipeline;
    RayTracingScene m_rt; // the AccelerationStructure + the BLAS/TLAS bookkeeping around it
    GIProbePipeline m_giProbePipeline;
    DebugLinePipeline m_debugLinePipeline;
    ParticlePipeline m_particlePipeline;
    DecalPipeline m_decalPipeline;
    ForceFieldPipeline m_forceFieldPipeline;
    ParticleState m_particles; // emitters, this frame's spawns, the tweaks, the rain box, the pool reset
    ForceFieldState m_force; // params, the emitter + query registries, the bake chunks, the grid job
    BindlessTextures m_textures; // the layout cap + live descriptor count, slot writes, deferred frees
    PerWorker<oc::vector<DebugLinePipeline::LineVertex>> m_debugLineVerts; // per-worker CPU staging, drained into the mapped buffer in present()

    SkyParams m_skyParams;
    ShadowParams m_shadowParams;
    glm::vec3 m_sceneFocus = glm::vec3(0.0f); // setSceneFocus
    bool m_sceneFocusEnabled = false;
    FogParams m_fogParams;
    TerrainResources m_terrain; // params, splat set + climate, the tweak blocks, the height map, wetness
    PostParams m_postParams;
    RTParams m_rtParams;
    RTAOParams m_rtaoParams;
    LightGridParams m_lightGridParams;
    TAAParams m_taaParams;

    glm::vec3 m_cameraPos = glm::vec3(0.0f);
    glm::vec3 m_prevCameraPos = glm::vec3(0.0f); // last frame's, for u_cameraVelocity (buildFrameUbo)
    bool m_havePrevCameraPos = false;
    glm::vec3 m_giPrevFocusPos = glm::vec3(0.0f); // last frame's scene focus (sceneFocusOrCamera); drives GI clipmap probe freshness
    float m_mipPixelScale = 0.0f; // viewportHeight / tan(fovY/2): projected diameter px = radius * scale / dist
    MeshLodParams m_lodParams;

    glm::mat4 m_sunCascadeViewProj[RendererVKLayout::NUM_SHADOW_CASCADES];
    uint32 m_numSunCascades = 0;
    glm::mat4 m_centerViewProj = glm::mat4(1.0f);

    // PERSISTS across frames: buildUboViews reprojects from last frame's mvps before overwriting them.
    RendererVKLayout::Ubo m_ubo;

    glm::ivec2 m_windowSize;
    const void* m_imguiDrawData = nullptr; // what present records: promoted from the pending slot on main
    oc::atomic<const void*> m_imguiPendingDrawData = nullptr; // see setImGuiDrawData
    Rect m_viewportRect = Rect();
    bool m_initialized = false;
    bool m_frameSlotWaited = false; // waitFrameSlot() ran for the current slot (cleared by present)
    bool m_windowMinimized = false;
    bool m_vsyncEnabled = true; // "Time/VSync" tweak (Saved; --no-vsync overrides it): FIFO vs Immediate, applied by a swapchain recreate
    glm::vec2 m_prevTaaJitter{ 0.0f }; // last frame's TAA jitter (ubo.taaJitter.zw): consumers of the PREV depth image compensate with it
    uint32 m_sceneViewCount = 1; // 2 in VR: SceneColor + forward pass are multiview (one layer per eye)

    VrEyeTargets m_vrEyes; // VR: the per-eye LDR composite targets


    uint32 m_meshDataGeneration = 0; // last seen MeshDataManager::getGeneration(); change -> re-record

    SkinnedMeshRegistry m_skinned;  // the skinning jobs, bone palettes, sources and spawn bundles
    InstanceStream m_instances;     // the per-frame mapped push buffers + the render-node transform slots
    FrameSubmission m_submission;   // lights / fog volumes / decals, and the light grid's GPU scratch

    oc::vector<ObjectContainer*> m_objectContainers;
    MeshLodRegistry m_meshLods; // the chains, the per-mesh mapping and the GPU selection buffers
    oc::array<uint32, RendererVKLayout::MAX_MESH_LODS> m_lodInstanceCounts{}; // stats snapshot of the GPU cull's per-level picks


    // A member, not a function-local static: /Zc:threadSafeInit- leaves those unguarded.
    Clock::time_point m_eyeAdaptLastTime;
    bool m_haveEyeAdaptTime = false;

    uint32 m_frameCounter = 0; // monotonic; rotates the GI probe ray/taa samples set each frame

    // The Renderer keeps the parallel CPU side tables (m_rt, m_instances) and reacts to a growth.
    SharedTable<RendererVKLayout::MeshInfo> m_meshInfos;
    SharedTable<RendererVKLayout::MaterialInfo> m_materials;
    SharedTable<RendererVKLayout::MeshInstanceOffset> m_instanceOffsets;
    oc::unordered_map<uint32, uint16> m_solidColorMaterials; // packed RGB8 -> material idx (tint cache)

    struct PerFrameData
    {
        SceneColor sceneColor; // colour + THE scene depth (no prepass: every depth reader samples this one)
        ShadowMap shadowMap;
        ShadowMap rainOcclusionMap; // single layer, RAIN_OCCLUSION_RESOLUTION: the weather volume's shelter depth

        // Per-eye in VR
        oc::array<DescriptorSet, 2> staticMeshPipelineDescriptorSet;
        DescriptorSet compositeDescriptorSet;
        DescriptorSet indirectCullPipelineDescriptorSet;
        DescriptorSet skinningDescriptorSet;
        DescriptorSet lightGridPipelineDescriptorSet;
        DescriptorSet shadowCullDescriptorSet;
        DescriptorSet shadowDrawDescriptorSet;
        DescriptorSet rainCullDescriptorSet;
        DescriptorSet rainDrawDescriptorSet;

        CommandBuffer primaryCommandBuffer;
        CommandBuffer staticMeshCommandBuffer;
        CommandBuffer aoCommandBuffer;
        CommandBuffer indirectCullCommandBuffer;
        CommandBuffer skinningCommandBuffer;
        CommandBuffer oceanSimCommandBuffer;
        CommandBuffer terrainWetnessCommandBuffer;
        CommandBuffer lightGridCommandBuffer;
        CommandBuffer imguiCommandBuffer;
        CommandBuffer shadowCullCommandBuffer;
        CommandBuffer shadowDrawCommandBuffer;
        CommandBuffer rainCullCommandBuffer;
        CommandBuffer rainDrawCommandBuffer;
        CommandBuffer globalIllumCommandBuffer; // cached: sky map + TLAS instances/build + trace (recordGlobalIllum)
        CommandBuffer giPrepCommandBuffer;      // per frame: BLAS builds / compaction / skinned rebuild (recordGlobalIllumPrep)
        CommandBuffer volumetricFogCommandBuffer;
        CommandBuffer fogApplyCommandBuffer;
        CommandBuffer giProbeDebugCommandBuffer;
        CommandBuffer debugLineCommandBuffer;
        CommandBuffer particleSimCommandBuffer;
        CommandBuffer particleCommandBuffer;
        CommandBuffer decalCommandBuffer;
        CommandBuffer forceFieldCommandBuffer;
        CommandBuffer forceUnionCommandBuffer; // the union-march fullscreen draw, its own scene stage for the GPU profiler
        CommandBuffer forceIntervalCommandBuffer; // the union march's interval pass (own render pass, before the scene stages)
        CommandBuffer forceMarchCommandBuffer;    // the half-res union march pass (own render pass; empty in full-res mode)
        CommandBuffer forceComputeCommandBuffer;
        CommandBuffer taaCommandBuffer;
        CommandBuffer eyeAdaptCommandBuffer;
        CommandBuffer compositeCommandBuffer;

        bool updated = false;
        Buffer ubo;
        Buffer lodStatsBuffer; // per-level LOD pick counts, written by the cull, read back for stats
        oc::span<uint32> mappedLodStats;


        RendererVKLayout::Ubo* mappedUniformBuffer = nullptr;


    };
    oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_RENDERER)
    Renderer rendererVK;
} // namespace Globals