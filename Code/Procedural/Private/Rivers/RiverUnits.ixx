export module Procedural:RiverUnits;

import Core;
import Settings;
import :GeneratorV3;
import :RiverRouting;
import :RiverNetwork;

// The river UNITS (Docs/RiverPlan.md 4.2-4.7): N x N full tiles on a fixed model-space lattice, routed at native
// resolution. A unit talks to its neighbours ONLY through CROSSINGS - the coarse links that cross its edge - so it is
// computable from its own tiles plus the outside tile of each crossing, never from its upstream catchment:
//   * a crossing sits at the low point of the shared tile edge near its middle, judged on BOTH tiles' border pixels
//     (min, smoothed), so the two units pick the same point; its Q is the coarse link's and its water level is
//     bed + depth(Q), so both sides start / end the river at the same height;
//   * the unit floods from the sea, its OUTLET crossings and sinks (a tile whose coarse water ends in it). The other
//     edge pixels are SOFT walls: water leaves through them only when every way to an outlet climbs more than the
//     "Edge wall" height, and a basin that spills through one is never a lake (hard walls made lakes far above the
//     ground, cut off at the unit edge). Inlets inject the coarse Q. The fine runoff of each tile is rescaled to the
//     coarse pixel's own, so the levels agree on the water budget;
//   * channels (Q >= the minimum) become segments between heads, junctions, inlets and their ends; each gets a water
//     profile, upstream segments first, that never rises downstream: it starts at the lowest of its own bed + depth,
//     its tributaries' water and an inlet's crossing level, follows bed + depth and cuts through a rim in its way (the
//     carve's gorge). Only the sea and a lake floor it. Then the paths are smoothed: resampled at 1 px, a Gaussian along
//     the path ("Path smoothing" px, the ends kept), MEANDERED (a sideways swing scaled by the channel width, kept on the
//     valley floor), Chaikin, Douglas-Peucker.
// Model frame: model metres, real m3/s. Points are in UNIT-LOCAL native pixels: unit pixel (row z, column x) is global
// native pixel (ui * tiles * 256 + z, uj * tiles * 256 + x), at engine (global * mpp - origin).
export namespace Procedural
{
	struct RiverUnitConfig
	{
		int32 unitTiles = 6;
		int32 crossWindow = 64;
		float breachDepth = 75.0f;
		int32 lakeMinCells = 8334;
		int32 lakeMaxCells = 20000; // native px: a bigger basin's lake is only its lowest this many (0 = no limit)
		float channelMinQ = 1.0f;
		float fadeQ = 2.0f;     // m3/s past the minimum over which a channel grows to its full width and depth
		float perennialQ = 0.0f;
		float widthA = 25.0f;
		float depthC = 1.5f;
		float rapidsSlope = 0.05f;
		float fallSlope = 0.3f;
		float fallMinHeight = 12.0f; // model m: a short steep run dropping this much becomes a waterfall at its top (0 = none)
		float fallMaxLength = 15.0f; // native px: the longest steep run that does (longer stays sloped rapids)
		float seaChannelDepth = 60.0f;      // model m (~10 engine m at mpp 5): a river runs on into the sea until the floor is this deep (0 = off)
		float seaChannelMaxLength = 200.0f; // native px: ... at most this far past its mouth
		float lakeChannelDepth = 12.0f;     // model m: a river runs on into its lake until the floor is this far under the level (0 = off)
		float lakeChannelMaxLength = 40.0f; // native px: ... at most this far past the shore
		float seaRescueMinQ = 5.0f;        // m3/s: a soft exit this big is sent on to the unit's sea or a lake (0 = off)
		float seaRescueMaxRim = 300.0f;    // model m: the highest rim over the exit's ground it may cut through to get there
		float seaRescueMaxLength = 1500.0f; // native px: the longest path it may take
		float fallFullQ = 10.0f;   // m3/s: from here up the whole max length; below x Q / this (a stream cuts no gorge)
		float edgeWall = 12.3f; // model m: water leaves through a non-crossing edge when every outlet climbs more
		float pathSmoothing = 10.0f; // native px: the Gaussian sigma the D8 path is smoothed with along its length
		float meanderAmplitude = 0.33f;  // x the channel width
		float meanderWavelength = 15.0f; // x the channel width
		float meanderSlope = 0.02f;      // m/m
		float meanderSmallAmplitude = 5.0f;  // the swing on the smallest stream, x the above (1 at meanderFullQ)
		float meanderSmallWavelength = 1.0f; // the wavelength on the smallest stream, x the above (1 at meanderFullQ)
		float meanderFullQ = 20.0f;          // m3/s: from here up a river meanders as set
		float endLakeArea = 300.0f;          // native px per m3/s: a river ending in a sink / running dry floods a lake there (0 = off)
		float endLakeMaxDepth = 20.0f;      // model m: the most a carved end lake digs under its highest pixel
		bool operator==(const RiverUnitConfig&) const = default;
	};

