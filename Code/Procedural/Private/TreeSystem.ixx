export module Procedural:TreeSystem;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;

import RendererVK;
import Physics;
import Threading;
import Settings;

import :TerrainSampler;
import :TreeSpecies;
import :TreeGenerator;
import :TreeLeafTexture;
import :TreeWorld;
#ifndef _MSC_VER
import :GeneratorV3; // teardown order on Linux: this global dies before the diffusion runtime (GeneratorV3.ixx)
#endif
import :MeshCache;

// Procedural trees ("Trees" tweaks). Loads every Assets/Trees/*.tree species, generates its piece library and its baked
// whole-tree variants, and draws the WORLD: TreeWorld's records expanded around the camera into one dynamic GPU set
// ("World mode" in Docs/Context/Procedural/Trees.md). Without TreeWorld or the terrain nothing draws.
export namespace Procedural
{
	class TreeSystem
	{
	public:
		TreeSystem() = default;
		~TreeSystem();
		TreeSystem(const TreeSystem&) = delete;
		TreeSystem& operator=(const TreeSystem&) = delete;

		void initialize(); // attaches the "Trees" listeners (and TreeWorld's)
		// Per frame, after the terrain. `maps`: the terrain the trees stand on (nullptr = no trees).
		void update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps);
		// The tree and rock COLLIDERS' focus points (at most MaxColliderFocus; before update, every frame): a body exists
		// within "Radius (m)" of any of them. None = the camera. The game passes its players and its Base. Only the trees
		// of the NEAR chunks (expanded around the camera) can have bodies: far from the camera a focus gets none.
		static constexpr int MaxColliderFocus = 8;
		void setColliderFocus(oc::span<const glm::vec3> points)
		{
			m_colliderFocusCount = glm::min((int)points.size(), MaxColliderFocus);
			for (int i = 0; i < m_colliderFocusCount; ++i)
				m_colliderFocus[i] = glm::vec2(points[i].x, points[i].z);
		}
		// Every tree and rock of the terrain ring as records (the ground clutter's forest floor reads them; main thread).
		const TreeWorld& world() const { return m_world; }
		// TreeWorld::surveyHarvest: each tree and rock around a point. Main thread, between update and kickWorldUpdate.
		bool surveyHarvest(glm::vec2 centre, float radius, TreeWorld::HarvestVisitor visit, void* user) const { return m_world.surveyHarvest(centre, radius, visit, user); }
		// TreeWorld's bookkeeping job: main.cpp kicks it after the last reader of the records this frame (the clutter);
		// the next update joins it.
		void kickWorldUpdate() { m_world.kickUpdate(); }
		// TreeWorld's pool job (kicked by update): main.cpp joins it before Renderer::present, which reads the record pool.
		void joinWorldPool() { m_world.joinPool(); }

