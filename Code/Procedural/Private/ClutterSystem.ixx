export module Procedural:ClutterSystem;

import Core;
import Core.glm;
import Core.Camera;

import RendererVK;
import Threading;
import Settings;

import :TerrainSampler;
import :ClutterType;

// GROUND CLUTTER ("Clutter" tweaks; Docs/GroundClutterPlan.md): pebbles, fallen branches, mushrooms and flowers. The
// objects themselves are placed on the GPU every frame (RendererVK ClutterPipeline, clutter_cull.cs.glsl) - nothing per
// object lives on the CPU. This system feeds it:
//   * THE TYPES: every Assets/Clutter/*.clutter (name-sorted), their meshes generated on Low jobs in the background
//     (ClutterGenerator), then handed to the renderer once (Renderer::setClutterAssets: a GPU type per Placement block).
//   * THE FOREST FLOOR MAP: the trees' and rocks' records near the camera (TreeWorld, through Globals::trees) splatted
//     on a Low job into CLUTTER_FLOOR_DIM^2 rgba8 texels - canopy, trunk proximity, rock proximity, occupied - and
//     handed to the renderer (Renderer::setClutterFloorMap). Re-baked when the camera leaves "Floor map/Rebake distance"
//     from its centre, or when the records under it change. The grass reads it too ("Grass/Cover/Canopy thinning").
export namespace Procedural
{
	class ClutterSystem
	{
	public:
		ClutterSystem() = default;
		~ClutterSystem(); // joins the jobs
		ClutterSystem(const ClutterSystem&) = delete;
		ClutterSystem& operator=(const ClutterSystem&) = delete;

		void initialize(); // attaches the "Clutter" listeners
		// Per frame, after trees.update (it reads their records). `maps`: nullptr while the terrain is off.
		void update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps);

	private:
		void loadTypes();             // main thread (IO): the .clutter files
		void kickGeneration();        // a Low job per type
		void finishGeneration(Renderer& renderer);
		void updateFloorMap(Renderer& renderer, const Camera& camera);

		// One loaded type and its generated meshes (written by its job).
		struct Type
		{
			ClutterTypeDesc desc;
			uint64 meshHash = 0; // the .clutter file's text (MeshCache's key, with the mesh resolution)
			oc::vector<Renderer::ClutterMesh> meshes;
		};
		// What a floor map bake reads: a copy of the records under the map and what their types mean.
		struct FloorRecords
		{
			glm::ivec2 coord{ 0 };
			oc::vector<uint32> records;
		};
		struct FloorBake
		{
			glm::vec2 centre{ 0.0f };
			float chunkSize = 256.0f;
			uint32 worldSeed = 1;
			oc::vector<FloorRecords> chunks;
			oc::vector<uint8> kind;        // per record type: 0 none, 1 tree, 2 rock, 3 dead wood (TreeWorld::FloorType)
			oc::vector<float> crown;       // tree: crown radius (m)
			oc::vector<float> trunk;       // tree: trunk radius (m)
			oc::vector<glm::vec2> rockScale;
			oc::vector<glm::vec2> footprint; // rock / wood: its capsule's half length and radius x the record's scale
			oc::vector<uint32> texels;     // the result
		};
		static void bakeFloor(FloorBake& bake);

		ClutterSettings& m_settings = Globals::settings.clutter; // "Clutter" (the buttons are cleared here)
		bool m_loaded = false;
		bool m_remesh = false;
		bool m_generating = false;
		bool m_floorSet = false;      // the renderer holds a floor map (cleared on disable)
		oc::vector<Type> m_types;
		oc::atomic<int32> m_genInFlight{ 0 };
		JobCounter m_genCounter;

		// The floor map: the bake in flight (its own memory; main reads it only after the counter drained).
		oc::unique_ptr<FloorBake> m_floorBake;
		oc::atomic<int32> m_floorInFlight{ 0 };
		JobCounter m_floorCounter;
		glm::vec2 m_floorCentre{ FLT_MAX };  // the centre the renderer's map was baked at
		uint64 m_floorSignature = 0;         // the records it was baked from (chunks and their counts)
		uint32 m_floorGeneration = UINT32_MAX; // TreeWorld's generation at that bake
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::ClutterSystem clutter;
}
