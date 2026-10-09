export module Procedural:RiverSystem;

import Core;
import Core.glm;
import Core.Camera;
import RendererVK;
import Threading;
import :RiverUnits;
import :RiverTerrain;

// The river WATER and the debug view (Docs/RiverPlan.md V1 + V3), owned by TerrainStreamer (no global of its own). It
// reads the units from the live RiverTerrain's store - the same units the sampler carves - pulling the ones around the
// camera ONE at a time on a Low job (nearest first; a cold unit fetches its tiles, so the wait parks the fiber).
//   * THE WATER ("Terrain/Rivers/Surface"): per unit ONE RenderMesh (built on the pull job, uploaded on main) with a
//     RIBBON per perennial segment at the carved water surface (a little wider than the channel: the carved bank hides
//     the rest) and a quad per lake row run at the lake's level (one pixel wider all round: the ground cuts the
//     shoreline). Drawn by RendererVK's River variant (EPipelineIndex::River), main pass only, pushed every frame for
//     the units inside "Radius".
//   * THE NEAR CELLS ("Near radius"): c_nearCell-metre squares around the camera, each ONE dense mesh of every river
//     crossing it (rows "Near spacing" apart, "Near across" vertices wide; the normal's x = 1 marks it dense), built on
//     their own Low job from the resident units, nearest first. The VS displaces them by the river's waves (the ocean's
//     FFT field, river_wave.inc.glsl) and sinks the light ribbons under them. A unit adopted or evicted makes every cell
//     STALE: it keeps its mesh until its rebuild lands.
//   * THE DEBUG LINES ("Terrain/Rivers/Debug lines"): channels by discharge (blue), ephemeral beds (tan), rapids
//     (orange), falls (red), the channel's edges (white) and the carved bed's outer edge (dim green), outlet / inlet
//     crossings (magenta / green ticks), lakes (row hatching: blue full, teal terminal, white pans).
export namespace Procedural
{
	class RiverSystem
	{
	public:
		RiverSystem() = default;
		~RiverSystem();
		RiverSystem(const RiverSystem&) = delete;
		RiverSystem& operator=(const RiverSystem&) = delete;

		// Main thread. The live river sampler (nullptr = no rivers). A new one drops the units - they reload from its store.
		void setTerrain(oc::shared_ptr<const RiverTerrain> terrain);
		// Main thread, every enabled terrain frame (after beginFrame: it pushes the water nodes).
		void update(Renderer& renderer, const Camera& camera);

		// A WHITEWATER point for the mist (Renderer::setRiverMistSources), unit-local engine m like the water mesh.
		struct MistPoint
		{
			glm::vec3 pos;      // on the water surface (y above sea level)
			float half = 0.0f;  // the channel's half-width
			glm::vec2 flow;     // the flow's velocity in XZ (m/s)
			float foam = 0.0f;  // the whitewater 0..1
			float depth = 0.0f; // the channel's depth (the river's size: "Full size depth")
			float len = 0.0f;   // the stretch of river it stands for
		};

	private:
		struct Line
		{
			glm::vec3 a, b;
			uint32 color;
		};
		struct Resident
		{
			oc::shared_ptr<const PreparedRiverUnit> unit;
			oc::vector<Line> lines; // engine space, built when the debug lines first need them
			bool linesBuilt = false;
			oc::vector<MistPoint> mist; // built with the water mesh
			RenderMesh mesh;        // declared BEFORE the node: the node is destroyed first
			RenderNode node;
		};
		struct Job
		{
			oc::shared_ptr<const RiverTerrain> terrain;
			int32 ui = 0, uj = 0;
			bool buildSurface = false;
			oc::shared_ptr<const PreparedRiverUnit> result;
			bool hasMesh = false;
			RenderMeshData mesh; // unit-local engine m; the node sits at the unit's origin, at sea level
			oc::vector<MistPoint> mist;
		};

		struct NearCell
		{
			uint32 generation = 0;  // the unit generation it was built from
			RenderMesh mesh;        // BEFORE the node: the node is destroyed first
			RenderNode node;
		};
		struct NearJob
		{
			oc::shared_ptr<const RiverTerrain> terrain;
			int32 cx = 0, cz = 0;
			uint32 generation = 0;
			// The resident units near the cell: their rivers, and their origin minus the cell's (engine m).
			oc::vector<oc::pair<oc::shared_ptr<const PreparedRiverUnit>, glm::vec2>> units;
			float spacing = 0.6f;
			int32 across = 12;
			bool hasMesh = false;
			RenderMeshData mesh; // cell-local engine m; the node sits at the cell's origin, at sea level
		};

		void buildLines(Resident& r) const;
		// Engine-space distance from the camera to the unit's square (0 inside).
		float unitDistance(int32 ui, int32 uj, glm::vec2 camera) const;
		glm::vec3 unitOrigin(int32 ui, int32 uj) const; // engine space, at sea level
		void updateNearCells(Renderer& renderer, glm::vec2 camera);
		void dropNearCells();

		oc::shared_ptr<const RiverTerrain> m_terrain;
		oc::unordered_map<uint64, Resident> m_units;
		oc::shared_ptr<Job> m_job;
		JobCounter m_counter;
		uint16 m_material = UINT16_MAX; // the River pipeline's one material (createMeshMaterial)
		bool m_surfaceWas = false;      // a "Surface" toggle re-pulls the units (they need their meshes)

		oc::unordered_map<uint64, NearCell> m_nearCells;
		oc::shared_ptr<NearJob> m_nearJob;
		JobCounter m_nearCounter;
		uint32 m_unitGeneration = 1;    // bumped on every unit adopted or evicted: the near cells go stale
		float m_nearSpacingWas = 0.0f;  // a change of the near mesh settings rebuilds the cells
		int32 m_nearAcrossWas = 0;

		// THE INLAND WATER MAP (Renderer::setRiverWaterMap: the fog's underwater boundary and the wetness under rivers and
		// lakes): RIVER_WATER_MAP_DIM^2 surface heights around the camera (RiverTerrain::sampleInlandWaterGrid), baked on
		// a Low job when the camera has moved past c_waterMapMove from the last bake's centre or the units changed.
		struct WaterMapJob
		{
			oc::shared_ptr<const RiverTerrain> terrain;
			glm::vec2 centre{ 0.0f };
			glm::vec2 origin{ 0.0f };
			uint32 generation = 0;
			oc::vector<float> heights;
		};
		void updateWaterMap(Renderer& renderer, glm::vec2 camera);

		// THE MIST SOURCES (Renderer::setRiverMistSources): the resident units' whitewater points within "Mist radius",
		// nearest first, re-sent when the camera has moved c_mistMove from the last send or the units changed.
		void updateMist(Renderer& renderer, glm::vec2 camera, bool clear);
		glm::vec2 m_mistCentre{ 1.0e30f };
		uint32 m_mistGeneration = 0;
		float m_mistRadiusWas = 0.0f;
		float m_mistFullSizeWas = 0.0f;
		float m_mistSizeWeightWas = 0.0f;
		bool m_mistSet = false;
		oc::shared_ptr<WaterMapJob> m_waterJob;
		JobCounter m_waterCounter;
		glm::vec2 m_waterCentre{ 1.0e30f };
		uint32 m_waterGeneration = 0;
		bool m_waterMapSet = false;
	};
}
