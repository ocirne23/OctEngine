module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import RendererVK;
import Threading;
import Settings;
import File;

import :GeneratorV3;
import :RiverRouting;
import :RiverUnits;
import :RiverTerrain;
import :RiverSystem;

namespace
{
	using namespace Procedural;

	uint64 unitKey(int32 ui, int32 uj) { return ((uint64)(uint32)ui << 32) | (uint64)(uint32)uj; }

	// A ribbon's half-width over the channel's: the carved bank rises above the water past the channel's edge and
	// hides the rest, so the water meets the bank instead of ending in the open.
	constexpr float c_ribbonWiden = 1.2f;

	// THE NEAR CELLS' size (engine m) - river_wave.inc.glsl's RIVER_NEAR_CELL, keep in step: the light ribbons stay sunk
	// out to where a resident cell can reach.
	constexpr float c_nearCell = 64.0f;

	// One point of a segment's water, in ENGINE metres relative to its unit's origin (y above sea level).
	struct WaterPoint
	{
		glm::vec2 pos;
		float y = 0.0f;
		float half = 0.0f;   // the ribbon's half-width
		glm::vec2 dir;       // downstream, unit length
		float speed = 0.0f;  // m/s
		float foam = 0.0f;   // whitewater 0..1: a smooth measure of how steep the water runs (not the rapids / fall flags)
		float along = 0.0f;  // the distance from the segment's start
		float depth = 0.0f;  // the channel's depth below the surface at its centre (engine m)
	};

	// A perennial segment's water points: the carved surface, the ribbon's half-width, the flow - shared by the unit's
	// light ribbons and the dense near cells, so the two lie on one another.
	void segmentWater(const RiverTerrain& terrain, const RiverUnit& u, const RiverSegment& seg, oc::vector<WaterPoint>& out)
	{
		const TerrainConfigV3& gc = terrain.base().config();
		const float mpp = gc.metersPerPixel;
		const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;
		const float nr = TerrainGenV3::nativeResolution();
		const RiverCarveConfig& carve = terrain.carveConfig();
		out.clear();
		float along = 0.0f;
		for (uint32 k = 0; k < seg.count; k++)
		{
			const RiverPoint& p = u.points[seg.first + k];
			const RiverPoint& a = u.points[seg.first + (k > 0 ? k - 1 : k)];
			const RiverPoint& b = u.points[seg.first + (k + 1 < seg.count ? k + 1 : k)];
			WaterPoint w;
			w.dir = glm::vec2(b.x - a.x, b.z - a.z);
			const float len = glm::length(w.dir);
			w.dir = len > 1e-6f ? w.dir / len : glm::vec2(1.0f, 0.0f);
			w.pos = glm::vec2(p.x * mpp, p.z * mpp);
			if (k > 0)
				along += glm::length(w.pos - out.back().pos);
			w.along = along;
			const float S = carve.surface(p.water, p.depth, p.q); // model m
			w.y = S * vs;
			w.half = p.halfWidth / nr * mpp * c_ribbonWiden;
			w.depth = carve.channelDepth(p.depth) * vs;

			// The flow speed, Manning-like: v = depth^(2/3) sqrt(slope) / n (model frame, n = 0.035), from the carved
			// surface's drop to the next point.
			const float dist = glm::length(glm::vec2(b.x - p.x, b.z - p.z)) * nr;
			const float drop = dist > 1e-3f ? (S - carve.surface(b.water, b.depth, b.q)) / dist : 0.0f;
			w.speed = glm::clamp(std::pow(glm::max(p.depth, 0.05f), 0.6667f) * std::sqrt(glm::max(drop, 1e-5f)) / 0.035f, 0.1f, 6.0f);
			// The WHITEWATER is a smooth measure of how steep the water runs, not a kind of reach: 0 below half the
			// rapids slope, 1 at the fall slope, eased between (the shader turns it into rougher, milkier, higher water,
			// with foam streaks only as it rises).
			const RiverUnitConfig& uc = terrain.unitConfig();
			w.foam = glm::smoothstep(0.5f * uc.rapidsSlope, glm::max(uc.fallSlope, uc.rapidsSlope + 1e-3f), drop);
			out.push_back(w);
		}
		// ... and blurred along the river, so it rises and falls over tens of metres instead of per point.
		for (int32 pass = 0; pass < 4 && out.size() > 2; pass++)
		{
			float prev = out[0].foam;
			for (size_t k = 1; k + 1 < out.size(); k++)
			{
				const float cur = out[k].foam;
				out[k].foam = 0.25f * prev + 0.5f * cur + 0.25f * out[k + 1].foam;
				prev = cur;
			}
		}

		// THE JUNCTION: the river it flows into stands LOWER at the junction (its bigger Q: a deeper channel and valley
		// sink under the same unit level), and this ribbon ran on into it, to its centre line - a second sheet a little
		// above the main water (2026-10-09). So the last stretch (3 x the main half-width, at least 20 m) EASES DOWN to the
		// main surface at the junction, and a clearly smaller river (a tributary: under 0.6 x the main half-width) STOPS
		// 0.85 x the main half-width before it - just inside the main water - instead of lying over it. The main river's
		// own upstream branch is not cut (its ribbon would leave a gap), only eased.
		if (seg.end != ERiverEnd::Junction || out.size() < 2)
			return;
		const RiverPoint& pe = u.points[seg.first + seg.count - 1];
		const RiverPoint* down = nullptr;
		const RiverSegment* downSeg = nullptr;
		bool largestArriving = true; // no other segment ending here is wider (ties: the first in the unit's order)
		for (const RiverSegment& t : u.segments)
		{
			if (&t == &seg || t.count == 0 || t.ephemeral)
				continue;
			const RiverPoint& p0 = u.points[t.first];
			if (std::abs(p0.x - pe.x) < 0.75f && std::abs(p0.z - pe.z) < 0.75f && (!down || p0.halfWidth > down->halfWidth))
			{
				down = &p0;
				downSeg = &t;
			}
			const RiverPoint& tl = u.points[t.first + t.count - 1];
			if (t.end == ERiverEnd::Junction && std::abs(tl.x - pe.x) < 0.75f && std::abs(tl.z - pe.z) < 0.75f
				&& (tl.halfWidth > pe.halfWidth || (tl.halfWidth == pe.halfWidth && &t < &seg)))
				largestArriving = false;
		}
		if (!down)
			return;
		const float downY = carve.surface(down->water, down->depth, down->q) * vs;
		const float downHalf = down->halfWidth / nr * mpp; // engine m
		// ITS FLOW at the junction, as the loop above computes it: the first piece's direction, the Manning speed and the
		// whitewater from that piece's drop.
		glm::vec2 downDir = out.back().dir;
		float downSpeed = out.back().speed, downFoam = out.back().foam;
		if (downSeg->count >= 2)
		{
			const RiverPoint& d1 = u.points[downSeg->first + 1];
			const glm::vec2 dd(d1.x - down->x, d1.z - down->z);
			const float dl = glm::length(dd);
			if (dl > 1e-6f)
			{
				downDir = dd / dl;
				const float S0 = carve.surface(down->water, down->depth, down->q), S1 = carve.surface(d1.water, d1.depth, d1.q);
				const float drop = (S0 - S1) / (dl * nr);
				downSpeed = glm::clamp(std::pow(glm::max(down->depth, 0.05f), 0.6667f) * std::sqrt(glm::max(drop, 1e-5f)) / 0.035f, 0.1f, 6.0f);
				const RiverUnitConfig& uc = terrain.unitConfig();
				downFoam = glm::smoothstep(0.5f * uc.rapidsSlope, glm::max(uc.fallSlope, uc.rapidsSlope + 1e-3f), drop);
			}
		}
		const float total = out.back().along;
		// The largest arriving branch is the main river's continuation: never cut, or two similar streams joining (each under
		// 0.6 x the wider river they make) both stopped short and left the junction's upstream side without water.
		const bool tributary = !largestArriving && pe.halfWidth < 0.6f * down->halfWidth;
		const float cutAlong = tributary ? glm::max(total - 0.85f * downHalf, 0.0f) : total;
		if (tributary && cutAlong > 0.0f)
		{
			// The last point at exactly cutAlong (interpolated), the rest dropped.
			size_t k = 1;
			while (k < out.size() && out[k].along < cutAlong)
				k++;
			if (k < out.size())
			{
				const WaterPoint& a = out[k - 1];
				const WaterPoint& b = out[k];
				const float t = b.along > a.along ? (cutAlong - a.along) / (b.along - a.along) : 0.0f;
				WaterPoint w = b;
				w.pos = glm::mix(a.pos, b.pos, t);
				w.y = glm::mix(a.y, b.y, t);
				w.half = glm::mix(a.half, b.half, t);
				w.depth = glm::mix(a.depth, b.depth, t);
				w.speed = glm::mix(a.speed, b.speed, t);
				w.foam = glm::mix(a.foam, b.foam, t);
				w.along = cutAlong;
				out.resize(k);
				out.push_back(w);
			}
		}
		// Over the same stretch the FLOW (direction and speed) and the WHITEWATER blend into the main river's too: a slow
		// stream joining a fast river showed a sheet of slow (or calm) water where its ribbon met the main one; now both
		// move alike there. The direction matters as much: a tributary at an angle dragged its ripples across the current.
		const float easeLen = glm::max(3.0f * downHalf, 20.0f);
		for (WaterPoint& w : out)
		{
			const float s = glm::clamp(1.0f - (cutAlong - w.along) / easeLen, 0.0f, 1.0f);
			const float ease = s * s * (3.0f - 2.0f * s);
			w.y = glm::min(w.y, glm::mix(w.y, downY, ease));
			w.speed = glm::mix(w.speed, downSpeed, ease);
			w.foam = glm::mix(w.foam, downFoam, ease);
			const glm::vec2 dir = glm::mix(w.dir, downDir, ease);
			const float dl = glm::length(dir);
			if (dl > 1e-3f)
				w.dir = dir / dl;
			else
				w.dir = ease > 0.5f ? downDir : w.dir; // head-on: no blend, the nearer one
		}
	}

