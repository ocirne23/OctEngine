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
import :TreeWorld;
import :MeshCache;

// Procedural trees ("Trees" tweaks). Loads every Assets/Trees/*.tree species, generates its piece library, and draws
// either the WORLD (TreeWorld's records expanded around the camera into a dynamic GPU set - "World mode" in
// Procedural CONTEXT) or a PREVIEW: a grove of composited trees in front of the camera plus the piece
// library itself, drawn through the plain RenderMesh path (one node per placed piece mesh, GPU mesh LOD
// chains, billboards beyond a distance). The dedicated GPU tree path (bone palettes, own
// shaders) replaces the preview draw in G4; see Docs/Context/Procedural/Trees.md.
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
		// Per frame, after the terrain. `maps` places the grove on the terrain (y = 0 without it).
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
		// LEVEL 0 of each piece mesh (uploadLevelZero: no GPU LOD chains, the levels above stay empty).
		struct PieceMeshes
		{
			RenderMesh bark[TREE_PIECE_LODS];
			RenderMesh leaves[TREE_PIECE_LODS];
			// Modules only, Far mode Billboards: the billboard cards (LitFoliage) with their own baked material (owned).
			RenderMesh billboard;
			RenderMaterial billboardMaterial;
			// Baked variants with billboards (the GPU MID tier): the bark split into the trunk's and the branches',
			// and the BRANCH CARD mesh - every module placement's billboard cards merged into one mesh,
			// UVs into the species' card atlas (Species::cardAtlasMaterial), each card's axis in its tangent w.
			RenderMesh trunkBark[TREE_PIECE_LODS];
			RenderMesh branchBark[TREE_PIECE_LODS];
			RenderMesh cards;
			// Baked TREE variants with "Voxel impostors" (GPU path): the voxel tier - the box the march starts from and its
			// material (the grid's albedo / normal atlases, owned; LitTreeVoxel). Main pass only, in the billboard's place.
			RenderMesh voxelBox;
			RenderMaterial voxelMaterial;
			glm::vec3 farCentre{ 0.0f }; // piece-local centre of the far representation (switch distance)
			float farRadius = 0.0f;      // piece-local radius around it (the crossfade band test)
			// Baked variants only: the far-tree volume's extinction grid (bakeTreeDensity) over [densityMin, densityMax].
			oc::vector<float> density;
			glm::vec3 densityMin{ 0.0f };
			glm::vec3 densityMax{ 0.0f };
		};
		// One placed piece of the preview. Beyond its far distance the billboard replaces the mesh LOD nodes; it is
		// real geometry and casts its own shadow, so the mesh nodes then stop drawing.
		//
		// Billboards CROSSFADE instead of switching: over a band of `Billboard FadeWidth` around the switch
		// distance the module draws as barkFade / leavesFade (the same meshes on LitTree with the species'
		// fade-OUT materials) plus the billboard (its material fades IN over the same band): a dither in the
		// lit FS hands every pixel to exactly one of them. Outside the band only one side draws, the mesh on its
		// normal (early-depth) materials.
		struct PlacedPiece
		{
			RenderNode bark;
			RenderNode leaves;
			RenderNode barkFade;               // billboard modules only: the crossfade copies of bark / leaves
			RenderNode leavesFade;
			RenderNode far;                    // invalid for trunks / Far mode None
			glm::vec3 centre{ 0.0f };          // world, for the switch distance
			float radius = 0.0f;               // world, around `centre`
			float farDistance = 0.0f;
			float fadeWidth = 0.0f;
		};
		struct Species
		{
			TreeSpeciesDesc desc;
			// The GPU meshes of the piece library and of the baked whole-tree variants (bakeTreeVariant: what the grove places,
			// one "piece" each). The CPU meshes are not kept: reload frees them once uploaded and baked.
			oc::vector<PieceMeshes> trunkMeshes;
			oc::vector<PieceMeshes> moduleMeshes;
			oc::vector<PieceMeshes> variantMeshes;
			float moduleLength = 1.0f; // the first module's length (m): the library row's spacing
			// The materials the pieces draw with: ownBarkMaterial's / ownLeafMaterial's, or a shared one (the solid leaf colour).
			uint16 barkMaterial = 0;
			uint16 leafMaterial = 0;
			RenderMaterial ownBarkMaterial; // the procedural bark texture material
			RenderMaterial ownLeafMaterial; // a cluster texture material
			RendererVKLayout::EPipelineIndex leafPipeline = RendererVKLayout::EPipelineIndex::LitOpaque;
			// The MID tier (GPU path): the module billboards stacked into ONE atlas (owned; LitFoliage, no edge fade). Declared
			// before the materials derived from it below, so they are destroyed first.
			RenderMaterial cardAtlasMaterial;
			// Billboard crossfade: bark / leaf materials derived with a distance fade-OUT (shared textures). Invalid without
			// billboards.
			RenderMaterial barkFadeMaterial;
			RenderMaterial leafFadeMaterial;
			// The MID tier's derived fade-IN (the mid band) / fade-OUT (the far band) copies of the atlas for the variants' card
			// meshes; the leaves' and the branch bark's fade-OUT over the mid band. The trunk keeps barkMaterial / barkFadeMaterial.
			RenderMaterial cardInMaterial;
			RenderMaterial cardOutMaterial;
			RenderMaterial leafMidFadeMaterial;
			RenderMaterial branchMidFadeMaterial;
			glm::vec3 volumeAlbedo{ 0.05f, 0.1f, 0.03f }; // the far-tree volume's leaf colour: the leaf texture's linear mean
		};

		// THE SPECIES LOAD runs on JOBS (one Low job per .tree, loadSpecies): the meshes (MeshCache or generated), the
		// textures (.tex or generated), the billboard bakes and the card meshes - everything but the GPU objects. Main
		// uploads each species as its job finishes (uploadSpecies: meshes, materials) and frees its CPU data; the trees
		// spawn once every species is in. The job reads only its slot and a snapshot of the settings.
		struct LoadSettings
		{
			bool compressTextures = true;
			bool regenerateTextures = false;
			bool billboards = true;   // Far mode Billboards
			bool showLibrary = false;
			uint32 billboardViews = 2;
			uint32 voxelRes = 0;      // "Voxel impostors": the voxel tier's grid resolution; 0 = off
		};
		struct PieceBillboard
		{
			CachedTexture texture; // uploaded when `upload` (and kept for a module: the atlas)
			TreeMesh cards;
			glm::vec3 farCentre{ 0.0f };
			float farRadius = 0.0f;
			bool upload = false;   // a whole tree's, or any with "Show piece library"
		};
		struct SpeciesLoad
		{
			oc::string path, fileName;
			bool valid = false;
			TreeSpeciesDesc desc;
			oc::string name;
			TreeLibrary library;
			oc::vector<TreePiece> variants;
			oc::vector<TreeDensityGrid> density;
			CachedTexture bark;
			CachedTexture leaves; // cluster species only
			glm::vec3 volumeAlbedo{ 0.0f };
			bool billboards = false;
			oc::vector<PieceBillboard> pieceBillboards[3]; // modules, trunks, baked variants
			CachedTexture atlas;                           // the mid tier's card atlas (empty: none)
			oc::vector<TreeMesh> variantCards;             // per baked variant: its branch-card mesh
			oc::vector<CachedTexture> voxelTextures;       // per baked variant: the voxel tier's atlases (empty: none)
			oc::vector<TreeMesh> voxelBoxes;               // ... and its box
		};
		struct LoadSlot
		{
			SpeciesLoad load;
			oc::atomic<bool> done{ false };
			bool uploaded = false;
		};
		void kickLoad();
		// Uploads the species whose jobs finished; true once every one is in (the species list is then final).
		bool pumpLoad(Renderer& renderer);
		void stopLoad(); // joins the load jobs, drops their slots
		static void loadSpecies(const LoadSettings& settings, SpeciesLoad& out);
		// The billboard bakes (or their .tex), the card atlas and the variants' card meshes, into `out`. The bark / leaf
		// level-0 images are the bake's input: empty (their .tex was loaded) = generated here on a bake miss.
		static void buildBillboards(const LoadSettings& settings, SpeciesLoad& out,
			oc::vector<uint8>& barkAlbedo, uint32& barkSize, oc::vector<uint8>& leafImage, uint32& leafSize);
		void uploadSpecies(Renderer& renderer, SpeciesLoad& load, Species& species);
		void clearAll(); // nodes first, then the species' meshes and owned materials
		void destroyTreeSet(); // the GPU expansion set (drains the GPU)
		// Writes every species' crossfade band (billboard distance x Far distance scale, FadeWidth) into its
		// fade-out materials and its billboards' fade-in materials.
		void applyFadeBands(Renderer& renderer);
		float midDistance(const Species& species) const; // the mid tier's switch distance (m, unscaled); 0 = off
		void spawnPreview(Renderer& renderer, const Camera& camera, const ITerrainSampler* maps);
		void spawnPiece(Renderer& renderer, const Species& species, const PieceMeshes& meshes, const Transform& transform);
		// The GPU set's piece TYPES: one per library piece (trunks, modules, baked variants) of every species.
		void buildGpuTypes(oc::vector<RenderTreeSet::Type>& outTypes, oc::unordered_map<const PieceMeshes*, uint32>& outTypeOf) const;
		// Hooks a set into the terrain chunks: the walk lists the drawn chunks (m_vegChunkOf: coordinate -> set chunk).
		void hookTerrain(uint32 numChunks);

		// --- WORLD MODE (Trees/World/Enabled + GPU expansion; Docs/Context/Procedural/Trees.md, W3) ---
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
		RenderTreeSet m_treeSet;              // the grove's GPU expansion set; destroyed before the meshes (destroyTreeSet)
		// The set's chunks are the TERRAIN's (spawnPreview: Globals::terrain.setVegetation): terrain chunk coordinate ->
		// set chunk, at the chunk size they were sorted with. The sink reads the band settings through the atomics (it
		// runs on the terrain's walk job). m_vegFallback: every chunk, when no walk lists them.
		oc::unordered_map<uint64, int32> m_vegChunkOf;
		oc::vector<uint64> m_prevSetKeys; // last frame's set chunks, sorted (updateWorld: the far voxel trees' exclusion)
		int m_vegChunkSize = 0;
		uint32 m_vegNumChunks = 0;
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
		JobCounter m_loadCounter;

		TreeWorld m_world; // every tree of the terrain ring as records (Docs/Context/Procedural/Trees.md)
		// World mode.
		bool m_worldMode = false;      // this spawn is the world (else the preview grove)
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

		// Nodes after the meshes: members destruct in reverse order, and a node must die before its mesh.
		oc::vector<Species> m_species;
		oc::vector<PlacedPiece> m_pieces;
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::TreeSystem trees;
}
