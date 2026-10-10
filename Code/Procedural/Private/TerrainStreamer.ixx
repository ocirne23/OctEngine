export module Procedural:TerrainStreamer;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;

import RendererVK;
import Spatial;
import Threading;
import File;
import Settings;

import :TerrainSampler;
import :TerrainGenerator;
import :TerrainChunk;
import :HeightMapBaker;
import :OceanGenerator;
import :RiverSystem;

export namespace Procedural
{
	// Render-only procedural terrain. Maintains a QUADTREE of terrain nodes around the camera (see Ring), each
	// generated on a worker thread and turned into a RenderMesh + RenderNode on the main thread, and pushes them
	// to the renderer every frame. The world-continuous height field keeps node and LOD boundaries consistent, and the
	// terrain VS stitches the edges between LODs (no skirts).
	//
	// A node that is replaced (or leaves the ring) destroys its RenderNode then its RenderMesh, which frees its
	// mega-buffer ranges and MeshInfo slot, so residency stays bounded across a session.
	class TerrainStreamer
	{
	public:
		TerrainStreamer() = default;
		~TerrainStreamer();
		TerrainStreamer(const TerrainStreamer&) = delete;
		TerrainStreamer& operator=(const TerrainStreamer&) = delete;

		void initialize();                                     // attaches the settings listeners + builds the maps (after Settings registered)
		void update(Renderer& renderer, const Camera& camera); // per-frame: stream, drain, evict (call after beginFrame)
		// Per-frame, after update() AND ocean.update(): kicks ocean.render, then this render job, whose
		// ONE walk of the Spatial visible hand-over (slot 1: RenderNode pointers, an ocean sector's
		// tagged SpatialTerrainTag_Ocean) pushes every node with its own pass mask - chunks and sectors
		// alike. Runs with the terrain disabled too (the walk still pushes the ocean's sectors).
		void render(Renderer& renderer, OceanGenerator& ocean);

		// THE VEGETATION STORED IN THE CHUNKS (Procedural TreeSystem's trees and bushes): a chunk holds the index of
		// its coordinate's vegetation (`lookup`; -1 = none), and render()'s walk draws it WITH the chunk - the same
		// visibility, the same pass mask (Main-stamped: every pass; the shadow/GI sphere: PASS_SHADOW | PASS_GI). The
		// walk merges a chunk drawn twice (a LOD handover's old + new resident: the masks OR'ed) and hands the frame's
		// list to `sink` at its end - ON THE WALK'S WORKER, before present (main.cpp joins it). Main thread; joins
		// the walk and re-stamps every resident. Empty functions = no vegetation.
		struct VegetationDraw { uint32 chunk; uint32 passMask; };
		void setVegetation(oc::function<int32(glm::ivec2)> lookup, oc::function<void(oc::span<const VegetationDraw>)> sink, uint32 numChunks);
		// One coordinate's vegetation changed (its owner added or removed it): re-stamps that coordinate's residents
		// (every LOD, the retired ones too) from `lookup`. Main thread, WITHOUT joining the walk: the walk sees the old
		// or the new index - the owner keeps a removed index unused until the next frame.
		void restampVegetation(glm::ivec2 coord);
		// This frame's render() routes the vegetation (the terrain draws its chunks): its owner submits nothing else.
		bool vegetationRouted() const { return m_vegRouted; }
		// Joins the render-push job render() kicked (the renderNode pushes run on a worker). main.cpp
		// calls it right before Renderer::present; update() and clearResidents() join it too before
		// they touch m_residents, so a caller never sees the map change under the job.
		void joinRender();
		// The chunk mesh upload (Renderer::createMesh: the staging copy into the mesh mega-buffers, which
		// can wait on staging fences and the per-frame shared-write GPU drain) runs on a job. update()
		// picks the chunks; main.cpp kicks the job right AFTER Renderer::present, so it overlaps the
		// frame-pacing wait, and joins it before the next frame's kicks (the begin-frame job and the
		// entity pass read the mesh tables it grows). The next update() spawns the nodes.
		void kickUploads(Renderer& renderer);
		void joinUploads();

