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

export enum class EValidation { ENABLED, DISABLED };
export enum class EVr { ENABLED, DISABLED };

export class Renderer final
{
public:

    Renderer() {}
    ~Renderer();

    // -- Main API --
    bool initialize(Window& window, EValidation validation, EVr vr = EVr::DISABLED);
    bool isInitialized() const { return m_initialized; }  // false in headless server mode; anything that may run headless must gate on it.

    bool waitFrameSlot(uint64 timeoutNs = UINT64_MAX); // VSync/GPU throttle of the whole loop. used by Time::beginFrame for frame pacing
    CullView setFrameView(const Camera& camera, const Rect& viewportRect); // returns the spatial cull's view of of this frame, call before kickBeginFrameJob().
    void kickBeginFrameJob(); // The frame's setup pass, as a job. VR defers instead: the join runs it on main, because xrWaitFrame owns VR pacing and would pin a worker.
    void joinBeginFrameJob();

    void renderNode(const RenderNode& node, uint32 passMask = RendererVKLayout::PASS_ALL); // [Concurrency: LOCK-FREE]
    void addLightInfo(const RendererVKLayout::LightInfo& light);                           // [Concurrency: LOCK-FREE]
    void addFogVolume(const RendererVKLayout::FogVolumeInfo& fogVolume);                   // [Concurrency: LOCK-FREE]
    void addPointLight(const PointLight& light);                                           // [Concurrency: LOCK-FREE]
    void addAreaLight(const AreaLight& areaLight);                                         // [Concurrency: LOCK-FREE]
    void addSpotLight(const SpotLight& spotLight);                                         // [Concurrency: LOCK-FREE]

    void kickGridBuilds(); // Call after the frame's LAST light add and the force update.
    void present();

    void reloadShaders();
    void setWindowMinimized(bool minimized);
    void recreateWindowSurface(Window& window); // Call on resize

    // -- View settings --
    // The world point every distance-based falloff measures from (u_sceneFocus). Sun cascades, RTAO fade, GI clipmap window, TLAS range. Cleared = camera pos.
    void setSceneFocus(const glm::vec3& worldPos) { m_sceneFocus = worldPos; m_sceneFocusEnabled = true; }
    void clearSceneFocus() { m_sceneFocusEnabled = false; }
    glm::vec3 sceneFocusOrCamera() const { return m_sceneFocusEnabled ? m_sceneFocus : m_cameraPos; }
    const glm::mat4& getCenterViewProj() const { return m_centerViewProj; } // What the GPU cull used (VR: the head-centred two-eye union), for CPU occlusion rasterization.
    const glm::vec3& cameraPos() const { return m_cameraPos; } // Valid after joinBeginFrameJob().

    // -- Metrics --
    Stats getStats();
    uint32 getNumMeshInstances() const { return m_instances.getInstanceCount(); }
    uint32 getNumMeshTypes() const { return m_meshInfos.count(); }
    uint32 getNumMaterials() const { return m_materials.count(); }