	// The "Terrain/Rivers" unit settings.
	inline RiverUnitConfig riverUnitConfigFromSettings(const TerrainSettings& s)
	{
		RiverUnitConfig c;
		c.unitTiles = s.riverUnitTiles;
		c.crossWindow = s.riverCrossWindow;
		c.breachDepth = s.riverUnitBreachDepth;
		c.lakeMinCells = s.riverUnitLakeMinCells;
		c.lakeMaxCells = s.riverUnitLakeMaxCells;
		c.channelMinQ = s.riverChannelMinQ;
		c.fadeQ = s.riverChannelFadeQ;
		c.perennialQ = s.riverPerennialQ;
		c.widthA = s.riverWidthA;
		c.depthC = s.riverDepthC;
		c.rapidsSlope = s.riverRapidsSlope;
		c.fallSlope = s.riverFallSlope;
		c.fallMinHeight = s.riverFallMinHeight;
		c.fallMaxLength = s.riverFallMaxLength;
		c.fallFullQ = s.riverFallFullQ;
		c.seaChannelDepth = s.riverSeaChannelDepth;
		c.seaChannelMaxLength = s.riverSeaChannelMaxLength;
		c.lakeChannelDepth = s.riverLakeChannelDepth;
		c.lakeChannelMaxLength = s.riverLakeChannelMaxLength;
		c.seaRescueMinQ = s.riverSeaRescueMinQ;
		c.seaRescueMaxRim = s.riverSeaRescueMaxRim;
		c.seaRescueMaxLength = s.riverSeaRescueMaxLength;
		c.edgeWall = s.riverEdgeWall;
		c.pathSmoothing = s.riverPathSmoothing;
		c.meanderAmplitude = s.riverMeanderAmplitude;
		c.meanderWavelength = s.riverMeanderWavelength;
		c.meanderSlope = s.riverMeanderSlope;
		c.meanderSmallAmplitude = s.riverMeanderSmallAmplitude;
		c.meanderSmallWavelength = s.riverMeanderSmallWavelength;
		c.meanderFullQ = s.riverMeanderFullQ;
		c.endLakeArea = s.riverEndLakeArea;
		c.endLakeMaxDepth = s.riverEndLakeMaxDepth;
		return c;
	}

	// How a segment ends: on the start of the next one (a junction or an inlet), off the unit through an outlet
	// crossing, in the sea, at a lake basin's rim, in a sink, dry (its water fell under the channel minimum), or over
	// the unit's edge where no crossing is (a soft wall: the neighbour does not continue it).
	enum class ERiverEnd : uint8 { Junction, Outlet, Sea, Lake, Sink, Dry, Edge };

	enum ERiverPointFlag : uint8
	{
		RiverPoint_Rapids = 1 << 0,
		RiverPoint_Fall = 1 << 1,
		RiverPoint_Lip = 1 << 2, // a waterfall's lip (RiverUnits shapeFalls): its piece is the straight drop
		RiverPoint_Gorge = 1 << 3, // in a waterfall's plunge gorge (the water lowered below the lip): narrow, steep walls
	};

	struct RiverPoint
	{
		float x = 0.0f, z = 0.0f; // unit-local native pixels
		float water = 0.0f;       // the water surface, model m (relative to sea level)
		float halfWidth = 0.0f;   // model m
		float depth = 0.0f;       // model m
		float q = 0.0f;           // m3/s
		uint8 flags = 0;          // ERiverPointFlag, of the reach from this point to the next
		uint8 pad[3] = {};
	};

	struct RiverSegment
	{
		uint32 first = 0;
		uint32 count = 0;
		ERiverEnd end = ERiverEnd::Dry;
		uint8 ephemeral = 0; // its water never reaches the perennial Q: a dry bed
		uint8 needsUpstream = 0; // from an inlet whose coarse Q is under "Channel min Q": kept only where the upstream river arrives
		uint8 pad = 0;
	};

	struct RiverCrossing
	{
		float x = 0.0f, z = 0.0f; // unit-local native pixels (the border pixel on this unit's side)
		float q = 0.0f;           // m3/s, the coarse link's
		float water = 0.0f;       // model m
		uint8 inlet = 0;          // 1 = water enters the unit here
		uint8 pad[3] = {};
	};

	struct RiverLake
	{
		float level = 0.0f; // model m
		ERiverWater kind = ERiverWater::Lake; // Lake / TerminalLake / Pan
		uint8 carved = 0; // an end lake the ground does not hold: RiverTerrain digs its pixels under the level
		uint8 pad[2] = {};
	};

	// The wet pixels of a lake (a pan's salt pixels), run-length per unit row.
	struct RiverLakeRun
	{
		uint16 row = 0, x0 = 0, len = 0, lake = 0;
	};

	struct RiverUnit
	{
		int32 ui = 0, uj = 0; // the unit (row z, column x)
		int32 tiles = 0;      // full tiles per side
		bool empty = false;   // no tile of it can be served
		oc::vector<RiverPoint> points;
		oc::vector<RiverSegment> segments;
		oc::vector<RiverCrossing> crossings;
		oc::vector<RiverLake> lakes;
		oc::vector<RiverLakeRun> lakeRuns;
	};

	// Builds one unit, or loads it from the disk cache next to the tiles (a unit is cached only when every tile it reads
	// lies inside the generated bounds). Blocking: fetches its tiles and its crossings' outside tiles - on a job the
	// waits park the fiber. nullptr when cancelled.
	oc::shared_ptr<const RiverUnit> buildRiverUnit(const TerrainGenV3& gen, const CoarseRiverNetwork& net,
	                                               const RiverUnitConfig& cfg, int32 ui, int32 uj,
	                                               const oc::atomic<bool>* cancel);
}
