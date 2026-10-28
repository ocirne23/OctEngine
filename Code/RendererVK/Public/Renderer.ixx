export module RendererVK:Renderer;

import Core;
import Core.glm;
import Core.Rect;
import Core.Transform;
import Core.Camera;
import Core.VrSession;
import Threading;

import :RendererTypes;
import :RendererState; // the state and the private helpers; through it, every partition the API names

import Core.fwd;

export enum class EValidation { ENABLED, DISABLED };
export enum class EVr { ENABLED, DISABLED };

// THE RENDERER'S API. The value types it takes are in RendererTypes.ixx (Renderer::X), the state and the private helpers in RendererState.ixx. The multi-line
// bodies are below the class.
export class Renderer final : public RendererState
{
public:

    // ---- Lifetime and the window ----

    bool initialize(Window& window, EValidation validation, EVr vr = EVr::DISABLED);
    bool isInitialized() const { return m_initialized; }  // false in headless server mode: anything that may run headless gates on it
    void recreateWindowSurface(Window& window);           // on resize
    void setWindowMinimized(bool minimized);
    bool isVSyncEnabled() const;                          // FIFO present: frames land on whole refresh periods (Time's stable-dt snap relies on it)
    void reloadShaders();
    void setResizeHold(bool hold) { m_resizeHold = hold; } // Mouse grabbing the window to resize, update swapchain on release instead of per-frame

    // ---- The frame, in call order ----

    bool waitFrameSlot(uint64 timeoutNs = UINT64_MAX);                     // the VSync / GPU throttle of the whole loop (Time::beginFrame)
    CullView setFrameView(const Camera& camera, const Rect& viewportRect); // the spatial cull's view of this frame; before kickBeginFrameJob
    void kickBeginFrameJob(); // The frame's setup pass, as a job. VR defers instead: the join runs it on main
    void joinBeginFrameJob();
    void kickGridBuilds(); // after the frame's LAST light add and the force update
    void present();
    void requestScreenshot(oc::string path) { m_screenshotPath = oc::move(path); } // Main thread. The NEXT frame is written to path as .png

    // ---- Per-frame submission [Concurrency: LOCK-FREE]: from any job, between beginFrame and present. ----

    void renderNode(const RenderNode& node, uint32 passMask);
    void renderNode(const RenderNode& node) { renderNode(node, node.getPassMask()); }
    void addLightInfo(const RendererVKLayout::LightInfo& light); // a PointLight / SpotLight / AreaLight / TubeLight (Light.ixx)
    void addFogVolume(const RendererVKLayout::FogVolumeInfo& fogVolume);
    void addDecal(const RendererVKLayout::DecalInfo& decal);
    void addDebugLine(const glm::vec3& a, const glm::vec3& b, uint32 color); // per-worker staging, merged in present

    // ----  The world focus point every distance-based falloff measures from ----

    void setSceneFocus(const glm::vec3& worldPos) { m_sceneFocus = worldPos; m_sceneFocusEnabled = true; }
    void clearSceneFocus() { m_sceneFocusEnabled = false; }

    // The SURFACE height (ground or water) under the camera, set each frame
    void setFocusGround(float surfaceHeight) { m_focusGround = surfaceHeight; m_haveFocusGround = true; }
    void clearFocusGround() { m_haveFocusGround = false; }
    glm::vec3 sceneFocusOrCamera() const;
    const glm::vec3& cameraPos() const { return m_cameraPos; }              // valid after joinBeginFrameJob
    const glm::mat4& getCenterViewProj() const { return m_centerViewProj; } // what the GPU cull used (VR: the two-eye union), for CPU occlusion rasterization

    // ---- Global rendering parameters ----
    // A code write into a lockable setting re-bakes (m_uboLocksDirty), like a tweak change.

    void setSunLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    void setAmbientLight(const glm::vec3& color, float intensity);
    void setSkyRadiance(const glm::vec3& color, float intensity);
    void setSkyParams(const SkyParams& sky);
    const SkyParams& getSkyParams() const { return m_skyParams; }
    const WindParams& getWindParams() const { return m_windParams; } // THE wind ("Sky/Wind"): the ocean reads it (x its speed scale), the rest through the UBO
    void setFogParams(const FogParams& fog);
    void setPostParams(const PostParams& post);
    void setShadowParams(const ShadowParams& params);
    const ShadowParams& shadowParams() const { return m_shadowParams; }
    void setCloudsSuppressed(bool suppressed) { m_cloudsSuppressed = suppressed; } // the game: clouds off, the "Sky/Clouds/Enabled" tweak untouched

