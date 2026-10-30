export module RendererVK:RendererState;

import Core;
import Core.glm;
import Core.Rect;
import Core.Sphere;
import Core.Transform;
import Core.Camera;
import Core.VrSession;
import Threading;

import :Layout;
import :UboBlock;
import :PushBlock;
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
import :RainOcclusionPipeline;
import :OceanSimulationPipeline;
import :TerrainWetnessPipeline;
import :VolumetricFogPipeline;
import :CloudPipeline;
import :BakedWorldMap;
import :SceneColor;
import :DebugLinePipeline;
import :ParticlePipeline;
import :TreeVolumePipeline;
import :TreeRecordPool;
import :GrassPipeline;
import :ClutterPipeline;
import :DecalPipeline;
import :ForceFieldPipeline;
import :TaaPipeline;
import :DlssPipeline;
import :Streamline;
import :MotionBlurPipeline;
import :BloomPipeline;
import :CompositePipeline;
import :EyeAdaptationPipeline;
import :GraphicsPipeline;
import :Light;
import :GpuCrashTracker;
import :RenderParams;
import Settings.fwd; // NOT Settings: a settings default change must not rebuild every importer of :Renderer
import :RenderNode;
import :RenderMesh;
import :RenderMaterial;
import :RenderTexture;
import :RenderTreeSet;
import :RenderParticleEmitter;
import :RenderForceEmitter;
import :RenderForceQuery;
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
import :RendererTypes;

import Core.fwd;

export class Renderer;
export class MeshStreamer;     // declared before their friend lines
export class RenderMaterial;
export class RenderTexture;
export class RenderTreeSet;
export class RenderParticleEmitter;
export class RenderForceEmitter;
export class RenderForceQuery;
export enum class EValidation; // Renderer.ixx (opaque: a scoped enum's underlying type is int)
export enum class EVr;         // Renderer.ixx

// Everything of the Renderer but its public API (Renderer.ixx): the state and the private helpers. A helper reaches
// the API through self().
export class RendererState : public RendererTypes
{
protected:
    struct PerFrameData;

    RendererState(); // binds the settings references (RendererSettings.cpp)
    ~RendererState();
    RendererState(const RendererState&) = delete;
    RendererState(const RendererState&&) = delete;
    RendererState& operator=(const RendererState&) = delete;
    RendererState& operator=(const RendererState&&) = delete;

    Renderer& self(); // this, as the Renderer (the only class derived from this one)

    uint32 getCurrentFrameIndex() const { return m_swapChain.getCurrentFrameIndex(); }

	void beginFrame(); // through kickBeginFrameJob() and joinBeginFrameJob(): sets up the frame's per-frame data, cull, and UBOs.

    void recordCommandBuffers();       // the frame's work batch (everything but the swapchain pass); before the acquire
    void recordPresentCommandBuffer(); // the swapchain pass (composite + ImGui): the batch that waits for the image
    void recordSceneSecondaries(uint32 frameIdx);
    void recordPrimaryPreScene(uint32 frameIdx, vk::CommandBuffer primary);
    void recordPrimaryVR(uint32 frameIdx, CommandBuffer& primary);
    void recordPrimaryDesktop(uint32 frameIdx, vk::CommandBuffer primary);
    void executeScoped(vk::CommandBuffer primary, const char* scope, vk::CommandBuffer secondary);

    vk::CommandBuffer beginComputeSecondary(CommandBuffer& cb); // Each caller ends its own cb.
    // opaque = a depth-writing stage: inherits SceneColor's opaque family (+ the motion target).
    vk::CommandBuffer beginScenePassSecondary(uint32 frameIdx, CommandBuffer& cb, bool opaque = false);

    void recordSkinning(uint32 frameIdx);
    void recordOceanSim(uint32 frameIdx);
    void recordTerrainWetness(uint32 frameIdx);
    void recordGrassCull(uint32 frameIdx);
    void recordClutterCull(uint32 frameIdx);
    void recordGrassNearShadow(uint32 frameIdx); // the near grass cascade's casters (the blades + the clutter; the shadow map's extra-layer pass)
    void recordIndirectCull(uint32 frameIdx);
    void recordLightGrid(uint32 frameIdx);
    void recordShadowCull(uint32 frameIdx);
    void recordShadowDraw(uint32 frameIdx);
    void recordRainAndParticleSim(vk::CommandBuffer primary, uint32 frameIdx); // after the GI step, both primaries
    void recordStaticMesh(uint32 frameIdx);
    void recordStaticMeshInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordSceneOpaqueToSampled(vk::CommandBuffer cb, const SceneColor& sceneColor, uint32 eyeIndex);
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
    void recordClouds(uint32 frameIdx);
    void recordCloudsInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    void recordCloudApply(uint32 frameIdx);
    void recordCloudApplyInto(CommandBuffer& cb, uint32 frameIdx, uint32 eyeIndex);
    bool cloudsEnabled() const;
    // DLSS (desktop only) REPLACES TAA: it writes TAA's resolved image, so the post chain reads the same image.
    bool dlssActive() const { return m_dlssMode != Streamline::DlssMode::Off && m_sceneViewCount == 1 && Streamline::dlssAvailable(); }
    bool taaActive() const;
    bool resolveActive() const { return taaActive() || dlssActive(); } // false: the post chain reads the scene colour directly
    // The scene renders below the output resolution (a DLSS mode other than DLAA).
    bool upscaling() const { return m_renderScale != glm::vec2(1.0f); }
    // Desktop only (a blur the head did not make is uncomfortable in VR); off = the pass is not recorded at all.
    // Not while upscaling: its velocity and gather assume one resolution for the depth and the resolved colour.
    bool motionBlurEnabled() const;
    // Desktop only (level 0 comes from the left eye's histogram in VR); off = no chain, no level-0 writes.
    bool bloomEnabled() const;
    // The bloom's share in the composite mix (u_post_bloomKeep / bloomScale): 0 while bloom is off. Floored at the
    // tweak min (0.005).
    float bloomMixIntensity() const;
    // CloudParams toggles -> the baked shader defines (buildLayoutPreamble). Returns whether they changed, so a
    // strength slider that only crosses 0 at the ends (Powder) reloads the shaders only when its define flips.
    bool syncCloudDefines();
    void recordTaa(uint32 frameIdx);
    void recordDlss(uint32 frameIdx);                                  // cached: the motion vector pass
    void recordDlssEvaluate(uint32 frameIdx, vk::CommandBuffer primary); // per frame, in the primary: mvec pass + the upscale
    void recordMotionBlur(uint32 frameIdx);
    void recordBloom(uint32 frameIdx);
    void recordEyeAdaptation(uint32 frameIdx);
    void recordComposite(uint32 frameIdx);