		// The live height/climate field, for systems that must agree with the rendered terrain (the
		// ocean's shore-depth bake). nullptr while terrain rendering is disabled - consumers treat that
		// as "no terrain" rather than sampling a field that isn't drawn. Thread-safe; the shared_ptr
		// keeps the maps valid across config rebuilds (workers holding the old maps finish against them).
		oc::shared_ptr<const ITerrainSampler> activeClimateMaps() { return m_settings.enabled ? currentMaps() : nullptr; }
		// The ocean-reach rule this streamer bakes into the terrain-data map, for anything baking the SAME
		// fields off the same sampler (the ocean's shore map, which overrides that map inside its range).
		// Handed out rather than duplicated so the two cannot drift apart - see applyWaterReach.
		// nullptr = the rule is off; bake the sampler's water level as-is.
		const WaterReach* activeWaterReach() const { return m_settings.waterReachEnabled ? &m_settings.waterReach : nullptr; }
		// The flow-direction rule, handed out for the same reason (the ocean's shore map must bake the
		// SAME directions this streamer bakes into the terrain-data map's 8 flow bits, or the wave travel
		// direction would turn where one map hands over to the other). nullptr = flow bits carry only what
		// the generator authors. See applyFlowField.
		const FlowField* activeFlowField() const { return m_settings.flowFieldEnabled ? &m_settings.flowField : nullptr; }
		// The offshore heading the baked flow eases back into - the ocean's swell heading, pushed in by the
		// app each frame (the streamer cannot know it; a stale angle just re-bakes one map). See FlowField.
		void setFlowWindAngle(float radians) { m_settings.flowField.windAngle = radians; }
		// THE sea level datum for the world, tweak-backed here because the terrain generator builds its
		// heights around it (so it regenerates chunks) - the ocean floats on this rather than owning a
		// second copy. Valid even while terrain is disabled, so the ocean can still run on its own.
		float seaLevel() const { return m_settings.seaLevel; }
		// CPU copy of the ACTIVE baked terrain-data map (the same texels setFogTerrainHeightMap shipped
		// to the GPU) - the ocean samples it for buoyancy water depth/level and wind-steering flow votes.
		// nullptr while no bake has shipped / terrain is disabled; a re-bake swaps in a NEW object, so
		// consumers holding the old shared_ptr keep a coherent snapshot.
		oc::shared_ptr<const BakedTerrainData> activeTerrainData() const { return m_terrainMapData; }
		// The rivers' and lakes' calm water surface world Y round the camera, -FLT_MAX where none (RiverSystem's
		// sampleWaterHeight): the App folds it into the buoyancy with the ocean's. Worker-safe on the entity pass.
		float sampleInlandWaterHeight(float x, float z) const { return m_rivers.sampleWaterHeight(x, z); }
		bool hasInlandWater() const { return m_rivers.hasWater(); }

		// How far the ring around the camera has streamed in - the lobby's "seeding the world" bar. Main
		// thread (it reads the residency sets). `settled` is the whole ring resident, the terrain-data map
		// shipped and nothing in flight; before the first update after an enable it is false even though
		// nothing is pending yet, because nothing is resident either.
		struct StreamStatus
		{
			bool enabled = false;
			bool modelsReady = false;  // V3 models loaded (false while loading; see failed)
			bool failed = false;       // the model load failed: the world stays empty
			uint32 resident = 0;       // chunks live in the renderer
			uint32 pending = 0;        // chunks requested, generating or waiting for upload
			bool mapShipped = false;   // the terrain-data map (climate, water reach) is live
			bool settled() const { return enabled && modelsReady && pending == 0 && resident > 0 && mapShipped; }
			float progress() const { return resident + pending == 0 ? 0.0f : (float)resident / (float)(resident + pending); }
		};
		StreamStatus streamStatus() const;
		// The V3 world scale the streamer is configured with (the "Terrain/V3/Meters per pixel" tweak), so a
		// preview sampled outside the streamer maps its texels to the same world metres the mesh will use.
		float v3MetersPerPixel() const { return m_settings.v3MetersPerPixel; }
		// The ring's radius in chunks and the chunk size in metres ("Terrain/Range (chunks)" / "Chunk size
		// (m)"): the lobby sizes the ring to the seeded playable area and restores the radius after.
		int ringRadius() const { return m_settings.ringRadius; }
		int chunkSize() const { return m_settings.chunkSize; }
		// The GENERATED bounds (engine metres): the playable area the lobby's seeder pre-generated
		// full tiles for. While set, the ring never requests a chunk outside it, and the generator
		// (TerrainConfigV3::bounded) answers every full-detail sample outside it from the coarse stage
		// - so nothing past the area's edge ever costs a cold tile. Main thread; rebuilds the maps.
		void setGeneratedBounds(bool bounded, glm::vec2 boundsMin, glm::vec2 boundsMax)
		{
			m_bounded = bounded;
			m_boundsMin = boundsMin;
			m_boundsMax = boundsMax;
			m_configDirty = true;
		}
		// False when unbounded (outMin / outMax untouched).
		bool generatedBounds(glm::vec2& outMin, glm::vec2& outMax) const
		{
			outMin = m_boundsMin;
			outMax = m_boundsMax;
			return m_bounded;
		}

