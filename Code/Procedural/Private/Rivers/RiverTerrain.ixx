export module Procedural:RiverTerrain;

import Core;
import Settings;
import Threading;
import :TerrainSampler;
import :GeneratorV3;
import :RiverNetwork;
import :RiverUnits;

// THE RIVER SAMPLER (Docs/RiverPlan.md 5): an ITerrainSampler that wraps the diffusion generator and carves the river
// units into it, so every terrain consumer - the chunks, the collider, both bakes, the tree / rock records - gets the
// rivers from the one datum with no change of its own. A wrapper, not a part of TerrainGenV3: the units are built FROM
// the generator's tiles.
//
// It owns the UNIT STORE: a unit is built (or loaded from disk) on first touch, blocking like a tile miss - concurrent
// requesters of the same unit park on its event, one builds. A query resolves the units within the carve reach of its
// rect once (sampleGrid: one store lookup per unit, not per point).
//
// The carve per channel piece, in the model frame (d = distance from the centre line, S = the water surface there,
// w = half-width, D = the channel depth): the channel - its sides at "Channel wall slope" from S at d = w down to a flat
// bed at S - D (RiverCarveConfig::channelCut) -, ONE CURVE over the bank and
// the floodplain (w x ("Bank factor" + "Floodplain factor") wide) rising "Bank height" x the hydraulic depth (not D) as (x / width)^"Floodplain
// curve", then the valley wall at "Valley slope" for "Carve reach" past the floodplain's edge, fading out over its last
// 30 %. The floodplain is always carved whole. It only ever LOWERS the ground (a soft min) and the pieces compose by
// min. A pure function of (x, z) - never of the query's grid step: the terrain's edge stitch needs that.
//
// Water: `TerrainPoint::inlandWater` + waterKind River inside a perennial channel, Lake over a lake's wet pixels (a
// pan is a dry bed). `waterLevel` stays the sea's, so the ocean, its bakes and buoyancy never see inland water (7.1).
// flowAngle01 = the channel's direction (the flow bake keeps an authored direction). Coarse queries pass through.
export namespace Procedural
{
	struct RiverCarveConfig
	{
		float channelDepthScale = 2.5f;
		float channelMinDepth = 0.2f;  // model m
		float wallSlope = 0.1f;        // the channel's sides, rise / run
		float bankHeight = 2.16f;
		float floodplainCurve = 1.0f;
		float valleyDepth = 0.0f;      // model m
		float valleyDepthPerQ = 0.5f;
		float bankFactor = 0.5f;
		float floodplainFactor = 0.15f;
		float valleySlope = 0.28f;
		float reach = 1500.0f; // model m PAST the floodplain's edge
		float reachQ = 50.0f; // m3/s: the whole reach from this Q up, (Q / this)^reachQExponent of it below (0 = always whole)
		float reachQExponent = 0.1f; // higher = a small stream's valley narrower
		float lakeBedDeepen = 0.0f;      // model m: a lake's bed lowered by this much away from its shore
		float lakeBedDeepenReach = 8.0f; // native px from the shore over which that grows in (smoothstep: the shore keeps its slope)
		float gorgeWallSlope = 2.0f;   // rise / run of a waterfall's plunge gorge walls past the channel (no floodplain)
		float fallFaceSlope = 6.0f;     // rise / run of the ground under a falling sheet (its face starts before the lip as needed)
		float fallClearance = 1.0f;    // x the half-width: the drop stays a sharp break at the lip within it
		float whitewaterDepth = 3.0f;    // the channel's depth x up to this on rapids and falls (the steepness; 1 = none)
		float whitewaterWiden = 2.0f;    // the channel's half-width x up to this there (a shelf at the waterline; the ribbon is 1.2 x the channel)
		float waterHumidity = 0.35f;     // the humidity pulled toward 1 by this at a river's edge / a lake's shore (0 = off)
		float waterHumiditySpread = 150.0f; // engine m it fades out over
		float waterHumidityFullQ = 5.0f; // m3/s: a river this big gets it whole, a smaller one less (smoothstep)
		float lakeShore = 0.75f;        // native px past a lake's wet pixels its influence (the shore's sand, no grass) fades over
		bool operator==(const RiverCarveConfig&) const = default;

