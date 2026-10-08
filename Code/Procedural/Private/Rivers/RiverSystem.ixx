export module Procedural:RiverSystem;

import Core;
import Core.glm;
import Core.Camera;
import RendererVK;
import Threading;
import :GeneratorV3;
import :RiverNetwork;
import :RiverUnits;

// The river units around the camera (Docs/RiverPlan.md V1), owned by TerrainStreamer (no global of its own). Units are
// built ONE at a time on a Low job, nearest the camera first - a cold unit fetches its tiles, so the wait parks the
// fiber - kept while near, and drawn as debug lines ("Terrain/Rivers/Debug lines"): channels by discharge (blue),
// ephemeral beds (tan), rapids (orange), falls (red), outlet / inlet crossings (magenta / green ticks), lakes (row
// hatching: blue full, teal terminal, white pans). Units build only while the lines are on, in V1.
export namespace Procedural
{
	class RiverSystem
	{
	public:
		RiverSystem() = default;
		~RiverSystem();
		RiverSystem(const RiverSystem&) = delete;
		RiverSystem& operator=(const RiverSystem&) = delete;

		// Main thread. The live generator (nullptr = no terrain). A new one drops the units - they reload from disk.
		void setGenerator(oc::shared_ptr<const TerrainGenV3> generator);
		// Main thread, every enabled terrain frame.
		void update(Renderer& renderer, const Camera& camera);

	private:
		struct Line
		{
			glm::vec3 a, b;
			uint32 color;
		};
		struct Resident
		{
			oc::shared_ptr<const RiverUnit> unit;
			oc::vector<Line> lines; // engine space, built once on adoption
		};
		struct Job
		{
			oc::shared_ptr<const TerrainGenV3> generator;
			oc::shared_ptr<const CoarseRiverNetwork> network;
			RiverUnitConfig cfg;
			int32 ui = 0, uj = 0;
			uint32 generation = 0;
			oc::atomic<bool> cancel{ false };
			oc::shared_ptr<const RiverUnit> result;
		};

		void dropUnits();
		void buildLines(Resident& r) const;
		// Engine-space distance from the camera to the unit's square (0 inside).
		float unitDistance(int32 ui, int32 uj, glm::vec2 camera) const;

		oc::shared_ptr<const TerrainGenV3> m_generator;
		oc::shared_ptr<const CoarseRiverNetwork> m_network;
		RiverConfig m_coarseCfg;
		RiverUnitConfig m_unitCfg;
		uint32 m_generation = 0; // a job of an older one is dropped
		oc::unordered_map<uint64, Resident> m_units;
		oc::shared_ptr<Job> m_job;
		JobCounter m_counter;
	};
}