    void setFullViewport(vk::CommandBuffer vkCb) const;
    void setViewportRect(const Rect& rect) { if (Globals::openXR.isEnabled()) return; if (rect.getSize().x <= 0 || rect.getSize().y <= 0) return; if (rect != m_viewportRect) { m_viewportRect = rect; updateRenderRect(); setHaveToRecordCommandBuffers(); } }
    void initBindlessTextures();

    // ---- Render resolution (DLSS) ----
    // The scene renders into RENDER-SIZE targets (SceneColor, RTAO, clouds, the force interval target, the DLSS
    // motion vectors) through m_renderRect = m_viewportRect x m_renderScale; everything after the resolve (TAA /
    // DLSS output, motion blur, eye adaptation, bloom, composite) stays at the swapchain size and m_viewportRect.
    // Without upscaling the two are identical.
    void updateRenderExtent();     // m_renderExtent + m_renderScale from the DLSS mode and the swapchain extent
    void updateRenderRect();       // m_renderRect from m_viewportRect; a change resets the DLSS history
    void recreateRenderTargets();  // GPU idle: every render-size image at m_renderExtent
    void applyRenderResolution();  // "Post/DLSS" Mode / Mip bias: GPU idle + the three above + the texture LOD bias
    float dlssMipBias() const;
    vk::Extent2D renderExtent() const { return vk::Extent2D{ m_renderExtent.x, m_renderExtent.y }; }

    // The focus without a set one (setSceneFocus): the camera's x / z at min(its height, the focus ground), or the camera itself without a focus ground.
    glm::vec3 defaultSceneFocus() const
    {
        return m_haveFocusGround ? glm::vec3(m_cameraPos.x, glm::min(m_cameraPos.y, m_focusGround), m_cameraPos.z) : m_cameraPos;
    }
    glm::mat4 computeCenterProjection(const Camera& camera) const; // pure, reversed-Z, unjittered (VR: both eyes)
    glm::mat4 computeCenterViewProj(const Camera& camera) const; // pure: projection (VR: both eyes) * view
    void applyVrHeadPose(const Camera& cameraIn, Camera& camera, glm::quat& vrBaseOrientation);
    void checkFrameCapacities();
    void snapshotLodStats(PerFrameData& frameData);
    void buildFrameUbo(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation, PerFrameData& frameData);
    void advanceSunCycle(); // "Sky/Sun cycle": the time of day on the sim clock -> the sun direction (main, kickBeginFrameJob)
    // The FRAME STATE the live UBO values read (buildFrameUbo runs these, then evaluates the block): what has a side
    // effect, carries over from the last frame, or comes out of one calculation with several results.
    void buildUboViews(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation);
    void buildUboWeather(const Camera& camera);
    void buildUboRayTracing();
    void buildUboClouds(const Camera& camera);
    void buildUboSunShadow(const Camera& camera);
    void buildUboForce();
    void buildUboTerrain();
    void buildUboGrass(const Camera& camera);