	struct MeshArrays
	{
		oc::vector<glm::vec3> positions, normals, tangents, texCoords;
		oc::vector<uint32> indices;
		// The normal carries data, not a direction (river.vs.glsl): x = the near cells' dense mark, y = the channel's centre
		// depth (the waves' size; a lake 1e4), z = the water column under the vertex (the troughs stay above the bed).
		void push(glm::vec3 p, glm::vec3 tangent, glm::vec2 uv, bool dense, float size, float column = 0.0f)
		{
			positions.push_back(p);
			normals.push_back(glm::vec3(dense ? 1.0f : 0.0f, size, column));
			tangents.push_back(tangent);
			texCoords.push_back(glm::vec3(uv, 0.0f));
		}
		bool build(RenderMeshData& out, const char* name) const
		{
			if (indices.empty())
				return false;
			MeshGeometryDesc geom;
			geom.positions = positions.data();
			geom.normals = normals.data();
			geom.tangents = tangents.data();
			geom.texCoords = texCoords.data();
			geom.numVertices = (uint32)positions.size();
			geom.indices = indices.data();
			geom.numIndices = (uint32)indices.size();
			geom.name = name;
			out.build(geom);
			return true;
		}
	};

	// THE NEAR CELL's dense mesh, pure (the near job builds it): every perennial river of the given units crossing the
	// cell, resampled every `spacing` m along its length, `across` vertices wide. A row pair belongs to the cell its
	// first row lies in, so neighbouring cells meet without overlap. Cell-local engine m, y above sea level.
	bool buildNearCellMesh(const RiverTerrain& terrain, const oc::vector<oc::pair<oc::shared_ptr<const PreparedRiverUnit>, glm::vec2>>& units,
	                       float spacing, int32 across, RenderMeshData& out)
	{
		MeshArrays m;
		oc::vector<WaterPoint> pts;
		const float wallSlope = glm::max(terrain.carveConfig().wallSlope, 0.01f);
		spacing = glm::max(spacing, 0.05f);
		across = glm::max(across, 2);
		for (const auto& ref : units)
		{
			const RiverUnit& u = *ref.first->unit;
			const glm::vec2 offset = ref.second; // unit origin - cell origin
			for (const RiverSegment& seg : u.segments)
			{
				if (seg.ephemeral || seg.count < 2)
					continue;
				segmentWater(terrain, u, seg, pts);
				// Cheap reject: the segment's box, grown by its widest half-width, against the cell.
				glm::vec2 mn(FLT_MAX), mx(-FLT_MAX);
				float maxHalf = 0.0f;
				for (const WaterPoint& w : pts)
				{
					mn = glm::min(mn, w.pos + offset);
					mx = glm::max(mx, w.pos + offset);
					maxHalf = glm::max(maxHalf, w.half);
				}
				if (mx.x + maxHalf < 0.0f || mx.y + maxHalf < 0.0f || mn.x - maxHalf > c_nearCell || mn.y - maxHalf > c_nearCell)
					continue;

				// Resample at `spacing` along the polyline; a row is emitted when it or the row before it is in the cell.
				const float total = pts.back().along;
				const int32 rows = (int32)std::floor(total / spacing) + 2;
				size_t piece = 0;
				int32 lastRow = -2;
				uint32 lastBase = 0;
				bool prevInCell = false;
				WaterPoint prev{};
				for (int32 r = 0; r < rows; r++)
				{
					const float s = glm::min((float)r * spacing, total);
					while (piece + 2 < pts.size() && pts[piece + 1].along < s)
						piece++;
					const WaterPoint& a = pts[piece];
					const WaterPoint& b = pts[piece + 1];
					const float t = b.along > a.along ? glm::clamp((s - a.along) / (b.along - a.along), 0.0f, 1.0f) : 0.0f;
					WaterPoint w;
					w.pos = glm::mix(a.pos, b.pos, t) + offset;
					w.y = glm::mix(a.y, b.y, t);
					w.half = glm::mix(a.half, b.half, t);
					w.dir = glm::mix(a.dir, b.dir, t);
					const float dl = glm::length(w.dir);
					w.dir = dl > 1e-6f ? w.dir / dl : a.dir;
					w.speed = glm::mix(a.speed, b.speed, t);
					w.foam = glm::mix(a.foam, b.foam, t);
					w.depth = glm::mix(a.depth, b.depth, t);
					w.along = s;
					const bool inCell = w.pos.x >= 0.0f && w.pos.y >= 0.0f && w.pos.x < c_nearCell && w.pos.y < c_nearCell;

					const auto emitRow = [&](const WaterPoint& row) -> uint32
					{
						const uint32 base = (uint32)m.positions.size();
						const glm::vec2 n(-row.dir.y, row.dir.x);
						const glm::vec3 tangent(row.dir.x * row.speed, row.foam, row.dir.y * row.speed);
						for (int32 j = 0; j < across; j++)
						{
							const float u01 = -1.0f + 2.0f * (float)j / (float)(across - 1);
							const glm::vec2 p = row.pos + n * (row.half * u01);
							// The carve's channel cross-section (RiverCarveConfig::channelCut, engine m): the sides rise to
							// the surface at the channel's edge, 1 / c_ribbonWiden of the ribbon's half-width.
							const float channelHalf = row.half / c_ribbonWiden;
							const float edgeIn = channelHalf - row.half * std::abs(u01);
							const float column = RiverCarveConfig::channelCut(edgeIn, row.depth, channelHalf, wallSlope);
							m.push(glm::vec3(p.x, row.y, p.y), tangent, glm::vec2(u01, row.along), true, row.depth, column);
						}
						return base;
					};
					if (prevInCell && r > 0)
					{
						const uint32 b0 = lastRow == r - 1 ? lastBase : emitRow(prev);
						const uint32 b1 = emitRow(w);
						for (int32 j = 0; j + 1 < across; j++)
							m.indices.insert(m.indices.end(), { b0 + j, b1 + j, b0 + j + 1, b0 + j + 1, b1 + j, b1 + j + 1 });						lastRow = r;
						lastBase = b1;
					}
					prevInCell = inCell;
					prev = w;
					if (s >= total)
						break;
				}
			}

			// THE LAKES: a grid over the cell every `spacing` m, a vertex IN where a wet lake pixel lies within one pixel
			// of it (the light quads' one-pixel margin: it reaches the shore, the ground cuts it), at that lake's level; a
			// quad where all four corners are in. The VS gives it the lakes' waves.
			const PreparedRiverUnit& pu = *ref.first;
			if (u.lakeRuns.empty() || pu.rowRuns.size() < 2)
				continue;
			const TerrainConfigV3& gc = terrain.base().config();
			const float mpp = gc.metersPerPixel;
			const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;
			const int32 unitPx = (int32)pu.rowRuns.size() - 1;
			// The cell in unit pixels; skip a unit it does not touch.
			const float px0 = -offset.x / mpp, pz0 = -offset.y / mpp, pxs = c_nearCell / mpp;
			if (px0 + pxs < -2.0f || pz0 + pxs < -2.0f || px0 > (float)unitPx + 2.0f || pz0 > (float)unitPx + 2.0f)
				continue;
			const auto lakeLevelAt = [&](float px, float pz) -> float
			{
				const int32 c0 = (int32)std::floor(px + 0.5f), r0 = (int32)std::floor(pz + 0.5f);
				for (int32 r = r0 - 1; r <= r0 + 1; r++)
				{
					if (r < 0 || r >= unitPx)
						continue;
					for (uint32 k = pu.rowRuns[r]; k < pu.rowRuns[r + 1]; k++)
					{
						const RiverLakeRun& run = u.lakeRuns[k];
						const RiverLake& lake = u.lakes[run.lake];
						if (lake.kind != ERiverWater::Pan && c0 + 1 >= (int32)run.x0 && c0 - 1 < (int32)run.x0 + (int32)run.len)
							return lake.level * vs;
					}
				}
				return FLT_MAX;
			};
			const int32 n = glm::max((int32)std::ceil(c_nearCell / spacing), 1);
			const float step = c_nearCell / (float)n;
			const int32 vpr = n + 1;
			oc::vector<float> level((size_t)vpr * vpr);
			bool any = false;
			for (int32 j = 0; j < vpr; j++)
				for (int32 i = 0; i < vpr; i++)
				{
					const float l = lakeLevelAt(px0 + (float)i * step / mpp, pz0 + (float)j * step / mpp);
					level[(size_t)j * vpr + i] = l;
					any |= l != FLT_MAX;
				}
			if (!any)
				continue;
			oc::vector<uint32> vertexOf((size_t)vpr * vpr, UINT32_MAX);
			const auto vertex = [&](int32 i, int32 j) -> uint32
			{
				uint32& v = vertexOf[(size_t)j * vpr + i];
				if (v == UINT32_MAX)
				{
					v = (uint32)m.positions.size();
					m.push(glm::vec3((float)i * step, level[(size_t)j * vpr + i], (float)j * step), glm::vec3(0.0f), glm::vec2(2.0f, 0.0f), true, 1.0e4f, 1.0e4f);
				}
				return v;
			};
			for (int32 j = 0; j < n; j++)
				for (int32 i = 0; i < n; i++)
				{
					if (level[(size_t)j * vpr + i] == FLT_MAX || level[(size_t)j * vpr + i + 1] == FLT_MAX
						|| level[(size_t)(j + 1) * vpr + i] == FLT_MAX || level[(size_t)(j + 1) * vpr + i + 1] == FLT_MAX)
						continue;
					const uint32 a = vertex(i, j), b = vertex(i + 1, j), c = vertex(i, j + 1), d = vertex(i + 1, j + 1);
					m.indices.insert(m.indices.end(), { a, c, b, b, c, d });
				}
		}
		return m.build(out, "RiverNear");
	}