		// The valley wall's reach past the floodplain's edge for a channel of discharge q (model m).
		float wallReach(float q) const
		{
			const float r = reach > 0.0f ? reach : 0.0f;
			return reachQ > 0.0f && q < reachQ ? r * std::pow((q > 0.0f ? q : 0.0f) / reachQ, reachQExponent > 0.0f ? reachQExponent : 0.0f) : r;
		}

		// The floodplain's outer edge from the centre line (model m), for a channel of half-width hw.
		float floodplainEdge(float hw) const { return hw * (1.0f + bankFactor + floodplainFactor); }

		// The water surface the carve cuts to (model m): the unit profile's level sits a hydraulic depth ABOVE the
		// ground under it (W = bed + depth), so the surface is that ground, sunk by the valley depth.
		float surface(float unitWater, float unitDepth, float q) const
		{
			return unitWater - unitDepth - (valleyDepth + valleyDepthPerQ * std::pow(q > 0.0f ? q : 0.0f, 0.4f));
		}
		// The channel's depth below that surface (model m).
		float channelDepth(float unitDepth) const
		{
			const float d = unitDepth * channelDepthScale;
			return d > channelMinDepth ? d : channelMinDepth;
		}
		// THE CHANNEL'S CROSS-SECTION: how far below the surface the bed lies `e` in from the channel's edge, for a channel
		// of half-width hw (e, hw and the result in any one unit; the slope is the same in the model frame and the world):
		// the sides at "Channel wall slope" down to a FLAT bed at the channel depth D, rounded at the foot (a smooth min over
		// 0.15 D). The depth does not steepen the sides - a deeper channel's are only longer - and the slope never makes
		// the channel shallower: where the sides would not reach D within 80 % of the half-width (a narrow, deep channel)
		// they steepen just enough to, so at least the middle 20 % is bed at the full depth.
		static float channelCut(float e, float D, float hw, float slope)
		{
			const float minSlope = D / (0.8f * (hw > 1e-4f ? hw : 1e-4f));
			const float wall = (e > 0.0f ? e : 0.0f) * (slope > minSlope ? slope : minSlope);
			const float k = 0.15f * D;
			if (k <= 1e-6f)
				return wall < D ? wall : D;
			const float diff = wall > D ? wall - D : D - wall;
			const float h = (k - diff > 0.0f ? k - diff : 0.0f) / k;
			return (wall < D ? wall : D) - h * h * k * 0.25f;
		}
	};

	inline RiverCarveConfig riverCarveConfigFromSettings(const TerrainSettings& s)
	{
		RiverCarveConfig c;
		c.channelDepthScale = s.riverChannelDepthScale;
		c.channelMinDepth = s.riverChannelMinDepth;
		c.wallSlope = s.riverChannelWallSlope;
		c.bankHeight = s.riverBankHeight;
		c.floodplainCurve = s.riverFloodplainCurve;
		c.valleyDepth = s.riverValleyDepth;
		c.valleyDepthPerQ = s.riverValleyDepthPerQ;
		c.bankFactor = s.riverBankFactor;
		c.floodplainFactor = s.riverFloodplainFactor;
		c.valleySlope = s.riverValleySlope;
		c.reach = s.riverCarveReach;
		c.reachQ = s.riverCarveReachQ;
		c.reachQExponent = s.riverCarveReachQExponent;
		c.lakeBedDeepen = s.riverLakeBedDeepen;
		c.lakeBedDeepenReach = s.riverLakeBedDeepenReach;
		c.lakeShore = s.riverLakeShore;
		c.gorgeWallSlope = s.riverGorgeWallSlope;
		c.fallClearance = s.riverFallClearance;
		c.fallFaceSlope = s.riverFallFaceSlope;
		c.whitewaterDepth = s.riverWhitewaterDepth;
		c.whitewaterWiden = s.riverWhitewaterWiden;
		c.waterHumidity = s.riverWaterHumidity;
		c.waterHumiditySpread = s.riverWaterHumiditySpread;
		c.waterHumidityFullQ = s.riverWaterHumidityFullQ;
		return c;
	}

