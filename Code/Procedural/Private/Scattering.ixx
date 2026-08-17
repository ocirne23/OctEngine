export module Procedural:Scattering;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;

import RendererVK;
import Spatial;
import Threading;

import :TerrainSampler;

export namespace Procedural
{
	// One scatterable model: how it looks and how it occupies ground. Assets are shared — any number of
	// rules can reference one by name. Configured in the SCATTER_ASSETS table (Scattering.cpp).
	struct ScatterAsset
	{
		const char* name = "";
		const char* ocPath = "";           // .oc file relative to Assets/ — the model path and import options
		                                   // (MergeNodes/PreTransformVertices/DecimationFactor) come from it,
		                                   // so scatter and World share one cooked .vsc per model
		oc::vector<const char*> nodes;    // spawnable node paths in the model — each placement picks one
		                                   // at random (variants); empty = spawn the model root
		float footprintRadius = 1.0f;      // ground exclusion radius (m at scale 1): no other footprinted
		                                   // instance may overlap it. 0 = never blocks/blocked (cheap grass)
		float minScale = 0.8f;             // uniform random scale range
		float maxScale = 1.3f;
		float slopeAlign = 0.0f;           // 0 = always upright, 1 = up-axis fully follows the terrain normal
		float sinkDepth = 0.08f;           // m (x scale) embedded below the surface — hides floating edges on slopes
	};

	// One asset's distribution in one climate — add as many rules as you like, and reference one asset from
	// any number of them. Density falls off smoothly with the local climate's distance to the rule's
	// attractor (the same (t, h) space the terrain's textures are picked in), so scatter borders are soft
	// and track the ground they stand on rather than snapping at a classification boundary.
	// Configured in the SCATTER_RULES table (Scattering.cpp).
	struct ScatterRule
	{
		// The climate this asset likes, in REAL units (mean annual temperature C / annual precipitation
		// mm/yr) — the same units the terrain's texture table uses. Density falls off as a Gaussian around
		// it, so a rule is an attractor in climate space rather than a member of a named region.
		// It named a biome enum once. That enum belonged to the old noise generator and went with it, but
		// the indirection was worth losing on its own: a rule now says what climate it wants instead of
		// pointing at a table entry that says it somewhere else.
		float temperatureC = 9.0f;
		float precipMm = 990.0f;
		const char* asset = "";
		float density = 20.0f;        // instances per hectare (100x100 m) at full climate weight
		float viewDistance = 400.0f;  // spawn range (m): instances exist only within this camera distance
		float climateWidth = 0.12f;   // sigma of the density falloff around the attractor, in climate space
		float clusterSize = 0.0f;     // patch feature size (m); 0 = even coverage
		float clusterCoverage = 0.5f; // ~fraction of the region covered by patches (clusterSize > 0 only)
		float maxSlope = 0.7f;        // reject ground steeper than this (rise/run)
		float minAltitude = 1.5f;     // placement band, m above the local water level (keeps things off beaches)
		float maxAltitude = FLT_MAX;
	};

	// Scatters objects (trees/rocks/plants) over the procedural terrain: a bounded ring of scatter cells
	// follows the camera, each cell's placements computed deterministically on a worker thread from the
	// SAME ITerrainSampler the terrain renders from (height/water/climate agree with the ground by
	// construction), then bridged to RenderNodes on the main thread. Placements are a pure function of
	// (cell, seed, config) — revisiting a cell always reproduces the identical instances. Per-rule view
	// distances spawn/despawn each cell's instance groups as the camera moves (trees carry far, pebbles
	// near) without regenerating the cell.
	class ScatterSystem
	{
	public:
		ScatterSystem() = default;
		~ScatterSystem();
		ScatterSystem(const ScatterSystem&) = delete;
		ScatterSystem& operator=(const ScatterSystem&) = delete;

		void initialize();  // registers Tweaks, loads the asset table's containers, starts the worker
		// Per frame, after terrain.update: maps = terrain.activeClimateMaps() — scatter follows the live
		// terrain field, clears itself while terrain is disabled, and regenerates when the field changes.
		void update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps);

	private:
		// One placed instance, produced by the worker (variantIdx indexes ScatterAsset::nodes).
		struct Placed
		{
			glm::vec3 pos;
			glm::quat rot;
			float scale;
			uint16 assetIdx;
			uint16 variantIdx;
		};
		struct GroupResult
		{
			uint16 ruleIdx;
			oc::vector<Placed> instances;
		};

		// Tweak-backed generation parameters, snapshotted into each request (the worker never reads members).
		struct GenParams
		{
			float cellSize;
			float densityScale;
			uint32 seed;
		};

		struct Request
		{
			uint64 key = 0;
			uint32 generation = 0;
			glm::ivec2 coord{ 0, 0 };
			GenParams params;
			oc::shared_ptr<const ITerrainSampler> maps;
		};

