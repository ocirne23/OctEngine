export module Procedural:TreeWorld;

import Core;
import Core.glm;
import Core.Camera;

import Threading;
import RendererVK;

import :TerrainSampler;
import :TreeSpecies;
import :TreeGenerator;

// WORLD TREE PLACEMENT (Docs/TreeRenderingPlan.md 3.5, phase W1): EVERY tree in the terrain ring, stored as a 4-byte
// RECORD per terrain chunk - x, z and the species, nothing else. The height is the terrain's; variant, scale and yaw
// come from the record's hash (treeRecordSeed), so the same tree comes back wherever and whenever it is asked for.
// The records are a pure function of (seed, chunk, the species' Placement blocks, the sampler): a chunk that leaves
// the ring is dropped and generated again when it comes back. Generated on Low pump jobs, nearest chunk first, over
// the terrain's own ring (radius, chunk size, generated bounds). Trees only - the bushes come from the placement
// function at expansion (W3), not from records.
// W3: TreeSystem expands the near chunks' CPU records into its dynamic tree set (TreeSystem "world mode").
// W2: every chunk's records go to the GPU (Renderer::addTreeRecordChunk: one fixed-size device-local pool + a chunk
// table), within an upload budget per frame. The CPU keeps a chunk's records only inside "CPU keep radius" (the near
// chunks, which expansion needs); beyond it they are dropped once uploaded and generated again on the way back in.
export namespace Procedural
{
	// x, z: 12 bits each, chunk-local on a TREE_RECORD_STEPS lattice (the cell centre); type: 8 bits, the species'
	// index in TreeWorld's name-sorted .tree list. MIRRORED in Assets/Shaders/tree_record.inc.glsl - keep in step.
	struct TreeRecord
	{
		uint32 bits = 0;
	};
	static_assert(sizeof(TreeRecord) == sizeof(uint32)); // uploaded as uint32s
	constexpr uint32 TREE_RECORD_STEPS = 4096;

	constexpr TreeRecord makeTreeRecord(uint32 qx, uint32 qz, uint32 type)
	{
		return TreeRecord{ (qx & 0xFFFu) | ((qz & 0xFFFu) << 12) | (type << 24) };
	}
	constexpr uint32 treeRecordType(TreeRecord r) { return r.bits >> 24; }
	// The chunk-local position (m) of a record in a chunk of `chunkSize` metres.
	constexpr glm::vec2 treeRecordLocal(TreeRecord r, float chunkSize)
	{
		const float s = chunkSize / (float)TREE_RECORD_STEPS;
		return glm::vec2(((float)(r.bits & 0xFFFu) + 0.5f) * s, ((float)((r.bits >> 12) & 0xFFFu) + 0.5f) * s);
	}
	// The record's random stream (variant, scale, yaw: treeHash(this, salt)). Keyed by the chunk and the QUANTIZED
	// position, never by the record's index, so a change elsewhere in the chunk leaves this tree as it was.
	constexpr uint32 treeRecordSeed(uint32 worldSeed, glm::ivec2 chunk, TreeRecord r)
	{
		return treeHash(treeHash(worldSeed, (uint32)chunk.x), treeHash((uint32)chunk.y, r.bits & 0xFFFFFFu));
	}

	class TreeWorld
	{
	public:
		TreeWorld() = default;
		~TreeWorld(); // joins the pumps
		TreeWorld(const TreeWorld&) = delete;
		TreeWorld& operator=(const TreeWorld&) = delete;

		void initialize(); // "Trees/World" tweaks
		// Main thread, every frame (also with the tree preview off): the ring, the requests, the finished chunks.
		void update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps);

		bool enabled() const { return m_enabled; }
		uint32 seed() const { return (uint32)m_seed; }
		float chunkSize() const { return m_chunkSize; }
		// The species name of a record type ("" = none).
		oc::string_view typeName(uint32 type) const;
		// Bumped by every restart (all records regenerated): what was built from the old records is stale.
		uint32 generation() const { return m_generation; }
		// Main thread: a chunk's records, when the CPU holds them (inside the keep radius); nullptr otherwise.
		const oc::vector<TreeRecord>* cpuRecords(glm::ivec2 coord) const;
		// The keep radius is at least this (the expansion's near radius needs the records).
		void requireKeepRadius(int chunks)
		{
			if (chunks != m_minKeepRadius)
				m_ringCam = glm::ivec2(INT32_MAX); // rescan: the chunks inside it get their CPU records back
			m_minKeepRadius = chunks;
		}