    // ---- Single meshes, No ObjectContainer (see RenderMesh; their materials: RenderMaterial). MAIN THREAD ----

    // Uploads + one MeshInfo (and its BLAS); invalid when empty. `raytraced` false: NO BLAS - any instance of it is inactive in the TLAS (no RT shadows / GI /
    // RTAO / reflections hit it); for meshes RT never needs to see.
    RenderMesh createMesh(const RenderMeshData& data, bool raytraced = true);

    // The same with LOD LEVELS (level 0 first, at most MAX_MESH_LODS; the levels end at the first empty one): one MeshInfo per level, and with 2+ levels the
    // GPU LOD chain over them - spawnMeshNode places level 0 and the cull picks each instance's level. `errors` = each level's geometric deviation in
    // mesh-local units (0 for level 0; the screen-space-error selector).
    RenderMesh createMesh(oc::span<const RenderMeshData* const> levels, oc::span<const float> errors, bool raytraced = true);
    const MeshLodParams& getLodParams() const { return m_lodParams; } // the "LOD" settings the chains are generated and picked with

    // One node drawing `mesh` with `materialIdx` on `pipeline`, placed by `transform` (a shared identity instance offset: the mesh is its own root).
    RenderNode spawnMeshNode(const RenderMesh& mesh, uint16 materialIdx, RendererVKLayout::EPipelineIndex pipeline, const Transform& transform);

    // ---- World tree records ----
    // (The procedural tree pieces themselves: RenderTreeSet.)
    // TreeRecordPool; Procedural TreeWorld. One thread at a time, after beginFrame and before present: TreeWorld's "TreeWorld pool" job (joined before
    // present), or main.

    // (Re)sizes the pool and drops every chunk (a size change drains the GPU). ringRadius (chunks) sizes the chunk map.
    void resetTreeRecords(uint64 poolBytes, uint32 ringRadius);

    // A chunk's ground (TREE_RECORD_GROUND_WORDS) and 4-byte records; the handle, or UINT32_MAX (no room). The far volume re-bakes (throttled).
    uint32 addTreeRecordChunk(glm::ivec2 coord, oc::span<const uint32> ground, oc::span<const uint32> records);
    void removeTreeRecordChunk(uint32 handle);

    // The record types (indexed by the record's type; empty: the volume ignores the records) and what expanding a record needs: the chunk size, the world seed,
    // and the tree set whose volume types the variants name (Procedural's world set). The far volume then takes EVERY tree from the records and none from that
    // set. Drains the GPU.
    void setTreeRecordTypes(oc::span<const TreeRecordTypeGpu> types, float chunkSize, uint32 worldSeed, const RenderTreeSet& volumeSet);

    // Once per frame: the deferred frees, and the current frame slot's chunk table.
    void updateTreeRecords() { m_treeRecords.update(m_swapChain.getCurrentFrameIndex(), m_frameCounter, m_treeVolume.bakeHoldSince()); }
    TreeRecordStats treeRecordStats() const { return m_treeRecords.stats(); }

    // ---- Terrain ----

    void setTerrainParams(float meshRadius, float seaLevel, float temperatureLapseRate = 0.0f) { m_terrain.setParams(meshRadius, temperatureLapseRate, seaLevel); }
    float getTerrainMeshRadius() const { return m_terrain.getMeshRadius(); }
    void setTerrainSplatMaterials(oc::span<const TerrainSplatMaterial> mats, const TerrainSplatCounts& counts); // see TerrainStreamer::registerTerrainTextures
    float giTlasRange() const { return m_giProbePipeline.getTlasRange(); } // metres from sceneFocusOrCamera

    // The terrain VS's EDGE STITCHING (its draw camera and ring bands; chunkSize 0 = off). Main thread, read by the next frame's UBO.
    void setTerrainStitch(float chunkSize, const glm::vec2& drawCamChunks, float fullRes, float lodStep, uint32 maxLod);

    // V3's world scale (the loaded model's native resolution - not a tweak): the crag thresholds and the crag wander ("Terrain/Textures") are metres at the
    // model's true scale. A change re-bakes the UBO values that read it.
    void setTerrainCragScale(float scale);

    // FOG_TERRAIN_CASCADES layers of FOG_TERRAIN_RES^2 RGBA float quads, near cascade first: R = terrain height, G = water surface level, B = regional fog
    // thickness [0,1], A = spare. Cascade i covers cascadeWorldSizes[i] m. Both the height fog base and the ocean's water depth / level read it. Staged
    // ping-pong: live next frame, no GPU sync and no re-record.
    void setFogTerrainHeightMap(oc::span<const float> heightTexels, const glm::vec2& centerXZ, const glm::vec2& cascadeWorldSizes, float seaLevel);
    void clearFogTerrainHeightMap() { m_terrain.getHeightMap().clear(); } // the fog reverts to the flat height base

