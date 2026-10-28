export module Procedural:RiverSystem;

import Core;
import Core.glm;
import Core.Camera;
import RendererVK;
import Threading;
import :RiverUnits;
import :RiverTerrain;

// The river WATER and the debug view (Docs/Context/Procedural/Rivers.md), owned by TerrainStreamer (no global of its own). It
// reads the units from the live RiverTerrain's store - the same units the sampler carves - pulling the ones around the
// camera ONE at a time on a Low job (nearest first; a cold unit fetches its tiles, so the wait parks the fiber).
//   * THE WATER ("Terrain/Rivers/Surface"): per unit its WATER TILES (one per terrain base chunk, built on the pull job,
//     uploaded on main) with a RIBBON per perennial segment at the carved water surface (a little wider than the channel:
//     the carved bank hides the rest) and a quad per lake row run at the lake's level (one pixel wider all round: the
//     ground cuts the shoreline). Drawn by RendererVK's River variant (EPipelineIndex::River), main pass only. Each tile
//     (and each near cell) rides the TERRAIN'S CULLING: TerrainStreamer's hand-over walk hands drawChunks the chunks it
//     drew in the main pass, and the tiles over them go with them; with culling off update pushes every tile itself.
//   * THE NEAR CELLS ("Near radius"): c_nearCell-metre squares around the camera, each ONE dense mesh of every river
//     crossing it (rows "Near spacing" apart, "Near across" vertices wide; the normal's x = 1 marks it dense), built on
//     their own Low job from the resident units, nearest first. The VS displaces them by the river's waves (the ocean's
//     FFT field, river_wave.inc.glsl) and sinks the light ribbons under them. A unit adopted or evicted makes every cell
//     STALE: it keeps its mesh until its rebuild lands.
//   * THE DEBUG LINES ("Terrain/Rivers/Debug lines"): channels by discharge (blue), ephemeral beds (tan), rapids
//     (orange), falls (red), the channel's edges (white) and the carved bed's outer edge (dim green), outlet / inlet
//     crossings (magenta / green ticks), lakes (row hatching: blue full, teal terminal, white pans).
export namespace Procedural
{
	class RiverSystem
	{
	public:
		RiverSystem() = default;
		~RiverSystem();
		RiverSystem(const RiverSystem&) = delete;
		RiverSystem& operator=(const RiverSystem&) = delete;

		// Main thread. The live river sampler (nullptr = no rivers). A new one drops the units - they reload from its store.
		void setTerrain(oc::shared_ptr<const RiverTerrain> terrain);
		// Main thread, every enabled terrain frame (after beginFrame; after TerrainStreamer joined its render walk).
		void update(Renderer& renderer, const Camera& camera, float chunkSize);
		// TerrainStreamer's render walk (ON ITS WORKER, between update and the walk's join): the terrain chunks it drew in
		// the MAIN pass - each a square of `span` x `span` base chunks from `firstBase`; the water tiles over them go to the
		// renderer (their own pass mask: 0 past "Radius"). The tiles only change in update, after that join.
		struct ChunkDraw { glm::ivec2 firstBase; uint32 span; };
		void drawChunks(Renderer& renderer, oc::span<const ChunkDraw> chunks);
		// Culling off (TerrainStreamer::render's gate): update pushes every tile itself.
		static bool cullingGate();

		// THE BUOYANCY'S WATER (the App folds it into PhysicsWorld::setWaterSurface with the ocean's): the river / lake
		// surface world Y at (x, z) without waves - the inland water map's CPU copy (nearest texel, the calm carved surface in
		// a channel, a lake's level), RIVER_WATER_MAP_DIM^2 texels round the camera; -FLT_MAX where there is none. A const
		// read for the entity pass's workers: the copy is swapped on main in update, after that pass has joined.
		float sampleWaterHeight(float x, float z) const;
		bool hasWater() const { return !m_waterHeights.empty(); }

		// A WHITEWATER point for the mist (Renderer::setRiverMistSources), unit-local engine m like the water mesh.
		struct MistPoint
		{
			glm::vec3 pos;      // on the water surface (y above sea level)
			float half = 0.0f;  // the channel's half-width
			glm::vec2 flow;     // the flow's velocity in XZ (m/s)
			float foam = 0.0f;  // the whitewater 0..1
			float depth = 0.0f; // the channel's depth (the river's size: "Full size depth")
			float len = 0.0f;   // the stretch of river it stands for
			float plunge = 0.0f; // m: a fall's drop ending here (its plunge pool's mist: len and foam set in updateMist), 0 = whitewater
		};
		// One water tile's mesh as the pull job builds it: its terrain base chunk, unit-local engine m.
		struct TileMesh
		{
			glm::ivec2 chunk{ 0 };
			RenderMeshData mesh;
		};