	private:
		struct Species
		{
			oc::string name;
			uint8 type = 0;
			TreePlacementDesc placement;
			// The ideal climate box in the normalized (temperature, precipitation) space.
			glm::vec2 climateMin{ 0.0f };
			glm::vec2 climateMax{ 1.0f };
		};
		// Everything a chunk's records are a function of: immutable, shared with the pumps.
		struct GenConfig
		{
			oc::shared_ptr<const ITerrainSampler> maps;
			oc::vector<Species> species; // the placed ones (density > 0, trees only)
			uint32 seed = 1;
			uint32 generation = 0;
			float chunkSize = 256.0f;
			float cellSize = 5.0f;       // the candidate lattice (m): at most one tree per cell
			float densityScale = 1.0f;
			float sharpness = 4.0f;      // the species pick: climate fit ^ this
			float fadeStart = 0.10f;     // a species' fit fades to 0 between these fractions of its peak
			float fadeEnd = 0.25f;
		};
		struct Request
		{
			glm::ivec2 coord{ 0 };
			uint32 generation = 0;
		};
		struct Result
		{
			glm::ivec2 coord{ 0 };
			uint32 generation = 0;
			bool dropped = false; // out of the ring when its pump got to it
			oc::vector<TreeRecord> records;
			oc::vector<uint32> ground; // TREE_RECORD_HEIGHT_WORDS (RendererVK TreeRecordPool): the far volume's ground
		};

		struct Chunk
		{
			oc::vector<TreeRecord> records; // empty unless hasCpu
			oc::vector<uint32> ground;      // until uploaded
			uint32 count = 0;               // its trees (the CPU may not hold them)
			uint32 gpu = UINT32_MAX;        // its pool handle (UINT32_MAX: not uploaded, no trees, or no room)
			bool hasCpu = true;
			bool uploaded = false;          // went through the upload queue
		};

		// The chunk's records, and its GROUND for the far volume (its height grid, TreeRecordPool's encoding).
		static void placeChunk(const GenConfig& config, glm::ivec2 coord, oc::vector<TreeRecord>& out, oc::vector<uint32>& outGround);
		void loadSpecies();
		void restart(Renderer& renderer, const oc::shared_ptr<const ITerrainSampler>& maps); // a new generation: drops every chunk
		void clear(Renderer& renderer, uint64 poolBytes);
		void rescanRing(Renderer& renderer);
		void uploadChunks(Renderer& renderer);
		bool insideKeepRadius(glm::ivec2 coord) const;
		void dropCpu(Chunk& chunk);
		void kickPump(size_t numNew);
		void pumpJob();
		void logStats(const Renderer& renderer) const;

		// --- Tweaks ---
		bool m_enabled = true;
		int m_seed = 1;
		float m_cellSize = 5.0f;
		float m_densityScale = 1.0f;
		float m_climateSharpness = 4.0f; // the species pick by climate fit ^ this (0 = every fitting species alike)
		float m_climateFadeStart = 0.10f; // a species' climate fit fades to 0 between these (fractions of its peak): no tail
		float m_climateFadeEnd = 0.25f;
		int m_maxGenJobs = 2;
		int m_poolMB = 128;         // the GPU record pool (MB; fixed - a change drains the GPU and regenerates): records + ~0.6 KB of ground per chunk
		int m_uploadKB = 1024;      // record bytes uploaded per frame
		int m_keepRadius = 4;       // chunks (Chebyshev) whose records the CPU keeps
		int m_minKeepRadius = 0;    // requireKeepRadius
		bool m_reloadSpecies = false; // button: re-read the Placement blocks, regenerate
		bool m_logStats = false;      // button
		bool m_configDirty = true;

		// Main thread.
		oc::vector<oc::string> m_typeNames;  // every .tree, name-sorted: the record type is the index
		oc::vector<Species> m_species;
		bool m_speciesLoaded = false;
		oc::shared_ptr<const GenConfig> m_config;
		uint32 m_generation = 0; // monotonic: a late pump result of an older config never matches
		const ITerrainSampler* m_lastMaps = nullptr;
		float m_chunkSize = 256.0f;
		int m_ringR = 0;
		int m_configRingR = -1; // the ring radius the pool's chunk map was sized for
		glm::ivec2 m_ringCam{ INT32_MAX };
		bool m_bounded = false;
		glm::vec2 m_boundsMin{ 0.0f }, m_boundsMax{ 0.0f };
		oc::unordered_map<uint64, Chunk> m_chunks;
		oc::unordered_set<uint64> m_inFlight; // requested, not merged yet
		oc::deque<uint64> m_uploadQueue;      // generated, not uploaded yet (merge order: nearest first)
		size_t m_numRecords = 0;              // every chunk's trees
		size_t m_cpuRecords = 0;              // the ones the CPU holds
		uint32 m_poolRefusedLogged = 0;

		// Shared with the pumps (m_mutex).
		mutable std::mutex m_mutex;
		oc::deque<Request> m_requests;          // nearest first (re-sorted when the ring moves)
		oc::vector<Result> m_results;
		oc::shared_ptr<const GenConfig> m_pumpConfig;
		glm::ivec2 m_pumpRingCam{ 0 };
		int m_pumpRingR = 0;
		oc::atomic<int32> m_numPumps{ 0 };
		JobCounter m_pumpCounter;
	};
}
