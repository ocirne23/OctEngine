export module Procedural:RiverRouting;

import Core;
import :GeneratorV3;

// The drainage router BOTH river levels run (Docs/RiverPlan.md 4.1 coarse, 4.3 the units): the sea, a priority-flood
// from the coast and the caller's seeds (a pit queue for flats), depressions -> lakes with their water balance, and
// the upstream-first accumulation with the dry-land channel loss. Model frame: model metres, real m3/s.
export namespace Procedural
{
	enum class ERiverWater : uint8 { Land, Sea, Lake, TerminalLake, Pan };

	struct DrainageGrid
	{
		int32 w = 0, h = 0;
		bool d8 = false;           // 8 neighbours (the units) or 4 (the coarse level: a link is a tile edge)
		oc::vector<float> elev;    // model m
		oc::vector<float> runoff;  // m3/s the pixel adds as land
		oc::vector<float> lakeNet; // m3/s the pixel adds as lake: rain - open-water evaporation (signed)
		oc::vector<float> aridity; // PET / P
		oc::vector<float> inject;  // m3/s entering the pixel from outside the grid (inlets); empty = none
		oc::vector<uint8> mask;    // 1 = part of the grid (empty = every pixel)
		oc::vector<uint8> sea;     // markSea's output (the router reads it)
		float cellKm = 1.0f;       // pixel size, model km (the channel loss is per km)
		float loss = 0.0f;         // RiverConfig::loss
		float seaDepth = 20.0f;
		float breachDepth = 30.0f;
		int32 lakeMinCells = 1;
		int32 lakeMaxCells = 0;    // a bigger basin holds water in only its lowest this many pixels (0 = no limit)
		bool  edgeHoldsNoLake = false; // a river unit: a lake stays under every grid-edge pixel it covers or touches
	};

	// A pixel that drains OFF the grid. `priority` orders it in the flood (its own height for a real outlet; higher for
	// a SOFT one, which the water only takes when every other way out climbs more than the difference). A basin that
	// spills through a soft seed is never a lake: its level would be set by the grid's edge, not the ground.
	struct DrainageSeed
	{
		int32 idx = 0;
		float priority = 0.0f;
		uint8 soft = 0;
	};

	struct DrainageLake
	{
		float spill = 0.0f;     // the spill level (the filled height of its cells)
		float level = 0.0f;     // the water level: the spill when full, lower when terminal
		ERiverWater kind = ERiverWater::Lake; // Lake / TerminalLake / Pan
	};

	struct DrainageResult
	{
		oc::vector<int32> receiver; // -1: a seed (drains off the grid), the sea, or outside the mask
		oc::vector<float> filled;
		oc::vector<int32> order;    // the flood's pop order: downstream first
		oc::vector<int32> rank;     // index into order, -1 = never routed
		oc::vector<float> outflow;  // m3/s leaving toward the receiver
		oc::vector<float> area;     // upstream catchment in pixels, this one included
		oc::vector<ERiverWater> water;
		oc::vector<int32> lakeOf;   // the lake whose BASIN the pixel is in (wet or dry floor), -1 = none
		oc::vector<DrainageLake> lakes;
		oc::vector<int32> root;     // the seed (or sea pixel) the pixel finally drains to, -1 = never routed
	};

	// Rain minus evaporation for one pixel, from the generator's raw planes and its climate shaping.
	void riverPixelClimate(const TerrainConfigV3& gen, float petPerC, float budykoW, float lakeEvap,
	                       float elev, float tempSea, float precip, float mmToQ,
	                       float& outRunoff, float& outLakeNet, float& outAridity);

	// Deep water (< -seaDepth) and the below-zero ground connected to it, inside the mask. V3's -0.05 m film is land.
	void markSea(DrainageGrid& g);

	// seeds: pixels that drain OFF the grid (outlets, sinks, a domain edge, soft walls). A masked pixel no seed or coast
	// reaches drains to the lowest pixel of its region, which becomes a seed too.
	void routeDrainage(const DrainageGrid& g, oc::span<const DrainageSeed> seeds, DrainageResult& out);
}