	private:
		// One baked whole-tree variant's GPU objects. LEVEL 0 of each mesh (uploadLevelZero: no GPU LOD chains, the levels
		// above stay empty).
		struct PieceMeshes
		{
			RenderMesh bark[TREE_PIECE_LODS];
			RenderMesh leaves[TREE_PIECE_LODS];
			// THE SHADOW BILLBOARD (a species with a `Billboard Distance`): three crossed cards with their own mask material
			// (owned; RendererVK MATERIAL_FLAG_BILLBOARD) - the variant's shadow caster and what the rays see. Never in the
			// main pass.
			RenderMesh billboard;
			RenderMaterial billboardMaterial;
			// THE VOXEL TIER ("Voxel impostors"): the grid's box (8 corners: the main pass past the species' band, the far voxel
			// trees and the voxel shadow caster each draw its first quad as a rectangle around it), its material (the
			// grid's opacity / surface brick atlases, owned; LitTreeVoxel) and the brick index texture the opacity atlas's
			// header names.
			RenderMesh voxelBox;
			RenderMaterial voxelMaterial;
			RenderTexture voxelIndex;
			// The grid as the MESH leaves find it (their sun self-shadow; RenderTreeSet::Type's leaf grid): its box in the
			// tree's own space and the index texture's header.
			glm::vec3 voxelGridMin{ 0.0f };
			glm::vec3 voxelGridSize{ 0.0f };
			uint8 voxelGridLog2Res = 0;
			uint8 voxelGridLevels = 0;
			uint8 voxelGridLog2Cols = 0;
			glm::vec3 farCentre{ 0.0f }; // tree-local centre of the variant's box (the switch distance, the RT range)
			float farRadius = 0.0f;      // tree-local radius around it (the crossfade band test)
			// The far-tree volume's extinction grid (bakeTreeDensity) over [densityMin, densityMax].
			oc::vector<float> density;
			glm::vec3 densityMin{ 0.0f };
			glm::vec3 densityMax{ 0.0f };
		};
		struct Species
		{
			TreeSpeciesDesc desc;
			// The GPU meshes of the baked whole-tree variants (bakeTreeVariant: what a record expands to). The CPU meshes
			// (the piece library too) are not kept: the load frees them once uploaded and baked.
			oc::vector<PieceMeshes> variantMeshes;
			// The materials the trees draw with: ownBarkMaterial's, and for the leaves a shared one - a LEAF TEXTURE's
			// (m_leafTextures: a cluster species) or the solid leaf colour's.
			uint16 barkMaterial = 0;
			uint16 leafMaterial = 0;
			RenderMaterial ownBarkMaterial; // the procedural bark texture material
			RendererVKLayout::EPipelineIndex leafPipeline = RendererVKLayout::EPipelineIndex::LitOpaque;
			// A cluster species' COLOURS for its leaf texture (the texture holds none): into its piece types (buildGpuTypes).
			TreeLeafPalette leafPalette;
			// The mesh -> voxels crossfade: the bark and the leaf material derived with a distance fade-OUT (shared
			// textures). Invalid without a far tier (`Billboard Distance` 0).
			RenderMaterial barkFadeMaterial;
			RenderMaterial leafFadeMaterial;
			glm::vec3 volumeAlbedo{ 0.05f, 0.1f, 0.03f }; // the far-tree volume's leaf colour: the leaf texture's linear mean
		};
		// A LEAF TEXTURE: the cluster atlas of one species' `Leaves` block and its normal map, with NO colour in it
		// (TreeLeafTexture.ixx). Every cluster species that names that species (`Leaves Texture`) - and the species itself -
		// draws this one material, each in its own colours (Species::leafPalette).
		struct LeafTexture
		{
			RenderMaterial material; // LitTree, MATERIAL_FLAG_LEAF
			glm::vec4 sums{ 0.0f };  // leafClusterSums: a species' mean leaf colour from its palette
		};