	private:
		// THE QUADTREE (see "Quadtree nodes" in the CONTEXT): a node (lod, coord at that lod) covers 2^lod x 2^lod BASE
		// chunks with the same lod0Res grid. Its LOD is a function of a camera and the bands: descend from maxLod and stop
		// at the first node with ringLodAt(its edge distance) >= its lod (leafLodAt). The ring adds the wanted area: the
		// leaves that touch the (2R + 1)^2 base chunks around the camera's chunk.
		struct Ring
		{
			glm::vec2 cam{ 0.0f };     // snapped camera, base-chunk units (a quarter-chunk lattice)
			glm::ivec2 camChunk{ 0 };  // the camera's base chunk
			int R = -1;                // the ring's Chebyshev radius in base chunks (-1: none yet)
			float fullRes = 0.0f, lodStep = 1.0f;
			uint32 maxLod = 0;
			bool valid = false;
			bool operator==(const Ring&) const = default;
			uint32 leafLodAt(glm::ivec2 base) const;               // MIRRORED by the terrain VS (terrainLeafLod)
			bool touches(glm::ivec2 coord, uint32 lod) const;      // the node overlaps the ring's base chunks
			bool isLeaf(glm::ivec2 coord, uint32 lod) const { return leafLodAt(coord * (1 << lod)) == lod; }
			float edgeDist(glm::ivec2 coord, uint32 lod) const;    // camera -> the node's footprint (Chebyshev, base chunks)
		};
		struct Request
		{
			uint64 key = 0;
			uint32 generation = 0;
			ChunkParams params;
			oc::shared_ptr<const ITerrainSampler> maps;
		};

		struct Result
		{
			uint64 key = 0;
			uint32 generation = 0;
			glm::ivec2 coord{ 0, 0 };
			uint32 lod = 0;
			// Built IN the pump job (the final vertex layout, pure): the main thread only uploads it
			// (Renderer::createMesh). Empty = dropped/failed; the key is released and update re-requests it
			// if still wanted (m_wantedDirty).
			RenderMeshData mesh;
		};

		// Heap-held (m_residents maps to a unique_ptr): the SpatialIndex hands the Resident* over as userData,
		// so the address must outlive the frame's hand-over - see retireResident.
		struct Resident
		{
			RenderMesh mesh;           // declared first -> destroyed AFTER the node that draws it
			glm::ivec2 coord{ 0, 0 };  // at its lod
			uint32 lod = 0;
			// The VEGETATION stored in this node: per BASE chunk it covers (4^lod, row-major), the index in the owner's
			// table (-1 = none), and how many are set (the walk skips a node without any). Atomic: restampVegetation
			// writes them on main while the walk reads them.
			oc::unique_ptr<oc::atomic<int32>[]> vegetation;
			oc::atomic<uint32> vegetationCount{ 0 };
			RenderNode node;
			SpatialEntry spatialEntry; // culling registration (SpatialLayer_Terrain, static; userData = this). Invalid = HELD
			uint32 registeredAt = 0;   // Spatial visibleCollectGeneration at registration: a Main stamp since = it was culled
			glm::ivec2 firstBase() const { return coord * (1 << lod); }
		};

		void pumpJob();                 // self-continuing Low-priority generation job
		void kickPump(size_t numNew);  // top pumps up to min(cap, new work) after appending requests
		// Disabled-path drain (no profile scope): runs the enable->disable transition, then keeps
		// dropping what is still in flight (late pump results, the terrain-data bake) until nothing is
		// left and m_disabledIdle parks update() entirely.
		void updateDisabled(Renderer& renderer, const Camera& camera);
		void rebuildMaps();                             // (re)builds the active generator from the tweak-backed config
		void clearResidents();
		// Pushes V3's crag scale every frame (the renderer reads the texture settings themselves), and registers
		// the texture set + climate boxes once the background DDS bake finishes (the TERRAIN shader falls back to
		// flat colors until then).
		void updateTerrainTextures(Renderer& renderer);
		void registerTerrainTextures(Renderer& renderer); // one-shot, from updateTerrainTextures
		oc::shared_ptr<const ITerrainSampler> currentMaps();
		// Bakes/refreshes the fog terrain height map around the camera (Renderer::setFogTerrainHeightMap);
		// maps == nullptr means "no terrain" and clears the map. See the .cpp for the bake scheme.
		void updateFogHeightMap(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps, float farRange);