	private:
		struct Line
		{
			glm::vec3 a, b;
			uint32 color;
		};
		// A WATER TILE: one mesh + node over ONE terrain base chunk (its coordinate: floor(world / "Chunk size")). It rides
		// the terrain's culling: drawChunks pushes the tiles of the chunks the terrain's hand-over walk drew.
		struct WaterTile
		{
			RenderMesh mesh;          // BEFORE the node: the node is destroyed first
			RenderNode node;
			glm::ivec2 chunk{ 0 };
			uint32 drawnWalk = 0;     // the walk that last pushed it (a chunk drawn twice: a LOD handover's old + new)
		};
		struct Resident
		{
			oc::shared_ptr<const PreparedRiverUnit> unit;
			oc::vector<Line> lines; // engine space, built when the debug lines first need them
			bool linesBuilt = false;
			oc::vector<MistPoint> mist; // built with the water mesh
			oc::vector<oc::unique_ptr<WaterTile>> tiles; // the unit's water, one per base chunk it has water in
		};
		struct Job
		{
			oc::shared_ptr<const RiverTerrain> terrain;
			int32 ui = 0, uj = 0;
			bool buildSurface = false;
			glm::vec2 origin{ 0.0f }; // the unit's origin, world XZ
			float chunkSize = 256.0f; // the terrain's base chunk
			oc::shared_ptr<const PreparedRiverUnit> result;
			oc::vector<TileMesh> meshes;
			oc::vector<MistPoint> mist;
		};

		struct NearCell
		{
			uint32 generation = 0;  // the unit generation it was built from
			oc::unique_ptr<WaterTile> tile;
		};
		struct NearJob
		{
			oc::shared_ptr<const RiverTerrain> terrain;
			int32 cx = 0, cz = 0;
			uint32 generation = 0;
			// The resident units near the cell: their rivers, and their origin minus the cell's (engine m).
			oc::vector<oc::pair<oc::shared_ptr<const PreparedRiverUnit>, glm::vec2>> units;
			float spacing = 0.6f;
			int32 across = 12;
			bool hasMesh = false;
			RenderMeshData mesh; // cell-local engine m; the node sits at the cell's origin, at sea level
		};

		void buildLines(Resident& r) const;
		// Engine-space distance from the camera to the unit's square (0 inside).
		float unitDistance(int32 ui, int32 uj, glm::vec2 camera) const;
		glm::vec3 unitOrigin(int32 ui, int32 uj) const; // engine space, at sea level
		void updateNearCells(Renderer& renderer, glm::vec2 camera);
		void dropNearCells();
		// A tile from its mesh: node at `origin`, main pass only, listed under its chunk (nullptr: no mesh).
		oc::unique_ptr<WaterTile> spawnTile(Renderer& renderer, const RenderMeshData& data, glm::vec3 origin, glm::ivec2 chunk);
		void dropTile(oc::unique_ptr<WaterTile> tile); // unlisted and destroyed (main, outside the walk)
		void dropUnit(Resident& r);
		void dropUnits();
		// Base chunk -> the tiles over it (one per unit there, and a near cell's).
		oc::unordered_map<uint64, oc::small_vector<WaterTile*, 2>> m_tilesByChunk;
		float m_chunkSizeWas = 0.0f; // the terrain's "Chunk size" the tiles were cut to: a change re-pulls
		uint32 m_walk = 0;           // drawChunks' counter (the walk's own)

		oc::shared_ptr<const RiverTerrain> m_terrain;
		RenderMaterial m_material; // the River pipeline's one material (RenderMaterial::createForMesh); declared before the tiles that draw it
		oc::unordered_map<uint64, Resident> m_units;
		oc::shared_ptr<Job> m_job;
		JobCounter m_counter;
		bool m_surfaceWas = false;      // a "Surface" toggle re-pulls the units (they need their meshes)

		oc::unordered_map<uint64, NearCell> m_nearCells;
		oc::shared_ptr<NearJob> m_nearJob;
		JobCounter m_nearCounter;
		uint32 m_unitGeneration = 1;    // bumped on every unit adopted or evicted: the near cells go stale
		float m_nearSpacingWas = 0.0f;  // a change of the near mesh settings rebuilds the cells
		int32 m_nearAcrossWas = 0;

		// THE INLAND WATER MAP (Renderer::setRiverWaterMap: the fog's underwater boundary and the wetness under rivers and
		// lakes): RIVER_WATER_MAP_DIM^2 surface heights around the camera (RiverTerrain::sampleInlandWaterGrid), baked on
		// a Low job when the camera has moved past c_waterMapMove from the last bake's centre or the units changed.
		struct WaterMapJob
		{
			oc::shared_ptr<const RiverTerrain> terrain;
			glm::vec2 centre{ 0.0f };
			glm::vec2 origin{ 0.0f };
			uint32 generation = 0;
			oc::vector<float> heights;
		};
		void updateWaterMap(Renderer& renderer, glm::vec2 camera);

		// THE MIST SOURCES (Renderer::setRiverMistSources): the resident units' whitewater points within "Mist radius",
		// nearest first, re-sent when the camera has moved c_mistMove from the last send or the units changed.
		void updateMist(Renderer& renderer, glm::vec2 camera, bool clear);
		glm::vec2 m_mistCentre{ 1.0e30f };
		uint32 m_mistGeneration = 0;
		float m_mistRadiusWas = 0.0f;
		float m_mistFullSizeWas = 0.0f;
		float m_mistSizeWeightWas = 0.0f;
		float m_mistPlungeWas = 0.0f;
		bool m_mistSet = false;
		oc::shared_ptr<WaterMapJob> m_waterJob;
		JobCounter m_waterCounter;
		glm::vec2 m_waterCentre{ 1.0e30f };
		uint32 m_waterGeneration = 0;
		bool m_waterMapSet = false;
		oc::vector<float> m_waterHeights; // the last bake's heights (sampleWaterHeight; empty = none)
		glm::vec2 m_waterOrigin{ 0.0f };
	};
}