    // -- GPU particles + projected decals (driven by the Particle library) --
    uint32 createParticleEmitter(const RendererVKLayout::ParticleEmitterGpu& desc); // [Concurrency: LOCKING]
    void updateParticleEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc);
    void emitParticles(uint32 slot, uint32 count);
    void destroyParticleEmitter(uint32 slot); // [Concurrency: LOCKING]
    void resetParticles() { m_particles.requestReset(); }
    void setRainOcclusionVolume(const glm::vec3& center, const glm::vec3& halfExtents);
    void setOceanSprayEmitter(uint32 slot) { m_oceanSimPipeline.setSprayEmitter(slot); }
    void addDecal(const RendererVKLayout::DecalInfo& decal); // [Concurrency: LOCK-FREE]
    uint16 loadEffectTexture(const char* filePath, bool sRGB = true);

    // -- Forcefield bubbles (driven by the Force library) --
    uint32 createForceEmitter(const RendererVKLayout::ForceEmitterGpu& desc); // [Concurrency: LOCKING]
    void updateForceEmitter(uint32 slot, const RendererVKLayout::ForceEmitterGpu& desc);
    void destroyForceEmitter(uint32 slot); // [Concurrency: LOCKING]
    uint32 createForceQuerySlot(); // [Concurrency: LOCKING]
    void setForceQuery(uint32 slot, const glm::vec3& pos);
    void setForceBakeChunks(oc::span<const glm::ivec4> chunks, float sampleY) { m_force.setBakeChunks(chunks, sampleY); }
    RendererVKLayout::ForceBakeReadback getForceBakeReadback() const;
    void destroyForceQuerySlot(uint32 slot); // [Concurrency: LOCKING]
    glm::vec4 getForceEmitterReadback(uint32 slot) const; // ~2 frames old, between beginFrame and present. xyz = applied force (opposing-field pressure integral), w = mean opposing pressure.
    RendererVKLayout::ForceQueryResult getForceQueryReadback(uint32 slot) const;
    void setForceFieldParams(const ForceFieldParams& params);

    // -- Global rendering parameters --
    void setSunLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    void setAmbientLight(const glm::vec3& color, float intensity) { m_skyParams.ambientColor = color; m_skyParams.ambientIntensity = intensity; }
    void setSkyRadiance(const glm::vec3& color, float intensity) { m_skyParams.skyRadianceColor = color; m_skyParams.skyRadianceIntensity = intensity; }
    void setSkyParams(const SkyParams& sky) { m_skyParams = sky; }
    const SkyParams& getSkyParams() const { return m_skyParams; }
    void setFogParams(const FogParams& fog) { m_fogParams = fog; }
    void setPostParams(const PostParams& post) { m_postParams = post; setHaveToRecordCommandBuffers(); }
    const ShadowParams& shadowParams() const { return m_shadowParams; }
    void setShadowParams(const ShadowParams& params) { m_shadowParams = params; }

	// -- Terrain parameters --
    void setTerrainParams(float meshRadius, float seaLevel, float temperatureLapseRate = 0.0f) { m_terrain.setParams(meshRadius, temperatureLapseRate, seaLevel); }
    float getTerrainMeshRadius() const { return m_terrain.getMeshRadius(); }
    using TerrainSplatMaterial = ::TerrainSplatMaterial;
    using TerrainSplatCounts = ::TerrainSplatCounts;
    using TerrainTexTweaks = ::TerrainTexTweaks;
    using TerrainWetTweaks = ::TerrainWetTweaks;
    void setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts); // See TerrainStreamer::registerTerrainTextures for docs
    void setTerrainTextureParams(const TerrainTexTweaks& params) { m_terrain.setTexTweaks(params); }
    void setTerrainWetParams(const TerrainWetTweaks& params) { m_terrain.setWetTweaks(params); }
    // FOG_TERRAIN_CASCADES layers of FOG_TERRAIN_RES^2 RGBA float quads, near cascade first: R = terrain height, G = water surface level, B = regional fog thickness [0,1], A = spare.
    // Cascade i covers cascadeWorldSizes[i] m. Both the height fog base and the ocean's water depth/level read it. Staged ping-pong: live next frame, no GPU sync and no re-record.
    void setFogTerrainHeightMap(oc::span<const float> heightTexels, const glm::vec2& centerXZ, const glm::vec2& cascadeWorldSizes, float seaLevel) { m_terrain.getHeightMap().upload(heightTexels, centerXZ, cascadeWorldSizes, seaLevel, m_swapChain.getCurrentFrameIndex()); }
    void clearFogTerrainHeightMap() { m_terrain.getHeightMap().clear(); } // fog reverts to the flat height base
    float giTlasRange() const { return m_giProbePipeline.getTlasRange(); } // Metres from sceneFocusOrCamera, for TerrainStreamer

    // -- Ocean generator -> sim piping --
    void setOceanWaveTrough(float meters) { m_oceanSimPipeline.setWaveTrough(meters); }
    void setOceanDisplacementExtent(float meters) { m_oceanSimPipeline.setDisplacementExtent(meters); }
    void setCameraWaterSurface(float worldY) { m_oceanSimPipeline.setCameraWaterSurface(worldY); }
    void clearCameraWaterSurface() { m_oceanSimPipeline.clearCameraWaterSurface(); }
    void setOceanParams(const OceanParams& ocean);
    // RGBA16F (Dx, h, Dz, dDxz), outRes^2 per cascade, cascades packed consecutively. Copy between beginFrame and present, the slot resubmits after that. ~2 frames old.
    oc::span<const uint16> getOceanDisplacementReadback(uint32& outRes) const
    {
        outRes = OceanSimulationPipeline::READBACK_RES;
        return m_oceanSimPipeline.getDisplacementReadback(m_swapChain.getCurrentFrameIndex());
    }

    // -- UI --
    void setImGuiDrawData(const void* drawData) { m_imguiPendingDrawData.store(drawData, oc::memory_order_release); }
    void updateImGuiTextures(); // MAIN THREAD, between the widget pass's join and the next UI::update. Uploads the queued font-atlas changes, which present() would otherwise do while the widget pass mutates the atlas.

    // -- VR --
    bool isVrEnabled() const { return Globals::openXR.isEnabled(); }
    bool isVSyncEnabled() const { return m_vsyncEnabled; } // FIFO present: frames land on whole refresh periods (Time's stable-dt snap relies on it)
    bool isVrStageSpace() const { return Globals::openXR.isStageSpace(); }
    IVrSession* getVrSession() { return Globals::openXR.isEnabled() ? &Globals::openXR : nullptr; }

    // -- Mesh streaming. -- A streamed-out mesh keeps its bounds but draws zero indices, so the cull's DGC draws, the shadow pass and the TLAS-instance writer all no-op for it.
    void setMeshStreamedOut(uint16 meshInfoIdx);
    void setMeshStreamedIn(uint16 meshInfoIdx, int32 vertexOffset, uint32 firstIndex, uint32 indexCount);
    const MeshLodParams& getLodParams() const { return m_lodParams; }

    // -- Debug rendering -- 
    uint16 getOrCreateSolidColorMaterial(const glm::vec3& color);
    void addDebugLine(const glm::vec3& a, const glm::vec3& b, uint32 color) // [Concurrency:LOCK - FREE - per - worker staging, merged in present]
    {
        oc::vector<DebugLinePipeline::LineVertex>& verts = m_debugLineVerts.local();
        const ThreadLocalScope tlsPin;
        verts.push_back({ a, color });
        verts.push_back({ b, color });
    }
    void toggleGiProbeDebug() { m_giProbePipeline.toggleDebug(); }
    void cycleGiProbeDebugMode() { m_giProbePipeline.cycleDebugMode(); setHaveToRecordCommandBuffers(); }