	// THE WATER MESH of a unit, pure (the pull job builds it). Unit-local ENGINE metres, y relative to sea level (the
	// node sits at the unit's origin at sea level). Vertex layout: see river.vs.glsl. Also its WHITEWATER points for the
	// mist: every water point with any whitewater (past c_mistMinFoam - the tweak's threshold cuts later), standing for
	// half the river to each neighbour.
	bool buildSurfaceMesh(const RiverTerrain& terrain, const RiverUnit& u, RenderMeshData& out, oc::vector<RiverSystem::MistPoint>& mist)
	{
		constexpr float c_mistMinFoam = 0.02f;
		mist.clear();
		const TerrainConfigV3& gc = terrain.base().config();
		const float mpp = gc.metersPerPixel;
		const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;

		MeshArrays m;
		oc::vector<WaterPoint> pts;
		// A RIBBON per perennial segment, at the carved water surface: two vertices per point across the channel.
		for (const RiverSegment& seg : u.segments)
		{
			if (seg.ephemeral || seg.count < 2)
				continue;
			segmentWater(terrain, u, seg, pts);
			const uint32 base = (uint32)m.positions.size();
			for (size_t k = 0; k < pts.size(); k++)
			{
				const WaterPoint& w = pts[k];
				if (w.foam > c_mistMinFoam)
				{
					const float prev = k > 0 ? glm::length(w.pos - pts[k - 1].pos) : 0.0f;
					const float next = k + 1 < pts.size() ? glm::length(pts[k + 1].pos - w.pos) : 0.0f;
					mist.push_back(RiverSystem::MistPoint{ glm::vec3(w.pos.x, w.y, w.pos.y), w.half / c_ribbonWiden,
						w.dir * w.speed, w.foam, w.depth, 0.5f * (prev + next) });
				}
				const glm::vec2 n(-w.dir.y, w.dir.x);
				const glm::vec3 tangent(w.dir.x * w.speed, w.foam, w.dir.y * w.speed);
				m.push(glm::vec3(w.pos.x + n.x * w.half, w.y, w.pos.y + n.y * w.half), tangent, glm::vec2(-1.0f, w.along), false, w.depth);
				m.push(glm::vec3(w.pos.x - n.x * w.half, w.y, w.pos.y - n.y * w.half), tangent, glm::vec2(1.0f, w.along), false, w.depth);
				if (k > 0)
				{
					const uint32 i = base + 2 * (uint32)(k - 1);
					m.indices.insert(m.indices.end(), { i, i + 2, i + 1, i + 1, i + 2, i + 3 });
				}
			}
		}

		// A QUAD per lake row run at the lake's level, one pixel wider all round (they overlap where opaque; the ground
		// cuts the shoreline where it rises above the level). No pans: a salt flat has no water.
		for (const RiverLakeRun& run : u.lakeRuns)
		{
			const RiverLake& lake = u.lakes[run.lake];
			if (lake.kind == ERiverWater::Pan)
				continue;
			const float y = lake.level * vs;
			const float x0 = ((float)run.x0 - 1.0f) * mpp, x1 = ((float)(run.x0 + run.len)) * mpp;
			const float z0 = ((float)run.row - 1.0f) * mpp, z1 = ((float)run.row + 1.0f) * mpp;
			const uint32 i = (uint32)m.positions.size();
			const glm::vec3 still(0.0f);
			m.push(glm::vec3(x0, y, z0), still, glm::vec2(2.0f, 0.0f), false, 1.0e4f);
			m.push(glm::vec3(x1, y, z0), still, glm::vec2(2.0f, 0.0f), false, 1.0e4f);
			m.push(glm::vec3(x0, y, z1), still, glm::vec2(2.0f, 0.0f), false, 1.0e4f);
			m.push(glm::vec3(x1, y, z1), still, glm::vec2(2.0f, 0.0f), false, 1.0e4f);
			m.indices.insert(m.indices.end(), { i, i + 2, i + 1, i + 1, i + 2, i + 3 });
		}
		return m.build(out, "RiverWater");
	}