    struct ViewMatrices // one camera view: m_views (VIEW_CENTER, then the VR eyes)
    {
        glm::mat4 mvp{ 1.0f };
        glm::mat4 invMvp{ 1.0f };
        glm::mat4 prevMvp{ 1.0f };    // last frame's (the reprojection)
        glm::mat4 prevInvMvp{ 1.0f };
        glm::mat4 reprojClip{ 1.0f }; // prevMvp * inverse(mvp), fused in double
        glm::mat4 rayFromNdc{ 1.0f }; // the view ray from NDC (writeView): columns 0-2 the 3x3, column 3 xyz the view's forward
        glm::vec4 viewPos{ 0.0f };
    };
    oc::array<ViewMatrices, RendererVKLayout::NUM_UBO_VIEWS> m_views;
    glm::vec4 m_taaJitter{ 0.0f };          // xy = this frame's TAA jitter (NDC), zw = last frame's
    uint32 m_uboFrameIndex = 0;             // u_frameIndex: the frame counter at the build (the instance stamps read it)
    float m_frameTime = 0.0f;               // u_timeSeconds (sim)
    float m_prevFrameTime = 0.0f;           // last frame's (the grass and tree wind motion vectors)
    glm::vec3 m_cameraVelocity{ 0.0f };
    float m_giFullBake = 0.0f;              // takeVisibilityParams' one-frame request
    glm::vec3 m_giUboPrevFocus{ 0.0f };     // the focus GI traced from last
    glm::vec4 m_cascadeSunSizeTexels{ 0.0f };
    glm::vec4 m_cascadePlanes[RendererVKLayout::NUM_SHADOW_CASCADES * 6] = {}; // normalized, cascade c at [c * 6]
    glm::dvec2 m_cloudWindStep{ 0.0 };      // this frame's cloud field displacement
    glm::vec3 m_cloudShadowAxis0{ 1.0f, 0.0f, 0.0f };
    glm::vec3 m_cloudShadowAxis1{ 0.0f, 0.0f, 1.0f };
    bool m_cloudShadowRendered = false;
    struct GrassNearCascade
    {
        glm::mat4 viewProj{ 1.0f };
        glm::vec2 centre{ 0.0f };
        float range = 0.0f;                 // its half size (m; 0 = off)
    } m_grassNear;
    // The far grass stripes' ANCHOR (buildUboGrass; grass.inc.glsl): the point the pattern is built around - a stand-in
    // EYE, height included (the stripes' length comes from its view angle too) - in steps behind the camera, the old one
    // crossfading to the new.
    struct GrassStripeAnchor
    {
        glm::vec3 now{ 0.0f };
        glm::vec3 old{ 0.0f };
        glm::vec3 lastCamera{ 0.0f };
        float blend = 1.0f;        // 0 = all old .. 1 = all new
        bool valid = false;
    } m_grassStripeAnchor;
    // The canopy's sun FLECKS' frame (buildUboGrass; grass.inc.glsl grassCanopySun): the noise's origin (XZ, near the
    // camera), its streak axis and 1 / its stretch - HELD between steps, the old one crossfading to the new.
    struct GrassFleckFrame
    {
        glm::vec4 now{ 0.0f, 0.0f, 1.0f, 0.0f }; // xy = the origin, zw = the streak axis (unit XZ)
        glm::vec4 old{ 0.0f, 0.0f, 1.0f, 0.0f };
        float invStretchNow = 1.0f;
        float invStretchOld = 1.0f;
        float blend = 1.0f;        // 0 = all old .. 1 = all new
        bool valid = false;
    } m_grassFleckFrame;
    glm::vec3 m_forceBakeMin{ 0.0f };
    glm::vec3 m_forceBakeInvSize{ 0.0f };
    TerrainResources::WetnessTick m_wetTick{};
    // The rain occlusion map's top-down view, this frame's (m_rainOcclusion: the UBO, the map's switch and its trace).
    struct RainOcclusionView
    {
        glm::mat4 viewProj{ 1.0f };
        float invRange = 0.0f;
        bool present = false;
    };
    RainOcclusionView rainOcclusionView() const;
    RainOcclusionView m_rainOcclusion;

    // ---- THE FRAME UBO (UboBlock): every value one line with its sources, by subject (registerUboValues), and the tweak
    // locks that bake the lockable ones (RendererUboBake.cpp) ----
    UboBlock m_ubo;
    oc::vector<uint8> m_uboBakedValues;        // the values the compiled shaders hold (the block's layout)
    oc::vector<uint8> m_uboLocked;             // parallel to the entries: every source locked (resolveUboLocks)
    oc::vector<uint8> m_uboBaked;              // parallel: a const in the compiled shaders
    oc::array<uint32, RendererVKLayout::NUM_UBO_LOCK_SECTIONS> m_uboLocks{}; // each section's TweakLock
    bool m_uboResolveDirty = false; // a lock click: re-resolve the entries' locks
    bool m_uboLocksDirty = false;   // ... or a baked value may have changed: re-bake (applyUboLocks)
    float m_terrainCragScale = 1.0f; // setTerrainCragScale
    void registerUboLocks();   // + the bake every pipeline is first built with
    void registerUboValues(UboBlock& block); // every UBO value (live and lockable) by subject, RendererUbo.cpp
    // The "Ocean" settings the UBO's ocean values read (RendererSettings.cpp; Renderer::getOceanWorldScale is the API's).
    float getOceanSwashAmp() const;
    float getOceanBubbleDepth() const;
    float getOceanBubbleBrightness() const;
    void resolveUboLocks();
    bool bakeUboValues();      // true when a const changed
    void applyUboLocks();      // main, before the begin-frame build: only after a lock click or a bakeable change
    void setUboDeclaration();  // from m_uboBakedValues + m_uboBaked

    // The push blocks with LOCKABLE values (PushBlock), owned by their pipelines. onRebake (main, in applyUboLocks)
    // reloads the block's pipelines after a change; empty = the owner updates the block itself, at the moment its values
    // belong to (the far-tree volume, TreeVolumePipeline::prepare).
    struct PushBlockOwner
    {
        PushBlock* block;
        oc::function<void()> onRebake;
    };
    oc::vector<PushBlockOwner> m_pushBlocks;
    void registerPushFields(); // the blocks' lockable values, RendererUboBake.cpp

    // ---- THE scene stage table ----
    // recordSceneSecondaries, recordPrimaryDesktop and recordPrimaryVR all read it, so a stage is added, re-ordered or re-gated in exactly ONE place. Table order IS draw order.
    struct SceneStage
    {
        const char* name;
        bool opaque;        // runs with the depth-WRITING group, before the read-only switch
        bool enabled;       // this frame's gate: desktop executes it, VR records it inline
        bool gateRecording; // Debug lines only: its vertex buffers may not exist yet.
        CommandBuffer* cb;  // the cached secondary (desktop)
        void (RendererState::*recordCached)(uint32);                         // fills cb
        void (RendererState::*recordInline)(CommandBuffer&, uint32, uint32); // VR, per eye; null = desktop only
    };
    static constexpr uint32 NUM_SCENE_STAGES = 10;
    oc::array<SceneStage, NUM_SCENE_STAGES> buildSceneStages(uint32 frameIdx);