private:
    struct PerFrameData;

    Renderer(const Renderer&) = delete;
    Renderer(const Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(const Renderer&&) = delete;

    uint32 getCurrentFrameIndex() const { return m_swapChain.getCurrentFrameIndex(); }
    CommandBuffer& getCurrentCommandBuffer() { return m_perFrameData[m_swapChain.getCurrentFrameIndex()].primaryCommandBuffer; }

	void beginFrame(); // through kickBeginFrameJob() and joinBeginFrameJob(): sets up the frame's per-frame data, cull, and UBOs.

    void recordCommandBuffers();
    void recordSceneSecondaries(uint32 frameIdx);
    void recordPrimaryPreScene(uint32 frameIdx, vk::CommandBuffer primary);
    void recordPrimaryVR(uint32 frameIdx, CommandBuffer& primary);
    void recordPrimaryDesktop(uint32 frameIdx, vk::CommandBuffer primary);
    void executeScoped(vk::CommandBuffer primary, const char* scope, vk::CommandBuffer secondary);

    vk::CommandBuffer beginComputeSecondary(CommandBuffer& cb); // Each caller ends its own cb.
    vk::CommandBuffer beginScenePassSecondary(uint32 frameIdx, CommandBuffer& cb);

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
    void recordForceShells(uint32 frameIdx);
    void recordForceUnion(uint32 frameIdx);
    void recordForceFieldInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex, ForceFieldPipeline::EDrawPart part = ForceFieldPipeline::EDrawPart::Both);
    void recordForceFieldBothInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex) { recordForceFieldInto(cb, frameIdx, eyeIndex, ForceFieldPipeline::EDrawPart::Both); }
    void recordForceCompute(uint32 frameIdx);
    void recordForceMarch(uint32 frameIdx);
    void recordAO(uint32 frameIdx);
    void recordVolumetricFog(uint32 frameIdx);
    void recordFogApply(uint32 frameIdx);
    void recordTaa(uint32 frameIdx);
    void recordEyeAdaptation(uint32 frameIdx);
    void recordComposite(uint32 frameIdx);

    void setFullViewport(vk::CommandBuffer vkCb) const;
    void setViewportRect(const Rect& rect) { if (Globals::openXR.isEnabled()) return; if (rect.getSize().x <= 0 || rect.getSize().y <= 0) return; if (rect != m_viewportRect) { m_viewportRect = rect; setHaveToRecordCommandBuffers(); } }
    void initBindlessTextures();

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

    // ---- THE scene stage table ----
    // recordSceneSecondaries, recordPrimaryDesktop and recordPrimaryVR all read it, so a stage is added, re-ordered or re-gated in exactly ONE place. Table order IS draw order.
    struct SceneStage
    {
        const char* name;
        bool opaque;        // runs with the depth-WRITING group, before the read-only switch
        bool enabled;       // this frame's gate: desktop executes it, VR records it inline
        bool gateRecording; // Debug lines only: its vertex buffers may not exist yet.
        CommandBuffer* cb;  // the cached secondary (desktop)
        void (Renderer::*recordCached)(uint32);                         // fills cb
        void (Renderer::*recordInline)(CommandBuffer&, uint32, uint32); // VR, per eye; null = desktop only
    };
    oc::array<SceneStage, 8> buildSceneStages(uint32 frameIdx);

    void recreateVrEyeTargets();
    void recordGlobalIllumPrep(uint32 frameIdx); // per frame: one-shot BLAS builds, compaction, the skinned rebuild, the one-time clear
    void recordGlobalIllum(uint32 frameIdx);     // cached: sky map, TLAS instances + build, probe trace
    void setHaveToRecordCommandBuffers();
    void recreateSwapchain();
    void initImgui(Window& window);
    void registerTweaks();
    bool initDeviceAndSwapchain(Window& window, EValidation validation, EVr vr);
    void initPipelines();
    void initPerFrameResources();
    void initSharedBuffers();

    friend class ObjectContainer;
    void addObjectContainer(ObjectContainer* pObjectContainer);
    void removeObjectContainer(ObjectContainer* pObjectContainer);
    void freeMeshInfoRange(uint32 baseMeshInfoIdx, uint32 count);

    friend class RenderNode;
    uint32 addRenderNodeTransform(const Transform& transform);
    inline Transform& getRenderNodeTransform(uint32 idx) { return m_instances.getTransform(idx); }
    void freeRenderNode(RenderNode& node);

    uint32 addMeshInfos(const oc::vector<RendererVKLayout::MeshInfo>& meshInfos, oc::span<const uint32> vertexCounts, bool skinnedOutputs = false);
    uint16 getRtMeshAlias(uint16 meshIdx) const { return (uint16)m_rt.accel().getMeshAlias(meshIdx); }
    uint32 addMaterialInfos(const oc::vector<RendererVKLayout::MaterialInfo>& materialInfos);
    uint32 addMeshInstanceOffsets(const oc::vector<RendererVKLayout::MeshInstanceOffset>& meshInstanceOffsets);
    uint32 addMeshLodGroup(const MeshLodGroup& group)
    {
        const uint8 rtLevel = (uint8)oc::clamp(m_rtParams.blasLodLevel, 0, (int)group.numLods - 1);
        for (uint8 k = 0; k < group.numLods; ++k)
            m_rt.accel().setMeshAlias(group.meshIdx[k], group.meshIdx[rtLevel]);
        return m_meshLods.addGroup(group);
    }
    void freeMeshLodGroup(uint32 groupIdx) { m_meshLods.freeGroup(groupIdx); }
    uint32 allocateLodStateRange(uint32 count) { const std::lock_guard lock(m_spawnMutex); return m_meshLods.allocateStateRange(count);  }

    void noteTextureUse(const RenderNode& node, uint32 passMask);
    void noteLodChainUse(const RenderNode& node, uint32 startIdx, InstanceStream::FrameSlot& instances);

    void waitForGpuAndFlushStaging();
    void onUniqueMeshCapacityGrown(uint32 maxUniqueMeshes); // SharedTable's onGrown hook: everything besides the mesh-info buffer that the capacity feeds.

    // -- Skinning resources -- 
    friend class AnimatorComponent;
    using SkinnedInstanceBundle = SkinnedMeshRegistry::Bundle;
    uint32 addSkinnedMeshSources(const oc::vector<RendererVKLayout::SkinnedMeshSource>& sources) { return m_skinned.addSources(sources); }
    const RendererVKLayout::SkinnedMeshSource& getSkinnedMeshSource(uint32 idx) const { return m_skinned.getSource(idx); }
    uint32 acquireSkinnedBundle(uint32 sourceKey) { const std::lock_guard lock(m_spawnMutex); return m_skinned.acquireBundle(sourceKey); }
    uint32 registerSkinnedBundle(const SkinnedInstanceBundle& bundle) { const std::lock_guard lock(m_spawnMutex); return m_skinned.registerBundle(bundle); }
    void releaseSkinnedBundle(uint32 bundleHandle) { const std::lock_guard lock(m_spawnMutex); m_skinned.parkBundle(bundleHandle); }
    void destroySkinnedBundle(uint32 bundleHandle);
    const SkinnedInstanceBundle& getSkinnedBundle(uint32 handle) const { return m_skinned.getBundle(handle); }
    uint32 allocateSkinningPalette(uint32 boneCount) { const std::lock_guard lock(m_spawnMutex); return m_skinned.allocatePalette(boneCount); }
    void setSkinningPalette(const RenderNode& node, oc::span<const glm::mat4> palette);
    uint32 allocateSkinningJobRange(uint32 count) { const std::lock_guard lock(m_spawnMutex); return m_skinned.allocateJobRange(count); }
    void setSkinnedInstance(uint32 jobIdx, uint32 baseVertexOffset, uint32 skinVertexOffset, uint32 outVertexOffset, uint32 vertexCount, uint32 paletteHandle, uint32 meshIdx, uint32 firstIndex, uint32 indexCount)
    {
        m_skinned.setInstance(jobIdx, baseVertexOffset, skinVertexOffset, outVertexOffset, vertexCount, paletteHandle, meshIdx, firstIndex, indexCount);
    }

