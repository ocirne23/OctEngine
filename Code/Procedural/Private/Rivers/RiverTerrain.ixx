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
// w = half-width, D = the channel depth): the channel from the bed (S - D) up to S at d = w, ONE CURVE over the bank and
// the floodplain (w x ("Bank factor" + "Floodplain factor") wide) rising "Bank height" x D as (x / width)^"Floodplain
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
		float channelDepthScale = 1.0f;
		float channelMinDepth = 0.2f;  // model m
		float channelShape = 3.0f;
		float bankHeight = 2.16f;
		float floodplainCurve = 1.0f;
		float valleyDepth = 0.0f;      // model m
		float valleyDepthPerQ = 0.5f;
		float bankFactor = 0.5f;
		float floodplainFactor = 0.15f;
		float valleySlope = 0.28f;
		float reach = 1500.0f; // model m PAST the floodplain's edge
		float reachQ = 10.0f;  // m3/s: the whole reach from this Q up, sqrt(Q / this) of it below (0 = always whole)
		bool operator==(const RiverCarveConfig&) const = default;

		// The valley wall's reach past the floodplain's edge for a channel of discharge q (model m).
		float wallReach(float q) const
		{
			const float r = reach > 0.0f ? reach : 0.0f;
			return reachQ > 0.0f && q < reachQ ? r * std::sqrt((q > 0.0f ? q : 0.0f) / reachQ) : r;
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
	};

	inline RiverCarveConfig riverCarveConfigFromSettings(const TerrainSettings& s)
	{
		RiverCarveConfig c;
		c.channelDepthScale = s.riverChannelDepthScale;
		c.channelMinDepth = s.riverChannelMinDepth;
		c.channelShape = s.riverChannelShape;
		c.bankHeight = s.riverBankHeight;
		c.floodplainCurve = s.riverFloodplainCurve;
		c.valleyDepth = s.riverValleyDepth;
		c.valleyDepthPerQ = s.riverValleyDepthPerQ;
		c.bankFactor = s.riverBankFactor;
		c.floodplainFactor = s.riverFloodplainFactor;
		c.valleySlope = s.riverValleySlope;
		c.reach = s.riverCarveReach;
		c.reachQ = s.riverCarveReachQ;
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
		float inlandWater(const Block& b, double worldX, double worldZ, float none) const; // apply's water, nothing else
		// The grid grows past the unit by m_reachPx (the largest piece reach); each piece is listed in the cells its own reach
		// (its floodplain edge + the carve reach) touches. Then the unit's water is SUNK under the other rivers' carves
		// (sinkUnderCarves).
		oc::shared_ptr<const PreparedRiverUnit> prepare(oc::shared_ptr<const RiverUnit> unit) const;
		void sinkUnderCarves(PreparedRiverUnit& p, RiverUnit& u) const;
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
	};
}