		// THE SPECIES LOAD runs on JOBS (one Low job per .tree, loadSpecies): the meshes (MeshCache or generated), the
		// textures (.tex or generated), the shadow billboards' masks and the voxel grids - everything but the GPU objects.
		// Main uploads each species as its job finishes (uploadSpecies: meshes, materials) and frees its CPU data; the trees
		// spawn once every species is in. The job reads only its slot and a snapshot of the settings.
		// kickLoad PARSES every .tree first (main): which leaf textures exist follows from the species (`Leaves Texture`),
		// and each one loads on its own job (loadLeafTexture). A species uploads once its leaf texture is in.
		struct LoadSettings
		{
			bool compressTextures = true;
			bool regenerateTextures = false;
			uint32 voxelRes = 0;      // "Voxel impostors": the voxel tier's grid resolution for a tree (a bush: a quarter); 0 = off
		};
		struct VariantBillboard // a baked variant's shadow billboard
		{
			CachedTexture mask; // two channels (the cover / bark share, the depth): `albedo` only (bakeBillboardMask)
			TreeMesh cards;
		};
		struct SpeciesLoad
		{
			oc::string path, fileName;
			bool parsed = false; // kickLoad read `desc` and `name` from the file
			bool valid = false;
			TreeSpeciesDesc desc;
			oc::string name;
			oc::vector<TreePiece> variants;
			oc::vector<glm::vec4> variantBounds; // per baked variant: its box's centre + half diagonal (tree space)
			oc::vector<TreeDensityGrid> density;
			CachedTexture bark;
			// A cluster species' LEAF TEXTURE (kickLoad): its index in m_leafLoadSlots / m_leafTextures, and the species
			// whose `Leaves` block draws it (the slot's: fixed while the jobs run) - the bakes' input is drawn from it.
			int32 leafTexture = -1;
			const TreeSpeciesDesc* leafShape = nullptr;
			// The textures' linear means: the bark's, and a Single species' leaf colour (a cluster species: from its leaf
			// texture's statistics, at the upload). leafless: no leaf geometry (a succulent's bark blades).
			glm::vec3 barkMean{ 0.0f };
			glm::vec3 leafMean{ 0.0f };
			bool leafless = false;
			bool farTier = false;                           // `Billboard Distance` > 0: the shadow billboards (and the voxel tier)
			oc::vector<VariantBillboard> variantBillboards; // per baked variant
			oc::vector<CachedTexture> voxelTextures;       // per baked variant: the voxel tier's atlases (empty: none)
			oc::vector<CachedTexture> voxelIndex;          // ... and its brick index
			oc::vector<TreeMesh> voxelBoxes;               // ... and its box
		};
		struct LoadSlot
		{
			SpeciesLoad load;
			oc::atomic<bool> done{ false };
			bool uploaded = false;
		};
		struct LeafLoadSlot // one leaf texture's load: `shape` and `name` are fixed by kickLoad, the job writes the texture
		{
			TreeSpeciesDesc shape; // the species whose `Leaves` block the atlas draws
			oc::string name;
			CachedTexture texture;
			oc::atomic<bool> done{ false };
			bool uploaded = false;
		};
		static void loadLeafTexture(const LoadSettings& settings, LeafLoadSlot& out);
		void kickLoad();
		// Uploads the species whose jobs finished; true once every one is in (the species list is then final).
		bool pumpLoad(Renderer& renderer);
		void stopLoad(); // joins the load jobs, drops their slots
		static void loadSpecies(const LoadSettings& settings, SpeciesLoad& out);
		// The variants' FAR TIER into `out`: their shadow billboards (the mask bakes or their .tex, the card meshes) and
		// their voxel grids. The bark / leaf level-0 images are the bakes' input: empty (their .tex was loaded) = generated
		// here on a bake miss.
		static void buildFarTier(const LoadSettings& settings, SpeciesLoad& out,
			oc::vector<uint8>& barkAlbedo, uint32& barkSize, oc::vector<uint8>& leafImage, uint32& leafSize);
		void uploadSpecies(Renderer& renderer, SpeciesLoad& load, Species& species);
		void clearAll(); // the set first, then the species' meshes and owned materials
		void destroyTreeSet(); // the world set (drains the GPU)
		// Writes every species' crossfade band (`Billboard Distance` x Far distance scale, FadeWidth) into its meshes'
		// fade-out materials and its voxel materials (fade-in).
		void applyFadeBands(Renderer& renderer);
		float fadeWidth(const Species& species) const;  // the blend band's width (m): FadeWidth x "Fade width scale"
		float farDistance(const Species& species) const; // the blend band's centre (m, before "Far distance scale")
		// The GPU set's piece TYPES: one per baked variant of every species.
		void buildGpuTypes(oc::vector<RenderTreeSet::Type>& outTypes, oc::unordered_map<const PieceMeshes*, uint32>& outTypeOf) const;
		// Hooks the set into the terrain chunks: the walk lists the drawn chunks (m_vegChunkOf: coordinate -> set chunk).
		void hookTerrain(uint32 numChunks);