	uint64 cellKey(int32 cx, int32 cz) { return ((uint64)(uint32)cz << 32) | (uint64)(uint32)cx; }

	// The renderer's packed 0xAABBGGRR.
	uint32 packColor(glm::vec3 c)
	{
		c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
		const uint32 r = (uint32)(c.x * 255.0f + 0.5f), g = (uint32)(c.y * 255.0f + 0.5f), b = (uint32)(c.z * 255.0f + 0.5f);
		return 0xFF000000u | (b << 16) | (g << 8) | r;
	}
}

namespace Procedural
{
	RiverSystem::~RiverSystem()
	{
		Globals::jobSystem.wait(m_counter);
		Globals::jobSystem.wait(m_nearCounter);
		Globals::jobSystem.wait(m_waterCounter);
	}

	void RiverSystem::updateWaterMap(Renderer& renderer, glm::vec2 camera)
	{
		using namespace RendererVKLayout;
		constexpr float c_waterMapMove = 40.0f; // m: the centre snaps to this lattice; a step re-bakes
		if (m_waterJob && m_waterCounter.isDone())
		{
			oc::shared_ptr<WaterMapJob> job = oc::move(m_waterJob);
			m_waterJob = nullptr;
			if (job->terrain == m_terrain)
			{
				renderer.setRiverWaterMap(job->origin, job->heights);
				m_waterMapSet = true;
				m_waterCentre = job->centre;
				m_waterGeneration = job->generation;
			}
		}
		const glm::vec2 centre = glm::floor(camera / c_waterMapMove) * c_waterMapMove;
		if (m_waterJob || (centre == m_waterCentre && m_waterGeneration == m_unitGeneration))
			return;
		auto job = oc::make_shared<WaterMapJob>();
		job->terrain = m_terrain;
		job->centre = centre;
		job->generation = m_unitGeneration;
		const float size = (float)RIVER_WATER_MAP_DIM * RIVER_WATER_MAP_TEXEL;
		job->origin = centre - glm::vec2(0.5f * size);
		m_waterJob = job;
		Globals::jobSystem.submit([job]()
		{
			job->heights.resize((size_t)RIVER_WATER_MAP_DIM * RIVER_WATER_MAP_DIM);
			// Texel centres: (origin + (i + 0.5) x texel).
			job->terrain->sampleInlandWaterGrid((double)job->origin.x + 0.5 * RIVER_WATER_MAP_TEXEL, (double)job->origin.y + 0.5 * RIVER_WATER_MAP_TEXEL,
				(double)RIVER_WATER_MAP_TEXEL, RIVER_WATER_MAP_DIM, RIVER_WATER_MAP_DIM, job->heights, RIVER_WATER_NONE);
		}, { "RiverSystem::bakeWaterMap", EProfileCategory::Procedural }, EJobPriority::Low, &m_waterCounter);
	}

