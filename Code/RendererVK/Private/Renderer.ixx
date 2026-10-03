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
import :CloudPipeline;
import :BakedWorldMap;
import :SceneColor;
import :DebugLinePipeline;
import :ParticlePipeline;
import :TreeVolumePipeline;
import :GrassPipeline;
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
import :Settings;
import :RenderNode;
import :RenderMesh;
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

    // Skips an empty or destroyed node and a 0 mask. The one-argument form pushes the node's own mask.
    void renderNode(const RenderNode& node, uint32 passMask);                                          // [Concurrency: LOCK-FREE]
    void renderNode(const RenderNode& node) { renderNode(node, node.m_passMask); }                     // [Concurrency: LOCK-FREE]
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
    // The Memory panel's VRAM view: every live GPU allocation by debug name, plus the heap totals.
    void forEachGpuAllocation(GpuAllocationVisit visit, void* ctx) const { Globals::gpuAllocator.forEachAllocation(visit, ctx); }
    GpuAllocator::MemoryUsage getGpuMemoryUsage() const { return Globals::gpuAllocator.getMemoryUsage(); }

    // -- GPU particles + projected decals (driven by the Particle library) --
    uint32 createParticleEmitter(const RendererVKLayout::ParticleEmitterGpu& desc); // [Concurrency: LOCKING]
    void updateParticleEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc);
    void emitParticles(uint32 slot, uint32 count);
    void destroyParticleEmitter(uint32 slot); // [Concurrency: LOCKING]
    void resetParticles() { m_particles.requestReset(); }
    void setRainOcclusionVolume(const glm::vec3& center, const glm::vec3& halfExtents);
    void setOceanSprayEmitter(uint32 slot) { m_oceanSimPipeline.setSprayEmitter(slot); }
    // "Ocean/World scale" (s): the spray emitter's lengths / speeds / accelerations ride it.
    float getOceanWorldScale() const { return glm::max(m_oceanSimPipeline.getOceanParams().worldScale, 0.001f); }
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
    // The game turns the volumetric clouds off without touching the user's "Sky/Clouds/Enabled" tweak.
    void setCloudsSuppressed(bool suppressed) { m_cloudsSuppressed = suppressed; }

	// -- Terrain parameters --
    void setTerrainParams(float meshRadius, float seaLevel, float temperatureLapseRate = 0.0f) { m_terrain.setParams(meshRadius, temperatureLapseRate, seaLevel); }
    float getTerrainMeshRadius() const { return m_terrain.getMeshRadius(); }
    using TerrainSplatMaterial = ::TerrainSplatMaterial;
    using TerrainSplatCounts = ::TerrainSplatCounts;
    using TerrainTexTweaks = ::TerrainTexTweaks;
    using TerrainWetTweaks = ::TerrainWetTweaks;
    void setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts); // See TerrainStreamer::registerTerrainTextures for docs
    // Flipping parallaxEnabled / tessEnabled rebakes (GPU idle + shader reload + re-record): see Renderer.cpp.
    void setTerrainTextureParams(const TerrainTexTweaks& params);
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

    // -- Single meshes (no ObjectContainer; see RenderMesh) -- MAIN THREAD.
    // A material for createMesh'd geometry: the fallback textures, opaque, onto `pipeline` (Ocean /
    // TerrainLit add their material flag, like an ObjectContainer override). Permanent: create one per
    // system and share it between its meshes.
    uint16 createMeshMaterial(RendererVKLayout::EPipelineIndex pipeline, bool rayTraced);
    // A material with its own generated sRGB RGBA8 diffuse, from a caller-built mip chain (level 0 first,
    // each level half the previous). alphaCutoff > 0 makes it alpha-tested (EAlphaMode::Mask, the cutoff
    // rides `opacity`): draw it on LitMasked. `normalMips` (optional, same size): a LINEAR RGB tangent-space
    // normal map (x along U, y along V, z out). Free with destroyTextureMaterial (textures + material slot).
    // `extraFlags`: RendererVKLayout::MATERIAL_FLAG_* to add (e.g. MATERIAL_FLAG_BILLBOARD).
    uint16 createTextureMaterial(uint32 width, uint32 height, const oc::vector<oc::span<uint8>>& mips, float alphaCutoff, const char* debugName,
        const oc::vector<oc::span<uint8>>* normalMips = nullptr, uint32 extraFlags = 0);
    void destroyTextureMaterial(uint16 materialIdx);
    // A copy of `source` with `extraFlags` added, SHARING its textures: free it with releaseMaterial (the slot
    // only), never destroyTextureMaterial.
    uint16 deriveMaterial(uint16 source, uint32 extraFlags);
    void releaseMaterial(uint16 materialIdx);
    // Rewrites a material's flags in place (e.g. a distance-fade band). No re-record: a contents upload.
    uint32 getMaterialFlags(uint16 materialIdx);
    void setMaterialFlags(uint16 materialIdx, uint32 flags);
    // Uploads + one MeshInfo (and its BLAS); invalid when empty. `raytraced` false: NO BLAS - any instance of it is
    // inactive in the TLAS (no RT shadows / GI / RTAO / reflections hit it); for meshes RT never needs to see.
    RenderMesh createMesh(const RenderMeshData& data, bool raytraced = true);
    // A GPU LOD chain over createMesh'd meshes (level 0 first, at most MAX_MESH_LODS), `errors` = each level's
    // geometric deviation in mesh-local units (0 for level 0; the screen-space-error selector). Spawn nodes on
    // LEVEL 0: the cull picks the level per instance. freeMeshLodChain BEFORE destroying the meshes.
    uint32 createMeshLodChain(oc::span<const RenderMesh* const> levels, oc::span<const float> errors);
    void freeMeshLodChain(uint32 groupIdx) { freeMeshLodGroup(groupIdx); }
    // One node drawing `mesh` with `materialIdx` on `pipeline`, placed by `transform` (a shared identity
    // instance offset: the mesh is its own root).
    RenderNode spawnMeshNode(const RenderMesh& mesh, uint16 materialIdx, RendererVKLayout::EPipelineIndex pipeline, const Transform& transform);

    // -- Procedural tree pieces, BAKED on the GPU (tree_cull.inc.glsl; RendererTrees.cpp) -- MAIN THREAD.
    // A SET is a grove of placed pieces sharing a table of piece types, uploaded once to device-local memory. Per
    // frame, renderTreeInstanceSet costs one instance claim and one count per distinct mesh - nothing per piece on
    // the CPU, and no pass of its own on the GPU: the culls build the records. ONE set draws per frame.
    struct TreeInstanceRep // one representation; mesh nullptr = none. Spawned-on mesh = a chain's LEVEL 0.
    {
        const RenderMesh* mesh = nullptr;
        uint16 material = 0;
        RendererVKLayout::EPipelineIndex pipeline = RendererVKLayout::EPipelineIndex::LitOpaque;
    };
    struct TreeInstanceType
    {
        // barkFade / leavesFade: the same meshes on LitMasked with fade-OUT materials (the crossfade band). With a
        // mid tier, both fade out over the MID band instead (the card mesh takes over from them there).
        TreeInstanceRep bark, barkFade, leaves, leavesFade, billboard;
        float farDistance = 0.0f; // billboard switch distance (m); 0 = always the mesh
        float fadeWidth = 1.0f;   // crossfade band (m), centred on farDistance
        // The MID tier (main pass only; shadows keep the billboard), from midDistance (0 = none) over a band of
        // midFadeWidth: `bark` is then the BRANCH bark only and gives way with the leaves to the CARD mesh (cardsIn:
        // fades in over the mid band; cardsOut: out over the far band); `trunk` stands through both, fading out only
        // in the far band (trunkFade). Needs a billboard, the card mesh and the trunk.
        TreeInstanceRep trunk, trunkFade, cardsIn, cardsOut;
        float midDistance = 0.0f;
        float midFadeWidth = 1.0f;
        // Beyond this distance (m) from the shadow cascades' centre it casts no sun shadow (small plants: a texel or
        // less in the far cascades). 0 = no limit.
        float shadowDistance = 0.0f;
        // The FAR-TREE VOLUME's view of the type (TreeVolumePipeline): its extinction (1/m) over [densityMin,
        // densityMax] in type space, densityRes^3 floats (x fastest); nullptr = the type is not in the volume.
        const float* density = nullptr;
        uint32 densityRes = 0;
        glm::vec3 densityMin{ 0.0f };
        glm::vec3 densityMax{ 0.0f };
        glm::vec3 albedo{ 0.1f, 0.16f, 0.07f }; // the leaf colour of its volume
    };
    struct TreeInstancePiece
    {
        Transform transform;      // the piece's node transform (static)
        glm::vec3 centre{ 0.0f }; // world centre + radius of the far representation (the band test)
        float radius = 0.0f;
        uint32 type = 0;
    };
    // A CHUNK of the set: a contiguous range of its pieces (the caller sorts them - Procedural: per terrain chunk).
    struct TreeInstanceChunk { uint32 first = 0; uint32 count = 0; };
    uint32 createTreeInstanceSet(oc::span<const TreeInstanceType> types, oc::span<const TreeInstancePiece> pieces,
        oc::span<const TreeInstanceChunk> chunks);
    // Drains the GPU first (rare: a respawn / reload).
    void destroyTreeInstanceSet(uint32 setId);
    // MAIN THREAD, every frame the set draws (before its renderTreeInstanceSet): the culls bind its buffers (a change
    // re-records).
    void bindTreeInstanceSet(uint32 setId);
    // This frame's DRAWN chunks, each with the passes it draws in (PASS_*: the main cull, the shadow culls, the TLAS):
    // the claim covers only their pieces. ANY THREAD, once per frame between beginFrame and present (Procedural: from
    // the terrain's render walk). distanceScale x every type's farDistance.
    struct TreeChunkDraw { uint32 chunk; uint32 passMask; };
    void renderTreeInstanceSet(uint32 setId, oc::span<const TreeChunkDraw> chunks, float distanceScale, bool forceFar);
    // The terrain height right under the camera (world Y), per frame: the far-tree volume's cell layout follows the
    // camera's height above it. Unset (NaN): the baked sea level.
    void setFarTreeCameraGround(float groundY) { m_farTreeCameraGround = groundY; }

    // -- Procedural grass (GrassPipeline, grass.inc.glsl; "Grass" tweaks) --
    // The blades stand on the terrain MESH: their roots are read from the chunks' own vertices. One resident chunk per
    // coordinate (the finest), all within grassRange() of the camera.
    struct GrassGroundChunk { glm::ivec2 coord; uint32 firstVertex = 0; uint32 res = 0; };
    // MAIN THREAD, every frame before present (Procedural TerrainStreamer::update). Valid for ONE frame: a frame
    // without a call draws no grass (a disabled terrain, or freed chunks, can never be read).
    void setGrassGround(float chunkSize, oc::span<const GrassGroundChunk> chunks);
    float grassRange() const { return grassActive() ? m_grassParams.range : 0.0f; } // m; 0 = no grass

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
    // opaque = a depth-writing stage: inherits SceneColor's opaque family (+ the motion target).
    vk::CommandBuffer beginScenePassSecondary(uint32 frameIdx, CommandBuffer& cb, bool opaque = false);

    void recordSkinning(uint32 frameIdx);
    void recordOceanSim(uint32 frameIdx);
    void recordTerrainWetness(uint32 frameIdx);
    void recordGrassCull(uint32 frameIdx);
    void recordIndirectCull(uint32 frameIdx);
    void recordLightGrid(uint32 frameIdx);
    void recordShadowCull(uint32 frameIdx);
    void recordShadowDraw(uint32 frameIdx);
    void recordRainOcclusionCull(uint32 frameIdx);
    void recordRainOcclusionDraw(uint32 frameIdx);
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
    bool cloudsEnabled() const { return m_cloudParams.enabled && !m_cloudsSuppressed; }
    // DLSS (desktop only) REPLACES TAA: it writes TAA's resolved image, so the post chain reads the same image.
    bool dlssActive() const { return m_dlssMode != Streamline::DlssMode::Off && m_sceneViewCount == 1 && Streamline::dlssAvailable(); }
    bool taaActive() const { return m_taaParams.taaEnabled && !dlssActive(); }
    bool resolveActive() const { return taaActive() || dlssActive(); } // false: the post chain reads the scene colour directly
    // The scene renders below the output resolution (a DLSS mode other than DLAA).
    bool upscaling() const { return m_renderScale != glm::vec2(1.0f); }
    // Desktop only (a blur the head did not make is uncomfortable in VR); off = the pass is not recorded at all.
    // Not while upscaling: its velocity and gather assume one resolution for the depth and the resolved colour.
    bool motionBlurEnabled() const { return m_motionBlurParams.enabled && m_motionBlurParams.shutter > 0.0f && m_sceneViewCount == 1 && !upscaling(); }
    // Desktop only (level 0 comes from the left eye's histogram in VR); off = no chain, no level-0 writes.
    bool bloomEnabled() const { return m_bloomParams.enabled && m_bloomParams.intensity > 0.0f && m_sceneViewCount == 1; }
    // CloudParams toggles -> the baked shader defines (buildLayoutPreamble). Returns whether they changed, so a
    // strength slider that only crosses 0 at the ends (Powder) reloads the shaders only when its define flips.
    bool syncCloudDefines()
    {
        const RendererVKLayout::CloudShaderConfig config{ .clouds = m_cloudParams.enabled, .shadows = m_cloudParams.shadows,
            .selfShadowFromMap = m_cloudParams.selfShadowFromMap, .powder = m_cloudParams.powder > 0.0f,
            .checkerboard = m_cloudParams.checkerboard, .debugMode = m_cloudParams.debugMode };
        if (config == RendererVKLayout::g_cloudShaders)
            return false;
        RendererVKLayout::g_cloudShaders = config;
        return true;
    }
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
    float dlssMipBias() const { return dlssActive() && m_dlssParams.mipBias && upscaling() ? glm::log2(m_renderScale.y) : 0.0f; }
    vk::Extent2D renderExtent() const { return vk::Extent2D{ m_renderExtent.x, m_renderExtent.y }; }

    glm::mat4 computeCenterProjection(const Camera& camera) const; // pure, reversed-Z, unjittered (VR: both eyes)
    glm::mat4 computeCenterViewProj(const Camera& camera) const; // pure: projection (VR: both eyes) * view
    void applyVrHeadPose(const Camera& cameraIn, Camera& camera, glm::quat& vrBaseOrientation);
    void checkFrameCapacities();
    void snapshotLodStats(PerFrameData& frameData);
    void buildFrameUbo(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation, PerFrameData& frameData);
    void buildUboViews(const Camera& cameraIn, const Camera& camera, const glm::quat& vrBaseOrientation);
    void buildUboSky();
    void buildUboClouds(const Camera& camera);
    void buildUboSunShadow(const Camera& camera);
    void buildUboRainOcclusion();
    void buildUboFog();
    void buildUboOcean();
    void buildUboForce();
    void buildUboTerrain();
    void buildUboGrass();

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
    static constexpr uint32 NUM_SCENE_STAGES = 10;
    oc::array<SceneStage, NUM_SCENE_STAGES> buildSceneStages(uint32 frameIdx);

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

    friend class RenderMesh;
    void destroyMesh(RenderMesh& mesh);
    uint32 m_identityInstanceOffsetIdx = UINT32_MAX; // spawnMeshNode's shared identity offset (first spawn)

    friend class RenderNode;
    uint32 addRenderNodeTransform(const Transform& transform);
    inline Transform& getRenderNodeTransform(uint32 idx) { return m_instances.getTransform(idx); }
    void freeRenderNode(RenderNode& node);

    uint32 addMeshInfos(const oc::vector<RendererVKLayout::MeshInfo>& meshInfos, oc::span<const uint32> vertexCounts, bool skinnedOutputs = false,
        bool raytraced = true);
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
    CloudPipeline m_cloudPipeline;
    TaaPipeline m_taaPipeline;
    DlssPipeline m_dlssPipeline;
    MotionBlurPipeline m_motionBlurPipeline;
    BloomPipeline m_bloomPipeline;
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
    // BAKED TREE RECORDS (RendererTrees.cpp, tree_cull.inc.glsl): the live sets (dead ones recycled by id). The culls
    // bind ONE set's static buffers (m_treeCullSet, bindTreeInstanceSet; a change re-records them) and build the
    // records of this frame's LISTED pieces (the drawn chunks' pieces, each with its pass bits) from the camera
    // distance in the range renderTreeInstanceSet claims (m_treeCullBase / Count, reset per frame in beginFrame,
    // into the UBO's u_treeCull).
    struct TreeInstanceSet
    {
        Buffer pieces;  // TreeCullPieceGpu per piece (device-local)
        Buffer types;   // TreeCullTypeGpu per type (device-local)
        uint32 numPieces = 0;
        oc::vector<TreeInstanceChunk> chunks;
        // Per chunk: its RT-capable pieces (an RT representation with a BLAS - tree_cull.inc.glsl treeCullRtPiece) come
        // FIRST in its range (createTreeInstanceSet reorders), rtCount of them, inside the sphere rtCentre / rtRadius.
        struct ChunkRt { uint32 rtCount = 0; glm::vec3 rtCentre{ 0.0f }; float rtRadius = 0.0f; };
        oc::vector<ChunkRt> chunkRt;
        // Per chunk its bucket sizes (level-0 mesh, instances): chunks[c]'s are meshCounts[meshCountsBegin[c] ..
        // meshCountsBegin[c + 1]).
        oc::vector<oc::pair<uint16, uint32>> meshCounts;
        oc::vector<uint32> meshCountsBegin;
        // This frame's list (piece | passMask << 28 per listed piece): host-visible, one per frame slot.
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> lists;
        oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> mappedLists;
        Buffer volumePieces; // TreeVolumePieceGpu per piece (the far-tree volume's bake)
        Buffer volumeTypes;  // TreeVolumeTypeGpu per type
        Buffer volumeData;   // the types' extinction mip chains (floats)
        bool hasVolume = false;
        bool alive = false;
    };
    oc::vector<TreeInstanceSet> m_treeSets;
    Buffer m_treeCullDummy;              // bound to both tree bindings while no set is
    uint32 m_treeCullSet = UINT32_MAX;   // the set whose buffers the recorded culls bind
    uint32 m_treeCullBase = 0;           // this frame's claimed range (0 / 0 = none)
    uint32 m_treeCullCount = 0;
    uint32 m_treeCullPieces = 0;         // the trees in it (TREE_RECORDS_PER_PIECE records each)
    uint32 m_treeCullRtPieces = 0;       // the list's first ones: the RT-capable trees of the GI / shadow chunks in RT range (TLAS slots)
    uint32 m_giTlasDemand = 0;           // last present()'s TLAS slot count before the capacity clamp (the growth check)
    oc::atomic<bool> m_treeCullTaken{ false }; // renderTreeInstanceSet ran this frame (any thread; reset in beginFrame)
    float m_treeCullDistanceScale = 1.0f;
    bool m_treeCullForceFar = false;
    Buffer& treeCullPieces();
    Buffer& treeCullTypes();
    Buffer& treeCullList(uint32 frameIdx);
    void uploadTreeCullUbo(PerFrameData& frameData);
    TreeVolumePipeline m_treeVolume;
    FarTreeParams m_farTreeParams;
    float m_farTreeCameraGround = std::numeric_limits<float>::quiet_NaN(); // setFarTreeCameraGround
    bool farTreesActive() const; // enabled, desktop, and a tree set with volume data
    float farTreesStart() const; // "Far start" scaled with the camera's height (the hand-over + the march's start)
    void recordFarTrees(uint32 frameIdx, vk::CommandBuffer primary); // bake when due + march (straight into the primary)
    void recordFarTreesApply(uint32 frameIdx);                        // the scene stage's cached secondary
    // PROCEDURAL GRASS: the patch cull + buffers (the blades draw in m_staticMeshGraphicsPipeline). The ground chunks
    // arrive per frame (setGrassGround) and go into the slot's ground table in present (uploadGrassFrame).
    GrassPipeline m_grassPipeline;
    GrassParams m_grassParams;
    oc::vector<GrassGroundChunk> m_grassGround;
    float m_grassChunkSize = 0.0f;
    float m_grassPrevTime = 0.0f; // last frame's timeSeconds (the blades' motion vectors)
    bool grassActive() const { return m_grassParams.enabled && m_sceneViewCount == 1; } // desktop only
    void uploadGrassFrame(uint32 frameIdx);
    DecalPipeline m_decalPipeline;
    ForceFieldPipeline m_forceFieldPipeline;
    ParticleState m_particles;
    ForceFieldState m_force;
    BindlessTextures m_textures;
    PerWorker<oc::vector<DebugLinePipeline::LineVertex>> m_debugLineVerts; // per-worker CPU staging, drained into the mapped buffer in present()

    SkyParams m_skyParams;
    ShadowParams m_shadowParams;
    FoliageParams m_foliageParams;
    FogParams m_fogParams;
    CloudParams m_cloudParams;
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
    PostParams m_postParams;
    RTParams m_rtParams;
    RTAOParams m_rtaoParams;
    LightGridParams m_lightGridParams;
    TAAParams m_taaParams;
    DlssParams m_dlssParams;
    MotionBlurParams m_motionBlurParams;
    BloomParams m_bloomParams;
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
        CommandBuffer grassCullCommandBuffer;
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
        CommandBuffer cloudCommandBuffer;
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