		// --- WORLD MODE (Trees/World/Enabled; Docs/Context/Procedural/Trees.md, W3) ---
		// The near chunks of TreeWorld's records, EXPANDED into one DYNAMIC set: per record the tree (variant, scale, yaw from
		// its hash, the exact ground) plus its bushes, chunk by chunk on Low jobs; a chunk that leaves the near radius frees
		// its set chunk. Everything an expansion reads is in an immutable ExpandContext.
		struct ExpandVariant
		{
			uint32 type = 0;            // the GPU type
			glm::vec3 farCentre{ 0.0f };
			float farRadius = 0.0f;
		};
		struct ExpandSpecies
		{
			glm::vec2 scale{ 1.0f };
			oc::vector<ExpandVariant> variants;
			oc::vector<uint32> bushes;  // trees: the bush species of their climate (indices into species), each `Weight` times
			float bushesPerTree = 0.0f; // trees: their undergrowth (bushUndergrowth)
			float bushRadius = 1.5f;    // trees: bushes out to this from the tree (m)
			// Trees: the COLLIDER - the trunk as a cylinder at scale 1 (its base radius with half the flare, its mean height),
			// tree-local; the body's scale is the tree's. Empty: none (a bush).
			oc::vector<glm::vec3> colliderPoints;
		};
		// A ROCK type of the world (RockSystem's WorldType, Docs/Context/Procedural/Rocks.md): a record expands to ONE piece -
		// its variant, scale (rockRecordScale) and yaw from the record's seed, laid on the ground under its footprint
		// (RockSystem::groundTransform: it follows the ground's normal by `align`).
		struct ExpandRockVariant
		{
			uint32 type = 0;            // the GPU type
			glm::vec3 centre{ 0.0f };   // rock-local bounds
			float radius = 0.0f;
			float height = 1.0f;        // nominal
			int32 colliderMesh = -1;    // into m_rockColliderMeshes (its LOD chain's last level); -1 = none
		};
		struct ExpandRock
		{
			glm::vec2 scale{ 1.0f };
			float sink = 0.0f;
			float align = 0.0f;
			float footprint = 1.0f; // RockSystem::WorldType::footprint
			oc::vector<ExpandRockVariant> variants;
		};
		struct ExpandContext
		{
			oc::shared_ptr<const ITerrainSampler> maps;
			oc::vector<int32> speciesOfRecordType; // TreeWorld record type -> species index (-1: not loaded / not a tree)
			oc::vector<ExpandSpecies> species;
			oc::vector<int32> rockOfRecordType;    // TreeWorld record type -> rocks index (-1: not a rock / not loaded)
			oc::vector<ExpandRock> rocks;
			float rockReach = 0.0f;                // the largest rock's footprint radius (m): the ground grid's margin
			uint32 worldSeed = 1;
			float chunkSize = 256.0f;
			float sizeVariation = 0.0f;
			float bushRadius = 1.5f;    // the LARGEST species' bush radius (m): the ground grid's margin
			float riverClear = 1.0f;    // "Terrain/Rivers/Vegetation clear": no bush where the river influence is above it
		};
		// THE WORLD COLLIDERS ("Trees/World/Colliders"): what the expansion places a static body for - a tree's trunk (its
		// species' cylinder) or a rock (its mesh); the bodies exist only within the radius of the camera (updateColliders).
		struct ExpandCollider
		{
			Transform transform;  // the piece's: a tree's base (scale = the tree's), a rock's ground transform
			int32 rockMesh = -1;  // into m_rockColliderMeshes; -1 = a tree
			uint32 species = 0;   // a tree: into ExpandContext::species (its colliderPoints)
		};
		struct ExpandResult
		{
			glm::ivec2 coord{ 0 };
			uint32 generation = 0;
			oc::vector<RenderTreeSet::Piece> pieces;
			oc::vector<ExpandCollider> colliders;
		};
		struct NearChunk
		{
			uint32 setChunk = UINT32_MAX; // the set chunk once added
			bool pending = true;          // an expansion job runs (or its result waits)
			oc::vector<ExpandCollider> colliders;
			oc::vector<PhysicsBody> bodies; // parallel to colliders once any is near (invalid: no body)
			uint32 liveBodies = 0;
		};
		void spawnWorld(Renderer& renderer, const oc::shared_ptr<const ITerrainSampler>& maps);
		void updateWorld(Renderer& renderer, const Camera& camera);
		void stopExpansion(); // joins the jobs, drops their results
		static void expandChunk(const ExpandContext& context, glm::ivec2 coord, const oc::vector<TreeRecord>& records,
			oc::vector<RenderTreeSet::Piece>& out, oc::vector<ExpandCollider>& outColliders);
		// Main thread, in updateWorld: creates the near chunks' bodies within "Radius (m)" of the camera ("Bodies per frame"
		// at most) and destroys those 10 % past it; with the tweak off, all of them. box3d create / destroy serialize on
		// g_bodyLifecycleMutex (the terrain collider's job may create its tiles meanwhile).
		void updateColliders(const Camera& camera);
		glm::vec2 m_colliderFocus[MaxColliderFocus]{}; // setColliderFocus
		int m_colliderFocusCount = 0;

