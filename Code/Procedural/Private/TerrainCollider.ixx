export module Procedural:TerrainCollider;

import Core;
import Core.glm;

import Physics;
import Threading;
import Settings;

import :TerrainSampler;

export namespace Procedural
{
	// Physics for the procedural terrain: a focus-centered ring of small static triangle-mesh collider
	// tiles, sampled from the SAME ITerrainSampler the terrain renders - heights are sampled on the render
	// LOD0 lattice with the render mesh's triangulation, so bodies rest exactly on the drawn surface.
	//
	// Kept performant by never touching the render chunks (a LOD0 chunk is ~500k triangles; a collider
	// only needs the ground near dynamic bodies): tiles are a few tens of meters, only exist within
	// "Radius" of the focus (the camera - thrown bodies start there), and each is built off the main
	// thread (sampleGrid + BVH build) with ONE build in flight, nearest-first. The per-frame body
	// create/destroy and tile scan run on the "Terrain collider" job, joined before the post-update
	// kick. Tiles clear and rebuild when the sampler identity changes (terrain regenerated), exactly
	// like the streamer's residents.
	class TerrainCollider
	{
	public:
		TerrainCollider() = default;
		TerrainCollider(const TerrainCollider&) = delete;
		TerrainCollider& operator=(const TerrainCollider&) = delete;
		// Wait out the in-flight jobs (main thread helps), then tiles destroy their
		// bodies/meshes - Globals::physics outlives any stack-local instance.
		~TerrainCollider()
		{
			Globals::jobSystem.wait(m_updateCounter);
			Globals::jobSystem.wait(m_buildCounter);
		}

		void initialize(void* terrainUserData); // attaches the "Terrain/Collision" listeners

		// Per frame, after TerrainStreamer::update. Kicks the update job. maps == nullptr (terrain
		// disabled, models still loading) clears every collider.
		// SEVERAL FOCUS POINTS (at most MaxFocus; the rest are dropped): a tile exists within "Radius" of ANY of them. The
		// game passes its players and its Base, so their ground stays under them while the camera flies somewhere else.
		// minRadius: the radius is at least this ("Radius" or it, the larger). The game passes the SIM LOD's reach: a body is
		// live that far from a focus, and a live body with no tile under it falls.
		static constexpr int MaxFocus = 8;
		void update(oc::span<const glm::vec3> focusPoints, oc::shared_ptr<const ITerrainSampler> maps, float minRadius = 0.0f);
		void update(const glm::vec3& focusPos, oc::shared_ptr<const ITerrainSampler> maps)
		{
			update(oc::span<const glm::vec3>(&focusPos, 1), oc::move(maps));
		}

		// Main thread, before kickPostUpdateJobs: the job creates/destroys box3d bodies, and the Sim
		// batch (the game's nav feed) and the next frame's main-thread physics users read the world.
		void joinUpdate() { Globals::jobSystem.wait(m_updateCounter); }

	private:
		void runUpdate();

		struct Tile
		{
			PhysicsMesh mesh; // declared first -> destroyed AFTER the body referencing it
			PhysicsBody body;
			glm::ivec2 coord{ 0, 0 };
		};

		struct BuildResult
		{
			uint64 key = 0;
			glm::ivec2 coord{ 0, 0 };
			uint32 generation = 0;
			PhysicsMesh mesh;
		};

		const TerrainColliderSettings& m_settings = Globals::settings.terrainCollider; // "Terrain/Collision"
		bool  m_configDirty = false; // geometry-affecting tweak changed: rebuild everything
		bool  m_inactiveIdle = false; // inactive AND cleared: update() only polls the in-flight build

		// The update job's inputs, written by update() on main before the kick. The config is a
		// snapshot so the job never reads a tweak member.
		JobCounter m_updateCounter;
		oc::shared_ptr<const ITerrainSampler> m_jobMaps;
		glm::vec2 m_jobFocus[MaxFocus]{};
		int m_jobFocusCount = 0;
		float m_jobTileSize = 32.0f;
		float m_jobRadius = 96.0f;
		float m_jobSpacing = 1.0f;
		float m_jobFriction = 0.8f;

		// ONE build in flight, submitted to the job system; the counter is the "future", the job
		// writes m_buildResult before signaling (single producer, consumed only after isDone).
		JobCounter m_buildCounter;
		BuildResult m_buildResult;
		bool m_buildInFlight = false;
		uint32 m_generation = 0;             // bumped on clear; stale in-flight results are dropped
		const ITerrainSampler* m_mapsIdentity = nullptr; // identity only (never dereferenced)
		oc::unordered_map<uint64, Tile> m_tiles;
		void* m_terrainUserData = nullptr;
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::TerrainCollider terrainCollider;
}