    // ---- Rivers and lakes,  Main thread. ----

    // The river water's NEAR COVERAGE: engine m around the centre view inside which every dense near cell is built - the light ribbons discard their fragments
    // there (river.fs.glsl), so the dense waves are the only surface. 0 = none. Read by the next frame's UBO.
    void setRiverNearCovered(float distance) { m_riverNearCovered = distance; }

    // THE INLAND WATER MAP (RendererVKLayout::RiverWaterMapGpu): RIVER_WATER_MAP_DIM^2 water surface heights (world Y, RIVER_WATER_NONE = no inland water) of
    // RIVER_WATER_MAP_TEXEL m from `origin` (texel (0, 0)'s min corner); empty = none. The volumetric fog's underwater boundary over rivers and lakes. Each
    // frame slot takes it in present.
    void setRiverWaterMap(glm::vec2 origin, oc::span<const float> heights);

    // THE RIVER MIST SOURCES: at most MAX_RIVER_MIST_SOURCES (the rest are dropped).
    void setRiverMistSources(oc::span<const RendererVKLayout::RiverMistSourceGpu> sources);
    void setRiverMistEmitter(uint32 slot) { m_riverMistEmitter = slot; } // the mist's emitter (Effects/river_mist.pfx; UINT32_MAX = off)

    // ---- Ocean ----

    void setOceanParams(const OceanParams& ocean);
    void setOceanWaveTrough(float meters) { m_oceanSimPipeline.setWaveTrough(meters); }
    void setOceanDisplacementExtent(float meters) { m_oceanSimPipeline.setDisplacementExtent(meters); }
    void setCameraWaterSurface(float worldY) { m_oceanSimPipeline.setCameraWaterSurface(worldY); }
    void clearCameraWaterSurface() { m_oceanSimPipeline.clearCameraWaterSurface(); }
    void setOceanSprayEmitter(uint32 slot) { m_oceanSimPipeline.setSprayEmitter(slot); }

    // RGBA16F (Dx, h, Dz, dDxz), outRes^2 per cascade, cascades packed consecutively. ~2 frames old. Copy between beginFrame and present.
    oc::span<const uint16> getOceanDisplacementReadback(uint32& outRes) const;

    // "Ocean/World scale" (s), from the "Ocean" settings (the value the pushed OceanParams carries): the spray emitter's lengths / speeds / accelerations ride it.
    float getOceanWorldScale() const;

    // ---- Procedural grass ----

    // The blades stand on the terrain MESH: their roots are read from the chunks' own vertices. One entry per BASE chunk coordinate, all within groundRange()
    // of the camera. MAIN THREAD, every frame before present (Procedural TerrainStreamer::update). Valid for ONE frame: a frame without a call draws no grass
    // (a disabled terrain, or freed chunks, can never be read).
    void setGrassGround(float chunkSize, uint32 vertexStride, oc::span<const GrassGroundChunk> chunks);
    // The terrain height right under the camera (world Y; NaN = unknown), per frame: the near grass cascade's placement and the far-tree volume's cell layout.
    void setCameraGround(float groundY) { m_cameraGround = groundY; }

    float groundRange() const { return glm::max(grassRange(), clutterRange()); } // m the ground chunks must cover: the grass's and the clutter's

    // ---- Ground clutter ----

    // The clutter types and their meshes. MAIN THREAD; idles the GPU and re-records. Empty = no clutter.
    void setClutterAssets(oc::span<const RendererVKLayout::ClutterTypeGpu> types, oc::span<const ClutterMesh> meshes);
    // THE FOREST FLOOR MAP: CLUTTER_FLOOR_DIM^2 rgba8 texels (canopy, trunk, rock, occupied) of CLUTTER_FLOOR_TEXEL m. MAIN THREAD;
    void setClutterFloorMap(glm::vec2 centre, oc::span<const uint32> texels);

    // ---- GPU particles and projected decals (the emitters: RenderParticleEmitter) ----

    void setRainOcclusionVolume(const glm::vec3& center, const glm::vec3& halfExtents);
    uint16 loadEffectTexture(const char* filePath, bool sRGB = true);

    // ---- Forcefield bubbles (the emitters and queries: RenderForceEmitter, RenderForceQuery). The readback is ~2 frames old, between beginFrame and present ----