		TreeSettings& m_settings = Globals::settings.trees; // "Trees" (the buttons are cleared here)
		bool m_fadeBandsDirty = false;        // the distance scale changed: rewrite the materials' fade bands
		RenderTreeSet m_treeSet;              // the world set; destroyed before the meshes (destroyTreeSet)
		// The set's chunks are the TERRAIN's (hookTerrain: Globals::terrain.setVegetation): terrain chunk coordinate ->
		// set chunk, at the chunk size of the spawn. The sink reads the band settings through the atomics (it runs on the
		// terrain's walk job). m_vegFallback: every chunk, when no walk lists them.
		oc::unordered_map<uint64, int32> m_vegChunkOf;
		oc::vector<uint64> m_prevSetKeys; // last frame's set chunks, sorted (updateWorld: the far voxel trees' exclusion)
		int m_vegChunkSize = 0;
		bool m_vegHooked = false;
		oc::vector<RenderTreeSet::ChunkDraw> m_vegFallback;
		oc::atomic<float> m_sinkDistanceScale{ 4.0f };
		oc::atomic<bool> m_sinkForceFar{ false };

		bool m_loaded = false;
		bool m_spawned = false;
		// The species load in flight (kickLoad): one slot per .tree, name-sorted - m_species[i] is slot i's until pumpLoad
		// drops the failed ones.
		bool m_loading = false;
		oc::vector<oc::unique_ptr<LoadSlot>> m_loadSlots;
		oc::vector<oc::unique_ptr<LeafLoadSlot>> m_leafLoadSlots; // m_leafTextures[i] is slot i's
		JobCounter m_loadCounter;

		TreeWorld m_world; // every tree of the terrain ring as records (Docs/Context/Procedural/Trees.md)
		// World mode.
		bool m_worldMode = false;      // this spawn has the world set (else none: TreeWorld is off, or there is no terrain)
		bool m_worldFullLogged = false;
		bool m_recordTypesSet = false; // the renderer holds this spawn's record types (the far volume's records)
		uint32 m_worldGeneration = 0;  // TreeWorld's, at the spawn
		// The world set's ROCK types draw RockSystem's meshes: RockSystem's generation at the spawn (a change
		// respawns), and whether this set holds any (RockSystem then drops the set before it frees them - initialize).
		uint32 m_rockGeneration = 0;
		bool m_setHasRocks = false;
		oc::shared_ptr<const ExpandContext> m_expandContext;
		// The rocks' collision BVHs (spawnWorld, from RockSystem's last LOD levels): declared before m_near, so the bodies
		// (its chunks') destruct first; destroyTreeSet frees them after the chunks.
		oc::vector<PhysicsMesh> m_rockColliderMeshes;
		uint32 m_colliderBodies = 0; // live, over every near chunk
		oc::unordered_map<uint64, NearChunk> m_near;
		uint32 m_expandGeneration = 0;
		std::mutex m_expandMutex;
		oc::vector<ExpandResult> m_expandResults;
		oc::atomic<int32> m_expandInFlight{ 0 };
		JobCounter m_expandCounter;

		// The leaf textures the cluster species draw. Declared before m_species: a species' fade-out leaf material is derived
		// from its leaf texture's, and goes first.
		oc::vector<LeafTexture> m_leafTextures;
		oc::vector<Species> m_species;
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::TreeSystem trees;
}