	// A unit ready to sample: the unit and its lookups (built on load, not stored on disk).
	struct PreparedRiverUnit
	{
		oc::shared_ptr<const RiverUnit> unit;
		int32 cellPx = 16;  // native px per grid cell
		int32 cells = 0;    // cells per side
		// Per cell, the PIECES whose carve reaches into it; a piece is the index of its first point (it runs to the
		// next point of the same segment).
		oc::vector<uint32> cellStart; // cells^2 + 1
		oc::vector<uint32> cellPieces;
		oc::vector<uint32> pointSegment; // per point: its segment
		oc::vector<uint16> pointSection; // per point: the waterfall lips before it in its segment (apply never blends across one)
		oc::vector<uint32> rowRuns;      // per unit row + 1: offsets into unit->lakeRuns
	};

	class RiverTerrain final : public ITerrainSampler
	{
	public:
		RiverTerrain(oc::shared_ptr<const TerrainGenV3> base, const RiverConfig& coarse, const RiverUnitConfig& units,
		             const RiverCarveConfig& carve);

		const TerrainGenV3& base() const { return *m_base; }
		const RiverUnitConfig& unitConfig() const { return m_unitCfg; }
		const RiverCarveConfig& carveConfig() const { return m_carve; }
		int32 unitPixels() const { return m_unitPx; } // native px per unit side

		// Blocking on a miss (builds or loads the unit; on a job the wait parks the fiber). Never null.
		oc::shared_ptr<const PreparedRiverUnit> unit(int32 ui, int32 uj) const;

		// The INLAND WATER SURFACE alone (world Y; `none` where no river channel or lake) on a grid laid out as
		// sampleGrid's, from the river units only (no terrain): the renderer's inland water map (the fog's underwater
		// boundary, the wetness under rivers and lakes). The river's calm carved surface inside its channel, else a lake's
		// level over its wet pixels (the nearest native pixel).
		void sampleInlandWaterGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
		                           oc::span<float> out, float none) const;

