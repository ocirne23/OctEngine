export module Procedural:TreeSystem;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;

import RendererVK;
import Threading;

import :TerrainSampler;
import :TreeSpecies;
import :TreeGenerator;
import :TreeWorld;

// Procedural trees ("Trees" tweaks). Loads every Assets/Trees/*.tree species, generates its piece library, and draws
// either the WORLD (TreeWorld's records expanded around the camera into a dynamic GPU set - "World mode" in
// Procedural CONTEXT) or a PREVIEW: a grove of composited trees in front of the camera plus the piece
// library itself, drawn through the plain RenderMesh path (one node per placed piece mesh, GPU mesh LOD
// chains, billboards beyond a distance). The dedicated GPU tree path (bone palettes, own
// shaders) replaces the preview draw in G4; see Docs/TreeRenderingPlan.md.
export namespace Procedural
{
	class TreeSystem
	{
	public:
		TreeSystem() = default;
		~TreeSystem();
		TreeSystem(const TreeSystem&) = delete;
		TreeSystem& operator=(const TreeSystem&) = delete;

		void initialize(); // registers Tweaks
		// Per frame, after scatter.update. `maps` places the grove on the terrain (y = 0 without it).
		void update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps);

	private:
		// Every LOD level uploaded; nodes spawn on level 0 and the GPU picks the level per instance through
		// the chains (UINT32_MAX = no chain). Chains are freed before the meshes (clearAll).
		struct PieceMeshes
		{
			RenderMesh bark[TREE_PIECE_LODS];
			RenderMesh leaves[TREE_PIECE_LODS];
			uint32 barkChain = UINT32_MAX;
			uint32 leafChain = UINT32_MAX;
			// Modules only, Far mode Billboards: the billboard cards (LitFoliage) with their own baked material (owned).
			RenderMesh billboard;
			uint16 billboardMaterial = UINT16_MAX;
			// Baked variants with billboards (the GPU MID tier): the bark split into the trunk's and the branches'
			// (LOD chains), and the BRANCH CARD mesh - every module placement's billboard cards merged into one mesh,
			// UVs into the species' card atlas (Species::cardAtlasMaterial), each card's axis in its tangent w.
			RenderMesh trunkBark[TREE_PIECE_LODS];
			RenderMesh branchBark[TREE_PIECE_LODS];
			uint32 trunkChain = UINT32_MAX;
			uint32 branchChain = UINT32_MAX;
			RenderMesh cards;
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
		// distance the module draws as barkFade / leavesFade (the same meshes on LitMasked with the species'
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
			TreeLibrary library;
			oc::vector<PieceMeshes> trunkMeshes;
			oc::vector<PieceMeshes> moduleMeshes;
			// Baked whole-tree variants (bakeTreeVariant): what the grove places. One "piece" each.
			oc::vector<TreePiece> variants;
			oc::vector<PieceMeshes> variantMeshes;
			uint16 barkMaterial = 0;
			uint16 leafMaterial = 0;
			bool ownsLeafMaterial = false; // a cluster texture material (freed with the species)
			bool ownsBarkMaterial = false; // the procedural bark texture material (freed with the species)
			RendererVKLayout::EPipelineIndex leafPipeline = RendererVKLayout::EPipelineIndex::LitOpaque;
			// Billboard crossfade: bark / leaf materials derived with a distance fade-OUT (shared textures,
			// released with the species). UINT16_MAX without billboards.
			uint16 barkFadeMaterial = UINT16_MAX;
			uint16 leafFadeMaterial = UINT16_MAX;
			// The MID tier (GPU path): the module billboards stacked into ONE atlas (owned; LitFoliage, no edge fade) and
			// its derived fade-IN (the mid band) / fade-OUT (the far band) copies for the variants' card meshes; the
			// leaves' and the branch bark's fade-OUT over the mid band. The trunk keeps barkMaterial / barkFadeMaterial.
			uint16 cardAtlasMaterial = UINT16_MAX;
			uint16 cardInMaterial = UINT16_MAX;
			uint16 cardOutMaterial = UINT16_MAX;
			uint16 leafMidFadeMaterial = UINT16_MAX;
			uint16 branchMidFadeMaterial = UINT16_MAX;
			glm::vec3 volumeAlbedo{ 0.05f, 0.1f, 0.03f }; // the far-tree volume's leaf colour: the leaf texture's linear mean
		};

		void reload(Renderer& renderer);
		// Bakes (or loads) every module's billboard cards. Needs the species' level-0 bark and leaf images.
		void buildBillboards(Renderer& renderer, Species& species, const oc::string& name,
			oc::span<const uint8> barkAlbedo, uint32 barkSize, oc::span<const uint8> leafImage, uint32 leafSize);
		void clearAll(); // nodes first, then the species' meshes and owned materials
		void destroyTreeSet(); // the GPU expansion set (drains the GPU)
		// Writes every species' crossfade band (billboard distance x Far distance scale, FadeWidth) into its
		// fade-out materials and its billboards' fade-in materials.
		void applyFadeBands(Renderer& renderer);
		float midDistance(const Species& species) const; // the mid tier's switch distance (m, unscaled); 0 = off
		void spawnPreview(Renderer& renderer, const Camera& camera, const ITerrainSampler* maps);
		void spawnPiece(Renderer& renderer, const Species& species, const PieceMeshes& meshes, const Transform& transform);
		// The GPU set's piece TYPES: one per library piece (trunks, modules, baked variants) of every species.
		void buildGpuTypes(oc::vector<Renderer::TreeInstanceType>& outTypes, oc::unordered_map<const PieceMeshes*, uint32>& outTypeOf) const;
		// Hooks a set into the terrain chunks: the walk lists the drawn chunks (m_vegChunkOf: coordinate -> set chunk).
		void hookTerrain(uint32 numChunks);