		// --- The "Terrain*" tweaks (Settings.Terrain; source of truth: the generator/ChunkParams are built from these) ---
		TerrainSettings& m_settings = Globals::settings.terrain;
		// See setGeneratedBounds. Not tweaks: the session sets them with the seeded world.
		bool m_bounded = false;
		glm::vec2 m_boundsMin = glm::vec2(0.0f);
		glm::vec2 m_boundsMax = glm::vec2(0.0f);

		// --- The Terrain Diffusion generator. ONNX-model backed: it needs 2.28 GB of
		// weights on disk and a DirectML-capable GPU, and it generates 7.68 km tiles rather than evaluating
		// a point function. See Private/Diffusion/GeneratorV3.ixx.
		// V3 can't generate until its models are downloaded+loaded. Building chunks before then would bake a
		// flat sea-level world into the resident cache, so the streamer idles instead and rebuilds on ready.
		bool m_v3AwaitingModels = false;
		double m_v3LastStatusLog = 0.0; // rate-limits the download/load progress log

		bool m_configDirty = false; // set by the settings listeners (initialize); consumed at the top of update()
		bool m_disabledIdle = false; // disabled AND fully drained: update() is a branch and a return

		// --- Shared terrain-data map (fog terrain-following + regional thickness, ocean shore fallback,
		// terrain coloring; height / water level / fog|falloff|temp|hum / altitude per texel) ---
		// The bake's config (enabled, ranges, the ocean reach and flow-direction rules) is in m_settings. Ocean reach:
		// how close real ocean must be for water here to still be the sea (see applyWaterReach). Flow direction: which
		// way the water moves per texel - the wave-travel field at the coast (see applyFlowField).
		HeightMapBaker m_terrainMapBaker;
		bool  m_terrainMapUploaded = false;    // a map is live in the renderer (cleared on disable)
		oc::shared_ptr<const BakedTerrainData> m_terrainMapData; // CPU copy of the active bake (activeTerrainData)

		// --- Terrain splat textures: source images baked to BC .dds (Assets/Local/TerrainTex) on a
		// background thread at startup (skipped when the cache is fresh), then registered once ---
		JobCounter        m_texBakeCounter;         // one Low job (internally a parallelFor over conversions)
		bool              m_texBakeKicked = false;  // submitted once, and only while the terrain is enabled
		void kickTexBake();
		oc::atomic<bool> m_texBakeStop{ false };
		oc::atomic<bool> m_texBakeDone{ false };
		bool              m_texSetRegistered = false;

		// --- Threading: generation runs on up to "Gen jobs" Low-priority pump jobs; V3 waits
		// inside them park their fibers (several pumps joining one cold tile all proceed when it
		// lands, and warm-tile mesh builds overlap the cold-tile wait). Claim invariant: a pump
		// leaving decrements m_numPumps BEFORE its empty-recheck and re-claims a slot if requests
		// remain, and every append is followed by kickPump - so requests never strand. ---
		oc::atomic<int32>      m_numPumps{ 0 };
		JobCounter              m_pumpCounter;
		std::mutex              m_mutex;
		// An unordered POOL of outstanding work, despite the deque: the worker rescans it on every dequeue
		// and takes whichever chunk is nearest the camera THEN, so insertion order carries no meaning and
		// neither side sorts it. Ordering it would just re-decide, one camera position stale, what the
		// worker decides correctly at pick time - and FIFO is what made distant chunks generate before the
		// ground underfoot.
		oc::deque<Request>     m_requests;
		// The ring the worker judges queued requests against at DEQUEUE time (guarded by m_mutex): both which are stale
		// (no longer a wanted leaf: dropped lazily, in the same pass) and which is nearest. Published by update when the
		// ring moved, before any request of it is appended.
		Ring m_pumpRing;
		oc::vector<Result>     m_results;      // filled by worker, drained on the main thread
		oc::vector<Result>     m_readyBacklog; // main-thread only: generated chunks over the per-frame upload cap
		// The upload batch (see kickUploads): update() appends the picked results (they stay in m_pending),
		// the "terrainUpload" job turns each `data` into `mesh` and frees the data, and the next update()
		// adopts them after the join. A result with an invalid mesh and non-empty data is not uploaded yet.
		struct Upload
		{
			Result result;
			RenderMesh mesh;
		};
		oc::vector<Upload>     m_uploads;
		JobCounter             m_uploadCounter;
		oc::shared_ptr<const ITerrainSampler> m_maps;
		uint32                  m_generation = 0;
		// The river units around the camera (Docs/RiverPlan.md): handed the live generator by rebuildMaps.
		RiverSystem             m_rivers;