		struct Result
		{
			uint64 key = 0;
			uint32 generation = 0;
			glm::ivec2 coord{ 0, 0 };
			bool dropped = false;               // stale at dequeue: main thread just releases the pending key
			oc::vector<GroupResult> groups;
		};

		// A resident cell's instances for one rule: nodes exist only while the cell is inside the rule's
		// view distance (spawn/despawn toggles with camera distance; the placements themselves persist).
		struct RuleGroup
		{
			uint16 ruleIdx = 0;
			oc::vector<Placed> instances;
			oc::vector<RenderNode> nodes;
			SpatialEntry spatialEntry;          // culling registration while spawned (SpatialLayer_Terrain)
		};

		// Per-asset runtime: the shared container + resolved spawn idx per variant (invalid assets warn
		// once at initialize and their rules drop out).
		struct AssetRuntime
		{
			oc::unique_ptr<ObjectContainer> container;
			oc::vector<NodeSpawnIdx> variants;
		};

		void pumpJob();                // self-continuing Low-priority generation job
		void kickPump(size_t numNew); // top pumps up to min(cap, new work) after appending requests
		void clearResidents();
		void spawnGroup(RuleGroup& group);
		void despawnGroup(RuleGroup& group);

		// --- Tweak-backed configuration ---
		bool  m_enabled = false;
		int   m_seed = 777;
		float m_cellSize = 64.0f;
		float m_densityScale = 1.0f;   // global multiplier on every rule's density
		float m_viewScale = 1.0f;      // global multiplier on every rule's view distance
		int   m_maxSpawnsPerFrame = 768; // instance node spawns per frame (group activations spread out)
		bool  m_configDirty = false;

		// --- Assets (loaded once at initialize; indices match the SCATTER_ASSETS table) ---
		oc::vector<AssetRuntime> m_assets;

		// --- Per-rule runtime, resolved at initialize (read-only afterwards, shared with the worker):
		// table asset index (UINT16_MAX = rule dropped), variant count, and the rule attractor's
		// coordinates in climate space. m_ruleOrder is the generation order — valid rules sorted by
		// footprint descending, so large objects claim ground before small ones fill the gaps.
		struct RuleRuntime
		{
			uint16 assetIdx = UINT16_MAX;
			uint16 numVariants = 1;
			glm::vec2 climateCoords{ 0.5f, 0.5f };
		};
		oc::vector<RuleRuntime> m_rules;
		oc::vector<uint16> m_ruleOrder;
		float m_maxViewDistance = 0.0f;

		// The placement function itself — pure, worker-thread; see the definition for the filter chain.
		static void generateCell(const ITerrainSampler& maps, glm::ivec2 coord, const GenParams& params,
			oc::span<const RuleRuntime> ruleRt, oc::span<const uint16> order, oc::vector<GroupResult>& outGroups);

		// --- Threading (mirrors TerrainStreamer: up to m_maxGenJobs Low-priority pump jobs, lazy
		// staleness at dequeue against published ring state; same claim/exit-recheck protocol.
		// generateCell is a pure function of const rule tables, so concurrent pumps are safe) ---
		oc::atomic<int32>      m_numPumps{ 0 };
		int                     m_maxGenJobs = 2;
		JobCounter              m_pumpCounter;
		std::mutex              m_mutex;
		oc::deque<Request>     m_requests;
		oc::vector<Result>     m_results;
		glm::ivec2              m_ringCam{ 0, 0 };
		int                     m_ringR = -1;
		uint32                  m_generation = 0;

		// --- Main-thread residency state. Cell keys pack the coord (cellKey/cellCoord), so anything that
		// only needs a coordinate range enumerates coords and hash-probes instead of walking a map. Groups
		// live in per-rule maps so the spawn scan probes only the cells inside each rule's OWN view-
		// distance ring — never the full residency ring, which is sized by the largest rule.
		oc::unordered_set<uint64> m_residentCells; // every generated ring cell, incl. placement-less ones
		oc::vector<oc::unordered_map<uint64, RuleGroup>> m_ruleGroups; // [ruleIdx][cellKey]
		oc::unordered_set<uint64> m_pending;

		// Spawned groups only: the per-frame despawn check and render push walk this instead of any map.
		struct ActiveGroup { uint64 key; uint16 ruleIdx; };
		oc::vector<ActiveGroup> m_active;
		bool m_ringDirty = true;   // rerun the enqueue scan without ring movement (reset, dropped result)
		bool m_spawnScan = true;   // rerun the spawn scan (admissions, over-budget deferrals, reset)
		glm::vec2 m_lastScanPos{ FLT_MAX, FLT_MAX };
		float m_lastScanViewScale = -1.0f;
		const ITerrainSampler* m_lastMaps = nullptr; // identity only (never dereferenced): detects terrain rebuilds
		int m_lastRingCX = INT_MIN;
		int m_lastRingCZ = INT_MIN;
		int m_lastRingR = -1;
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::ScatterSystem scatter;
}
