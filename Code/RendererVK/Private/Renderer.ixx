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
import Settings;
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
    // RESIZE HOLD (App, while the left mouse button is down - a window-border or ImGui-splitter drag): an out-of-date
    // swapchain does not rebuild per frame of the drag; the frame is skipped and the ONE rebuild runs when the hold
    // ends (a rebuild per frame recreated the DLSS feature every frame and broke it). The App holds the viewport rect
    // and the window-resize event the same way.
    void setResizeHold(bool hold) { m_resizeHold = hold; }
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
    static void gpuEnumName(GpuEnum kind, uint64 value, char* out, size_t outSize) { GpuAllocator::enumName(kind, value, out, outSize); }

    // -- GPU particles + projected decals (driven by the Particle library) --
    uint32 createParticleEmitter(const RendererVKLayout::ParticleEmitterGpu& desc); // [Concurrency: LOCKING]
    void updateParticleEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc);
    void emitParticles(uint32 slot, uint32 count);
    void destroyParticleEmitter(uint32 slot); // [Concurrency: LOCKING]
    void resetParticles() { m_particles.requestReset(); }
    void setRainOcclusionVolume(const glm::vec3& center, const glm::vec3& halfExtents);
    void setOceanSprayEmitter(uint32 slot) { m_oceanSimPipeline.setSprayEmitter(slot); }
    // "Ocean/World scale" (s): the spray emitter's lengths / speeds / accelerations ride it.
    // From the "Ocean" settings (the same values the pushed OceanParams carry): the UBO's lockable ocean values and the
    // live work both read these.
    float getOceanWorldScale() const { return oceanWorldScaled(Globals::settings.ocean).s; }
    float getOceanSwashAmp() const { return glm::clamp(Globals::settings.ocean.swashAmp, 0.0f, 4.0f); }
    float getOceanBubbleDepth() const { return glm::max(oceanWorldScaled(Globals::settings.ocean).bubbleDepth, 0.0f); }
    float getOceanBubbleBrightness() const { return glm::max(Globals::settings.ocean.bubbleBrightness, 0.0f); }
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
    // A code write into a lockable setting re-bakes (m_uboLocksDirty), like a tweak change.
    void setSkyRadiance(const glm::vec3& color, float intensity) { m_skyParams.skyRadianceColor = color; m_skyParams.skyRadianceIntensity = intensity; m_uboLocksDirty = true; }
    void setSkyParams(const SkyParams& sky) { m_skyParams = sky; m_uboLocksDirty = true; }
    const SkyParams& getSkyParams() const { return m_skyParams; }
    // THE wind ("Sky/Wind"): the ocean reads it (x its own speed scale) - everything renderer-side through the UBO.
    const WindParams& getWindParams() const { return m_windParams; }
    void setFogParams(const FogParams& fog) { m_fogParams = fog; m_uboLocksDirty = true; }
    void setPostParams(const PostParams& post) { m_postParams = post; setHaveToRecordCommandBuffers(); m_uboLocksDirty = true; }
    const ShadowParams& shadowParams() const { return m_shadowParams; }
    void setShadowParams(const ShadowParams& params) { m_shadowParams = params; m_uboLocksDirty = true; }
    // The game turns the volumetric clouds off without touching the user's "Sky/Clouds/Enabled" tweak.
    void setCloudsSuppressed(bool suppressed) { m_cloudsSuppressed = suppressed; }

	// -- Terrain parameters --
    void setTerrainParams(float meshRadius, float seaLevel, float temperatureLapseRate = 0.0f) { m_terrain.setParams(meshRadius, temperatureLapseRate, seaLevel); }
    float getTerrainMeshRadius() const { return m_terrain.getMeshRadius(); }
    // The terrain VS's EDGE STITCHING (Procedural TerrainStreamer: its draw camera and ring bands; chunkSize 0 = off).
    // Main thread, read by the next frame's UBO.
    void setTerrainStitch(float chunkSize, const glm::vec2& drawCamChunks, float fullRes, float lodStep, uint32 maxLod)
    {
        m_terrainStitch = glm::vec4(chunkSize, drawCamChunks, 0.0f);
        m_terrainStitchBands = glm::vec4(fullRes, lodStep, (float)maxLod, 0.0f);
    }
    using TerrainSplatMaterial = ::TerrainSplatMaterial;
    using TerrainSplatCounts = ::TerrainSplatCounts;
    void setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts); // See TerrainStreamer::registerTerrainTextures for docs
    // V3's world scale (the loaded model's native resolution - not a tweak): the crag thresholds and the crag wander
    // ("Terrain/Textures") are metres at the model's true scale. A change re-bakes the UBO values that read it.
    void setTerrainCragScale(float scale);
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
    bool isVSyncEnabled() const { return Globals::settings.renderer.vsync; } // FIFO present: frames land on whole refresh periods (Time's stable-dt snap relies on it)
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
    // The encodings say what the mip bytes ARE: RGBA8, or BC blocks the caller compressed (File TextureConvert::
    // compressBlockRows) - the albedo sRGB, the normal linear. A BC5 normal sets MATERIAL_FLAG_BC5_NORMAL (Z rebuilt).
    enum class ETextureEncoding : uint8 { Rgba8, BC1, BC3, BC5 };
    uint16 createTextureMaterial(uint32 width, uint32 height, const oc::vector<oc::span<uint8>>& mips, float alphaCutoff, const char* debugName,
        const oc::vector<oc::span<uint8>>* normalMips = nullptr, uint32 extraFlags = 0,
        ETextureEncoding albedoEncoding = ETextureEncoding::Rgba8, ETextureEncoding normalEncoding = ETextureEncoding::Rgba8);
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
        // A SOLID (a rock, Procedural's world rocks): `density` is its OCCUPANCY (0..1), and the volume gives it
        // "Trees/Far rock extinction" at any size (a crown's extinction thins with its scale; a rock's does not). Its
        // colour is the climate's bedrock - or, with solidOwnColour (dead wood), `albedo`.
        bool solid = false;
        bool solidOwnColour = false;
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
    // A DYNAMIC set (Procedural's world trees): the types fixed, CHUNKS added and removed at run time into a fixed capacity
    // of piece slots (no growth; no GPU drain). MAIN THREAD, after beginFrame.
    uint32 createDynamicTreeInstanceSet(oc::span<const TreeInstanceType> types, uint32 pieceCapacity);
    // The new chunk's index (TreeChunkDraw::chunk), or UINT32_MAX when the set has no room.
    uint32 addTreeInstanceChunk(uint32 setId, oc::span<const TreeInstancePiece> pieces);
    // Its piece slots are reused NUM_FRAMES_IN_FLIGHT frames later, its index from the next frame on (this frame's
    // terrain walk may still name it).
    void removeTreeInstanceChunk(uint32 setId, uint32 chunk);
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

    // -- WORLD TREE RECORDS (TreeRecordPool; Procedural TreeWorld). MAIN THREAD, after beginFrame. --
    // (Re)sizes the pool and drops every chunk (a size change drains the GPU). ringRadius (chunks) sizes the chunk map.
    void resetTreeRecords(uint64 poolBytes, uint32 ringRadius)
    {
        m_treeVolume.invalidate(); // a running bake reads the old chunks
        m_treeRecords.reset(poolBytes, m_frameCounter, ringRadius);
    }
    // A chunk's ground (TREE_RECORD_HEIGHT_WORDS) and 4-byte records; the handle, or UINT32_MAX (no room). The far
    // volume re-bakes (throttled).
    uint32 addTreeRecordChunk(glm::ivec2 coord, oc::span<const uint32> ground, oc::span<const uint32> records)
    {
        m_treeSetsChanged = true;
        return m_treeRecords.add(coord, ground, records);
    }
    void removeTreeRecordChunk(uint32 handle)
    {
        m_treeSetsChanged = true;
        m_treeRecords.remove(handle, m_frameCounter);
    }
    // The record types (indexed by the record's type; empty: the volume ignores the records) and what expanding a record
    // needs: the chunk size, the world seed, and the tree set whose volume types the variants name (Procedural's world
    // set). The far volume then takes EVERY tree from the records and none from that set. Drains the GPU.
    void setTreeRecordTypes(oc::span<const TreeRecordTypeGpu> types, float chunkSize, uint32 worldSeed, uint32 volumeSet)
    {
        m_treeVolume.invalidate(); // a running bake reads the old types
        m_treeRecords.setTypes(types);
        m_treeRecordChunkSize = chunkSize;
        m_treeRecordSeed = worldSeed;
        m_treeRecordSet = volumeSet;
    }
    // Once per frame: the deferred frees, and the current frame slot's chunk table.
    void updateTreeRecords() { m_treeRecords.update(m_swapChain.getCurrentFrameIndex(), m_frameCounter, m_treeVolume.bakeHoldSince()); }
    TreeRecordStats treeRecordStats() const { return m_treeRecords.stats(); }

    // -- Procedural grass (GrassPipeline, grass.inc.glsl; "Grass" tweaks) --
    // The blades stand on the terrain MESH: their roots are read from the chunks' own vertices. One resident chunk per
    // coordinate (the finest), all within grassRange() of the camera.
    struct GrassGroundChunk { glm::ivec2 coord; uint32 firstVertex = 0; uint32 res = 0; };
    // MAIN THREAD, every frame before present (Procedural TerrainStreamer::update). Valid for ONE frame: a frame
    // without a call draws no grass (a disabled terrain, or freed chunks, can never be read).
    void setGrassGround(float chunkSize, oc::span<const GrassGroundChunk> chunks);
    // The terrain height right under the camera (world Y; NaN = unknown), per frame (Procedural TerrainStreamer::update):
    // the near grass cascade's placement (where the bottom of the view meets the ground).
    void setCameraGround(float groundY) { m_cameraGround = groundY; }
    float grassRange() const { return grassActive() ? m_grassParams.range : 0.0f; } // m; 0 = no grass
    // The range the ground chunks must cover (setGrassGround): the grass's and the ground clutter's.
    float groundRange() const { return glm::max(grassRange(), clutterRange()); }

    // -- Ground clutter (ClutterPipeline, clutter_cull.cs.glsl; Procedural ClutterSystem; "Clutter" tweaks) --
    // One rigid variant mesh: its LOD levels (level 0 first; an empty level = none), y up, the lowest point at 0.
    struct ClutterMesh
    {
        oc::vector<RendererVKLayout::ClutterVertexGpu> vertices[RendererVKLayout::CLUTTER_LODS];
        oc::vector<uint32> indices[RendererVKLayout::CLUTTER_LODS];
    };
    // The clutter types and their meshes (a type's info.x / .y name its first mesh and its count; flowers have none).
    // MAIN THREAD; idles the GPU and re-records. Empty = no clutter.
    void setClutterAssets(oc::span<const RendererVKLayout::ClutterTypeGpu> types, oc::span<const ClutterMesh> meshes);
    // THE FOREST FLOOR MAP: CLUTTER_FLOOR_DIM^2 rgba8 texels (canopy, trunk, rock, occupied) of CLUTTER_FLOOR_TEXEL m
    // centred on `centre` (empty = none). MAIN THREAD; each frame slot takes it in present.
    void setClutterFloorMap(glm::vec2 centre, oc::span<const uint32> texels);
    float clutterRange() const; // m: the farthest type's range x "Range scale", capped by the patch grid; 0 = no clutter

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
    // The bloom's share in the composite mix (u_post_bloomKeep / bloomScale): 0 while bloom is off.
    float bloomMixIntensity() const { return bloomEnabled() ? m_bloomParams.intensity : 0.0f; }
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
    // The renderer's reactions to its settings (shader reloads, re-records, resizes) as Tweak::onChange listeners,
    // plus the hand-over of the baked state the pipelines are built with (RendererSettings.cpp). The settings
    // must be registered first (Settings::register*, from main).
    void attachSettingsListeners();
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
    RainOcclusionPipeline m_rainOcclusionPipeline;
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
    // into the UBO's u_present_tree*).
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
        // A DYNAMIC set (createDynamicTreeInstanceSet): a fixed capacity of piece slots in blocks of TREE_SET_BLOCK, a
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
    // (renderTreeInstanceSet).
    std::mutex m_treeSetMutex;
    bool m_treeSetsChanged = false;    // a dynamic set changed: the far volume re-bakes (throttled, recordFarTrees)
    uint32 m_treeVolumeMarkFrame = 0;
    uint32 allocTreeSet();
    void initTreeSetTypes(TreeInstanceSet& set, oc::span<const TreeInstanceType> types);
    void initTreeSetPieces(TreeInstanceSet& set, uint32 capacity);
    // Writes a chunk's pieces (RT-capable first) into the set's slots [first, first + pieces.size()).
    void writeTreeChunk(TreeInstanceSet& set, TreeInstanceSet::Chunk& chunk, oc::span<const TreeInstancePiece> pieces, uint32 first);
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
    FarTreeParams& m_farTreeParams = Globals::settings.farTree;
    float m_farTreeCameraGround = std::numeric_limits<float>::quiet_NaN(); // setFarTreeCameraGround
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
    GrassParams& m_grassParams = Globals::settings.grass;
    RockParams& m_rockParams = Globals::settings.rock; // the rock material (EPipelineIndex::LitRock): UBO-driven, "Rocks/Material"
    oc::vector<GrassGroundChunk> m_grassGround;
    float m_grassChunkSize = 0.0f;
    float m_cameraGround = std::numeric_limits<float>::quiet_NaN(); // setCameraGround
    bool grassActive() const { return m_grassParams.enabled && m_sceneViewCount == 1; } // desktop only
    float grassPatchSize() const; // the patch grid (uploadGrassFrame) and u_grass_patchSize
    float grassGridRange() const; // capped so the grid fits GRASS_MAX_PATCHES: the grid and u_grass_range
    // The near grass cascade is drawn (and read): grass, its toggle, and the PCSS sun (RT sun shadows skip the shadow
    // map - the cascades' pass that this layer must follow).
    bool grassNearShadowActive() const { return grassActive() && m_grassParams.nearShadows && !m_rtParams.effectiveSunShadow(); }
    void uploadGrassFrame(uint32 frameIdx);
    bool m_groundTableValid = false; // this frame's ground table holds chunks (uploadGrassFrame): the clutter's ground
    // GROUND CLUTTER: the cull + buffers (the objects draw in m_staticMeshGraphicsPipeline). The patch grid and the
    // floor map go into the slot's clutter frame in present (uploadClutterFrame).
    ClutterPipeline m_clutterPipeline;
    ClutterSettings& m_clutterParams = Globals::settings.clutter;
    float m_clutterMaxRange = 0.0f;                    // the types' largest `Range` (setClutterAssets)
    oc::vector<uint32> m_clutterFloor;                 // the floor map's texels (setClutterFloorMap)
    glm::vec2 m_clutterFloorCentre{ 0.0f };
    oc::array<bool, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_clutterFloorDirty{}; // the slot has not taken the map yet
    bool clutterActive() const { return m_clutterParams.enabled && m_sceneViewCount == 1 && m_clutterPipeline.numTypes() > 0; } // desktop only
    float clutterPatchSize() const { return glm::clamp(m_clutterParams.patchSize, 1.0f, 16.0f); }
    void uploadClutterFrame(uint32 frameIdx);
    DecalPipeline m_decalPipeline;
    ForceFieldPipeline m_forceFieldPipeline;
    ParticleState m_particles;
    ForceFieldState m_force;
    BindlessTextures m_textures;
    PerWorker<oc::vector<DebugLinePipeline::LineVertex>> m_debugLineVerts; // per-worker CPU staging, drained into the mapped buffer in present()

    // The settings the renderer reads (Globals::settings: registered by Settings, written by the TweakPanel and the
    // setters above). References, so the many reads stay short; nothing here holds a copy.
    SkyParams& m_skyParams = Globals::settings.sky;
    WindParams& m_windParams = Globals::settings.wind;
    ShadowParams& m_shadowParams = Globals::settings.shadow;
    FoliageParams& m_foliageParams = Globals::settings.foliage;
    FogParams& m_fogParams = Globals::settings.fog;
    CloudParams& m_cloudParams = Globals::settings.clouds;
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
    PostParams& m_postParams = Globals::settings.post;
    RTParams& m_rtParams = Globals::settings.rt;
    RTAOParams& m_rtaoParams = Globals::settings.rtao;
    LightGridParams& m_lightGridParams = Globals::settings.lightGrid;
    TAAParams& m_taaParams = Globals::settings.taa;
    DlssParams& m_dlssParams = Globals::settings.dlss;
    MotionBlurParams& m_motionBlurParams = Globals::settings.motionBlur;
    BloomParams& m_bloomParams = Globals::settings.bloom;
    MeshLodParams& m_lodParams = Globals::settings.lod;

    Frustum m_centerFrustum;     // the centre view's (buildUboViews); VR's spatial cull takes last frame's

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

        oc::array<DescriptorSet, 2> staticMeshPipelineDescriptorSet; // Per-eye in VR
        DescriptorSet compositeDescriptorSet;
        DescriptorSet indirectCullPipelineDescriptorSet;
        DescriptorSet skinningDescriptorSet;
        DescriptorSet lightGridPipelineDescriptorSet;
        DescriptorSet shadowCullDescriptorSet;
        DescriptorSet shadowDrawDescriptorSet;

        CommandBuffer primaryCommandBuffer;
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

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_RENDERER)
    Renderer rendererVK;
} // namespace Globals

static_assert(oc::size(ForceFieldParams{}.teamColors) == RendererVKLayout::MAX_FORCE_TEAMS, "ForceFieldParams::teamColors must cover MAX_FORCE_TEAMS (Settings.Render cannot import :Layout)");
static_assert(GrassParams::MAX_BLADES == (int)RendererVKLayout::GRASS_MAX_BLADES, "GrassParams::MAX_BLADES (the \"Blades per patch\" range) must match GRASS_MAX_BLADES");