		// --- Main-thread residency state ---
		oc::unordered_map<uint64, oc::unique_ptr<Resident>> m_residents;
		oc::unordered_set<uint64>           m_pending; // requested/queued, not yet resident
		// Residents that left m_residents: node, mesh and culling entry released at once, the MEMORY
		// kept until the Spatial collect generation moves past `generation` - until then a hand-over
		// list may still hold &node (whose destroyed node the push skips). Freed at the top of update.
		struct RetiredResident { uint32 generation; oc::unique_ptr<Resident> resident; };
		oc::vector<RetiredResident>         m_retired;
		void retireResident(oc::unique_ptr<Resident> resident);
		uint16                              m_material = UINT16_MAX; // shared by every chunk (created at first upload)
		// The wanted leaves of m_ring (rebuilt when it moves; ~100-200 nodes) and the ring itself. Every per-frame decision
		// below walks these and the residents directly, on main: the node counts are small.
		Ring                                m_ring;
		oc::unordered_set<uint64>           m_wanted;
		oc::vector<uint64>                  m_wantedList;
		float                               m_wantedHalfExtent = 0.0f; // m: the farthest wanted node edge from the camera chunk
		bool                                m_wantedDirty = true;      // a request finished without a resident: re-request
		void rebuildWanted();
		bool insideBounds(glm::ivec2 coord, uint32 lod, float chunkSize) const; // the generated bounds (setGeneratedBounds)
		// EDGE STITCHING (no skirts; see "Edge stitching" in the CONTEXT): the terrain VS snaps each node edge onto the
		// coarser side, with BOTH sides' LODs computed from the DRAW CAMERA (a Ring: cam + bands), not the ring camera.
		// Invariant: a resident is registered (drawn) only while it lies inside a leaf of the draw camera's tree
		// (lod <= m_drawCam.leafLodAt(firstBase)), so the two nodes of an edge always agree on it. The draw camera
		// follows the ring camera as far as the registered residents allow (DRAW_CAM_STEPS points tried per frame).
		static constexpr uint32 DRAW_CAM_STEPS = 8;
		Ring                                m_drawCam;
		void updateResidency(bool gate);         // evict covered residents, move the draw camera, register the held
		bool coveredFor(const Resident& old, bool oldVisible, bool gate) const; // its area is drawn by wanted residents
		bool drawnReady(uint64 key, bool oldVisible, bool gate) const;
		void registerResident(Resident& resident); // its culling entry: from now on it draws
		// The render push runs on workers (see render / joinRender): the hand-over walk fans out over a
		// parallelFor, the sphere query runs beside it as a job of its own.
		JobCounter                          m_renderCounter;
		JobCost                             m_renderPushCost{ 250, "terrainRenderPushChunk" };
		bool                                m_renderReady = false; // update() ran enabled: render() pushes chunks
		// The vegetation (setVegetation): the owner's hooks, and the walk's per-frame merge (a mask per vegetation chunk,
		// the chunks touched, the list handed to the sink) - only the walk's jobs touch them between kick and join.
		// The pushes run in parallel: a mask is OR'ed through an atomic_ref, and the push that first sets one lists its
		// chunk in m_vegTouched (one slot per vegetation chunk, m_vegTouchedCount filled).
		oc::function<int32(glm::ivec2)>                         m_vegLookup;
		oc::function<void(oc::span<const VegetationDraw>)>      m_vegSink;
		oc::vector<uint8>                   m_vegMasks;
		oc::vector<uint32>                  m_vegTouched;
		oc::atomic<uint32>                  m_vegTouchedCount{ 0 };
		oc::vector<VegetationDraw>          m_vegDraws;
		bool                                m_vegRouted = false;
		oc::vector<Renderer::GrassGroundChunk> m_grassGround; // update's per-frame list for Renderer::setGrassGround (kept memory)
		void noteVegetation(const Resident& resident, uint32 passMask); // the walk job
		void flushVegetation();                                          // the walk job's end
		void stampVegetation(Resident& resident);                        // every base chunk's index from m_vegLookup
		oc::vector<uint64>                  m_scratchKeys; // updateResidency's resident walk (kept memory)
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::TerrainStreamer terrain;
}
