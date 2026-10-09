export module Procedural:RiverNetwork;

import Core;
import Settings;
import :GeneratorV3;
import :RiverRouting;

// The river drainage network, COARSE level (Docs/RiverPlan.md 4.1). One coarse pixel is one full tile, so a flow
// direction here is the direction water leaves a full tile: the backbone the river units refine.
//
// Per coarse tile, routed over its own DOMAIN - the (2R+1)^2 coarse tiles centred on it - so the result is a pure
// function of (seed, config) and never of which tile was asked for first:
//   1. sea = deeper than the sea depth, plus the below-zero ground connected to it (not V3's sea-level film);
//   2. priority-flood from the sea and the domain edge (a pit queue for flats, so they drain breadth-first): every
//      pixel gets a D4 receiver and a filled height;
//   3. runoff per pixel = rain - evaporation (the Budyko curve over the model's precipitation and temperature);
//   4. accumulation upstream-first; a depression deeper than the breach depth is a LAKE that balances its inflow
//      against open-water evaporation: full (spills the rest), terminal (no outflow, a lower level) or a dry pan;
//      dry land loses channel water on the way (aridity past 1).
// Steps 1, 2 and 4 are the router the river units run too (:RiverRouting), on D4 here: a coarse link is a tile edge.
// Model frame throughout: model metres, real m3/s, so metersPerPixel scales the rivers with the world.
export namespace Procedural
{
	struct RiverConfig
	{
		int32 coarseDomain = 2;     // coarse tiles of margin around the tile
		float seaDepth = 20.0f;     // model m below sea level that seeds the sea
		float petPerC = 64.0f;      // mm/yr of potential evaporation per C above -5 C
		float budykoW = 5.0f;
		float lakeEvap = 2.5f;      // x the potential evaporation
		float loss = 0.001f;        // m3/s per km per sqrt(m3/s) per unit of aridity past 1
		float breachDepth = 110.0f; // model m
		int32 lakeMinCells = 30;
		bool operator==(const RiverConfig&) const = default;
	};

	// The "Terrain/Rivers" coarse settings.
	inline RiverConfig riverConfigFromSettings(const TerrainSettings& s)
	{
		RiverConfig c;
		c.coarseDomain = s.riverCoarseDomain;
		c.seaDepth = s.riverSeaDepth;
		c.petPerC = s.riverPetPerC;
		c.budykoW = s.riverBudykoW;
		c.lakeEvap = s.riverLakeEvap;
		c.loss = s.riverLoss;
		c.breachDepth = s.riverBreachDepth;
		c.lakeMinCells = s.riverLakeMinCells;
		return c;
	}

	// The D4 neighbour a pixel drains to. None: the sea, a terminal lake, or off the domain.
	enum class ECoarseDir : uint8 { None, PosX, PosZ, NegX, NegZ };

	struct CoarseRiverTile
	{
		int32 ti = 0, tj = 0;   // the coarse tile (row z, column x)
		int32 size = 0;         // pixels per side (TerrainGenV3::coarseTilePixels)
		// Row-major, row = z.
		oc::vector<ECoarseDir> dir;
		oc::vector<float> q;      // m3/s leaving the pixel toward dir (after the lake balance and the losses)
		oc::vector<float> runoff; // m3/s the pixel's own land adds (0 on the sea and in lake basins): the budget the
		                          // full tile under it is rescaled to
		oc::vector<float> area;   // upstream catchment in coarse pixels, this one included
		oc::vector<float> filled; // the conditioned surface, model m
		oc::vector<ERiverWater> water;
		oc::vector<float> level;  // model m: the water level of a Lake / TerminalLake pixel
	};

	class CoarseRiverNetwork
	{
	public:
		CoarseRiverNetwork(oc::shared_ptr<const TerrainGenV3> generator, const RiverConfig& cfg);

		// Blocking on a miss: fetches the domain's coarse tiles (a disk read or a few model calls each; on a job the
		// wait parks the fiber) and routes it. nullptr when the generator cannot serve every tile of the domain.
		oc::shared_ptr<const CoarseRiverTile> tile(int32 ti, int32 tj) const;

		const RiverConfig& config() const { return m_cfg; }
		const TerrainGenV3& generator() const { return *m_generator; }

	private:
		oc::shared_ptr<const CoarseFieldPlanes> planes(int32 ti, int32 tj) const;
		oc::shared_ptr<const CoarseRiverTile> compute(int32 ti, int32 tj) const;

		oc::shared_ptr<const TerrainGenV3> m_generator;
		RiverConfig m_cfg;
		// Neither lock is held across a fetch or a route (both can park the fiber).
		mutable std::mutex m_mutex;
		mutable oc::unordered_map<uint64, oc::shared_ptr<const CoarseFieldPlanes>> m_planes;
		mutable oc::unordered_map<uint64, oc::shared_ptr<const CoarseRiverTile>> m_tiles;
	};
}