	void RiverSystem::setTerrain(oc::shared_ptr<const RiverTerrain> terrain)
	{
		if (terrain == m_terrain)
			return;
		m_terrain = oc::move(terrain);
		m_units.clear(); // a job in flight finishes against its own terrain; its result is dropped (see update)
		dropNearCells();
		m_waterCentre = glm::vec2(1.0e30f); // re-bake the inland water map against the new terrain
	}

	void RiverSystem::dropNearCells()
	{
		m_nearCells.clear(); // a near job in flight lands against an older generation and is dropped
		++m_unitGeneration;
	}

	void RiverSystem::updateNearCells(Renderer& renderer, glm::vec2 camera)
	{
		const TerrainSettings& s = Globals::settings.terrain;
		const float R = glm::max(s.riverNearRadius, 0.0f);
		if (s.riverNearSpacing != m_nearSpacingWas || s.riverNearAcross != m_nearAcrossWas)
		{
			m_nearSpacingWas = s.riverNearSpacing;
			m_nearAcrossWas = s.riverNearAcross;
			dropNearCells();
		}

		// The finished cell: kept if its unit generation is still current (a stale one still replaces nothing older).
		if (m_nearJob && m_nearCounter.isDone())
		{
			oc::shared_ptr<NearJob> job = oc::move(m_nearJob);
			m_nearJob = nullptr;
			if (job->terrain == m_terrain && job->generation == m_unitGeneration)
			{
				NearCell& cell = m_nearCells[cellKey(job->cx, job->cz)];
				cell.node = RenderNode();
				cell.mesh = RenderMesh();
				cell.generation = job->generation;
				if (job->hasMesh)
				{
					cell.mesh = renderer.createMesh(job->mesh, false);
					if (cell.mesh.isValid())
					{
						const glm::vec3 origin((float)job->cx * c_nearCell, m_terrain->base().config().seaLevel, (float)job->cz * c_nearCell);
						cell.node = renderer.spawnMeshNode(cell.mesh, m_material, RendererVKLayout::EPipelineIndex::River,
							Transform(origin, 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
						cell.node.setPassMask(RendererVKLayout::PASS_MAIN);
					}
				}
			}
		}
		if (R <= 0.0f)
		{
			if (!m_nearCells.empty())
				m_nearCells.clear();
			renderer.setRiverNearCovered(0.0f);
			return;
		}

		// Cells whose centre lies within R + one cell (river_wave.inc.glsl sinks the light ribbons that far).
		const float keep = R + c_nearCell;
		const auto centreDist = [&](int32 cx, int32 cz)
		{
			return glm::length(glm::vec2(((float)cx + 0.5f) * c_nearCell, ((float)cz + 0.5f) * c_nearCell) - camera);
		};
		for (auto it = m_nearCells.begin(); it != m_nearCells.end();)
		{
			const int32 cz = (int32)(it->first >> 32), cx = (int32)(uint32)(it->first & 0xFFFFFFFFu);
			if (centreDist(cx, cz) > keep)
				it = m_nearCells.erase(it);
			else
				++it;
		}

		// Build the nearest missing (then stale) cell.
		if (!m_nearJob && m_material != UINT16_MAX)
		{
			const int32 c0x = (int32)std::floor((camera.x - keep) / c_nearCell), c1x = (int32)std::floor((camera.x + keep) / c_nearCell);
			const int32 c0z = (int32)std::floor((camera.y - keep) / c_nearCell), c1z = (int32)std::floor((camera.y + keep) / c_nearCell);
			float best = FLT_MAX;
			int32 bx = 0, bz = 0;
			bool found = false;
			for (int32 cz = c0z; cz <= c1z; cz++)
				for (int32 cx = c0x; cx <= c1x; cx++)
				{
					const float d = centreDist(cx, cz);
					if (d > keep)
						continue;
					auto it = m_nearCells.find(cellKey(cx, cz));
					const bool missing = it == m_nearCells.end();
					if (!missing && it->second.generation == m_unitGeneration)
						continue;
					const float score = d + (missing ? 0.0f : 1e6f); // missing cells first
					if (score < best)
					{
						best = score;
						bx = cx;
						bz = cz;
						found = true;
					}
				}
			if (found)
			{
				auto job = oc::make_shared<NearJob>();
				job->terrain = m_terrain;
				job->cx = bx;
				job->cz = bz;
				job->generation = m_unitGeneration;
				job->spacing = s.riverNearSpacing;
				job->across = s.riverNearAcross;
				const glm::vec2 cellOrigin((float)bx * c_nearCell, (float)bz * c_nearCell);
				const float unitM = (float)m_terrain->unitPixels() * m_terrain->base().config().metersPerPixel;
				for (const auto& [key, r] : m_units)
				{
					if (!r.unit || r.unit->unit->empty)
						continue;
					const int32 ui = (int32)(key >> 32), uj = (int32)(uint32)(key & 0xFFFFFFFFu);
					const glm::vec3 o = unitOrigin(ui, uj);
					const glm::vec2 offset = glm::vec2(o.x, o.z) - cellOrigin;
					// The unit's square, grown by a generous river half-width, against the cell.
					constexpr float c_margin = 500.0f;
					if (offset.x > c_nearCell + c_margin || offset.y > c_nearCell + c_margin
						|| offset.x + unitM < -c_margin || offset.y + unitM < -c_margin)
						continue;
					job->units.push_back({ r.unit, offset });
				}
				m_nearJob = job;
				Globals::jobSystem.submit([job]()
				{
					job->hasMesh = buildNearCellMesh(*job->terrain, job->units, job->spacing, job->across, job->mesh);
				}, { "RiverSystem::buildNearCell", EProfileCategory::Procedural }, EJobPriority::Low, &m_nearCounter);
			}
		}

		for (auto& [key, cell] : m_nearCells)
			if (cell.node.isValid())
				renderer.renderNode(cell.node);

		// THE NEAR COVERAGE: how far around the camera every cell is built (a stale one still has its mesh) - the
		// nearest missing cell's centre less half its diagonal, at most the radius. The light ribbons discard their
		// fragments inside it (river.fs.glsl), so the dense waves are the only water surface there; outside it, or while
		// cells are still building, the sunk light ribbon shows instead (no hole).
		float covered = R;
		{
			const int32 c0x = (int32)std::floor((camera.x - keep) / c_nearCell), c1x = (int32)std::floor((camera.x + keep) / c_nearCell);
			const int32 c0z = (int32)std::floor((camera.y - keep) / c_nearCell), c1z = (int32)std::floor((camera.y + keep) / c_nearCell);
			for (int32 cz = c0z; cz <= c1z; cz++)
				for (int32 cx = c0x; cx <= c1x; cx++)
					if (!m_nearCells.count(cellKey(cx, cz)))
						covered = glm::min(covered, centreDist(cx, cz) - 0.7072f * c_nearCell);
		}
		renderer.setRiverNearCovered(glm::max(covered, 0.0f));
	}

	void RiverSystem::updateMist(Renderer& renderer, glm::vec2 camera, bool clear)
	{
		using namespace RendererVKLayout;
		if (clear)
		{
			if (m_mistSet)
			{
				renderer.setRiverMistSources({});
				m_mistSet = false;
			}
			m_mistCentre = glm::vec2(1.0e30f);
			return;
		}
		const TerrainSettings& s = Globals::settings.terrain;
		constexpr float c_mistMove = 10.0f; // engine m
		const float R = glm::max(s.riverMistRadius, 1.0f);
		if (glm::length(camera - m_mistCentre) < c_mistMove && m_mistGeneration == m_unitGeneration
			&& R == m_mistRadiusWas && s.riverFullSizeDepth == m_mistFullSizeWas && s.riverMistSizeWeight == m_mistSizeWeightWas)
			return;
		m_mistCentre = camera;
		m_mistGeneration = m_unitGeneration;
		m_mistRadiusWas = R;
		m_mistFullSizeWas = s.riverFullSizeDepth;
		m_mistSizeWeightWas = s.riverMistSizeWeight;

		// The whitewater x the river's size ("Full size depth", as the waves) by "Mist size weight": a stream's riffle
		// barely mists.
		const float fullSize = glm::max(s.riverFullSizeDepth, 1e-3f);
		const float sizeWeight = glm::clamp(s.riverMistSizeWeight, 0.0f, 1.0f);
		oc::vector<oc::pair<float, RiverMistSourceGpu>> found;
		for (const auto& [key, r] : m_units)
		{
			if (r.mist.empty())
				continue;
			const int32 ui = (int32)(key >> 32), uj = (int32)(uint32)(key & 0xFFFFFFFFu);
			if (unitDistance(ui, uj, camera) > R)
				continue;
			const glm::vec3 origin = unitOrigin(ui, uj);
			for (const MistPoint& p : r.mist)
			{
				const glm::vec3 world = origin + p.pos;
				const glm::vec2 d(world.x - camera.x, world.z - camera.y);
				const float d2 = glm::dot(d, d);
				if (d2 > R * R)
					continue;
				RiverMistSourceGpu g;
				g.posHalf = glm::vec4(world, p.half);
				const float size = glm::mix(1.0f, glm::clamp(p.depth / fullSize, 0.0f, 1.0f), sizeWeight);
				g.flowFoam = glm::vec4(p.flow, p.foam * size, p.len);
				found.push_back({ d2, g });
			}
		}
		if (found.size() > MAX_RIVER_MIST_SOURCES)
		{
			oc::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			found.resize(MAX_RIVER_MIST_SOURCES);
		}
		oc::vector<RiverMistSourceGpu> sources;
		sources.reserve(found.size());
		for (const auto& [d2, g] : found)
			sources.push_back(g);
		renderer.setRiverMistSources(sources);
		m_mistSet = true;
	}

	float RiverSystem::unitDistance(int32 ui, int32 uj, glm::vec2 camera) const
	{
		const TerrainConfigV3& gc = m_terrain->base().config();
		const float size = (float)m_terrain->unitPixels() * gc.metersPerPixel;
		const glm::vec2 lo((float)uj * size - gc.originX, (float)ui * size - gc.originZ);
		const glm::vec2 d = glm::max(glm::max(lo - camera, camera - (lo + glm::vec2(size))), glm::vec2(0.0f));
		return glm::length(d);
	}

	glm::vec3 RiverSystem::unitOrigin(int32 ui, int32 uj) const
	{
		const TerrainConfigV3& gc = m_terrain->base().config();
		const double size = (double)m_terrain->unitPixels() * (double)gc.metersPerPixel;
		return glm::vec3((float)((double)uj * size - (double)gc.originX), gc.seaLevel,
		                 (float)((double)ui * size - (double)gc.originZ));
	}

	void RiverSystem::update(Renderer& renderer, const Camera& camera)
	{
		const TerrainSettings& s = Globals::settings.terrain;
		const bool surface = s.riverSurface && renderer.isInitialized();
		if (surface != m_surfaceWas)
		{
			m_surfaceWas = surface;
			m_units.clear(); // re-pulled with (or without) their water meshes; the store keeps the units
			dropNearCells();
		}

		if (m_job && m_counter.isDone())
		{
			oc::shared_ptr<Job> job = oc::move(m_job);
			m_job = nullptr;
			if (job->terrain == m_terrain && job->result && job->buildSurface == surface)
			{
				Resident& r = m_units[unitKey(job->ui, job->uj)];
				r.unit = job->result;
				r.linesBuilt = false;
				r.mist = oc::move(job->mist);
				++m_unitGeneration; // the near cells may cross its rivers: rebuild them
				if (job->hasMesh)
				{
					if (m_material == UINT16_MAX)
						m_material = renderer.createMeshMaterial(RendererVKLayout::EPipelineIndex::River, false); // not in the TLAS
					r.mesh = renderer.createMesh(job->mesh, false);
					if (r.mesh.isValid())
					{
						r.node = renderer.spawnMeshNode(r.mesh, m_material, RendererVKLayout::EPipelineIndex::River,
							Transform(unitOrigin(job->ui, job->uj), 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
						r.node.setPassMask(RendererVKLayout::PASS_MAIN); // no shadow, no GI: a thin surface
					}
				}
			}
		}

		const bool lines = s.riverDebugLines && renderer.isInitialized();
		if (!m_terrain && m_waterMapSet) // the rivers went (disabled, or no terrain): no inland water any more
		{
			renderer.setRiverWaterMap(glm::vec2(0.0f), {});
			m_waterMapSet = false;
		}
		if (!m_terrain || !surface)
			updateMist(renderer, glm::vec2(0.0f), true);
		if (!m_terrain || (!lines && !surface))
			return;

		const TerrainConfigV3& gc = m_terrain->base().config();
		const glm::vec2 cam(camera.position.x, camera.position.z);
		const float lineRadius = glm::max(s.riverDebugRadius, 1.0f);
		const float surfaceRadius = glm::max(s.riverSurfaceRadius, 1.0f);
		const float radius = glm::max(lines ? lineRadius : 0.0f, surface ? surfaceRadius : 0.0f);
		const float unitM = (float)m_terrain->unitPixels() * gc.metersPerPixel;
		const int32 cu = (int32)std::floor((cam.x + gc.originX) / unitM);
		const int32 cv = (int32)std::floor((cam.y + gc.originZ) / unitM);
		const int32 reach = (int32)std::ceil(radius / unitM) + 1;

		// Evict the far ones (a margin past the radius, so turning on the spot does not rebuild).
		for (auto it = m_units.begin(); it != m_units.end();)
		{
			const int32 ui = (int32)(it->first >> 32), uj = (int32)(uint32)(it->first & 0xFFFFFFFFu);
			if (unitDistance(ui, uj, cam) > radius * 1.5f + unitM)
			{
				it = m_units.erase(it);
				++m_unitGeneration; // its rivers leave the near cells
			}
			else
				++it;
		}

		// Pull the nearest missing unit (from the sampler's store: resident there = no build).
		if (!m_job)
		{
			float bestDist = 0.0f;
			int32 bestI = 0, bestJ = 0;
			bool found = false;
			for (int32 i = cv - reach; i <= cv + reach; i++)
				for (int32 j = cu - reach; j <= cu + reach; j++)
				{
					const float dist = unitDistance(i, j, cam);
					if (dist > radius || m_units.count(unitKey(i, j)))
						continue;
					if (!found || dist < bestDist)
					{
						found = true;
						bestDist = dist;
						bestI = i;
						bestJ = j;
					}
				}
			if (found)
			{
				auto job = oc::make_shared<Job>();
				job->terrain = m_terrain;
				job->ui = bestI;
				job->uj = bestJ;
				job->buildSurface = surface;
				m_job = job;
				Globals::jobSystem.submit([job]()
				{
					job->result = job->terrain->unit(job->ui, job->uj);
					if (job->buildSurface && job->result && !job->result->unit->empty)
						job->hasMesh = buildSurfaceMesh(*job->terrain, *job->result->unit, job->mesh, job->mist);
				}, { "RiverSystem::pullUnit", EProfileCategory::Procedural }, EJobPriority::Low, &m_counter);
			}
		}

		if (!renderer.isInitialized())
			return;
		for (auto& [key, r] : m_units)
		{
			const int32 ui = (int32)(key >> 32), uj = (int32)(uint32)(key & 0xFFFFFFFFu);
			const float dist = unitDistance(ui, uj, cam);
			if (surface && r.node.isValid() && dist <= surfaceRadius)
				renderer.renderNode(r.node);
			if (lines && dist <= lineRadius)
			{
				if (!r.linesBuilt)
				{
					buildLines(r);
					r.linesBuilt = true;
				}
				for (const Line& l : r.lines)
					renderer.addDebugLine(l.a, l.b, l.color);
			}
		}
		if (surface)
		{
			updateNearCells(renderer, cam);
			updateWaterMap(renderer, cam);
			updateMist(renderer, cam, false);
		}
		else
		{
			if (!m_nearCells.empty())
				dropNearCells();
			renderer.setRiverNearCovered(0.0f);
			if (m_waterMapSet)
			{
				renderer.setRiverWaterMap(glm::vec2(0.0f), {});
				m_waterMapSet = false;
				m_waterCentre = glm::vec2(1.0e30f);
			}
		}
	}

	void RiverSystem::buildLines(Resident& res) const
	{
		const RiverUnit& u = *res.unit->unit;
		res.lines.clear();
		if (u.empty)
			return;
		const TerrainConfigV3& gc = m_terrain->base().config();
		const double mpp = (double)gc.metersPerPixel;
		const float vs = TerrainGenV3::worldScale(gc.metersPerPixel) * gc.heightScale;
		const double px0 = (double)u.uj * (double)m_terrain->unitPixels();
		const double pz0 = (double)u.ui * (double)m_terrain->unitPixels();
		constexpr float c_lift = 0.5f; // engine m above the water, so the line is not inside the ground
		const auto world = [&](float x, float z, float water)
		{
			return glm::vec3((float)((px0 + (double)x) * mpp - (double)gc.originX),
			                 gc.seaLevel + water * vs + c_lift,
			                 (float)((pz0 + (double)z) * mpp - (double)gc.originZ));
		};
		// A channel point's line height: the CARVED water surface (the unit's level, sunk to the ground and the valley).
		const RiverCarveConfig& carveCfg = m_terrain->carveConfig();
		const auto surface = [&](const RiverPoint& p) { return carveCfg.surface(p.water, p.depth, p.q); };
		const float perennialQ = glm::max(m_terrain->unitConfig().perennialQ, 1e-3f);

		for (const RiverSegment& seg : u.segments)
			for (uint32 k = 0; k + 1 < seg.count; k++)
			{
				const RiverPoint& a = u.points[seg.first + k];
				const RiverPoint& b = u.points[seg.first + k + 1];
				glm::vec3 c;
				if (a.flags & RiverPoint_Fall)
					c = glm::vec3(1.0f, 0.1f, 0.1f);
				else if (a.flags & RiverPoint_Rapids)
					c = glm::vec3(1.0f, 0.55f, 0.0f);
				else if (seg.ephemeral)
					c = glm::vec3(0.70f, 0.55f, 0.35f);
				else
				{
					const float t = glm::clamp(std::log10(glm::max(a.q, 1e-6f) / perennialQ) / 2.5f, 0.0f, 1.0f);
					c = glm::mix(glm::vec3(0.40f, 0.70f, 1.0f), glm::vec3(0.05f, 0.15f, 0.85f), t);
				}
				res.lines.push_back(Line{ world(a.x, a.z, surface(a)), world(b.x, b.z, surface(b)), packColor(c) });
			}

		// THE WIDTH: the channel's edges (+-half-width, white) and the carved bed's outer edge (+ the bank and the
		// floodplain, dim green - where the influence reaches 0), offset along each point's averaged normal. The
		// half-widths are model metres; the points are native pixels (nr model m each).
		const float nr = TerrainGenV3::nativeResolution();
		const RiverCarveConfig& carve = m_terrain->carveConfig();
		const float bedFactor = 1.0f + carve.bankFactor + carve.floodplainFactor;
		const uint32 channelColor = packColor(glm::vec3(0.95f, 0.95f, 1.0f));
		const uint32 bedColor = packColor(glm::vec3(0.35f, 0.6f, 0.3f));
		for (const RiverSegment& seg : u.segments)
		{
			if (seg.count < 2)
				continue;
			glm::vec2 prevL[2], prevR[2];
			float prevW = 0.0f;
			for (uint32 k = 0; k < seg.count; k++)
			{
				const RiverPoint& p = u.points[seg.first + k];
				const RiverPoint& a = u.points[seg.first + (k > 0 ? k - 1 : k)];
				const RiverPoint& b = u.points[seg.first + (k + 1 < seg.count ? k + 1 : k)];
				glm::vec2 dir(b.x - a.x, b.z - a.z);
				const float len = glm::length(dir);
				dir = len > 1e-6f ? dir / len : glm::vec2(1.0f, 0.0f);
				const glm::vec2 n(-dir.y, dir.x);
				const glm::vec2 c(p.x, p.z);
				const float hwPx = p.halfWidth / nr;
				const glm::vec2 l[2] = { c + n * hwPx, c + n * (hwPx * bedFactor) };
				const glm::vec2 r[2] = { c - n * hwPx, c - n * (hwPx * bedFactor) };
				if (k > 0)
					for (int e = 0; e < 2; e++)
					{
						const uint32 color = e == 0 ? channelColor : bedColor;
						res.lines.push_back(Line{ world(prevL[e].x, prevL[e].y, prevW), world(l[e].x, l[e].y, surface(p)), color });
						res.lines.push_back(Line{ world(prevR[e].x, prevR[e].y, prevW), world(r[e].x, r[e].y, surface(p)), color });
					}
				for (int e = 0; e < 2; e++)
				{
					prevL[e] = l[e];
					prevR[e] = r[e];
				}
				prevW = surface(p);
			}
		}

		// The crossings: a short tick up from the carved water at the border pixel (magenta = the water leaves the unit,
		// green = it enters).
		const RiverUnitConfig& unitCfg = m_terrain->unitConfig();
		for (const RiverCrossing& x : u.crossings)
		{
			const float depth = unitCfg.depthC * std::pow(glm::max(x.q, 0.0f), 0.4f);
			const glm::vec3 p = world(x.x, x.z, carveCfg.surface(x.water, depth, x.q));
			const uint32 color = packColor(x.inlet ? glm::vec3(0.1f, 1.0f, 0.2f) : glm::vec3(1.0f, 0.1f, 1.0f));
			res.lines.push_back(Line{ p, p + glm::vec3(0.0f, 4.0f, 0.0f), color });
		}

		for (const RiverLakeRun& run : u.lakeRuns)
		{
			const RiverLake& lake = u.lakes[run.lake];
			glm::vec3 c;
			switch (lake.kind)
			{
			case ERiverWater::TerminalLake: c = glm::vec3(0.30f, 0.65f, 0.65f); break;
			case ERiverWater::Pan:          c = glm::vec3(0.92f, 0.90f, 0.84f); break;
			default:                        c = glm::vec3(0.0f, 0.75f, 1.0f); break;
			}
			const float x0 = (float)run.x0 - 0.5f, x1 = (float)(run.x0 + run.len) - 0.5f;
			res.lines.push_back(Line{ world(x0, (float)run.row, lake.level), world(x1, (float)run.row, lake.level), packColor(c) });
		}
	}
}
