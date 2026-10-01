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
// library itself, drawn through the plain RenderMesh path (one node per placed piece). The dedicated GPU
// tree path (bone palettes, own shaders) replaces the preview draw in G4; see Docs/TreeRenderingPlan.md.
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
		struct PieceMeshes
		{
			RenderMesh bark;
			RenderMesh leaves;
		};
		struct Species
		{
			TreeSpeciesDesc desc;
			TreeLibrary library;
			oc::vector<PieceMeshes> trunkMeshes;
			oc::vector<PieceMeshes> moduleMeshes;
			uint16 barkMaterial = 0;
			uint16 leafMaterial = 0;
			bool ownsLeafMaterial = false; // a cluster texture material (freed with the species)
			bool ownsBarkMaterial = false; // the procedural bark texture material (freed with the species)
			RendererVKLayout::EPipelineIndex leafPipeline = RendererVKLayout::EPipelineIndex::LitOpaque;
		};

		void reload(Renderer& renderer);
		void clearAll(); // nodes first, then the species' meshes and owned materials
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
		int m_seed = 1;

		bool m_loaded = false;
		bool m_spawned = false;

		// Nodes after the meshes: members destruct in reverse order, and a node must die before its mesh.
		oc::vector<Species> m_species;
		oc::vector<RenderNode> m_nodes;
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::TreeSystem trees;
}