    void recreateVrEyeTargets();
    void recordGlobalIllumPrep(uint32 frameIdx); // per frame: one-shot BLAS builds, compaction, the skinned rebuild, the one-time clear
    void recordGlobalIllum(uint32 frameIdx);     // cached: sky map, TLAS instances + build, probe trace
    void setHaveToRecordCommandBuffers();
    void recreateSwapchain();
    void initImgui(Window& window);
    // The renderer's reactions to its settings (shader reloads, re-records, resizes) as Tweak::onChange listeners,
    // plus the hand-over of the baked state the pipelines are built with (RendererSettings.cpp). The settings
    // must be registered first (Settings::register*, from main).
    void attachSettingsListeners();
    bool initDeviceAndSwapchain(Window& window, EValidation validation, EVr vr);
    void initPipelines();
    void initPerFrameResources();
    void initSharedBuffers();

    friend class ObjectContainer;
    friend class ObjectContainerState; // its load steps register the meshes, materials and LOD groups
    void addObjectContainer(ObjectContainer* pObjectContainer);
    void removeObjectContainer(ObjectContainer* pObjectContainer);
    void freeMeshInfoRange(uint32 baseMeshInfoIdx, uint32 count);

    friend class RenderMesh;
    void destroyMesh(RenderMesh& mesh);

    // The material and texture handles' creates, frees and flags (RenderMaterial / RenderTexture: their API).
    friend class RenderMaterial;
    friend class RenderTexture;
    uint16 addMeshMaterial(RendererVKLayout::EPipelineIndex pipeline, bool rayTraced);
    uint16 addTextureMaterial(const RenderMaterial::TextureDesc& desc);
    uint16 addDerivedMaterial(uint16 source, uint32 extraFlags);
    uint16 getOrCreateSolidColorMaterial(const glm::vec3& color);
    uint16 addTexture(uint32 width, uint32 height, const oc::vector<oc::span<uint8>>& mips, const char* debugName,
                      RenderMaterial::ETextureEncoding encoding);
    void destroyTextureMaterial(uint16 materialIdx); // the slot and its textures
    void releaseMaterial(uint16 materialIdx);        // the slot only: a mesh or derived material
    uint32 getMaterialFlags(uint16 materialIdx);
    void setMaterialFlags(uint16 materialIdx, uint32 flags);
    void releaseTexture(uint16 texIdx);
    uint32 m_identityInstanceOffsetIdx = UINT32_MAX; // spawnMeshNode's shared identity offset (first spawn)

    friend class RenderNode;
    uint32 addRenderNodeTransform(const Transform& transform);
    inline Transform& getRenderNodeTransform(uint32 idx) { return m_instances.getTransform(idx); }
    void freeRenderNode(RenderNode& node);

    // MESH STREAMING (MeshStreamer): a streamed-out mesh keeps its bounds but draws zero indices, so the cull's DGC draws, the shadow pass and the
    // TLAS-instance writer all no-op for it.
    friend class MeshStreamer;
    void setMeshStreamedOut(uint16 meshInfoIdx);
    void setMeshStreamedIn(uint16 meshInfoIdx, int32 vertexOffset, uint32 firstIndex, uint32 indexCount);

