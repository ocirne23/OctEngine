export module Procedural:TreeSystem;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;

import RendererVK;

import :TerrainSampler;
import :TreeSpecies;
import :TreeGenerator;

// Procedural trees ("Trees" tweaks). Loads every Assets/Trees/*.tree species, generates its piece library,
// and - for now - shows a PREVIEW: a grove of composited trees in front of the camera plus the piece
// library itself, drawn through the plain RenderMesh path (one node per placed piece mesh, GPU mesh LOD
// chains, branch-module impostors beyond a distance). The dedicated GPU tree path (bone palettes, own
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
			// Modules only, per Trees/Far mode: the billboard cards (LitMasked) or the impostor quad (TreeImpostor
			// pipeline), each with its own baked material (owned).
			RenderMesh billboard;
			uint16 billboardMaterial = UINT16_MAX;
			RenderMesh impostor;
			uint16 impostorMaterial = UINT16_MAX;
			glm::vec3 farCentre{ 0.0f }; // piece-local centre of the far representation (switch distance)
			float farRadius = 0.0f;      // piece-local radius around it (the crossfade band test)
		};
		// One placed piece of the preview. Beyond its far distance the far node (billboard or impostor) replaces
		// the mesh LOD nodes. A BILLBOARD is real geometry and casts its own shadow: the mesh nodes then stop
		// drawing. An IMPOSTOR cannot cast through the shadow pass's own vertex shader: it draws in the MAIN pass
		// only and the mesh nodes keep drawing in shadow + GI.
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
			bool farIsBillboard = false;
			bool farActive = false;
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
		};

		void reload(Renderer& renderer);
		// Bakes (or loads) every module's impostor atlas and quad. Needs the species' level-0 bark and leaf images.
		void buildImpostors(Renderer& renderer, Species& species, const oc::string& name,
			oc::span<const uint8> barkAlbedo, uint32 barkSize, oc::span<const uint8> leafImage, uint32 leafSize);
		// The same for the billboards (the default far representation).
		void buildBillboards(Renderer& renderer, Species& species, const oc::string& name,
			oc::span<const uint8> barkAlbedo, uint32 barkSize, oc::span<const uint8> leafImage, uint32 leafSize);
		void clearAll(); // nodes first, then the species' meshes and owned materials
		void destroyTreeSet(); // the GPU expansion set (drains the GPU)
		// Writes every species' crossfade band (billboard distance x Far distance scale, FadeWidth) into its
		// fade-out materials and its billboards' fade-in materials.
		void applyFadeBands(Renderer& renderer);
		void spawnPreview(Renderer& renderer, const Camera& camera, const ITerrainSampler* maps);
		void spawnPiece(Renderer& renderer, const Species& species, const PieceMeshes& meshes, const Transform& transform);

		// --- Tweaks ---
		bool m_enabled = false;
		bool m_reload = false;      // button: re-read the .tree files, regenerate, respawn
		bool m_respawn = false;     // button: respawn the preview in front of the camera
		bool m_regenerateTextures = false; // button: regenerate the species textures over the files on disk
		bool m_showLibrary = true;
		int m_gridSize = 5;
		float m_spacing = 14.0f;
		float m_positionJitter = 1.0f;        // random offset per tree, x spacing (1 = anywhere in its cell, > 1 overlaps)
		int m_seed = 1;
		int m_groveType = 0;                  // 0 = mixed (species alternate), else GROVE_TYPES[i] by species name
		int m_farMode = 0;                    // 0 = billboards, 1 = octahedral impostors (fallback), 2 = none (reloads)
		int m_billboardViews = 0;             // 0 = 2 views (back faces show the front through the card), 1 = 4 (reloads)
		float m_impostorDistanceScale = 1.0f; // x every species' far distance (billboard or impostor); 0 = off
		bool m_forceImpostors = false;        // debug: every module as its far representation
		bool m_fadeBandsDirty = false;        // the distance scale changed: rewrite the materials' fade bands
		bool m_gpuExpansion = true;           // G4: pieces expanded on the GPU (one set) instead of a CPU push per node
		uint32 m_treeSet = UINT32_MAX;        // the grove's GPU expansion set (Renderer::createTreeInstanceSet)

		bool m_loaded = false;
		bool m_spawned = false;

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
