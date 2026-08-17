export module Procedural:TerrainCollider;

import Core;
import Core.glm;

import Physics;
import Threading;

import :TerrainSampler;

export namespace Procedural
{
	// Physics for the procedural terrain: a focus-centered ring of small static triangle-mesh collider
	// tiles, sampled from the SAME ITerrainSampler the terrain renders — heights are sampled on the render
	// LOD0 lattice with the render mesh's triangulation, so bodies rest exactly on the drawn surface.
	//
	// Kept performant by never touching the render chunks (a LOD0 chunk is ~500k triangles; a collider
	// only needs the ground near dynamic bodies): tiles are a few tens of meters, only exist within
	// "Radius" of the focus (the camera — thrown bodies start there), and each is built off the main
	// thread (sampleGrid + BVH build) with ONE build in flight, nearest-first. The main thread only
	// creates/destroys the static bodies, which is cheap in box3d. Tiles clear and rebuild when the
	// sampler identity changes (terrain regenerated), exactly like the streamer's residents.
	class TerrainCollider
	{
	public:
		TerrainCollider() = default;
		TerrainCollider(const TerrainCollider&) = delete;
		TerrainCollider& operator=(const TerrainCollider&) = delete;
		// Wait out the in-flight build job (main thread helps), then tiles destroy their
		// bodies/meshes — Globals::physics outlives any stack-local instance.
		~TerrainCollider() { Globals::jobSystem.wait(m_buildCounter); }

		void initialize(void* terrainUserData); // registers the Tweaks ("Terrain/Collision")

		// Per frame, after TerrainStreamer::update. maps == nullptr (terrain disabled, models still
		// loading) clears every collider.
		void update(const glm::vec3& focusPos, oc::shared_ptr<const ITerrainSampler> maps);

	private:
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

		// --- Tweak-backed configuration ("Terrain/Collision") ---
		bool  m_enabled = true;
		float m_radius = 96.0f;   // world m around the focus that carries colliders
		float m_tileSize = 32.0f; // world m per collider tile
		float m_spacing = 1.0f;   // m between height samples (render LOD0 is chunkSize/lod0Res = 1 m)
		float m_friction = 0.8f;
		bool  m_configDirty = false; // geometry-affecting tweak changed: rebuild everything

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