    uint32 addMeshInfos(const oc::vector<RendererVKLayout::MeshInfo>& meshInfos, oc::span<const uint32> vertexCounts, bool skinnedOutputs = false,
        bool raytraced = true);
    uint16 getRtMeshAlias(uint16 meshIdx) const { return (uint16)m_rt.accel().getMeshAlias(meshIdx); }
    uint32 addMaterialInfos(const oc::vector<RendererVKLayout::MaterialInfo>& materialInfos);
    uint32 addMeshInstanceOffsets(const oc::vector<RendererVKLayout::MeshInstanceOffset>& meshInstanceOffsets);
    uint32 addMeshLodGroup(const MeshLodGroup& group);
    void freeMeshLodGroup(uint32 groupIdx) { const std::lock_guard lock(m_spawnMutex); m_meshLods.freeGroup(groupIdx); } // see freeMeshInfoRange
    uint32 allocateLodStateRange(uint32 count) { return m_meshLods.allocateStateRange(count); } // lock-free

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
    void releaseSkinnedBundle(uint32 bundleHandle) { m_skinned.queuePark(bundleHandle); } // lock-free; parked at beginFrame
    void destroySkinnedBundle(uint32 bundleHandle);
    const SkinnedInstanceBundle& getSkinnedBundle(uint32 handle) const { return m_skinned.getBundle(handle); }
    uint32 allocateSkinningPalette(uint32 boneCount) { const std::lock_guard lock(m_spawnMutex); return m_skinned.allocatePalette(boneCount); }
    void setSkinningPalette(const RenderNode& node, oc::span<const glm::mat4> palette);
    uint32 allocateSkinningJobRange(uint32 count) { const std::lock_guard lock(m_spawnMutex); return m_skinned.allocateJobRange(count); }
    void setSkinnedInstance(uint32 jobIdx, uint32 baseVertexOffset, uint32 skinVertexOffset, uint32 outVertexOffset, uint32 vertexCount, uint32 paletteHandle, uint32 meshIdx, uint32 firstIndex, uint32 indexCount)
    {
        m_skinned.setInstance(jobIdx, baseVertexOffset, skinVertexOffset, outVertexOffset, vertexCount, paletteHandle, meshIdx, firstIndex, indexCount);
    }

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
    oc::string m_screenshotPath;       // requestScreenshot: written by the next present
    void captureScreenshot(const oc::string& path);
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
    CloudPipeline m_cloudPipeline;
    TaaPipeline m_taaPipeline;
    DlssPipeline m_dlssPipeline;
    MotionBlurPipeline m_motionBlurPipeline;
    BloomPipeline m_bloomPipeline;
    ShadowCullComputePipeline m_shadowCullComputePipeline;
    ShadowMapGraphicsPipeline m_shadowMapGraphicsPipeline;
    RainOcclusionPipeline m_rainOcclusionPipeline;
    CompositePipeline m_compositePipeline;
    EyeAdaptationPipeline m_eyeAdaptationPipeline;
    RayTracingScene m_rt;
    GIProbePipeline m_giProbePipeline;
    DebugLinePipeline m_debugLinePipeline;
    ParticlePipeline m_particlePipeline;
    // BAKED TREE RECORDS (RendererTrees.cpp, tree_cull.inc.glsl): the live sets (dead ones recycled by id). The culls
    // bind ONE set's static buffers (m_treeCullSet, bindTreeSet; a change re-records them) and build the
    // records of this frame's LISTED pieces (the drawn chunks' pieces, each with its pass bits) from the camera
    // distance in the range renderTreeSet claims (m_treeCullBase / Count, reset per frame in beginFrame,
    // into the UBO's u_present_tree*).
    friend class RenderTreeSet;
    using TreeInstanceRep = RenderTreeSet::Rep;
    using TreeInstanceType = RenderTreeSet::Type;
    using TreeInstancePiece = RenderTreeSet::Piece;
    using TreeInstanceChunk = RenderTreeSet::Range;
    using TreeChunkDraw = RenderTreeSet::ChunkDraw;
    struct TreeInstanceSet
    {
        Buffer pieces;  // TreeCullPieceGpu per piece slot (device-local)
        Buffer types;   // TreeCullTypeGpu per type (device-local)
        uint32 numPieces = 0; // the piece slots up to the highest one in use (the volume's splat range)
        // One CHUNK: its piece range (count 0 = a dead or empty chunk), with its RT-capable pieces (an RT representation
        // with a BLAS - tree_cull.inc.glsl treeCullRtPiece) FIRST, rtCount of them inside the sphere rtCentre / rtRadius,
        // and its bucket sizes (level-0 mesh, instances).
        struct Chunk
        {
            uint32 first = 0;
            uint32 count = 0;
            uint32 rtCount = 0;
            glm::vec3 rtCentre{ 0.0f };
            float rtRadius = 0.0f;
            oc::vector<oc::pair<uint16, uint32>> meshCounts;
        };
        oc::vector<Chunk> chunks;
        // Per type: whether its RT representation has a BLAS, and the meshes a piece of it draws (one per representation
        // pair - a piece draws the normal OR the fade material of a mesh) for the bucket sizes.
        oc::vector<uint8> typeRtCapable;
        oc::vector<oc::small_vector<uint16, 5>> typeMeshes;
        // A DYNAMIC set (createDynamicTreeSet): a fixed capacity of piece slots in blocks of TREE_SET_BLOCK, a
        // removed chunk's blocks reused NUM_FRAMES_IN_FLIGHT frames later, its index the frame after.
        bool dynamic = false;
        uint32 numBlocks = 0;
        uint32 topBlock = 0;
        IndexRangeFreeList freeBlocks;
        struct PendingFree { uint32 firstBlock = 0; uint32 blocks = 0; uint32 readyFrame = 0; };
        oc::vector<PendingFree> pendingFrees;
        oc::vector<uint32> freeChunks;
        oc::vector<oc::pair<uint32, uint32>> pendingChunks; // (index, the frame it was removed in)
        uint32 emptyVolumeType = 0;                         // the volume's sentinel type (res 0) for free slots
        oc::span<TreeVolumePieceGpu> mappedVolumePieces;
        // This frame's list (piece | passMask << 28 per listed piece): host-visible, one per frame slot.
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> lists;
        oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> mappedLists;
        Buffer volumePieces; // TreeVolumePieceGpu per piece slot (the far-tree volume's bake; host-visible)
        Buffer volumeTypes;  // TreeVolumeTypeGpu per type, + the sentinel
        Buffer volumeData;   // the types' extinction mip chains (floats)
        bool hasVolume = false;
        bool alive = false;
    };
    static constexpr uint32 TREE_SET_BLOCK = 64; // a dynamic set's piece-slot allocation unit
    oc::vector<TreeInstanceSet> m_treeSets;
    // A dynamic set's chunks change on MAIN (add / remove) while the terrain walk lists them on a worker
    // (renderTreeSet).
    std::mutex m_treeSetMutex;
    bool m_treeSetsChanged = false;    // a dynamic set changed: the far volume re-bakes (throttled, recordFarTrees)
    uint32 m_treeVolumeMarkFrame = 0;
    uint32 allocTreeSet();
    void initTreeSetTypes(TreeInstanceSet& set, oc::span<const TreeInstanceType> types);
    void initTreeSetPieces(TreeInstanceSet& set, uint32 capacity);
    // Writes a chunk's pieces (RT-capable first) into the set's slots [first, first + pieces.size()).
    void writeTreeChunk(TreeInstanceSet& set, TreeInstanceSet::Chunk& chunk, oc::span<const TreeInstancePiece> pieces, uint32 first);
    // The tree set handle's calls (RenderTreeSet: their API).
    uint32 createTreeSet(oc::span<const TreeInstanceType> types, oc::span<const TreeInstancePiece> pieces, oc::span<const TreeInstanceChunk> chunks);
    uint32 createDynamicTreeSet(oc::span<const TreeInstanceType> types, uint32 pieceCapacity);
    uint32 addTreeSetChunk(uint32 setId, oc::span<const TreeInstancePiece> pieces);
    void removeTreeSetChunk(uint32 setId, uint32 index);
    void destroyTreeSet(uint32 setId);
    void bindTreeSet(uint32 setId);
    void renderTreeSet(uint32 setId, oc::span<const TreeChunkDraw> chunks, float distanceScale, bool forceFar);
    Buffer m_treeCullDummy;             // bound to both tree bindings while no set is
    uint32 m_treeCullSet = UINT32_MAX;   // the set whose buffers the recorded culls bind
    uint32 m_treeCullBase = 0;           // this frame's claimed range (0 / 0 = none)
    uint32 m_treeCullCount = 0;
    uint32 m_treeCullPieces = 0;         // the trees in it (TREE_RECORDS_PER_PIECE records each)
    uint32 m_treeCullRtPieces = 0;       // the list's first ones: the RT-capable trees of the GI / shadow chunks in RT range (TLAS slots)
    uint32 m_giTlasDemand = 0;           // last present()'s TLAS slot count before the capacity clamp (the growth check)
    oc::atomic<bool> m_treeCullTaken{ false }; // renderTreeSet ran this frame (any thread; reset in beginFrame)
    float m_treeCullDistanceScale = 1.0f;
    bool m_treeCullForceFar = false;
    Buffer& treeCullPieces();
    Buffer& treeCullTypes();
    Buffer& treeCullList(uint32 frameIdx);
    // The present-time values (u_present_*, UboPresent): the culls' thread count, the far-tree volume's start, the TLAS
    // writer's live count. RendererTrees.cpp / Renderer.cpp.
    uint32 treeCullThreads() const;
    float treeCullVolumeStart() const;
    uint32 giTlasLiveCount() const;
    TreeVolumePipeline m_treeVolume;
    TreeRecordPool m_treeRecords;
    float m_treeRecordChunkSize = 256.0f;
    uint32 m_treeRecordSeed = 1;
    uint32 m_treeRecordSet = UINT32_MAX; // the set whose volume types the record types' variants name
    FarTreeParams& m_farTreeParams;
    bool farTreesActive() const; // enabled, desktop, and a tree set with volume data
    float farTreesStart() const; // "Far start" scaled with the camera's height (the hand-over + the march's start)
    // The far-tree volume's bake when due, then `march` with this frame's record params (the march's phases run in the
    // post-scene compute group). Straight into the primary.
    void recordFarTrees(uint32 frameIdx, vk::CommandBuffer primary, const oc::function<void(const TreeVolumePipeline::RecordParams*)>& march);
    // THE POST-SCENE COMPUTE GROUP (desktop): RTAO, the cloud march and the far-tree march - each reads this frame's
    // depth, none another's output - phase by phase, interleaved, with ONE barrier between phases instead of each pass's
    // own ("Renderer/Overlap compute"; off: one pass after the other, each with its own GPU scope).
    void recordPostSceneCompute(uint32 frameIdx, vk::CommandBuffer primary);
    void recordFarTreesApply(uint32 frameIdx);                        // the scene stage's cached secondary
    // PROCEDURAL GRASS: the patch cull + buffers (the blades draw in m_staticMeshGraphicsPipeline). The ground chunks
    // arrive per frame (setGrassGround) and go into the slot's ground table in present (uploadGrassFrame).
    GrassPipeline m_grassPipeline;
    GrassParams& m_grassParams;
    RockParams& m_rockParams; // the rock material (EPipelineIndex::LitRock): UBO-driven, "Rocks/Material"
    oc::vector<GrassGroundChunk> m_grassGround;
    float m_grassChunkSize = 0.0f;
    uint32 m_grassGroundStride = 1; // setGrassGround: the terrain node meshes' row stride (vertices)
    float m_cameraGround = std::numeric_limits<float>::quiet_NaN(); // setCameraGround: the near grass cascade and the far-tree volume (NaN: the baked sea level)
    bool grassActive() const; // desktop only
    float grassRange() const; // m; 0 = no grass
    float grassPatchSize() const; // the patch grid (uploadGrassFrame) and u_grass_patchSize
    float grassGridRange() const; // capped so the grid fits GRASS_MAX_PATCHES: the grid and u_grass_range
    // The near grass cascade is drawn (and read): grass, its toggle, and the PCSS sun (RT sun shadows skip the shadow
    // map - the cascades' pass that this layer must follow).
    bool grassNearShadowActive() const;
    void uploadGrassFrame(uint32 frameIdx);
    bool m_groundTableValid = false; // this frame's ground table holds chunks (uploadGrassFrame): the clutter's ground
    // GROUND CLUTTER: the cull + buffers (the objects draw in m_staticMeshGraphicsPipeline). The patch grid and the
    // floor map go into the slot's clutter frame in present (uploadClutterFrame).
    ClutterPipeline m_clutterPipeline;
    ClutterSettings& m_clutterParams;
    float m_clutterMaxRange = 0.0f;                    // the types' largest `Range` (setClutterAssets)
    oc::vector<uint32> m_clutterFloor;                 // the floor map's texels (setClutterFloorMap)
    glm::vec2 m_clutterFloorCentre{ 0.0f };
    oc::array<bool, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_clutterFloorDirty{}; // the slot has not taken the map yet
    // The inland water map (setRiverWaterMap): the CPU copy, and per frame slot its host-visible buffer (fog binding 14).
    oc::vector<float> m_riverWater;
    glm::vec2 m_riverWaterOrigin{ 0.0f };
    oc::array<bool, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_riverWaterDirty{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_riverWaterMaps;
    oc::array<RendererVKLayout::RiverWaterMapGpu*, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedRiverWaterMaps{};
    void uploadRiverWaterMap(uint32 frameIdx);
    // The river mist sources (setRiverMistSources): the CPU copy, and per frame slot its host-visible buffer (the
    // particle sim pass's mist producer).
    oc::vector<RendererVKLayout::RiverMistSourceGpu> m_riverMist;
    oc::array<bool, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_riverMistDirty{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_riverMistBuffers;
    oc::array<RendererVKLayout::RiverMistSourcesGpu*, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedRiverMist{};
    uint32 m_riverMistEmitter = UINT32_MAX;
    void uploadRiverMist(uint32 frameIdx);
    bool clutterActive() const; // desktop only
    float clutterRange() const; // m: the farthest type's range x "Range scale", capped by the patch grid; 0 = no clutter
    float clutterPatchSize() const;
    void uploadClutterFrame(uint32 frameIdx);
    DecalPipeline m_decalPipeline;
    ForceFieldPipeline m_forceFieldPipeline;
    ParticleState m_particles;
    ForceFieldState m_force;
    // The emitter and query handles' calls (RenderParticleEmitter / RenderForceEmitter / RenderForceQuery: their API). The slot tables are lock-free
    // (parallel entity spawning); the slot contracts (the KILL flag drain, retirement) live in ParticleState and ForceFieldState.
    friend class RenderParticleEmitter;
    friend class RenderForceEmitter;
    friend class RenderForceQuery;
    glm::vec4 getForceEmitterReadback(uint32 slot) const;
    RendererVKLayout::ForceQueryResult getForceQueryReadback(uint32 slot) const;
    BindlessTextures m_textures;
    PerWorker<oc::vector<DebugLinePipeline::LineVertex>> m_debugLineVerts; // per-worker CPU staging, drained into the mapped buffer in present()

    // The settings the renderer reads (Globals::settings: registered by Settings, written by the TweakPanel and the
    // setters of the API). References, so the many reads stay short; nothing here holds a copy. Bound in the
    // constructor (RendererSettings.cpp): this interface sees only Settings.fwd.
    SkyParams& m_skyParams;
    WindParams& m_windParams;
    ShadowParams& m_shadowParams;
    FoliageParams& m_foliageParams;
    FogParams& m_fogParams;
    CloudParams& m_cloudParams;
    bool m_cloudsSuppressed = false;
    glm::dvec2 m_cloudWindOffset = glm::dvec2(0.0); // accumulated wind (m), wrapped by the weather period
    double m_cloudEvolveOffset = 0.0;              // accumulated detail drift (m), wrapped by the detail period
    // The cloud shadow map cascades (buildUboClouds): world-space centres + extents of what each layer holds.
    oc::array<glm::dvec3, CloudPipeline::SHADOW_CASCADES> m_cloudShadowCenter{};
    oc::array<double, CloudPipeline::SHADOW_CASCADES> m_cloudShadowExtent{};
    glm::dvec3 m_cloudShadowSun = glm::dvec3(0.0);
    oc::array<bool, CloudPipeline::SHADOW_CASCADES> m_cloudShadowValid{};    // the centre is set (else: re-centre + full)
    oc::array<uint32, CloudPipeline::SHADOW_CASCADES> m_cloudShadowSplit{};  // THIS frame: 0 = all texels, 1 = 1/4, 2 = 1/16
    oc::array<uint32, CloudPipeline::SHADOW_CASCADES> m_cloudShadowPhase{};  // THIS frame's texel of the 2x2 / 4x4 pattern
    uint32 m_cloudShadowMask = 0; // the cascades the primary renders this frame (bit per cascade)
    TerrainResources m_terrain;
    glm::vec4 m_terrainStitch{ 0.0f };      // setTerrainStitch: x = chunk size (0 = off), yz = the draw camera in chunks
    glm::vec4 m_terrainStitchBands{ 0.0f }; // x = full-res distance, y = LOD step, z = max LOD (chunks)
    float m_riverNearCovered = 0.0f;        // setRiverNearCovered
    PostParams& m_postParams;
    RTParams& m_rtParams;
    RTAOParams& m_rtaoParams;
    LightGridParams& m_lightGridParams;
    TAAParams& m_taaParams;
    DlssParams& m_dlssParams;
    MotionBlurParams& m_motionBlurParams;
    BloomParams& m_bloomParams;
    MeshLodParams& m_lodParams;

    Frustum m_centerFrustum;     // the centre view's (buildUboViews); VR's spatial cull takes last frame's

    glm::vec3 m_sceneFocus = glm::vec3(0.0f); // setSceneFocus
    bool m_sceneFocusEnabled = false;
    float m_focusGround = 0.0f;               // setFocusGround: the surface height under the camera
    bool m_haveFocusGround = false;
    glm::vec3 m_frozenSceneFocus = glm::vec3(0.0f); // "Editor/Freeze scene focus": the focus when it was switched on
    bool m_sceneFocusFrozen = false;
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
    // Render resolution (see updateRenderExtent): the render-size targets, their scale from the swapchain, and
    // the scene's rect in them.
    glm::uvec2 m_renderExtent{ 0 };
    glm::vec2 m_renderScale{ 1.0f };
    Rect m_renderRect = Rect();
    // The APPLIED "Post/DLSS" Mode: set only by updateRenderExtent, together with the sizes it implies. The tweak
    // itself changes the moment the UI writes it, a frame or more before its onChange re-creates the targets and
    // re-records - reading it live renders a frame with the new mode and the old sizes (NGX InvalidParameter), or
    // executes the never-recorded DLSS secondary (device lost).
    Streamline::DlssMode m_dlssMode = Streamline::DlssMode::Off;
    bool m_dlssReset = true; // the next DLSS evaluate starts without history
    bool m_dlssFailed = false; // logged once
    bool m_initialized = false;
    bool m_frameSlotWaited = false;
    bool m_windowMinimized = false;
    bool m_resizeHold = false;      // setResizeHold
    bool m_swapchainStale = false;  // out of date during a hold: rebuild when it ends
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
    oc::unordered_map<uint32, uint16> m_solidColorMaterials; // packed RGB8 -> material idx (tint cache; m_solidColorMutex)
    std::mutex m_solidColorMutex;                            // a NEW colour only (getOrCreateSolidColorMaterial)
    // Its LOCK-FREE read side: an open-addressed mirror, (key + 1) << 16 | material idx per entry, 0 = empty.
    // Written only under m_solidColorMutex after the map; a lookup that misses it takes the locked path.
    static constexpr uint32 SOLID_COLOR_TABLE_SIZE = 1024;
    oc::array<oc::atomic<uint64>, SOLID_COLOR_TABLE_SIZE> m_solidColorTable;

    SkinnedMeshRegistry m_skinned;  // the skinning jobs, bone palettes, sources and spawn bundles
    InstanceStream m_instances;     // the per-frame mapped push buffers + the render-node transform slots
    FrameSubmission m_submission;   // lights / fog volumes / decals, and the light grid's GPU scratch
    MeshLodRegistry m_meshLods;     // the chains, the per-mesh mapping and the GPU selection buffers
    oc::array<uint32, RendererVKLayout::MAX_MESH_LODS> m_lodInstanceCounts{}; // stats snapshot of the GPU cull's per-level picks

    // PARALLEL ENTITY SPAWNING: one coarse mutex over the registries the spawn/despawn path reaches that are
    // not lock-free (the shared tables, LOD groups, skinned bundles, the solid-colour cache's miss). The hot
    // per-entity allocators are lock-free instead: transform slots, LOD state ranges, emitter / query slots
    // (SlotAlloc). RECURSIVE because spawnSkinnedNode / the rebased-offset fill hold it while calling the
    // locked leaves. The parallel entity PASS never takes it.
    std::recursive_mutex m_spawnMutex;

    struct PerFrameData
    {
        SceneColor sceneColor; // colour + scene depth (no prepass: every depth reader samples this one)
        ShadowMap shadowMap;
        ShadowMap grassNearShadowMap; // the near grass cascade (one layer; GrassPipeline + ClutterPipeline cast into it)

        oc::array<DescriptorSet, 2> staticMeshPipelineDescriptorSet; // Per-eye in VR
        DescriptorSet compositeDescriptorSet;
        DescriptorSet indirectCullPipelineDescriptorSet;
        DescriptorSet skinningDescriptorSet;
        DescriptorSet lightGridPipelineDescriptorSet;
        DescriptorSet shadowCullDescriptorSet;
        DescriptorSet shadowDrawDescriptorSet;

        CommandBuffer primaryCommandBuffer; // the frame's work: its own batch, submitted before the acquire
        CommandBuffer presentCommandBuffer; // the swapchain pass alone: the batch that waits for the acquired image
        CommandBuffer staticMeshCommandBuffer;
        // One cached secondary per PHASE (desktop): the post-scene compute group interleaves them with the other passes'
        // (recordPostSceneCompute). RTAO: the trace, the temporal pass, the blur; the clouds: the march, the temporal pass.
        oc::array<CommandBuffer, 3> aoCommandBuffers;
        CommandBuffer indirectCullCommandBuffer;
        CommandBuffer skinningCommandBuffer;
        CommandBuffer oceanSimCommandBuffer;
        CommandBuffer terrainWetnessCommandBuffer;
        CommandBuffer grassCullCommandBuffer;
        CommandBuffer clutterCullCommandBuffer;
        CommandBuffer grassNearShadowCommandBuffer;
        CommandBuffer lightGridCommandBuffer;
        CommandBuffer imguiCommandBuffer;
        CommandBuffer shadowCullCommandBuffer;
        CommandBuffer shadowDrawCommandBuffer;
        CommandBuffer globalIllumCommandBuffer;
        CommandBuffer giPrepCommandBuffer;
        CommandBuffer volumetricFogCommandBuffer;
        CommandBuffer fogApplyCommandBuffer;
        oc::array<CommandBuffer, 2> cloudCommandBuffers;
        CommandBuffer cloudApplyCommandBuffer;
        CommandBuffer farTreesApplyCommandBuffer;
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
        CommandBuffer dlssCommandBuffer;
        CommandBuffer motionBlurCommandBuffer;
        CommandBuffer bloomCommandBuffer;
        CommandBuffer eyeAdaptCommandBuffer;
        CommandBuffer compositeCommandBuffer;

        bool updated = false;
        Buffer ubo;
        Buffer lodStatsBuffer;
        oc::span<uint32> mappedLodStats;
    };
    oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
};
