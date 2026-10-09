module Procedural;

import Core;
import Threading;

import :TerrainSampler;
import :GeneratorV3;
import :RiverRouting;
import :RiverNetwork;
import :RiverUnits;
import :RiverTerrain;

namespace
{
	using namespace Procedural;

	int32 floorDivI(int32 a, int32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
	uint64 unitKey(int32 ui, int32 uj) { return ((uint64)(uint32)ui << 32) | (uint64)(uint32)uj; }

	constexpr size_t c_maxUnits = 256; // resident prepared units (a few MB each at most)
	// The cap on a piece's floodplain edge (model m): the unit lookups' margin must be bounded, and the half-width grows
	// with Q without limit (a ~1000 m3/s river at "Width a" 20 is ~300 m model half-width).
	constexpr float c_maxFloodplainM = 1500.0f;

	float smoothstep01(float t)
	{
		t = oc::clamp(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// A segment's first (or, atEnd, last) `lenPx` GROWS IN (fades out): its width and depth from x `start` at that end to
	// x 1 (smoothstep), the surface kept (water - depth - the valley sink).
	void growSegmentEnd(RiverUnit& u, const RiverSegment& s, bool atEnd, float start, float lenPx)
	{
		float along = 0.0f;
		for (uint32 i = 0; i < s.count; i++)
		{
			const uint32 k = atEnd ? s.count - 1 - i : i;
			RiverPoint& p = u.points[s.first + k];
			if (i > 0)
			{
				const RiverPoint& a = u.points[s.first + (atEnd ? k + 1 : k - 1)];
				along += std::sqrt((p.x - a.x) * (p.x - a.x) + (p.z - a.z) * (p.z - a.z));
			}
			if (along >= lenPx)
				break;
			const float t = along / lenPx;
			const float grow = start + (1.0f - start) * (t * t * (3.0f - 2.0f * t));
			p.halfWidth *= grow;
			p.water -= p.depth * (1.0f - grow);
			p.depth *= grow;
		}
	}

	// Polynomial smooth min: equal to min(a, b) once they are k apart.
	float smin(float a, float b, float k)
	{
		const float h = oc::max(k - std::abs(a - b), 0.0f) / k;
		return oc::min(a, b) - h * h * k * 0.25f;
	}

	// A segment's carve is the 1 / d^6 blend of its pieces' carves (d = the distance to the piece, model m): on the river
	// line its own piece alone, a continuous ramp where two parts of one river meet (a hairpin with a drop between).
	float segmentWeight(float d)
	{
		const float x = d * (1.0f / 100.0f);
		const float x2 = x * x;
		return 1.0f / (x2 * x2 * x2 + 1e-12f);
	}
}

namespace Procedural
{
	struct RiverTerrain::Block
	{
		int32 ui0 = 0, uj0 = 0, uh = 0, uw = 0;
		oc::vector<oc::shared_ptr<const PreparedRiverUnit>> units;
		const PreparedRiverUnit* at(int32 ui, int32 uj) const
		{
			const int32 i = ui - ui0, j = uj - uj0;
			if (i < 0 || j < 0 || i >= uh || j >= uw)
				return nullptr;
			return units[(size_t)i * uw + j].get();
		}
	};

	RiverTerrain::RiverTerrain(oc::shared_ptr<const TerrainGenV3> base, const RiverConfig& coarse,
	                           const RiverUnitConfig& units, const RiverCarveConfig& carve)
		: m_base(oc::move(base))
		, m_unitCfg(units)
		, m_carve(carve)
	{
		m_network = oc::make_shared<const CoarseRiverNetwork>(m_base, coarse);
		m_unitPx = oc::clamp(m_unitCfg.unitTiles, 1, 8) * TerrainGenV3::fullTilePixels();
		m_reachPx = (c_maxFloodplainM + oc::max(m_carve.reach, 0.0f)) / TerrainGenV3::nativeResolution();
	}

	// The valley wall's reach at discharge q: "Carve reach" scaled by sqrt(Q / "Carve reach Q") below that, and by the
	// channel's growth ("Fade Q", as its width and depth) - a stream's head cuts no valley.
	float RiverTerrain::wallReach(float q) const
	{
		const float growth = smoothstep01((q - m_unitCfg.channelMinQ) / oc::max(m_unitCfg.fadeQ, 1e-4f));
		return m_carve.wallReach(q) * growth;
	}

	float RiverTerrain::pieceReach(float halfWidth, float q) const
	{
		return oc::min(m_carve.floodplainEdge(halfWidth), c_maxFloodplainM) + wallReach(q);
	}

	// The cross-section: the channel (its sides at the wall slope down to a flat bed: channelCut), then ONE curve over the bank and the floodplain
	// up to "Bank height" x the hydraulic depth at the floodplain's edge, then the valley wall. The floodplain is carved whole; the wall fades
	// out over the last 30 % of its reach past the edge.
	float RiverTerrain::pieceCarve(const RiverPoint& a, const RiverPoint& c, float t, float d, float hg, float& outWater) const
	{
		const float hw = oc::max(a.halfWidth + (c.halfWidth - a.halfWidth) * t, 0.05f);
		const float unitD = oc::max(a.depth + (c.depth - a.depth) * t, 0.02f);
		const float q = a.q + (c.q - a.q) * t;
		// The water at the original ground, sunk by the valley depth; the channel cut below it.
		const float W = m_carve.surface(a.water + (c.water - a.water) * t, unitD, q);
		const float D = oc::max(m_carve.channelDepth(unitD), 0.02f);
		// The bank's rise is the HYDRAULIC depth's, not the channel's: "Channel depth scale" deepens the channel under the
		// water only - scaling the bank too, over its fixed width, steepened it with every step.
		const float floodH = m_carve.bankHeight * unitD;
		const float edge = oc::min(m_carve.floodplainEdge(hw), c_maxFloodplainM);
		const float floodW = oc::max(edge - hw, 1e-3f);
		float target;
		if (d < hw)
			target = W - RiverCarveConfig::channelCut(hw - d, D, hw, oc::max(m_carve.wallSlope, 0.01f));
		else if (d < edge)
			target = W + floodH * std::pow((d - hw) / floodW, oc::max(m_carve.floodplainCurve, 0.1f));
		else
			target = W + floodH + m_carve.valleySlope * (d - edge);
		const float soft = oc::max(0.5f * unitD, 0.1f);
		const float reach = wallReach(q);
		const float fade = reach > 1e-3f
			? 1.0f - smoothstep01((d - edge - 0.7f * reach) / (0.3f * reach))
			: (d < edge ? 1.0f : 0.0f);
		outWater = W;
		return hg + (smin(hg, target, soft) - hg) * fade;
	}

	oc::shared_ptr<const PreparedRiverUnit> RiverTerrain::prepare(oc::shared_ptr<const RiverUnit> unit) const
	{
		auto p = oc::make_shared<PreparedRiverUnit>();
		const RiverUnit& u = *unit;
		const int32 W = u.tiles * TerrainGenV3::fullTilePixels();
		const int32 pad = (int32)std::ceil(m_reachPx);
		const float nr = TerrainGenV3::nativeResolution();
		// The grid covers the unit grown by the reach, so a query in a NEIGHBOUR unit finds the pieces that reach it.
		p->cellPx = 16;
		p->cells = (W + 2 * pad + p->cellPx - 1) / p->cellPx;
		const int32 cells = p->cells;
		const auto cellRange = [&](float lo, float hi, int32& c0, int32& c1)
		{
			c0 = oc::clamp((int32)std::floor((lo + (float)pad) / (float)p->cellPx), 0, cells - 1);
			c1 = oc::clamp((int32)std::floor((hi + (float)pad) / (float)p->cellPx), 0, cells - 1);
		};

		p->pointSegment.assign(u.points.size(), 0);
		oc::vector<uint32> counts((size_t)cells * cells + 1, 0);
		for (int32 pass = 0; pass < 2; pass++)
		{
			for (uint32 s = 0; s < (uint32)u.segments.size(); s++)
			{
				const RiverSegment& seg = u.segments[s];
				for (uint32 k = 0; k < seg.count; k++)
					p->pointSegment[seg.first + k] = s;
				for (uint32 k = 0; k + 1 < seg.count; k++)
				{
					const RiverPoint& a = u.points[seg.first + k];
					const RiverPoint& b = u.points[seg.first + k + 1];
					// This piece's own reach (the wider, bigger end's), within the grid's pad.
					const float reachPx = oc::min(pieceReach(oc::max(a.halfWidth, b.halfWidth), oc::max(a.q, b.q)) / nr, (float)pad);
					int32 x0, x1, z0, z1;
					cellRange(oc::min(a.x, b.x) - reachPx, oc::max(a.x, b.x) + reachPx, x0, x1);
					cellRange(oc::min(a.z, b.z) - reachPx, oc::max(a.z, b.z) + reachPx, z0, z1);
					for (int32 cz = z0; cz <= z1; cz++)
						for (int32 cx = x0; cx <= x1; cx++)
						{
							const size_t c = (size_t)cz * cells + cx;
							if (pass == 0)
								counts[c + 1]++;
							else
								p->cellPieces[counts[c]++] = seg.first + k;
						}
				}
			}
			if (pass == 0)
			{
				for (size_t c = 1; c < counts.size(); c++)
					counts[c] += counts[c - 1];
				p->cellStart = counts;
				p->cellPieces.resize(counts.back());
			}
		}

		// The lake runs are row-major already: per-row offsets.
		p->rowRuns.assign((size_t)W + 1, 0);
		for (const RiverLakeRun& r : u.lakeRuns)
			p->rowRuns[(size_t)r.row + 1]++;
		for (size_t r = 1; r < p->rowRuns.size(); r++)
			p->rowRuns[r] += p->rowRuns[r - 1];
		if (u.empty || u.points.empty())
		{
			p->unit = oc::move(unit);
			return p;
		}
		auto sunk = oc::make_shared<RiverUnit>(u); // the store's unit stays as built (and as cached on disk)
		matchInlets(*sunk);
		fadeOpenEnds(*sunk);
		sinkUnderCarves(*p, *sunk);
		p->unit = oc::move(sunk);
		return p;
	}

	// THE OPEN ENDS: a river that ends neither in water nor in another river - through a unit's soft wall (Edge: the
	// neighbour does not continue it), in a sink (its coarse tile drains nowhere), or where its water runs dry - kept
	// its full width and depth to the end and stopped as a cut (seen 2026-10-09). It FADES OUT instead: its width and
	// depth to 0 over its last c_fadePx (at most half the segment), the surface kept.
	// An OUTLET the downstream unit cannot carry on counts too: the river reaches its crossing more than 2 x "Unit breach
	// depth" under the crossing's level (the edge reroute's gorge from water already under its exit's ground: 164 model m,
	// 2026-10-09), past what the inlet match lowers - the neighbour's inlet grows in from nothing instead (matchInlets).
	void RiverTerrain::fadeOpenEnds(RiverUnit& u) const
	{
		constexpr float c_fadePx = 200.0f; // native px (1 km at mpp 5)
		const float maxDrop = 2.0f * m_unitCfg.breachDepth;
		const auto unmatchedOutlet = [&](const RiverSegment& s)
		{
			if (s.end != ERiverEnd::Outlet)
				return false;
			const RiverPoint& pe = u.points[s.first + s.count - 1];
			for (const RiverCrossing& c : u.crossings)
				if (!c.inlet && std::abs(c.x - pe.x) <= 1.5f && std::abs(c.z - pe.z) <= 1.5f)
					return pe.water <= c.water - maxDrop;
			return false;
		};
		for (const RiverSegment& s : u.segments)
		{
			if (s.count < 2)
				continue;
			if (s.end != ERiverEnd::Edge && s.end != ERiverEnd::Sink && s.end != ERiverEnd::Dry && !unmatchedOutlet(s))
				continue;
			float total = 0.0f;
			for (uint32 k = 1; k < s.count; k++)
			{
				const RiverPoint& a = u.points[s.first + k - 1];
				const RiverPoint& b = u.points[s.first + k];
				total += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
			}
			const float len = oc::min(c_fadePx, 0.5f * total);
			if (len > 1e-3f)
				growSegmentEnd(u, s, /*atEnd*/ true, 0.0f, len);
		}
	}

	// THE INLET MATCH: a crossing's level is the shared tile edge's ground + depth, and a segment starting at an inlet
	// starts there - but the upstream unit's water can reach that point LOWER: the edge reroute (RiverUnits) breaches a
	// rim on the way and keeps the exit's level through its gorge, tens of metres under the crossing's ground - a step at
	// the unit edge. So a segment starting on the unit's border looks up the upstream unit's segment ending at the same
	// boundary point (its EDGE SUMMARY, `unitEdges`: from its raw unit; units build independently, so this never chains) and carries that lower level on: its
	// water is held at or below it - the gorge continues and the carve cuts it out of the rim until the ground falls under
	// it - and so are the segments it flows on into (a junction's next one). Only ever lowers.
	void RiverTerrain::matchInlets(RiverUnit& u) const
	{
		const int32 W = u.tiles * TerrainGenV3::fullTilePixels();
		const float edgeLo = -0.25f, edgeHi = (float)W - 0.75f; // the boundary points sit half a pixel outside the unit
		oc::vector<oc::pair<uint32, float>> lowered; // segment -> the level it must stay under
		for (uint32 si = 0; si < (uint32)u.segments.size(); si++)
		{
			const RiverSegment& s = u.segments[si];
			if (s.count == 0)
				continue;
			const RiverPoint& p0 = u.points[s.first];
			int32 ni = u.ui, nj = u.uj;
			if (p0.x <= edgeLo) nj--;
			else if (p0.x >= edgeHi) nj++;
			else if (p0.z <= edgeLo) ni--;
			else if (p0.z >= edgeHi) ni++;
			else
				continue; // not on the unit's border
			const oc::shared_ptr<const UnitEdges> nb = unitEdges(ni, nj);
			if (!nb || nb->empty)
				continue;
			const float bx = p0.x + (float)((u.uj - nj) * W), bz = p0.z + (float)((u.ui - ni) * W);
			float level = FLT_MAX;
			float upHalf = 0.0f; // the arriving river's half-width at the boundary
			for (const UnitEdges::End& pe : nb->outlets)
				if (std::abs(pe.x - bx) < 0.75f && std::abs(pe.z - bz) < 0.75f)
				{
					level = oc::min(level, pe.water);
					upHalf = oc::max(upHalf, pe.halfWidth);
				}
			// UNBACKED or UNDERSIZED: no river of the upstream unit reaches this inlet - its water drained elsewhere (a pond,
			// another edge) where the coarse network did not see it - or one far smaller than the coarse Q the inlet injects
			// (the outlet's Q blend only matches sizes that are close: RiverUnits). Rather than start a full-size river
			// there, it GROWS IN: its width and depth from the arriving river's (0 when none) to its own over c_growInPx.
			// At most 2 x "Unit breach depth": no breach is deeper (a deeper basin is a pond, RiverRouting), so a bigger
			// mismatch is no gorge to continue but a disagreement - carried on for kilometres it carved a canyon. Such an
			// inlet is UNBACKED too (the upstream river fades out before the edge: fadeOpenEnds).
			const float maxDrop = 2.0f * m_unitCfg.breachDepth;
			constexpr float c_growInPx = 200.0f; // native px (1 km at mpp 5)
			if (level == FLT_MAX || level <= p0.water - maxDrop)
			{
				growSegmentEnd(u, s, /*atEnd*/ false, 0.0f, c_growInPx);
				continue;
			}
			if (upHalf < 0.5f * p0.halfWidth && p0.halfWidth > 1e-4f)
				growSegmentEnd(u, s, /*atEnd*/ false, upHalf / p0.halfWidth, c_growInPx);
			if (level < p0.water - 0.01f && level > p0.water - maxDrop)
				lowered.push_back({ si, level });
		}
		// Hold each one under its level, then the segments it flows on into (a segment starting where it ends).
		for (size_t n = 0; n < lowered.size(); n++)
		{
			const auto [si, level] = lowered[n];
			const RiverSegment& s = u.segments[si];
			float last = level;
			for (uint32 k = 0; k < s.count; k++)
			{
				RiverPoint& p = u.points[s.first + k];
				p.water = oc::min(p.water, level);
				last = p.water;
			}
			if (s.end != ERiverEnd::Junction)
				continue;
			const RiverPoint& pe = u.points[s.first + s.count - 1];
			for (uint32 ti = 0; ti < (uint32)u.segments.size(); ti++)
			{
				const RiverSegment& t = u.segments[ti];
				if (ti == si || t.count == 0)
					continue;
				const RiverPoint& tp = u.points[t.first];
				if (std::abs(tp.x - pe.x) < 0.75f && std::abs(tp.z - pe.z) < 0.75f && last < tp.water - 0.01f && lowered.size() < 4096)
					lowered.push_back({ ti, last });
			}
		}
	}

	// THE OTHER RIVERS' CARVES: a river's profile was walked over the UNCARVED ground, but every other river's valley wall
	// cuts the ground down to its slope - a steep stream on the side of a big river's valley was left standing on the old
	// hillside, tens of metres above the carved one (seen 2026-10-09). So each point's surface is held at or below the
	// ground the OTHER segments' carves leave there (never below such a river's own surface: a tributary meets the main
	// river at its level), then made non-rising downstream again; the river's own channel is then cut below that. Two
	// passes, as a sunk river's carve moves too. Only this unit's rivers: a neighbour unit's wall reaching across the
	// boundary is not seen.
	void RiverTerrain::sinkUnderCarves(PreparedRiverUnit& p, RiverUnit& u) const
	{
		const TerrainConfigV3& gc = m_base->config();
		const double mpp = (double)gc.metersPerPixel;
		const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;
		const float nr = TerrainGenV3::nativeResolution();
		const int32 pad = (int32)std::ceil(m_reachPx);
		const double px0 = (double)u.uj * (double)m_unitPx, pz0 = (double)u.ui * (double)m_unitPx;

		// The uncarved ground under every point (model m).
		oc::vector<float> ground(u.points.size(), 0.0f);
		for (size_t k = 0; k < u.points.size(); k++)
		{
			const RiverPoint& pt = u.points[k];
			ground[k] = (m_base->sampleHeight((px0 + (double)pt.x) * mpp - (double)gc.originX,
				(pz0 + (double)pt.z) * mpp - (double)gc.originZ) - gc.seaLevel) / vs;
		}

		for (int32 pass = 0; pass < 2; pass++)
			for (uint32 s = 0; s < (uint32)u.segments.size(); s++)
			{
				const RiverSegment& seg = u.segments[s];
				float prevS = FLT_MAX;
				for (uint32 i = 0; i < seg.count; i++)
				{
					const uint32 idx = seg.first + i;
					RiverPoint& pt = u.points[idx];
					const float S = m_carve.surface(pt.water, pt.depth, pt.q);
					float limit = oc::min(S, prevS);
					const int32 cx = (int32)std::floor((pt.x + (float)pad) / (float)p.cellPx);
					const int32 cz = (int32)std::floor((pt.z + (float)pad) / (float)p.cellPx);
					if (cx >= 0 && cz >= 0 && cx < p.cells && cz < p.cells)
					{
						// Per other segment the blend of its pieces' carves and surfaces (apply's rule); a cell lists its
						// pieces segment by segment.
						const float hg = ground[idx];
						const size_t cell = (size_t)cz * p.cells + cx;
						uint32 curSeg = UINT32_MAX;
						float sumW = 0.0f, sumC = 0.0f, sumS = 0.0f;
						const auto commit = [&]
						{
							if (sumW > 0.0f)
								limit = oc::min(limit, oc::max(sumC / sumW, sumS / sumW));
						};
						for (uint32 n = p.cellStart[cell]; n < p.cellStart[cell + 1]; n++)
						{
							const uint32 k = p.cellPieces[n];
							const uint32 ks = p.pointSegment[k];
							if (ks == s)
								continue;
							if (ks != curSeg)
							{
								commit();
								curSeg = ks;
								sumW = sumC = sumS = 0.0f;
							}
							const RiverPoint& a = u.points[k];
							const RiverPoint& c = u.points[k + 1];
							const float abx = c.x - a.x, abz = c.z - a.z;
							const float len2 = abx * abx + abz * abz;
							const float t = len2 > 1e-12f ? oc::clamp(((pt.x - a.x) * abx + (pt.z - a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
							const float dx = a.x + abx * t - pt.x, dz = a.z + abz * t - pt.z;
							const float d = std::sqrt(dx * dx + dz * dz) * nr;
							const float hw = oc::max(a.halfWidth + (c.halfWidth - a.halfWidth) * t, 0.05f);
							if (d >= pieceReach(hw, a.q + (c.q - a.q) * t))
								continue;
							float Wj;
							const float w = segmentWeight(d);
							sumC += w * pieceCarve(a, c, t, d, hg, Wj);
							sumS += w * Wj;
							sumW += w;
						}
						commit();
					}
					pt.water -= S - limit; // surface() is the water less a fixed sink: the same shift
					prevS = limit;
				}
			}

		// The rapids / fall marks follow the new levels.
		for (const RiverSegment& seg : u.segments)
			for (uint32 i = 0; i + 1 < seg.count; i++)
			{
				RiverPoint& a = u.points[seg.first + i];
				const RiverPoint& b = u.points[seg.first + i + 1];
				const float dist = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z)) * nr;
				const float slope = dist > 1e-3f ? (a.water - b.water) / dist : 0.0f;
				a.flags &= (uint8)~(RiverPoint_Rapids | RiverPoint_Fall);
				if (slope >= m_unitCfg.fallSlope)
					a.flags |= RiverPoint_Fall;
				else if (slope >= m_unitCfg.rapidsSlope)
					a.flags |= RiverPoint_Rapids;
			}
	}

	// The unit AS BUILT (buildRiverUnit - from its disk cache when it has one - or an empty unit): taken from the hand-off
	// when its edge summary was just built from it, else built. Units build independently, so nothing here chains.
	oc::shared_ptr<const RiverUnit> RiverTerrain::buildRaw(int32 ui, int32 uj) const
	{
		const uint64 key = unitKey(ui, uj);
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_handoff.find(key);
			if (it != m_handoff.end())
			{
				oc::shared_ptr<const RiverUnit> raw = oc::move(it->second);
				m_handoff.erase(it); // its key stays in the order queue: a later pop finds nothing to erase
				return raw;
			}
		}
		oc::shared_ptr<const RiverUnit> built = buildRiverUnit(*m_base, *m_network, m_unitCfg, ui, uj, nullptr);
		if (!built)
		{
			auto empty = oc::make_shared<RiverUnit>();
			empty->ui = ui;
			empty->uj = uj;
			empty->tiles = m_unitCfg.unitTiles;
			empty->empty = true;
			built = empty;
		}
		return built;
	}

	// The unit's EDGE SUMMARY - what a neighbour's inlet match reads - deduplicated like `unit`. Building it builds the
	// raw unit, which then waits in the hand-off for this unit's own preparation (at most c_handoffUnits of them).
	oc::shared_ptr<const RiverTerrain::UnitEdges> RiverTerrain::unitEdges(int32 ui, int32 uj) const
	{
		constexpr size_t c_maxEdges = 4096;   // tiny each
		constexpr size_t c_handoffUnits = 16;
		const uint64 key = unitKey(ui, uj);
		oc::shared_ptr<EdgesPending> pending;
		bool owner = false;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_edges.find(key);
			if (it != m_edges.end())
				return it->second;
			auto pit = m_edgesPending.find(key);
			if (pit != m_edgesPending.end())
				pending = pit->second;
			else
			{
				pending = oc::make_shared<EdgesPending>();
				m_edgesPending[key] = pending;
				owner = true;
			}
		}
		if (!owner)
		{
			pending->done.wait();
			return pending->result;
		}
		oc::shared_ptr<const RiverUnit> raw = buildRaw(ui, uj);
		auto edges = oc::make_shared<UnitEdges>();
		edges->empty = raw->empty;
		for (const RiverSegment& s : raw->segments)
			if (s.end == ERiverEnd::Outlet && s.count > 0)
			{
				const RiverPoint& pe = raw->points[s.first + s.count - 1];
				edges->outlets.push_back({ pe.x, pe.z, pe.water, pe.halfWidth });
			}
		pending->result = edges;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_edges[key] = edges;
			m_edgesOrder.push_back(key);
			while (m_edgesOrder.size() > c_maxEdges)
			{
				m_edges.erase(m_edgesOrder.front());
				m_edgesOrder.pop_front();
			}
			m_handoff[key] = oc::move(raw);
			m_handoffOrder.push_back(key);
			while (m_handoffOrder.size() > c_handoffUnits)
			{
				m_handoff.erase(m_handoffOrder.front());
				m_handoffOrder.pop_front();
			}
			m_edgesPending.erase(key);
		}
		pending->done.signal();
		return pending->result;
	}

	oc::shared_ptr<const PreparedRiverUnit> RiverTerrain::unit(int32 ui, int32 uj) const
	{
		const uint64 key = unitKey(ui, uj);
		oc::shared_ptr<Pending> pending;
		bool owner = false;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_units.find(key);
			if (it != m_units.end())
				return it->second;
			auto pit = m_pending.find(key);
			if (pit != m_pending.end())
				pending = pit->second;
			else
			{
				pending = oc::make_shared<Pending>();
				m_pending[key] = pending;
				owner = true;
			}
		}
		if (!owner)
		{
			// One build serves everyone: a fiber parks, an unregistered thread blocks.
			pending->done.wait();
			return pending->result;
		}

		// Its edge summary first (a neighbour may already have it built, or will want it): that builds the raw unit and
		// hands it on, so buildRaw only takes it - built once either way.
		(void)unitEdges(ui, uj);
		pending->result = prepare(buildRaw(ui, uj));
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_units[key] = pending->result;
			m_unitOrder.push_back(key);
			while (m_unitOrder.size() > c_maxUnits)
			{
				m_units.erase(m_unitOrder.front()); // holders keep their shared_ptr
				m_unitOrder.pop_front();
			}
			m_pending.erase(key);
		}
		pending->done.signal();
		return pending->result;
	}

	void RiverTerrain::resolve(double x0, double z0, double x1, double z1, Block& out) const
	{
		const TerrainConfigV3& gc = m_base->config();
		const double inv = 1.0 / (double)gc.metersPerPixel;
		const double pad = (double)m_reachPx + 1.0;
		const int32 j0 = floorDivI((int32)std::floor((x0 + (double)gc.originX) * inv - pad), m_unitPx);
		const int32 j1 = floorDivI((int32)std::floor((x1 + (double)gc.originX) * inv + pad), m_unitPx);
		const int32 i0 = floorDivI((int32)std::floor((z0 + (double)gc.originZ) * inv - pad), m_unitPx);
		const int32 i1 = floorDivI((int32)std::floor((z1 + (double)gc.originZ) * inv + pad), m_unitPx);
		out.ui0 = i0;
		out.uj0 = j0;
		out.uh = i1 - i0 + 1;
		out.uw = j1 - j0 + 1;
		out.units.assign((size_t)out.uh * out.uw, nullptr);
		for (int32 i = 0; i < out.uh; i++)
			for (int32 j = 0; j < out.uw; j++)
			{
				// Past the generated bounds no unit has tiles: skip the build (the sample there is coarse anyway).
				const int32 t = m_unitPx / TerrainGenV3::fullTilePixels();
				bool anyTile = false;
				for (int32 a = 0; a < t && !anyTile; a++)
					for (int32 b = 0; b < t && !anyTile; b++)
						anyTile = m_base->fullTileInBounds((i0 + i) * t + a, (j0 + j) * t + b);
				if (anyTile)
					out.units[(size_t)i * out.uw + j] = unit(i0 + i, j0 + j);
			}
	}

	void RiverTerrain::apply(const Block& b, double worldX, double worldZ, TerrainPoint& p) const
	{
		const TerrainConfigV3& gc = m_base->config();
		const double inv = 1.0 / (double)gc.metersPerPixel;
		const double gx = (worldX + (double)gc.originX) * inv;
		const double gz = (worldZ + (double)gc.originZ) * inv;
		const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;
		const float nr = TerrainGenV3::nativeResolution();

		const float hModel = (p.height - gc.seaLevel) / vs;
		float carved = hModel;
		float influence = 0.0f, speed = 0.0f;
		float bestRatio = 2.0f; // inside a channel when < 1
		float bestW = 0.0f, bestAngle = -1.0f;
		bool bestDry = false;

		const int32 ui0 = floorDivI((int32)std::floor(gz - (double)m_reachPx), m_unitPx);
		const int32 ui1 = floorDivI((int32)std::floor(gz + (double)m_reachPx), m_unitPx);
		const int32 uj0 = floorDivI((int32)std::floor(gx - (double)m_reachPx), m_unitPx);
		const int32 uj1 = floorDivI((int32)std::floor(gx + (double)m_reachPx), m_unitPx);
		for (int32 ui = ui0; ui <= ui1; ui++)
			for (int32 uj = uj0; uj <= uj1; uj++)
			{
				const PreparedRiverUnit* pu = b.at(ui, uj);
				if (!pu || pu->unit->empty || pu->cells == 0)
					continue;
				const RiverUnit& u = *pu->unit;
				const float lx = (float)(gx - (double)uj * (double)m_unitPx);
				const float lz = (float)(gz - (double)ui * (double)m_unitPx);
				const int32 pad = (int32)std::ceil(m_reachPx);
				const int32 cx = (int32)std::floor((lx + (float)pad) / (float)pu->cellPx);
				const int32 cz = (int32)std::floor((lz + (float)pad) / (float)pu->cellPx);
				if (cx < 0 || cz < 0 || cx >= pu->cells || cz >= pu->cells)
					continue;
				const size_t cell = (size_t)cz * pu->cells + cx;
				// EACH SEGMENT CARVES WITH A DISTANCE BLEND OF ITS PIECES (segmentWeight); the segments compose by min. A
				// min over every piece let a reach steeper than "Valley slope" cut under its own upstream water: the
				// downstream pieces' lower walls reached back up the river line (a fall stood on the old hillside,
				// 2026-10-09); the nearest piece alone left cliffs where it switched. A cell lists its pieces segment by
				// segment.
				uint32 curSeg = UINT32_MAX;
				float sumW = 0.0f, sumC = 0.0f;
				for (uint32 n = pu->cellStart[cell]; n < pu->cellStart[cell + 1]; n++)
				{
					const uint32 k = pu->cellPieces[n];
					const uint32 ks = pu->pointSegment[k];
					if (ks != curSeg)
					{
						if (sumW > 0.0f)
							carved = oc::min(carved, sumC / sumW);
						curSeg = ks;
						sumW = sumC = 0.0f;
					}
					const RiverPoint& a = u.points[k];
					const RiverPoint& c = u.points[k + 1];
					const float abx = c.x - a.x, abz = c.z - a.z;
					const float len2 = abx * abx + abz * abz;
					const float t = len2 > 1e-12f ? oc::clamp(((lx - a.x) * abx + (lz - a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
					const float px = a.x + abx * t - lx, pz = a.z + abz * t - lz;
					const float d = std::sqrt(px * px + pz * pz) * nr; // model m
					const float hw = oc::max(a.halfWidth + (c.halfWidth - a.halfWidth) * t, 0.05f);
					const float q = a.q + (c.q - a.q) * t;
					if (d >= pieceReach(hw, q))
						continue;
					{
						float W;
						const float w = segmentWeight(d);
						sumW += w;
						sumC += w * pieceCarve(a, c, t, d, hModel, W);
					}

					const float bankEnd = oc::min(m_carve.floodplainEdge(hw), c_maxFloodplainM);
					const float inf = d < hw ? 1.0f : oc::clamp(1.0f - (d - hw) / oc::max(bankEnd - hw, 1e-3f), 0.0f, 1.0f);
					if (inf > influence)
					{
						influence = inf;
						// Its FLOW SPEED, as the water ribbons' (RiverSystem segmentWater): Manning, v = depth^(2/3)
						// sqrt(slope) / 0.035, the slope the carved surface's drop over the piece. A dry bed has none.
						speed = 0.0f;
						if (!u.segments[pu->pointSegment[k]].ephemeral)
						{
							const float lenM = std::sqrt(len2) * nr;
							const float drop = lenM > 1e-3f
								? (m_carve.surface(a.water, a.depth, a.q) - m_carve.surface(c.water, c.depth, c.q)) / lenM : 0.0f;
							speed = oc::clamp(std::pow(oc::max(a.depth + (c.depth - a.depth) * t, 0.05f), 0.6667f)
								* std::sqrt(oc::max(drop, 1e-5f)) / 0.035f, 0.1f, 6.0f);
						}
					}
					const float ratio = d / hw;
					if (ratio < bestRatio)
					{
						bestRatio = ratio;
						bestW = m_carve.surface(a.water + (c.water - a.water) * t, oc::max(a.depth + (c.depth - a.depth) * t, 0.02f), q);
						bestDry = u.segments[pu->pointSegment[k]].ephemeral != 0;
						float ang = std::atan2(abz, abx) * (0.5f / 3.14159265f);
						bestAngle = ang < 0.0f ? ang + 1.0f : ang;
					}
				}
				if (sumW > 0.0f)
					carved = oc::min(carved, sumC / sumW);
			}

		p.height = gc.seaLevel + carved * vs;
		p.river = influence;
		p.riverSpeed = speed;
		if (bestRatio < 1.0f)
		{
			p.flowAngle01 = bestAngle;
			if (bestDry)
				p.dryBed = 1;
			else
			{
				p.waterKind = ETerrainWater::River;
				p.inlandWater = gc.seaLevel + bestW * vs;
			}
		}

		// Lakes: the wet pixel under the point (nearest native pixel).
		const int32 ui = floorDivI((int32)std::floor(gz), m_unitPx), uj = floorDivI((int32)std::floor(gx), m_unitPx);
		if (const PreparedRiverUnit* pu = b.at(ui, uj); pu && !pu->unit->empty && !pu->unit->lakeRuns.empty())
		{
			const RiverUnit& u = *pu->unit;
			const int32 row = (int32)std::floor(gz - (double)ui * (double)m_unitPx + 0.5);
			const int32 col = (int32)std::floor(gx - (double)uj * (double)m_unitPx + 0.5);
			bool inLake = false;
			if (row >= 0 && col >= 0 && row < m_unitPx && col < m_unitPx)
				for (uint32 n = pu->rowRuns[row]; n < pu->rowRuns[row + 1]; n++)
				{
					const RiverLakeRun& run = u.lakeRuns[n];
					if (col < (int32)run.x0 || col >= (int32)run.x0 + (int32)run.len)
						continue;
					const RiverLake& lake = u.lakes[run.lake];
					if (lake.kind == ERiverWater::Pan)
						p.dryBed = 1;
					else
					{
						inLake = true;
						const float level = gc.seaLevel + lake.level * vs;
						// A CARVED end lake (RiverUnits): the ground it does not hold dug c_carvedLakeBedM under its level.
						constexpr float c_carvedLakeBedM = 3.0f; // model m
						if (lake.carved)
							p.height = oc::min(p.height, level - c_carvedLakeBedM * vs);
						// THE BED DEEPENING: "Lake bed deepen" lower, growing in from the shore over "Lake bed deepen reach" px
						// as a smoothstep - flat at the waterline, so the shore keeps its own slope. The shore = the nearest
						// pixel centre no lake (a pan counts as shore) covers, half a pixel in; past the unit is not one.
						if (m_carve.lakeBedDeepen > 0.0f)
						{
							const float reachPx = oc::max(m_carve.lakeBedDeepenReach, 1.0f);
							const float fx = (float)(gx - (double)uj * (double)m_unitPx), fz = (float)(gz - (double)ui * (double)m_unitPx);
							const int32 R = (int32)std::ceil(reachPx + 0.5f);
							const auto wet = [&](uint32 k) { return u.lakes[u.lakeRuns[k].lake].kind != ERiverWater::Pan; };
							float best2 = (reachPx + 0.5f) * (reachPx + 0.5f);
							for (int32 r = oc::max(row - R, 0); r <= oc::min(row + R, m_unitPx - 1); r++)
							{
								const float dz = (float)r - fz;
								if (dz * dz >= best2)
									continue;
								const uint32 r0 = pu->rowRuns[r], r1 = pu->rowRuns[r + 1];
								uint32 at = r1;
								for (uint32 k = r0; k < r1; k++)
								{
									const RiverLakeRun& rr = u.lakeRuns[k];
									if (col >= (int32)rr.x0 && col < (int32)rr.x0 + (int32)rr.len && wet(k))
									{
										at = k;
										break;
									}
								}
								float dx;
								if (at == r1)
									dx = std::abs(fx - (float)col);
								else
								{
									// The water around col, across touching runs (two lakes side by side are one water).
									int32 w0 = u.lakeRuns[at].x0, w1 = w0 + u.lakeRuns[at].len - 1;
									for (uint32 k = at; k > r0 && wet(k - 1) && (int32)u.lakeRuns[k - 1].x0 + (int32)u.lakeRuns[k - 1].len == w0; k--)
										w0 = u.lakeRuns[k - 1].x0;
									for (uint32 k = at + 1; k < r1 && wet(k) && (int32)u.lakeRuns[k].x0 == w1 + 1; k++)
										w1 = u.lakeRuns[k].x0 + u.lakeRuns[k].len - 1;
									dx = FLT_MAX;
									if (w0 > 0)
										dx = fx - (float)(w0 - 1);
									if (w1 < m_unitPx - 1)
										dx = oc::min(dx, (float)(w1 + 1) - fx);
									if (dx == FLT_MAX)
										continue;
								}
								best2 = oc::min(best2, dx * dx + dz * dz);
							}
							const float t = oc::clamp((std::sqrt(best2) - 0.5f) / reachPx, 0.0f, 1.0f);
							p.height -= m_carve.lakeBedDeepen * t * t * (3.0f - 2.0f * t) * vs;
						}
						if (level > p.height && p.waterKind != ETerrainWater::River)
						{
							p.waterKind = ETerrainWater::Lake;
							p.inlandWater = level;
						}
					}
					break;
				}

			// The LAKE'S INFLUENCE (`river`): 1 over its wet pixels, fading to 0 within "Lake shore (px)" of them - the
			// bed's beach texture and the shoreline's, no grass or clutter under the water (they read `river` like a river's).
			// The same search finds the nearest lake for THE RIM GUARD (below).
			constexpr int32 c_rimPx = 3;
			const float shorePx = oc::max(m_carve.lakeShore, 0.1f);
			const float fx = (float)(gx - (double)uj * (double)m_unitPx), fz = (float)(gz - (double)ui * (double)m_unitPx);
			const int32 reach = oc::max((int32)std::ceil(shorePx + 0.5f), c_rimPx);
			float best2 = FLT_MAX, nearLevel = 0.0f;
			for (int32 r = oc::max(row - reach, 0); r <= oc::min(row + reach, m_unitPx - 1); r++)
				for (uint32 n = pu->rowRuns[r]; n < pu->rowRuns[r + 1]; n++)
				{
					const RiverLakeRun& run = u.lakeRuns[n];
					if (u.lakes[run.lake].kind == ERiverWater::Pan)
						continue;
					const float dx = oc::max(oc::max((float)run.x0 - fx, fx - (float)(run.x0 + run.len - 1)), 0.0f);
					const float dz = (float)r - fz;
					if (dx * dx + dz * dz < best2)
					{
						best2 = dx * dx + dz * dz;
						nearLevel = u.lakes[run.lake].level;
					}
				}
			if (best2 < FLT_MAX)
				p.river = oc::max(p.river, oc::clamp(1.0f - (std::sqrt(best2) - 0.5f) / shorePx, 0.0f, 1.0f));

			// THE RIM GUARD: the river carve knows nothing of lakes - an outlet's floodplain and valley wall cut the
			// rim away, and the lake's sheet stood in the air past the ground (2026-10-09). Beside a lake (not on its
			// wet pixels, not in a river's channel, which must cut through: the outlet) the carve keeps the ground at
			// the lake's level + c_rimClearM at the shore pixel, that floor falling at "Valley slope" farther out -
			// never above the uncarved ground.
			constexpr float c_rimClearM = 0.1f; // model m
			if (!inLake && bestRatio >= 1.0f && best2 < FLT_MAX)
			{
				const float distPx = oc::max(std::sqrt(best2) - 1.0f, 0.0f);
				const float floorM = nearLevel + c_rimClearM - oc::max(m_carve.valleySlope, 0.0f) * distPx * nr;
				p.height = oc::max(p.height, gc.seaLevel + oc::min(hModel, floorM) * vs);
			}
		}
		if (p.waterKind == ETerrainWater::None && p.height < gc.seaLevel)
			p.waterKind = ETerrainWater::Sea;
	}

	float RiverTerrain::influence(const Block& b, double worldX, double worldZ) const
	{
		const TerrainConfigV3& gc = m_base->config();
		const double inv = 1.0 / (double)gc.metersPerPixel;
		const double gx = (worldX + (double)gc.originX) * inv;
		const double gz = (worldZ + (double)gc.originZ) * inv;
		const float nr = TerrainGenV3::nativeResolution();
		float best = 0.0f;
		const int32 ui0 = floorDivI((int32)std::floor(gz - (double)m_reachPx), m_unitPx);
		const int32 ui1 = floorDivI((int32)std::floor(gz + (double)m_reachPx), m_unitPx);
		const int32 uj0 = floorDivI((int32)std::floor(gx - (double)m_reachPx), m_unitPx);
		const int32 uj1 = floorDivI((int32)std::floor(gx + (double)m_reachPx), m_unitPx);
		for (int32 ui = ui0; ui <= ui1; ui++)
			for (int32 uj = uj0; uj <= uj1; uj++)
			{
				const PreparedRiverUnit* pu = b.at(ui, uj);
				if (!pu || pu->unit->empty || pu->cells == 0)
					continue;
				const RiverUnit& u = *pu->unit;
				const float lx = (float)(gx - (double)uj * (double)m_unitPx);
				const float lz = (float)(gz - (double)ui * (double)m_unitPx);
				const int32 pad = (int32)std::ceil(m_reachPx);
				const int32 cx = (int32)std::floor((lx + (float)pad) / (float)pu->cellPx);
				const int32 cz = (int32)std::floor((lz + (float)pad) / (float)pu->cellPx);
				if (cx < 0 || cz < 0 || cx >= pu->cells || cz >= pu->cells)
					continue;
				const size_t cell = (size_t)cz * pu->cells + cx;
				for (uint32 n = pu->cellStart[cell]; n < pu->cellStart[cell + 1]; n++)
				{
					const uint32 k = pu->cellPieces[n];
					const RiverPoint& a = u.points[k];
					const RiverPoint& c = u.points[k + 1];
					const float abx = c.x - a.x, abz = c.z - a.z;
					const float len2 = abx * abx + abz * abz;
					const float t = len2 > 1e-12f ? oc::clamp(((lx - a.x) * abx + (lz - a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
					const float px = a.x + abx * t - lx, pz = a.z + abz * t - lz;
					const float d = std::sqrt(px * px + pz * pz) * nr;
					const float hw = oc::max(a.halfWidth + (c.halfWidth - a.halfWidth) * t, 0.05f);
					// The same falloff as apply: 1 in the channel, 0 at the floodplain's edge.
					const float band = hw * (m_carve.bankFactor + m_carve.floodplainFactor);
					const float inf = d < hw ? 1.0f : oc::clamp(1.0f - (d - hw) / oc::max(band, 1e-3f), 0.0f, 1.0f);
					best = oc::max(best, inf);
				}
			}
		return best;
	}

	float RiverTerrain::inlandWater(const Block& b, double worldX, double worldZ, float none) const
	{
		const TerrainConfigV3& gc = m_base->config();
		const double inv = 1.0 / (double)gc.metersPerPixel;
		const double gx = (worldX + (double)gc.originX) * inv;
		const double gz = (worldZ + (double)gc.originZ) * inv;
		const float nr = TerrainGenV3::nativeResolution();
		const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;
		float bestRatio = 1.0f; // inside a channel when < 1
		float water = none;
		const int32 ui0 = floorDivI((int32)std::floor(gz - (double)m_reachPx), m_unitPx);
		const int32 ui1 = floorDivI((int32)std::floor(gz + (double)m_reachPx), m_unitPx);
		const int32 uj0 = floorDivI((int32)std::floor(gx - (double)m_reachPx), m_unitPx);
		const int32 uj1 = floorDivI((int32)std::floor(gx + (double)m_reachPx), m_unitPx);
		for (int32 ui = ui0; ui <= ui1; ui++)
			for (int32 uj = uj0; uj <= uj1; uj++)
			{
				const PreparedRiverUnit* pu = b.at(ui, uj);
				if (!pu || pu->unit->empty || pu->cells == 0)
					continue;
				const RiverUnit& u = *pu->unit;
				const float lx = (float)(gx - (double)uj * (double)m_unitPx);
				const float lz = (float)(gz - (double)ui * (double)m_unitPx);
				const int32 pad = (int32)std::ceil(m_reachPx);
				const int32 cx = (int32)std::floor((lx + (float)pad) / (float)pu->cellPx);
				const int32 cz = (int32)std::floor((lz + (float)pad) / (float)pu->cellPx);
				if (cx < 0 || cz < 0 || cx >= pu->cells || cz >= pu->cells)
					continue;
				const size_t cell = (size_t)cz * pu->cells + cx;
				for (uint32 n = pu->cellStart[cell]; n < pu->cellStart[cell + 1]; n++)
				{
					const uint32 k = pu->cellPieces[n];
					if (u.segments[pu->pointSegment[k]].ephemeral)
						continue;
					const RiverPoint& a = u.points[k];
					const RiverPoint& c = u.points[k + 1];
					const float abx = c.x - a.x, abz = c.z - a.z;
					const float len2 = abx * abx + abz * abz;
					const float t = len2 > 1e-12f ? oc::clamp(((lx - a.x) * abx + (lz - a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
					const float px = a.x + abx * t - lx, pz = a.z + abz * t - lz;
					const float d = std::sqrt(px * px + pz * pz) * nr;
					const float hw = oc::max(a.halfWidth + (c.halfWidth - a.halfWidth) * t, 0.05f);
					if (d / hw >= bestRatio)
						continue;
					bestRatio = d / hw;
					const float unitD = oc::max(a.depth + (c.depth - a.depth) * t, 0.02f);
					const float q = a.q + (c.q - a.q) * t;
					water = gc.seaLevel + m_carve.surface(a.water + (c.water - a.water) * t, unitD, q) * vs;
				}
			}
		if (bestRatio < 1.0f)
			return water;

		// A lake's wet pixel under the point (nearest native pixel), as apply's.
		const int32 ui = floorDivI((int32)std::floor(gz), m_unitPx), uj = floorDivI((int32)std::floor(gx), m_unitPx);
		if (const PreparedRiverUnit* pu = b.at(ui, uj); pu && !pu->unit->empty && !pu->unit->lakeRuns.empty())
		{
			const RiverUnit& u = *pu->unit;
			const int32 row = (int32)std::floor(gz - (double)ui * (double)m_unitPx + 0.5);
			const int32 col = (int32)std::floor(gx - (double)uj * (double)m_unitPx + 0.5);
			if (row >= 0 && col >= 0 && row < m_unitPx && col < m_unitPx)
				for (uint32 n = pu->rowRuns[row]; n < pu->rowRuns[row + 1]; n++)
				{
					const RiverLakeRun& run = u.lakeRuns[n];
					if (col < (int32)run.x0 || col >= (int32)run.x0 + (int32)run.len)
						continue;
					const RiverLake& lake = u.lakes[run.lake];
					if (lake.kind != ERiverWater::Pan)
						return gc.seaLevel + lake.level * vs;
					break;
				}
		}
		return none;
	}

	void RiverTerrain::sampleInlandWaterGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
	                                         oc::span<float> out, float none) const
	{
		if (resX == 0 || resZ == 0)
			return;
		Block b;
		resolve(originX, originZ, originX + step * (double)(resX - 1), originZ + step * (double)(resZ - 1), b);
		for (uint32 j = 0; j < resZ; j++)
		{
			const double wz = originZ + step * (double)j;
			for (uint32 i = 0; i < resX; i++)
				out[(size_t)j * resX + i] = inlandWater(b, originX + step * (double)i, wz, none);
			Globals::jobSystem.preemptionPoint();
		}
	}

	void RiverTerrain::sampleRiverGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
	                                   oc::span<float> outInfluence) const
	{
		if (resX == 0 || resZ == 0)
			return;
		Block b;
		resolve(originX, originZ, originX + step * (double)(resX - 1), originZ + step * (double)(resZ - 1), b);
		for (uint32 j = 0; j < resZ; j++)
		{
			const double wz = originZ + step * (double)j;
			for (uint32 i = 0; i < resX; i++)
				outInfluence[(size_t)j * resX + i] = influence(b, originX + step * (double)i, wz);
			Globals::jobSystem.preemptionPoint();
		}
	}

	void RiverTerrain::samplePoint(double worldX, double worldZ, TerrainPoint& out, ESampleDetail detail) const
	{
		m_base->samplePoint(worldX, worldZ, out, detail);
		if (detail != ESampleDetail::Full)
			return;
		Block b;
		resolve(worldX, worldZ, worldX, worldZ, b);
		apply(b, worldX, worldZ, out);
	}

	void RiverTerrain::sampleGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
	                              oc::span<TerrainPoint> out, ESampleDetail detail) const
	{
		m_base->sampleGrid(originX, originZ, step, resX, resZ, out, detail);
		if (detail != ESampleDetail::Full || resX == 0 || resZ == 0)
			return;
		Block b;
		resolve(originX, originZ, originX + step * (double)(resX - 1), originZ + step * (double)(resZ - 1), b);
		for (uint32 j = 0; j < resZ; j++)
		{
			const double wz = originZ + step * (double)j;
			for (uint32 i = 0; i < resX; i++)
				apply(b, originX + step * (double)i, wz, out[(size_t)j * resX + i]);
			Globals::jobSystem.preemptionPoint(); // no lock held: the units are resolved
		}
	}

	float RiverTerrain::sampleHeight(double worldX, double worldZ) const
	{
		TerrainPoint p;
		samplePoint(worldX, worldZ, p);
		return p.height;
	}
	float RiverTerrain::sampleWaterHeight(double worldX, double worldZ) const { return m_base->sampleWaterHeight(worldX, worldZ); }
	float RiverTerrain::sampleHeightAndWater(double worldX, double worldZ, float& outWaterLevel) const
	{
		TerrainPoint p;
		samplePoint(worldX, worldZ, p);
		outWaterLevel = p.waterLevel;
		return p.height;
	}
	float RiverTerrain::sampleTemperature(double worldX, double worldZ) const { return m_base->sampleTemperature(worldX, worldZ); }
	float RiverTerrain::sampleHumidity(double worldX, double worldZ) const { return m_base->sampleHumidity(worldX, worldZ); }
	float RiverTerrain::sampleFogThickness(double worldX, double worldZ) const { return m_base->sampleFogThickness(worldX, worldZ); }
	float RiverTerrain::sampleFogHeightFalloff(double worldX, double worldZ) const { return m_base->sampleFogHeightFalloff(worldX, worldZ); }
	float RiverTerrain::sampleFlowAngle01(double worldX, double worldZ) const
	{
		TerrainPoint p;
		samplePoint(worldX, worldZ, p);
		return p.flowAngle01;
	}
	float RiverTerrain::sampleAltitude(double worldX, double worldZ) const { return m_base->sampleAltitude(worldX, worldZ); }
	float RiverTerrain::seaLevel() const { return m_base->seaLevel(); }
	float RiverTerrain::lapseRatePerMetre() const { return m_base->lapseRatePerMetre(); }
}
