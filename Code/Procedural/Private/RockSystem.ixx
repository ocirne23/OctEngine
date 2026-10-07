export module Procedural:RockSystem;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;

import RendererVK;
import Threading;
import Settings;

import :TerrainSampler;
import :RockType;
import :RockGenerator;

// Procedural rocks ("Rocks" tweaks; Docs/RockRenderingPlan.md). Loads every Assets/Rocks/*.rock type and generates its
// variants (an SDF each, meshed by surface nets into a regular mesh LOD chain) for the WORLD (TreeWorld's rock records,
// drawn through TreeSystem's world set) and, on request ("Show preview"), a PREVIEW field in front of the camera - one
// row per type, one rock per variant - as plain RenderNodes. Both draw on the rock material (RendererVK
// EPipelineIndex::LitRock); the GPU picks each rock's LOD level (the "LOD" tweaks).
export namespace Procedural
{
	class RockSystem
	{
	public:
		RockSystem() = default;
		~RockSystem();
		RockSystem(const RockSystem&) = delete;
		RockSystem& operator=(const RockSystem&) = delete;

		void initialize(); // attaches the "Rocks" listeners
		// Per frame, after trees.update. `maps` places the preview on the terrain (y = 0 without it).
		void update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps);

		// --- THE WORLD's view (TreeSystem's world mode; Docs/RockRenderingPlan.md 6) - MAIN THREAD ---
		// TreeWorld places rock RECORDS from the .rock Placement blocks; TreeSystem expands them into its instance set
		// with these meshes. A loaded variant: its LOD chain's level 0 (the set's culls pick the level) and its
		// rock-local bounds.
		struct WorldVariant
		{
			const RenderMesh* mesh = nullptr;
			glm::vec3 centre{ 0.0f };
			float radius = 0.0f;
			float height = 1.0f; // nominal: the body's top above its lowest point
			// The far volume's occupancy grid (RockVariant::density: ROCK_DENSITY_RES^3 over the box, rock-local).
			const float* density = nullptr;
			glm::vec3 densityMin{ 0.0f };
			glm::vec3 densityMax{ 0.0f };
		};
		struct WorldType
		{
			oc::string name;
			glm::vec2 scale{ 1.0f };
			float sink = 0.0f;
			float align = 0.0f;
			float footprint = 1.0f;       // RockFootprint::flat: groundTransform's footprint x FOOTPRINT x the scale
			uint16 material = 0;          // the instances' (a wood type's is its bark texture: the rock shader samples it)
			bool wood = false;            // `Surface Wood`
			glm::vec3 woodAlbedo{ 0.0f }; // linear: the far volume's colour of a wood type (its bark texture's mean)
			oc::vector<WorldVariant> variants;
		};
		// Rock records are wanted ("Rocks/Enabled" + "Rocks/World/Enabled"); the meshes may still be generating.
		bool worldEnabled() const { return m_settings.enabled && m_settings.worldEnabled; }
		const RockWorldDesc& worldRules() const { return m_settings.worldRules; }
		// Bumped by "Reload types": the .rock files were read again, so TreeWorld reads their Placement blocks again.
		uint32 typesRevision() const { return m_typesRevision; }
		// The types with their meshes uploaded: empty until the load finishes, and after every clear. Valid until
		// worldGeneration() changes.
		oc::span<const WorldType> worldTypes() const { return m_worldTypes; }
		uint32 worldGeneration() const { return m_worldGeneration; }
		// Called right BEFORE the world types' meshes are freed (a reload, a disable): the user drops everything that
		// draws them. Never called from the destructor.
		void setMeshUser(oc::function<void()> beforeFree) { m_beforeFree = oc::move(beforeFree); }

		// A ROCK ON THE GROUND - the one rule for the preview and the world (TreeSystem::expandChunk): the rock leans
		// from upright toward the ground's NORMAL by `align` (1 = it lies on its slope), stands on the ground under its
		// footprint and is sunk by `sink` of its height along its own up axis. `ground`: the terrain height at p, then at
		// p -/+ (r, 0) and p -/+ (0, r) with r = FOOTPRINT x scale x the type's footprint (RockFootprint::flat: a snag's
		// is its thin base, not its height). Pure.
		static constexpr float FOOTPRINT = 0.35f;
		static Transform groundTransform(glm::vec2 p, const float ground[5], float r, float scale, float height, float sink, float align, const glm::quat& yaw);

	private:
		// The uploaded LOD chain: nodes spawn on level 0 and the cull redirects each instance (UINT32_MAX = one level
		// only, no chain). The chain is freed before its meshes (clearAll).
		struct Variant
		{
			RockVariant data;
			RenderMesh lods[ROCK_MAX_LODS];
			uint32 chain = UINT32_MAX;
		};
		struct Type
		{
			RockTypeDesc desc;
			oc::vector<Variant> variants;
			// A WOOD type's material: its `Bark` species' bark texture x its tint (BC1 albedo + BC5 normal map), which the
			// rock shader maps around the wood (RendererVK "The rock material", WOOD). Its job (kickGeneration) fills the
			// chains, finishLoad uploads and drops them. UINT16_MAX = none (a rock: m_material). Kept over a remesh.
			uint16 woodMaterial = UINT16_MAX;
			oc::vector<oc::vector<uint8>> barkAlbedo, barkNormal;
			uint32 barkSize = 0;          // 0 = no bark loaded
			glm::vec3 barkMean{ 0.1f };   // linear: the tinted texture's mean (WorldType::woodAlbedo)
		};

		// Reads the .rock files (main) and kicks one generation job per variant (and one per wood type's bark): no
		// main-thread stall. finishLoad uploads the meshes once every job is done.
		void reload();
		void kickGeneration(); // one Low job per variant of every loaded type, one per wood type without its material
		void finishLoad(Renderer& renderer);
		void spawnPreview(Renderer& renderer, const Camera& camera, const ITerrainSampler* maps);
		// Joins the generation jobs, then frees the nodes, the LOD chains and the meshes, in that order; the types stay
		// loaded (their variants empty, ready for kickGeneration) and keep their wood materials.
		void clearMeshes();
		void clearAll(); // clearMeshes, then the wood materials and the types too

		RockSettings& m_settings = Globals::settings.rockSystem; // "Rocks" (the buttons are cleared here)
		bool m_remesh = false;    // "Grid resolution" changed: regenerate the meshes of the loaded types (no re-read)

		bool m_loaded = false;     // the types are read (generation may still run)
		bool m_generating = false; // generation jobs kicked, meshes not uploaded yet
		bool m_spawned = false;
		oc::atomic<int32> m_genInFlight{ 0 };
		JobCounter m_genCounter;
		uint16 m_material = 0;     // every rock's (LitRock does not read it; an instance needs one)
		oc::vector<WorldType> m_worldTypes; // point into m_types' meshes
		uint32 m_worldGeneration = 0;
		uint32 m_typesRevision = 0;
		oc::function<void()> m_beforeFree;

		// Nodes after the meshes: members destruct in reverse order, and a node must die before its mesh.
		oc::vector<Type> m_types;
		oc::vector<RenderNode> m_nodes;
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::RockSystem rocks;
}