private:

    Instance m_instance;
    Device m_device;
    Surface m_surface;
    SwapChain m_swapChain;
    GpuProfiler m_gpuProfiler;
    JobCounter m_gpuCollectCounter;    // the in-flight timestamp-collect job (beginFrame kicks -> recordCommandBuffers joins)
    Camera m_frameCamera;              // setFrameView; the begin-frame job reads these, so they only
    Rect m_frameRect;                  // change while no job is in flight.
    JobCounter m_beginFrameJobCounter;
    bool m_frameViewSet = false;       // setFrameView ran for this frame (cleared by present)
    bool m_beginFrameDeferred = false; // VR: kick stored, join runs beginFrame synchronously
    JobCounter m_gridJobCounter;       // Both grid jobs share it; what each measured lives in the object that owns that grid.
    bool m_gridBuildsKicked = false;
    void joinGridBuilds(uint32 frameIdx, PerFrameData& frameData);
    Camera m_lastCullCamera;           // VR one-frame-latent cull view (see setFrameView); written at the end of beginFrame, VR only.
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
    ShadowCullComputePipeline m_rainCullComputePipeline;
    ShadowMapGraphicsPipeline m_rainMapGraphicsPipeline;
    CompositePipeline m_compositePipeline;
    EyeAdaptationPipeline m_eyeAdaptationPipeline;
    RayTracingScene m_rt;
    GIProbePipeline m_giProbePipeline;
    DebugLinePipeline m_debugLinePipeline;
    ParticlePipeline m_particlePipeline;
    DecalPipeline m_decalPipeline;
    ForceFieldPipeline m_forceFieldPipeline;
    ParticleState m_particles;
    ForceFieldState m_force;
    BindlessTextures m_textures;
    PerWorker<oc::vector<DebugLinePipeline::LineVertex>> m_debugLineVerts; // per-worker CPU staging, drained into the mapped buffer in present()

    SkyParams m_skyParams;
    ShadowParams m_shadowParams;
    FogParams m_fogParams;
    TerrainResources m_terrain;
    PostParams m_postParams;
    RTParams m_rtParams;
    RTAOParams m_rtaoParams;
    LightGridParams m_lightGridParams;
    TAAParams m_taaParams;
    MeshLodParams m_lodParams;

    RendererVKLayout::Ubo m_ubo; // buildUboViews reprojects from last frame's mvps before overwriting them.

    glm::vec3 m_sceneFocus = glm::vec3(0.0f); // setSceneFocus
    bool m_sceneFocusEnabled = false;
    glm::vec3 m_cameraPos = glm::vec3(0.0f);
    glm::vec3 m_prevCameraPos = glm::vec3(0.0f);
    bool m_havePrevCameraPos = false;
    glm::vec3 m_giPrevFocusPos = glm::vec3(0.0f);
    float m_mipPixelScale = 0.0f; // viewportHeight / tan(fovY/2): projected diameter px = radius * scale / dist
    Clock::time_point m_eyeAdaptLastTime;
    bool m_haveEyeAdaptTime = false;
    glm::mat4 m_sunCascadeViewProj[RendererVKLayout::NUM_SHADOW_CASCADES];
    uint32 m_numSunCascades = 0;
    glm::mat4 m_centerViewProj = glm::mat4(1.0f);
    glm::ivec2 m_windowSize;
    const void* m_imguiDrawData = nullptr;
    oc::atomic<const void*> m_imguiPendingDrawData = nullptr;
    Rect m_viewportRect = Rect();
    bool m_initialized = false;
    bool m_frameSlotWaited = false;
    bool m_windowMinimized = false;
    bool m_vsyncEnabled = true;
    glm::vec2 m_prevTaaJitter{ 0.0f };
    
    uint32 m_sceneViewCount = 1; // 2 in VR: SceneColor + forward pass are multiview (one layer per eye)
    VrEyeTargets m_vrEyes;       // VR: the per-eye LDR composite targets

    uint32 m_meshDataGeneration = 0; // last seen MeshDataManager::getGeneration(); change -> re-record
    uint32 m_frameCounter = 0; // monotonic; rotates the GI probe ray/taa samples set each frame

    oc::vector<ObjectContainer*> m_objectContainers;
    // The Renderer keeps the parallel CPU side tables (m_rt, m_instances) and reacts to a growth.
    SharedTable<RendererVKLayout::MeshInfo> m_meshInfos;
    SharedTable<RendererVKLayout::MaterialInfo> m_materials;
    SharedTable<RendererVKLayout::MeshInstanceOffset> m_instanceOffsets;
    oc::unordered_map<uint32, uint16> m_solidColorMaterials; // packed RGB8 -> material idx (tint cache)

    SkinnedMeshRegistry m_skinned;  // the skinning jobs, bone palettes, sources and spawn bundles
    InstanceStream m_instances;     // the per-frame mapped push buffers + the render-node transform slots
    FrameSubmission m_submission;   // lights / fog volumes / decals, and the light grid's GPU scratch
    MeshLodRegistry m_meshLods;     // the chains, the per-mesh mapping and the GPU selection buffers
    oc::array<uint32, RendererVKLayout::MAX_MESH_LODS> m_lodInstanceCounts{}; // stats snapshot of the GPU cull's per-level picks

    // PARALLEL ENTITY SPAWNING: one coarse mutex over every allocator the spawn/despawn path reaches.
    // RECURSIVE because ObjectContainer::spawnNodeForIdx holds it across its whole body while calling
    // the locked leaves below. The parallel entity PASS never takes it and stays lock-free.
    std::recursive_mutex m_spawnMutex;

    struct PerFrameData
    {
        SceneColor sceneColor; // colour + scene depth (no prepass: every depth reader samples this one)
        ShadowMap shadowMap;
        ShadowMap rainOcclusionMap;

        oc::array<DescriptorSet, 2> staticMeshPipelineDescriptorSet; // Per-eye in VR
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
        CommandBuffer globalIllumCommandBuffer;
        CommandBuffer giPrepCommandBuffer;
        CommandBuffer volumetricFogCommandBuffer;
        CommandBuffer fogApplyCommandBuffer;
        CommandBuffer giProbeDebugCommandBuffer;
        CommandBuffer debugLineCommandBuffer;
        CommandBuffer particleSimCommandBuffer;
        CommandBuffer particleCommandBuffer;
        CommandBuffer decalCommandBuffer;
        CommandBuffer forceFieldCommandBuffer;
        CommandBuffer forceUnionCommandBuffer;
        CommandBuffer forceIntervalCommandBuffer;
        CommandBuffer forceMarchCommandBuffer;
        CommandBuffer forceComputeCommandBuffer;
        CommandBuffer taaCommandBuffer;
        CommandBuffer eyeAdaptCommandBuffer;
        CommandBuffer compositeCommandBuffer;

        bool updated = false;
        Buffer ubo;
        Buffer lodStatsBuffer;
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

static_assert(oc::size(ForceFieldParams{}.teamColors) == RendererVKLayout::MAX_FORCE_TEAMS, "ForceFieldParams::teamColors must cover MAX_FORCE_TEAMS (Settings.ixx doesn't import :Layout)");