		// --- WORLD MODE (Trees/World/Enabled + GPU expansion; Docs/TreeRenderingPlan.md 3.5, W3) ---
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
			oc::vector<uint32> bushes;  // trees: the bush species of their climate (indices into species)
		};
		struct ExpandContext
		{
			oc::shared_ptr<const ITerrainSampler> maps;
			oc::vector<int32> speciesOfRecordType; // TreeWorld record type -> species index (-1: not loaded / not a tree)
			oc::vector<ExpandSpecies> species;
			uint32 worldSeed = 1;
			float chunkSize = 256.0f;
			float sizeVariation = 0.0f;
			float bushesPerTree = 0.0f;
			float bushRadius = 1.5f;    // bushes out to this from their tree (m)
		};
		struct ExpandResult
		{
			glm::ivec2 coord{ 0 };
			uint32 generation = 0;
			oc::vector<Renderer::TreeInstancePiece> pieces;
		};
		struct NearChunk
		{
			uint32 setChunk = UINT32_MAX; // the set chunk once added
			bool pending = true;          // an expansion job runs (or its result waits)
		};
		void spawnWorld(Renderer& renderer, const oc::shared_ptr<const ITerrainSampler>& maps);
		void updateWorld(Renderer& renderer, const Camera& camera);
		void stopExpansion(); // joins the jobs, drops their results
		static void expandChunk(const ExpandContext& context, glm::ivec2 coord, const oc::vector<TreeRecord>& records,
			oc::vector<Renderer::TreeInstancePiece>& out);

		// --- Tweaks ---
		bool m_enabled = true;
		bool m_reload = false;      // button: re-read the .tree files, regenerate, respawn
		bool m_respawn = false;     // button: respawn the preview in front of the camera
		bool m_regenerateTextures = false; // button: regenerate the species textures over the files on disk
		bool m_showLibrary = true;
		int m_gridSize = 350;
		float m_spacing = 11.0f;
		float m_positionJitter = 0.8f;        // random offset per tree, x spacing (1 = anywhere in its cell, > 1 overlaps)
		float m_sizeVariation = 0.6f;        // extra per-tree scale on top of the species range: x 2^(+-this), log-uniform
		float m_bushesPerTree = 4.0f;        // `Kind Bush` species scattered around each grove tree (the fraction by chance)
		float m_bushShadowDistance = 100.0f;  // bushes cast no sun shadow beyond this from the cascades' centre (m); 0 = no limit
		int m_seed = 1;
		int m_groveType = 1;                  // 0 = mixed (species alternate), else GROVE_TYPES[i] by species name (1 = Oak)
		int m_farMode = 0;                    // 0 = billboards, 1 = none (reloads)
		int m_billboardViews = 0;             // 0 = 2 views (back faces show the front through the card), 1 = 4 (reloads)
		float m_farDistanceScale = 4.0f;      // x every species' billboard distance; 0 = off
		float m_branchCardDistance = 0.4f;    // the mid tier (GPU path): branch cards from this x the billboard distance; 0 = off
		bool m_forceFar = false;              // debug: every module as its far representation
		bool m_fadeBandsDirty = false;        // the distance scale changed: rewrite the materials' fade bands
		bool m_gpuExpansion = true;           // G4: pieces expanded on the GPU (one set) instead of a CPU push per node
		uint32 m_treeSet = UINT32_MAX;        // the grove's GPU expansion set (Renderer::createTreeInstanceSet)
		// The set's chunks are the TERRAIN's (spawnPreview: Globals::terrain.setVegetation): terrain chunk coordinate ->
		// set chunk, at the chunk size they were sorted with. The sink reads the band settings through the atomics (it
		// runs on the terrain's walk job). m_vegFallback: every chunk, when no walk lists them.
		oc::unordered_map<uint64, int32> m_vegChunkOf;
		int m_vegChunkSize = 0;
		uint32 m_vegNumChunks = 0;
		bool m_vegHooked = false;
		oc::vector<Renderer::TreeChunkDraw> m_vegFallback;
		oc::atomic<float> m_sinkDistanceScale{ 4.0f };
		oc::atomic<bool> m_sinkForceFar{ false };

		bool m_loaded = false;
		bool m_spawned = false;

		TreeWorld m_world; // every tree of the terrain ring as records (Docs/TreeRenderingPlan.md 3.5)
		// World mode.
		int m_nearRadius = 3;          // chunks (Chebyshev) around the camera chunk whose trees are expanded
		int m_worldCapacity = 600000;  // the dynamic set's piece slots (trees + bushes)
		int m_expandPerFrame = 2;      // expanded chunks added to the set per frame
		bool m_worldMode = false;      // this spawn is the world (else the preview grove)
		bool m_worldFullLogged = false;
		bool m_recordTypesSet = false; // the renderer holds this spawn's record types (the far volume's records)
		uint32 m_worldGeneration = 0;  // TreeWorld's, at the spawn
		oc::shared_ptr<const ExpandContext> m_expandContext;
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