    void setForceFieldParams(const ForceFieldParams& params);
    void setForceBakeChunks(oc::span<const glm::ivec4> chunks, float sampleY) { m_force.setBakeChunks(chunks, sampleY); }
    RendererVKLayout::ForceBakeReadback getForceBakeReadback() const;

    // ---- UI ----

    void setImGuiDrawData(const void* drawData) { m_imguiPendingDrawData.store(drawData, oc::memory_order_release); }
    // MAIN THREAD, between the widget pass's join and the next UI::update. Uploads the queued font-atlas changes
    void updateImGuiTextures();

    // ---- VR ----

    bool isVrEnabled() const { return Globals::openXR.isEnabled(); }
    bool isVrStageSpace() const { return Globals::openXR.isStageSpace(); }
    IVrSession* getVrSession() { return Globals::openXR.isEnabled() ? &Globals::openXR : nullptr; }

    // ---- Metrics ----

    uint32 getNumMeshInstances() const { return m_instances.getInstanceCount(); }
    uint32 getNumMeshTypes() const { return m_meshInfos.count(); }
    uint32 getNumMaterials() const { return m_materials.count(); }
    void forEachGpuAllocation(GpuAllocationVisit visit, void* ctx) const { Globals::gpuAllocator.forEachAllocation(visit, ctx); }
    GpuAllocator::MemoryUsage getGpuMemoryUsage() const { return Globals::gpuAllocator.getMemoryUsage(); }
    static void gpuEnumName(GpuEnum kind, uint64 value, char* out, size_t outSize) { GpuAllocator::enumName(kind, value, out, outSize); }
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_RENDERER)
    Renderer rendererVK;
} // namespace Globals

// ---- Inline bodies ----

inline void Renderer::addDebugLine(const glm::vec3& a, const glm::vec3& b, uint32 color)
{
    oc::vector<DebugLinePipeline::LineVertex>& verts = m_debugLineVerts.local();
    const ThreadLocalScope tlsPin;
    verts.push_back({ a, color });
    verts.push_back({ b, color });
}

inline glm::vec3 Renderer::sceneFocusOrCamera() const
{
    return m_sceneFocusFrozen ? m_frozenSceneFocus : m_sceneFocusEnabled ? m_sceneFocus : defaultSceneFocus();
}

inline void Renderer::resetTreeRecords(uint64 poolBytes, uint32 ringRadius)
{
    m_treeVolume.invalidate(); // a running bake reads the old chunks
    m_treeRecords.reset(poolBytes, m_frameCounter, ringRadius);
}

inline uint32 Renderer::addTreeRecordChunk(glm::ivec2 coord, oc::span<const uint32> ground, oc::span<const uint32> records)
{
    m_treeSetsChanged = true;
    return m_treeRecords.add(coord, ground, records);
}

inline void Renderer::removeTreeRecordChunk(uint32 handle)
{
    m_treeSetsChanged = true;
    m_treeRecords.remove(handle, m_frameCounter);
}

inline void Renderer::setTreeRecordTypes(oc::span<const TreeRecordTypeGpu> types, float chunkSize, uint32 worldSeed, const RenderTreeSet& volumeSet)
{
    m_treeVolume.invalidate(); // a running bake reads the old types
    m_treeRecords.setTypes(types);
    m_treeRecordChunkSize = chunkSize;
    m_treeRecordSeed = worldSeed;
    m_treeRecordSet = volumeSet.m_setId;
}

inline void Renderer::setTerrainStitch(float chunkSize, const glm::vec2& drawCamChunks, float fullRes, float lodStep, uint32 maxLod)
{
    m_terrainStitch = glm::vec4(chunkSize, drawCamChunks, 0.0f);
    m_terrainStitchBands = glm::vec4(fullRes, lodStep, (float)maxLod, 0.0f);
}

inline void Renderer::setFogTerrainHeightMap(oc::span<const float> heightTexels, const glm::vec2& centerXZ, const glm::vec2& cascadeWorldSizes, float seaLevel)
{
    m_terrain.getHeightMap().upload(heightTexels, centerXZ, cascadeWorldSizes, seaLevel, m_swapChain.getCurrentFrameIndex());
}

inline oc::span<const uint16> Renderer::getOceanDisplacementReadback(uint32& outRes) const
{
    outRes = OceanSimulationPipeline::READBACK_RES;
    return m_oceanSimPipeline.getDisplacementReadback(m_swapChain.getCurrentFrameIndex());
}