		void samplePoint(double worldX, double worldZ, TerrainPoint& out, ESampleDetail detail = ESampleDetail::Full) const override;
		void sampleGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
		                oc::span<TerrainPoint> out, ESampleDetail detail = ESampleDetail::Full) const override;
		void sampleRiverGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
		                     oc::span<float> outInfluence) const override;
		float sampleHeight(double worldX, double worldZ) const override;
		float sampleWaterHeight(double worldX, double worldZ) const override;
		float sampleHeightAndWater(double worldX, double worldZ, float& outWaterLevel) const override;
		float sampleTemperature(double worldX, double worldZ) const override;
		float sampleHumidity(double worldX, double worldZ) const override;
		float sampleFogThickness(double worldX, double worldZ) const override;
		float sampleFogHeightFalloff(double worldX, double worldZ) const override;
		float sampleFlowAngle01(double worldX, double worldZ) const override;
		float sampleAltitude(double worldX, double worldZ) const override;
		float seaLevel() const override;
		float lapseRatePerMetre() const override;

	private:
		struct Block; // the units a query rect touches, resolved up front
		void resolve(double x0, double z0, double x1, double z1, Block& out) const;
		void apply(const Block& b, double worldX, double worldZ, TerrainPoint& p) const;
		float influence(const Block& b, double worldX, double worldZ) const; // apply's `river`, nothing else
		// apply's water, nothing else; `river` (optional) = the water is a river channel's, not a lake's
		float inlandWater(const Block& b, double worldX, double worldZ, float none, bool* river = nullptr) const;
		// The grid grows past the unit by m_reachPx (the largest piece reach); each piece is listed in the cells its own reach
		// (its floodplain edge + the carve reach) touches. Then the unit's water is SUNK under the other rivers' carves
		// (sinkUnderCarves).
		oc::shared_ptr<const PreparedRiverUnit> prepare(oc::shared_ptr<const RiverUnit> unit) const;
		void matchInlets(RiverUnit& u) const;
		void fadeOpenEnds(RiverUnit& u) const;
		void meetLakeLevels(RiverUnit& u) const;
		void sinkUnderCarves(PreparedRiverUnit& p, RiverUnit& u) const;
		// The unit's edge summary (blocking on a miss, like `unit`: it builds the raw unit then, and hands it on).
		struct UnitEdges;
		oc::shared_ptr<const UnitEdges> unitEdges(int32 ui, int32 uj) const;
		// The unit as built (buildRiverUnit, or an empty one): from the hand-off when its summary was just built, else built.
		oc::shared_ptr<const RiverUnit> buildRaw(int32 ui, int32 uj) const;
		float pieceReach(float halfWidth, float q) const; // model m: the floodplain's edge (capped) + the wall's reach at q
		float wallReach(float q) const;                   // model m: the valley wall's reach past the floodplain at q
		// The ground (model m) piece a -> c leaves at distance d (model m) from its point at t, over the ground hg; its
		// water surface there in outWater.
		float pieceCarve(const RiverPoint& a, const RiverPoint& c, float t, float d, float hg, float& outWater) const;

		oc::shared_ptr<const TerrainGenV3> m_base;
		oc::shared_ptr<const CoarseRiverNetwork> m_network;
		RiverUnitConfig m_unitCfg;
		RiverCarveConfig m_carve;
		int32 m_unitPx = 1024;
		float m_reachPx = 15.0f; // the LARGEST piece reach in native px: the unit lookups' and grids' margin

		struct Pending
		{
			JobEvent done;
			oc::shared_ptr<const PreparedRiverUnit> result;
		};
		mutable std::mutex m_mutex; // never held across a build
		mutable oc::unordered_map<uint64, oc::shared_ptr<const PreparedRiverUnit>> m_units;
		mutable oc::deque<uint64> m_unitOrder; // insertion order, for the eviction
		mutable oc::unordered_map<uint64, oc::shared_ptr<Pending>> m_pending;
		// A unit's EDGE SUMMARY: the ends of its segments that leave through an outlet crossing, as built (all a
		// neighbour's inlet match reads) - a few floats per unit, so every unit touched keeps one, not its raw unit.
		struct UnitEdges
		{
			struct End
			{
				float x = 0.0f, z = 0.0f; // unit-local native px (the boundary point)
				float water = 0.0f;       // model m
				float halfWidth = 0.0f;   // model m
				float depth = 0.0f;       // model m
				float q = 0.0f;           // m3/s
			};
			oc::vector<End> outlets;
			bool empty = true;
		};
		struct EdgesPending
		{
			JobEvent done;
			oc::shared_ptr<const UnitEdges> result;
		};
		mutable oc::unordered_map<uint64, oc::shared_ptr<const UnitEdges>> m_edges;
		mutable oc::deque<uint64> m_edgesOrder;
		mutable oc::unordered_map<uint64, oc::shared_ptr<EdgesPending>> m_edgesPending;
		// THE HAND-OFF: a raw unit built for its edge summary waits here for its own `unit` call (which takes it out
		// instead of building it again); a few at most - one never asked for is dropped.
		mutable oc::unordered_map<uint64, oc::shared_ptr<const RiverUnit>> m_handoff;
		mutable oc::deque<uint64> m_handoffOrder;
	};
}
