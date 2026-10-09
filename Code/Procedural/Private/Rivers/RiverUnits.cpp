module Procedural;

import Core;
import File;

import :GeneratorV3;
import :RiverRouting;
import :RiverNetwork;
import :RiverUnits;

namespace
{
	using namespace Procedural;

	// Bump when the file layout or ANYTHING that shapes a unit's content changes (the settings are in the hash).
	constexpr uint32 RIVER_UNIT_MAGIC = 0x55525652; // 'RVRU'
	constexpr uint32 RIVER_UNIT_VERSION = 15; // 2: soft unit walls, ERiverEnd::Edge. 3: breach profile. 4: path smoothing.
	                                          // 5: upstream-first profile, no backwater floor. 6: W-aware simplify.
	                                          // 7: crossing segments meet on the tile boundary, outlet Q blend. 8: meanders.
	                                          // 9: the W ground clamp on the final path. 10: the edge reroute.
	                                          // 11: deep small basins are ponds, not cut (RiverRouting).
	                                          // 12: crossing segments aligned to the edge's normal at the crossing.
	                                          // 13: the meander pull-back smoothed over half a wavelength, no cap.
	                                          // 14: the meander's steepness factor smoothed the same way.
	                                          // 15: the outlet Q blend only where the fine Q is at least half the coarse

	struct UnitHeader
	{
		uint32 magic = 0;
		uint32 version = 0;
		uint64 hash = 0;
		int32 ui = 0, uj = 0, tiles = 0;
		uint32 pad = 0;
	};

	// D4 in ECoarseDir order (PosX, PosZ, NegX, NegZ).
	constexpr int32 c_dx4[4] = { 1, 0, -1, 0 };
	constexpr int32 c_dz4[4] = { 0, 1, 0, -1 };

	int32 floorDivI(int32 a, int32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

	float smoothstep01(float t)
	{
		t = oc::clamp(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	uint64 tileKey(int32 ti, int32 tj) { return ((uint64)(uint32)ti << 32) | (uint64)(uint32)tj; }

	uint64 fnv(uint64 h, const void* data, size_t bytes)
	{
		const uint8* p = (const uint8*)data;
		for (size_t i = 0; i < bytes; i++)
			h = (h ^ p[i]) * 0x100000001B3ull;
		return h;
	}
	template<typename T>
	void hashValue(uint64& h, T v) { h = fnv(h, &v, sizeof(v)); }

	// Every input a unit depends on besides its tiles (the seed and precision are the folder).
	uint64 unitHash(const TerrainConfigV3& gen, const RiverConfig& rc, const RiverUnitConfig& uc)
	{
		uint64 h = 0xCBF29CE484222325ull;
		hashValue(h, gen.temperatureOffset);
		hashValue(h, gen.lapseRate);
		hashValue(h, gen.humidityOffset);
		hashValue(h, gen.precipForFullHumidity);
		hashValue(h, rc.coarseDomain);
		hashValue(h, rc.seaDepth);
		hashValue(h, rc.petPerC);
		hashValue(h, rc.budykoW);
		hashValue(h, rc.lakeEvap);
		hashValue(h, rc.loss);
		hashValue(h, rc.breachDepth);
		hashValue(h, rc.lakeMinCells);
		hashValue(h, uc.unitTiles);
		hashValue(h, uc.crossWindow);
		hashValue(h, uc.breachDepth);
		hashValue(h, uc.lakeMinCells);
		hashValue(h, uc.channelMinQ);
		hashValue(h, uc.fadeQ);
		hashValue(h, uc.perennialQ);
		hashValue(h, uc.widthA);
		hashValue(h, uc.depthC);
		hashValue(h, uc.rapidsSlope);
		hashValue(h, uc.fallSlope);
		hashValue(h, uc.edgeWall);
		hashValue(h, uc.pathSmoothing);
		hashValue(h, uc.meanderAmplitude);
		hashValue(h, uc.meanderWavelength);
		hashValue(h, uc.meanderSlope);
		hashValue(h, uc.meanderSmallAmplitude);
		hashValue(h, uc.meanderSmallWavelength);
		hashValue(h, uc.meanderFullQ);
		return h;
	}

	oc::string unitPath(const TerrainGenV3& gen, int32 ui, int32 uj, int32 tiles)
	{
		return FileSystem::join(gen.tileCacheFolder(), oc::format("river_x{}_z{}_n{}.rvu", uj, ui, tiles));
	}

	struct Writer
	{
		oc::vector<uint8> data;
		void bytes(const void* src, size_t n)
		{
			const uint8* p = (const uint8*)src;
			data.insert(data.end(), p, p + n);
		}
		template<typename T>
		void vec(const oc::vector<T>& v)
		{
			const uint32 n = (uint32)v.size();
			bytes(&n, sizeof(n));
			bytes(v.data(), sizeof(T) * v.size());
		}
	};
	struct Reader
	{
		const oc::vector<uint8>& data;
		size_t cursor = 0;
		bool ok = true;
		bool bytes(void* dst, size_t n)
		{
			if (!ok || cursor + n > data.size())
				return ok = false;
			std::memcpy(dst, data.data() + cursor, n);
			cursor += n;
			return true;
		}
		template<typename T>
		bool vec(oc::vector<T>& v)
		{
			uint32 n = 0;
			if (!bytes(&n, sizeof(n)) || (size_t)n * sizeof(T) > data.size() - cursor)
				return ok = false;
			v.resize(n);
			return bytes(v.data(), sizeof(T) * n);
		}
	};

	oc::shared_ptr<const RiverUnit> loadUnit(const oc::string& path, uint64 hash, int32 ui, int32 uj, int32 tiles)
	{
		oc::vector<uint8> data;
		if (!FileSystem::readFileBytes(path, data) || data.empty())
			return nullptr;
		Reader r{ data };
		UnitHeader h;
		r.bytes(&h, sizeof(h));
		// A mismatch is a miss, not an error: the unit is rebuilt and the file overwritten.
		if (!r.ok || h.magic != RIVER_UNIT_MAGIC || h.version != RIVER_UNIT_VERSION || h.hash != hash
			|| h.ui != ui || h.uj != uj || h.tiles != tiles)
			return nullptr;
		auto u = oc::make_shared<RiverUnit>();
		u->ui = ui;
		u->uj = uj;
		u->tiles = tiles;
		if (!r.vec(u->points) || !r.vec(u->segments) || !r.vec(u->crossings) || !r.vec(u->lakes) || !r.vec(u->lakeRuns)
			|| r.cursor != data.size())
			return nullptr;
		return u;
	}

	void saveUnit(const oc::string& path, uint64 hash, const RiverUnit& u)
	{
		FileSystem::createDirectories(FileSystem::parentPath(path));
		Writer w;
		UnitHeader h;
		h.magic = RIVER_UNIT_MAGIC;
		h.version = RIVER_UNIT_VERSION;
		h.hash = hash;
		h.ui = u.ui;
		h.uj = u.uj;
		h.tiles = u.tiles;
		w.bytes(&h, sizeof(h));
		w.vec(u.points);
		w.vec(u.segments);
		w.vec(u.crossings);
		w.vec(u.lakes);
		w.vec(u.lakeRuns);
		FileSystem::writeFileBytes(path, oc::span<const uint8>(w.data.data(), w.data.size()));
	}

	// The coarse pixel over a full tile (one coarse pixel IS one full tile).
	struct CoarseLookup
	{
		const CoarseRiverNetwork& net;
		int32 size = 0;
		oc::unordered_map<uint64, oc::shared_ptr<const CoarseRiverTile>> tiles;

		const CoarseRiverTile* at(int32 ti, int32 tj, size_t& outIndex)
		{
			const int32 cti = floorDivI(ti, size), ctj = floorDivI(tj, size);
			const uint64 key = tileKey(cti, ctj);
			auto it = tiles.find(key);
			if (it == tiles.end())
				it = tiles.emplace(key, net.tile(cti, ctj)).first;
			outIndex = (size_t)(ti - cti * size) * size + (size_t)(tj - ctj * size);
			return it->second.get();
		}
	};

	ECoarseDir dirOf(int32 k) { return (ECoarseDir)(k + 1); }
	ECoarseDir opposite(ECoarseDir d)
	{
		switch (d)
		{
		case ECoarseDir::PosX: return ECoarseDir::NegX;
		case ECoarseDir::NegX: return ECoarseDir::PosX;
		case ECoarseDir::PosZ: return ECoarseDir::NegZ;
		case ECoarseDir::NegZ: return ECoarseDir::PosZ;
		default: return ECoarseDir::None;
		}
	}

	// A point of a channel while it is built: position (unit px), water level, discharge.
	struct ChainPoint
	{
		float x, z, w, q;
	};

	// THE PATH SMOOTHING: a D8 path is long straight runs at 0 / 45 / 90 degrees with sharp kinks, which no corner cutting
	// inside a pixel hides. Resampled at 1 px of arc length, then a Gaussian average of the plan position along it
	// (sigma in px). The window shrinks SYMMETRICALLY toward the ends, so the end points (junctions, crossings, mouths)
	// stay exactly where they are and the smoothing ramps in from them. W and Q stay with their arc position.
	void smoothPath(oc::vector<ChainPoint>& pts, float sigma)
	{
		if (sigma <= 0.0f || pts.size() < 3)
			return;
		oc::vector<ChainPoint> even;
		even.push_back(pts.front());
		float carry = 0.0f; // arc length since the last emitted point
		for (size_t i = 0; i + 1 < pts.size(); i++)
		{
			const ChainPoint& a = pts[i];
			const ChainPoint& b = pts[i + 1];
			const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
			float s = 1.0f - carry; // the next sample's distance along this piece
			while (s < len)
			{
				const float t = s / len;
				even.push_back(ChainPoint{ a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t, a.q + (b.q - a.q) * t });
				s += 1.0f;
			}
			carry = len - (s - 1.0f);
		}
		even.push_back(pts.back());

		const int32 n = (int32)even.size();
		const int32 radius = (int32)std::ceil(3.0f * sigma);
		const float inv2s2 = 1.0f / (2.0f * sigma * sigma);
		pts.resize((size_t)n);
		for (int32 i = 0; i < n; i++)
		{
			const int32 r = oc::min(radius, oc::min(i, n - 1 - i));
			float sx = 0.0f, sz = 0.0f, sw = 0.0f;
			for (int32 k = -r; k <= r; k++)
			{
				const float wgt = std::exp(-(float)(k * k) * inv2s2);
				sx += even[i + k].x * wgt;
				sz += even[i + k].z * wgt;
				sw += wgt;
			}
			pts[i] = ChainPoint{ sx / sw, sz / sw, even[i].w, even[i].q };
		}
	}

	uint32 hashU32(uint32 x)
	{
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		x ^= x >> 16;
		return x;
	}

	// THE MEANDERS: a smoothed D8 path still runs straight wherever the flow did, and real rivers almost never do. A
	// sideways swing along the 1 px resampled path: two sines over an ACCUMULATED phase (the wavelength follows the
	// channel width as it changes, so the phase never jumps), "Meander wavelength" x the width apart, "Meander
	// amplitude" x the width wide, the phase from `seed`. It fades to 0 within half a wavelength of each end (junctions,
	// crossings and mouths stay put) and shrinks toward 30 % as the water surface steepens past "Meander slope" (a
	// mountain stream is straighter). Small streams wind tighter and wider in widths ("Meander small amplitude" /
	// "small wavelength" up to "Meander full Q", log Q). Then it is KEPT ON THE VALLEY FLOOR: where the swung point's ground stands more
	// than the channel's depth above the point's own, the swing there halves (up to three times, else none), and that
	// scale is box-smoothed along the path so a pull-back does not kink. W and Q stay with their arc position.
	void meander(oc::vector<ChainPoint>& pts, const RiverUnitConfig& cfg, const oc::vector<float>& elev,
	             const oc::vector<uint8>& mask, int32 W, uint32 seed, float nr)
	{
		const size_t n = pts.size();
		if (n < 4 || cfg.meanderAmplitude <= 0.0f)
			return;
		const auto elevAt = [&](float x, float z) -> float
		{
			const int32 x0 = (int32)std::floor(x), z0 = (int32)std::floor(z);
			if (x0 < 0 || z0 < 0 || x0 + 1 >= W || z0 + 1 >= W)
				return FLT_MAX; // off the unit: never swing there
			const size_t i = (size_t)z0 * W + x0;
			if (!mask[i] || !mask[i + 1] || !mask[i + W] || !mask[i + W + 1])
				return FLT_MAX;
			const float fx = x - (float)x0, fz = z - (float)z0;
			return glm::mix(glm::mix(elev[i], elev[i + 1], fx), glm::mix(elev[i + W], elev[i + W + 1], fx), fz);
		};

		oc::vector<float> s(n, 0.0f), off(n, 0.0f), scale(n, 1.0f), halfLambda(n, 8.0f), steepness(n, 1.0f);
		oc::vector<glm::vec2> normal(n);
		for (size_t k = 1; k < n; k++)
			s[k] = s[k - 1] + glm::length(glm::vec2(pts[k].x - pts[k - 1].x, pts[k].z - pts[k - 1].z));
		const float total = s[n - 1];
		const float ph1 = (float)(hashU32(seed) & 0xFFFF) / 65535.0f * 6.2831853f;
		const float ph2 = (float)(hashU32(seed ^ 0x5BD1E995u) & 0xFFFF) / 65535.0f * 6.2831853f;
		float phase = 0.0f;
		float slope = 0.0f;
		// SMALL STREAMS wind tighter and wider (in widths): from "Channel min Q" up to "Meander full Q" (log Q) the swing
		// goes from x "Meander small amplitude" to x 1 and the wavelength from x "Meander small wavelength" to x 1.
		const float logMin = std::log(glm::max(cfg.channelMinQ, 1e-4f));
		const float logSpan = glm::max(std::log(glm::max(cfg.meanderFullQ, 1e-4f)) - logMin, 1e-3f);
		for (size_t k = 0; k < n; k++)
		{
			const ChainPoint& p = pts[k];
			const float size = glm::clamp((std::log(glm::max(p.q, 1e-4f)) - logMin) / logSpan, 0.0f, 1.0f);
			const float widthPx = glm::max(cfg.widthA * std::sqrt(glm::max(p.q, 0.0f)) / nr, 0.2f);
			const float lambda = glm::max(cfg.meanderWavelength * glm::mix(cfg.meanderSmallWavelength, 1.0f, size) * widthPx, 4.0f);
			halfLambda[k] = 0.5f * lambda;
			if (k > 0)
				phase += 6.2831853f * (s[k] - s[k - 1]) / lambda;
			if (k + 1 < n)
				slope = (p.w - pts[k + 1].w) / glm::max((s[k + 1] - s[k]) * nr, 1e-3f);
			steepness[k] = glm::mix(1.0f, 0.3f, glm::smoothstep(0.0f, glm::max(cfg.meanderSlope, 1e-4f), slope));
			const float ends = glm::smoothstep(0.0f, 0.5f * lambda, s[k]) * glm::smoothstep(0.0f, 0.5f * lambda, total - s[k]);
			off[k] = cfg.meanderAmplitude * glm::mix(cfg.meanderSmallAmplitude, 1.0f, size) * widthPx * ends
				* (0.75f * std::sin(phase + ph1) + 0.25f * std::sin(0.43f * phase + ph2));

			const ChainPoint& a = pts[k > 0 ? k - 1 : k];
			const ChainPoint& b = pts[k + 1 < n ? k + 1 : k];
			glm::vec2 dir(b.x - a.x, b.z - a.z);
			const float len = glm::length(dir);
			dir = len > 1e-6f ? dir / len : glm::vec2(1.0f, 0.0f);
			normal[k] = glm::vec2(-dir.y, dir.x);
		}

		// Keep it on the valley floor: the largest of 1, 3/4, 1/2, 1/4 of the swing whose ground stands at most the channel's
		// depth above the point's own (else none) ...
		for (size_t k = 0; k < n; k++)
		{
			if (off[k] == 0.0f)
				continue;
			const ChainPoint& p = pts[k];
			const float base = elevAt(p.x, p.z);
			const float tolerance = cfg.depthC * std::pow(glm::max(p.q, 0.0f), 0.4f); // the channel's own depth
			scale[k] = 0.0f;
			for (int32 t = 4; t >= 1; t--)
			{
				const float f = 0.25f * (float)t;
				if (elevAt(p.x + normal[k].x * off[k] * f, p.z + normal[k].y * off[k] * f) <= base + tolerance)
				{
					scale[k] = f;
					break;
				}
			}
		}
		// ... and the steepness factor too - BOTH SMOOTHED over half a meander wavelength (the samples are 1 px apart): a
		// pull-back eases in over a bend. Both jumped between neighbouring samples - the steepness with the breach
		// profile's staircase of flats and drops (0.3 / 1 per sample), the pull-back by halves, kept by a cap at each
		// sample's own limit - and every jump moved the swing sideways at once: Z-shaped jogs wherever it was large (seen
		// 2026-10-09). A swing smoothed a little past its limit only climbs the bank; the ground clamp after keeps its water down.
		oc::vector<float> smoothScale(n);
		for (size_t k = 0; k < n; k++)
		{
			const int32 radius = glm::clamp((int32)halfLambda[k], 8, 64);
			float sumScale = 0.0f, sumSteep = 0.0f;
			int32 count = 0;
			for (int32 o = -radius; o <= radius; o++)
			{
				const int64 j = (int64)k + o;
				if (j >= 0 && j < (int64)n)
				{
					sumScale += scale[(size_t)j];
					sumSteep += steepness[(size_t)j];
					count++;
				}
			}
			smoothScale[k] = sumScale * sumSteep / (float)(count * count);
		}
		for (size_t k = 0; k < n; k++)
		{
			pts[k].x += normal[k].x * off[k] * smoothScale[k];
			pts[k].z += normal[k].y * off[k] * smoothScale[k];
		}
	}

	// THE GROUND CLAMP, after all the path shaping: the profile's W was walked along the raw D8 pixels, and the smoothing
	// and the meanders then move the path sideways with W tied to its arc position. Where the moved path lies over LOWER
	// ground (the downhill side of a slope, across a bend) W stood above it - water in the air, which the carve cannot
	// fix (it only lowers). So on the FINAL path W may stand at most the channel's depth above the ground under it, and
	// it never rises downstream again (a running min). Off the unit's tiles the ground is unknown: no clamp there.
	void clampToGround(oc::vector<ChainPoint>& pts, const RiverUnitConfig& cfg, const oc::vector<float>& elev,
	                   const oc::vector<uint8>& mask, int32 W)
	{
		for (size_t k = 0; k < pts.size(); k++)
		{
			ChainPoint& p = pts[k];
			const int32 x0 = (int32)std::floor(p.x), z0 = (int32)std::floor(p.z);
			if (x0 >= 0 && z0 >= 0 && x0 + 1 < W && z0 + 1 < W)
			{
				const size_t i = (size_t)z0 * W + x0;
				if (mask[i] && mask[i + 1] && mask[i + W] && mask[i + W + 1])
				{
					const float fx = p.x - (float)x0, fz = p.z - (float)z0;
					const float ground = glm::mix(glm::mix(elev[i], elev[i + 1], fx), glm::mix(elev[i + W], elev[i + W + 1], fx), fz);
					p.w = glm::min(p.w, ground + cfg.depthC * std::pow(glm::max(p.q, 0.0f), 0.4f));
				}
			}
			if (k > 0)
				p.w = glm::min(p.w, pts[k - 1].w);
		}
	}

	// Chaikin corner cutting with the end points kept: the D8 staircase becomes a curve inside its own pixels.
	void chaikin(oc::vector<ChainPoint>& pts, int32 iterations)
	{
		oc::vector<ChainPoint> next;
		for (int32 it = 0; it < iterations && pts.size() > 2; it++)
		{
			next.clear();
			next.push_back(pts.front());
			for (size_t i = 0; i + 1 < pts.size(); i++)
			{
				const ChainPoint& a = pts[i];
				const ChainPoint& b = pts[i + 1];
				const auto lerp = [](const ChainPoint& p, const ChainPoint& q, float t)
				{
					return ChainPoint{ p.x + (q.x - p.x) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t, p.q + (q.q - p.q) * t };
				};
				if (i > 0)
					next.push_back(lerp(a, b, 0.25f));
				if (i + 2 < pts.size())
					next.push_back(lerp(a, b, 0.75f));
			}
			next.push_back(pts.back());
			pts.swap(next);
		}
	}

	// Douglas-Peucker: drops the points a straight reach does not need - straight in plan AND in its water level. W is
	// linear between the kept points (the carve and the lines read it so), so a point is kept when either its plan offset
	// passes tolerancePx or its W passes toleranceW (model m) from the interpolation. Plan-only, a reach straight in
	// plan that drops into a canyon partway kept just its ends, and the water sloped evenly through the air over the drop.
	void simplify(const oc::vector<ChainPoint>& in, float tolerancePx, float toleranceW, oc::vector<ChainPoint>& out)
	{
		out.clear();
		if (in.size() <= 2)
		{
			out = in;
			return;
		}
		oc::vector<uint8> keep(in.size(), 0);
		keep.front() = keep.back() = 1;
		oc::vector<oc::pair<size_t, size_t>> stack;
		stack.push_back({ 0, in.size() - 1 });
		const float invTol2 = 1.0f / oc::max(tolerancePx * tolerancePx, 1e-12f);
		const float invTolW2 = 1.0f / oc::max(toleranceW * toleranceW, 1e-12f);
		while (!stack.empty())
		{
			const size_t a = stack.back().first, b = stack.back().second;
			stack.pop_back();
			const float dx = in[b].x - in[a].x, dz = in[b].z - in[a].z;
			const float len2 = dx * dx + dz * dz;
			float worst = -1.0f; // the larger of the plan and the W error, each over its tolerance (squared)
			size_t worstAt = a;
			for (size_t i = a + 1; i < b; i++)
			{
				const float px = in[i].x - in[a].x, pz = in[i].z - in[a].z;
				float d2, t;
				if (len2 > 1e-12f)
				{
					const float cr = px * dz - pz * dx;
					d2 = cr * cr / len2;
					t = oc::clamp((px * dx + pz * dz) / len2, 0.0f, 1.0f);
				}
				else
				{
					d2 = px * px + pz * pz;
					t = 0.0f;
				}
				const float dw = in[i].w - (in[a].w + (in[b].w - in[a].w) * t);
				const float err = oc::max(d2 * invTol2, dw * dw * invTolW2);
				if (err > worst)
				{
					worst = err;
					worstAt = i;
				}
			}
			if (worst > 1.0f)
			{
				keep[worstAt] = 1;
				stack.push_back({ a, worstAt });
				stack.push_back({ worstAt, b });
			}
		}
		for (size_t i = 0; i < in.size(); i++)
			if (keep[i])
				out.push_back(in[i]);
	}
}

namespace Procedural
{
	oc::shared_ptr<const RiverUnit> buildRiverUnit(const TerrainGenV3& gen, const CoarseRiverNetwork& net,
	                                               const RiverUnitConfig& cfg, int32 ui, int32 uj,
	                                               const oc::atomic<bool>* cancel)
	{
		const auto cancelled = [cancel]() { return cancel && cancel->load(oc::memory_order_relaxed); };
		const int32 N = oc::clamp(cfg.unitTiles, 1, 8);
		const int32 TP = TerrainGenV3::fullTilePixels();
		const int32 W = N * TP;
		const size_t NP = (size_t)W * W;
		const int32 ti0 = ui * N, tj0 = uj * N;

		// Cacheable only when every tile it reads (its own, and every edge neighbour a crossing may read) lies in the
		// generated bounds: past them the result depends on which tiles exist.
		bool cacheable = true;
		for (int32 a = -1; a <= N && cacheable; a++)
			for (int32 b = -1; b <= N && cacheable; b++)
			{
				const bool corner = (a < 0 || a >= N) && (b < 0 || b >= N);
				if (!corner && !gen.fullTileInBounds(ti0 + a, tj0 + b))
					cacheable = false;
			}
		const uint64 hash = unitHash(gen.config(), net.config(), cfg);
		const oc::string path = unitPath(gen, ui, uj, N);
		if (cacheable)
			if (oc::shared_ptr<const RiverUnit> u = loadUnit(path, hash, ui, uj, N))
				return u;

		auto unit = oc::make_shared<RiverUnit>();
		unit->ui = ui;
		unit->uj = uj;
		unit->tiles = N;

		// --- The unit's tiles.
		oc::vector<oc::shared_ptr<const FullFieldPlanes>> planes((size_t)N * N);
		bool any = false;
		for (int32 lt = 0; lt < N; lt++)
			for (int32 lc = 0; lc < N; lc++)
			{
				if (cancelled())
					return nullptr;
				auto p = oc::make_shared<FullFieldPlanes>();
				if (gen.fetchFullTilePlanes(ti0 + lt, tj0 + lc, *p))
				{
					planes[(size_t)lt * N + lc] = p;
					any = true;
				}
				else
					cacheable = false;
			}
		if (!any)
		{
			unit->empty = true;
			return unit;
		}

		// --- The raster and its climate.
		const float nr = TerrainGenV3::nativeResolution(); // model m per native pixel
		const float mmToQ = nr * nr * 0.001f / 3.156e7f;
		const RiverConfig& rc = net.config();
		DrainageGrid g;
		g.w = W;
		g.h = W;
		g.d8 = true;
		g.elev.assign(NP, 0.0f);
		g.runoff.assign(NP, 0.0f);
		g.lakeNet.assign(NP, 0.0f);
		g.aridity.assign(NP, 0.0f);
		g.inject.assign(NP, 0.0f);
		g.mask.assign(NP, 0);
		g.cellKm = nr * 0.001f;
		g.loss = rc.loss;
		g.seaDepth = rc.seaDepth;
		g.breachDepth = cfg.breachDepth;
		g.lakeMinCells = cfg.lakeMinCells;
		for (int32 lt = 0; lt < N; lt++)
			for (int32 lc = 0; lc < N; lc++)
			{
				const FullFieldPlanes* p = planes[(size_t)lt * N + lc].get();
				if (!p)
					continue;
				for (int32 r = 0; r < TP; r++)
					for (int32 c = 0; c < TP; c++)
					{
						const size_t src = (size_t)r * TP + c;
						const size_t idx = (size_t)(lt * TP + r) * W + (size_t)(lc * TP + c);
						g.mask[idx] = 1;
						g.elev[idx] = p->elev[src];
						riverPixelClimate(gen.config(), rc.petPerC, rc.budykoW, rc.lakeEvap, p->elev[src], p->tempSea[src],
							p->precip[src], mmToQ, g.runoff[idx], g.lakeNet[idx], g.aridity[idx]);
					}
			}
		markSea(g);

		// --- Each tile's land runoff rescaled to its coarse pixel's own, so the two levels agree on the budget.
		CoarseLookup coarse{ net, TerrainGenV3::coarseTilePixels() };
		for (int32 lt = 0; lt < N; lt++)
			for (int32 lc = 0; lc < N; lc++)
			{
				if (!planes[(size_t)lt * N + lc])
					continue;
				size_t ci = 0;
				const CoarseRiverTile* ct = coarse.at(ti0 + lt, tj0 + lc, ci);
				if (!ct || ct->runoff[ci] <= 0.0f)
					continue;
				double sum = 0.0;
				for (int32 r = 0; r < TP; r++)
					for (int32 c = 0; c < TP; c++)
					{
						const size_t idx = (size_t)(lt * TP + r) * W + (size_t)(lc * TP + c);
						if (!g.sea[idx])
							sum += (double)g.runoff[idx];
					}
				if (sum <= 0.0)
					continue;
				const float scale = (float)((double)ct->runoff[ci] / sum);
				for (int32 r = 0; r < TP; r++)
					for (int32 c = 0; c < TP; c++)
						g.runoff[(size_t)(lt * TP + r) * W + (size_t)(lc * TP + c)] *= scale;
			}

		// THE GROWTH: a channel's width and depth rise from 0 at "Channel min Q" to the hydraulic geometry at min Q + "Fade
		// Q", so a stream fades in at its head and out where its water drains away (dry land, a sink) instead of starting
		// and stopping at full size.
		const auto growth = [&cfg](float q) { return smoothstep01((q - cfg.channelMinQ) / oc::max(cfg.fadeQ, 1e-4f)); };
		const auto depthOf = [&cfg, &growth](float q) { return cfg.depthC * std::pow(oc::max(q, 0.0f), 0.4f) * growth(q); };
		const auto halfWidthOf = [&cfg, &growth](float q) { return 0.5f * cfg.widthA * std::sqrt(oc::max(q, 0.0f)) * growth(q); };

		// --- Crossings: every coarse link across the unit's edge (or into one of its missing tiles).
		oc::vector<DrainageSeed> seeds;
		// A crossing as its segment needs it: the shared level and Q, and the point ON the tile boundary between the two
		// border pixels - both units compute the same one, so their segments meet there.
		struct CrossingEnd
		{
			float water = 0.0f;
			float q = 0.0f;
			float edgeX = 0.0f, edgeZ = 0.0f; // unit px
		};
		oc::unordered_map<int32, CrossingEnd> outlets, inlets;
		oc::unordered_map<uint64, oc::shared_ptr<const FullFieldPlanes>> outside;
		const auto outsidePlanes = [&](int32 ti, int32 tj) -> const FullFieldPlanes*
		{
			const uint64 key = tileKey(ti, tj);
			auto it = outside.find(key);
			if (it == outside.end())
			{
				auto p = oc::make_shared<FullFieldPlanes>();
				oc::shared_ptr<const FullFieldPlanes> got;
				if (gen.fetchFullTilePlanes(ti, tj, *p))
					got = p;
				else
					cacheable = false;
				it = outside.emplace(key, got).first;
			}
			return it->second.get();
		};
		oc::vector<float> edge(TP), smooth(TP);
		for (int32 lt = 0; lt < N; lt++)
			for (int32 lc = 0; lc < N; lc++)
			{
				const FullFieldPlanes* tp = planes[(size_t)lt * N + lc].get();
				if (!tp)
					continue;
				const int32 ti = ti0 + lt, tj = tj0 + lc;
				size_t ci = 0;
				const CoarseRiverTile* ct = coarse.at(ti, tj, ci);
				if (!ct)
					continue;
				for (int32 k = 0; k < 4; k++)
				{
					const int32 nlt = lt + c_dz4[k], nlc = lc + c_dx4[k];
					const bool inUnit = nlt >= 0 && nlc >= 0 && nlt < N && nlc < N;
					if (inUnit && planes[(size_t)nlt * N + nlc])
						continue; // internal: the fine routing decides that path
					const ECoarseDir dir = dirOf(k);
					const bool outlet = ct->dir[ci] == dir;
					size_t ni = 0;
					const CoarseRiverTile* nt = coarse.at(ti + c_dz4[k], tj + c_dx4[k], ni);
					const bool inlet = !outlet && nt && nt->dir[ni] == opposite(dir);
					if (!outlet && !inlet)
						continue;
					if (cancelled())
						return nullptr;
					const float q = outlet ? ct->q[ci] : nt->q[ni];
					const FullFieldPlanes* np = inUnit ? nullptr : outsidePlanes(ti + c_dz4[k], tj + c_dx4[k]);

					// Both tiles' border pixels along the edge, e = the position along it (the row of an x edge, the
					// column of a z edge) - the same e from either side.
					const auto tilePx = [TP](int32 kk, int32 e, bool near, int32& r, int32& c)
					{
						// near = this tile's border; otherwise the neighbour's border facing it
						switch (kk)
						{
						case 0: r = e; c = near ? TP - 1 : 0; break;      // +x
						case 2: r = e; c = near ? 0 : TP - 1; break;      // -x
						case 1: r = near ? TP - 1 : 0; c = e; break;      // +z
						default: r = near ? 0 : TP - 1; c = e; break;     // -z
						}
					};
					for (int32 e = 0; e < TP; e++)
					{
						int32 r, c;
						tilePx(k, e, true, r, c);
						float v = tp->elev[(size_t)r * TP + c];
						if (np)
						{
							tilePx(k, e, false, r, c);
							v = oc::min(v, np->elev[(size_t)r * TP + c]);
						}
						edge[e] = v;
					}
					constexpr int32 c_smoothRadius = 3;
					for (int32 e = 0; e < TP; e++)
					{
						float s = 0.0f;
						int32 n = 0;
						for (int32 o = -c_smoothRadius; o <= c_smoothRadius; o++)
						{
							const int32 ee = e + o;
							if (ee >= 0 && ee < TP)
							{
								s += edge[ee];
								n++;
							}
						}
						smooth[e] = s / (float)n;
					}
					const int32 win = oc::clamp(cfg.crossWindow, 0, TP / 2 - 1);
					const int32 lo = TP / 2 - 1 - win, hi = TP / 2 + win;
					int32 best = lo;
					for (int32 e = lo + 1; e <= hi; e++)
						if (smooth[e] < smooth[best])
							best = e;

					int32 r, c;
					tilePx(k, best, true, r, c);
					const int32 idx = (lt * TP + r) * W + (lc * TP + c);
					RiverCrossing x;
					x.x = (float)(lc * TP + c);
					x.z = (float)(lt * TP + r);
					x.q = q;
					x.water = edge[best] + depthOf(q);
					x.inlet = inlet ? 1 : 0;
					unit->crossings.push_back(x);
					const CrossingEnd ce{ x.water, q, x.x + 0.5f * (float)c_dx4[k], x.z + 0.5f * (float)c_dz4[k] };
					if (outlet)
					{
						seeds.push_back(DrainageSeed{ idx, g.elev[idx], 0 });
						outlets[idx] = ce;
					}
					else
					{
						g.inject[idx] += q;
						inlets[idx] = ce;
					}
				}

				// A tile whose coarse water ends in it (a terminal lake, a pan, the sea at coarse level) with no sea of its
				// own drains to its lowest pixel.
				if (ct->dir[ci] == ECoarseDir::None)
				{
					int32 lowest = -1;
					bool hasSea = false;
					for (int32 r = 0; r < TP && !hasSea; r++)
						for (int32 c = 0; c < TP; c++)
						{
							const int32 idx = (lt * TP + r) * W + (lc * TP + c);
							if (g.sea[idx])
							{
								hasSea = true;
								break;
							}
							if (lowest < 0 || g.elev[idx] < g.elev[lowest])
								lowest = idx;
						}
					if (!hasSea && lowest >= 0)
						seeds.push_back(DrainageSeed{ lowest, g.elev[lowest], 0 });
				}
			}

		// The soft walls: every edge pixel of the present region (the unit's edge, or beside a missing tile) that is
		// not a crossing, ordered "Edge wall" metres higher than its ground.
		oc::unordered_set<int32> hardSeeds;
		for (const DrainageSeed& s : seeds)
			hardSeeds.insert(s.idx);
		for (int32 z = 0; z < W; z++)
			for (int32 x = 0; x < W; x++)
			{
				const int32 i = z * W + x;
				if (!g.mask[i] || g.sea[i] || hardSeeds.count(i) || inlets.count(i))
					continue;
				bool border = false;
				for (int32 dz = -1; dz <= 1 && !border; dz++)
					for (int32 dx = -1; dx <= 1; dx++)
					{
						const int32 nx = x + dx, nz = z + dz;
						if (nx < 0 || nz < 0 || nx >= W || nz >= W || !g.mask[(size_t)nz * W + nx])
						{
							border = true;
							break;
						}
					}
				if (border)
					seeds.push_back(DrainageSeed{ i, g.elev[i] + cfg.edgeWall, 1 });
			}
		oc::unordered_set<int32> softSeeds;
		for (const DrainageSeed& s : seeds)
			if (s.soft)
				softSeeds.insert(s.idx);

		if (cancelled())
			return nullptr;
		DrainageResult d;
		routeDrainage(g, seeds, d);

		// THE EDGE REROUTE (the user, 2026-10-09): the coarse network decides where water crosses a unit's edge, the fine
		// routing where it really goes - over a ridge higher than "Edge wall" a big river left through the SOFT wall (it
		// ended at the border) while the coarse crossing a few tiles along the same edge got almost nothing, and the
		// NEIGHBOUR, which injects the coarse Q there, started a full-size river from nothing. So an outlet whose fine water
		// falls short (under half its coarse Q) takes the largest soft exit on the SAME unit edge within c_rerouteAlong px
		// carrying at least c_rerouteShare of it: the exit's water runs on to the outlet along the least-climb path inside
		// the unit (within c_rerouteBand px of the edge; the profile's breach cuts the rim, the outlet's Q blend matches the
		// Q). The path's pixels take the exit's rank, so whatever joins them is profiled first. The water keeps the exit's
		// level through the rim (a gorge); the NEIGHBOUR carries that lower level on from its inlet (RiverTerrain's inlet
		// match), so the two meet without a step.
		{
			constexpr int32 c_rerouteAlong = 3 * 256;
			constexpr int32 c_rerouteBand = 48;
			constexpr float c_rerouteShare = 0.3f;
			constexpr float c_climbCost = 0.5f; // extra cost per step, per model m above the exit's ground
			// The highest rim a reroute may breach (model m): a basin any deeper is a pond (RiverRouting), never cut.
			const float c_rerouteMaxRim = 2.0f * cfg.breachDepth;
			oc::unordered_set<int32> usedExits;
			oc::vector<float> dist;
			oc::vector<int32> prev;
			oc::vector<int32> route;
			for (const auto& [oidx, oce] : outlets)
			{
				if (oce.q < cfg.channelMinQ || d.outflow[oidx] >= 0.5f * oce.q)
					continue;
				const int32 ox = oidx % W, oz = oidx / W;
				const int32 side = ox == 0 ? 0 : ox == W - 1 ? 1 : oz == 0 ? 2 : oz == W - 1 ? 3 : -1;
				if (side < 0)
					continue; // beside a missing tile, not the unit's edge
				const auto alongOf = [&](int32 i) { return side < 2 ? i / W : i % W; };
				const auto inwardOf = [&](int32 i) { const int32 x = i % W, z = i / W; return side == 0 ? x : side == 1 ? W - 1 - x : side == 2 ? z : W - 1 - z; };
				int32 exitIdx = -1;
				float exitQ = oc::max(cfg.channelMinQ, c_rerouteShare * oce.q);
				for (const int32 s : softSeeds)
				{
					if (inwardOf(s) != 0 || usedExits.count(s) || std::abs(alongOf(s) - alongOf(oidx)) > c_rerouteAlong)
						continue;
					if (d.outflow[s] > exitQ)
					{
						exitQ = d.outflow[s];
						exitIdx = s;
					}
				}
				if (exitIdx < 0)
					continue;
				usedExits.insert(exitIdx);

				// Dijkstra over the band between the two along the edge (+ the band's width each way).
				const int32 a0 = oc::max(oc::min(alongOf(exitIdx), alongOf(oidx)) - c_rerouteBand, 0);
				const int32 a1 = oc::min(oc::max(alongOf(exitIdx), alongOf(oidx)) + c_rerouteBand, W - 1);
				const int32 la = a1 - a0 + 1, lb = c_rerouteBand + 1;
				const auto toPixel = [&](int32 along, int32 inward)
				{
					switch (side)
					{
					case 0: return along * W + inward;
					case 1: return along * W + (W - 1 - inward);
					case 2: return inward * W + along;
					default: return (W - 1 - inward) * W + along;
					}
				};
				const auto local = [&](int32 i) { return (alongOf(i) - a0) * lb + inwardOf(i); };
				dist.assign((size_t)la * lb, FLT_MAX);
				prev.assign((size_t)la * lb, -1);
				const float baseElev = g.elev[exitIdx];
				using Entry = oc::pair<float, int32>;
				oc::priority_queue<Entry, oc::vector<Entry>, oc::greater<Entry>> open;
				dist[(size_t)local(exitIdx)] = 0.0f;
				open.push({ 0.0f, exitIdx });
				bool reached = false;
				while (!open.empty())
				{
					const Entry top = open.top();
					open.pop();
					const int32 p = top.second;
					if (top.first > dist[(size_t)local(p)])
						continue;
					if (p == oidx)
					{
						reached = true;
						break;
					}
					const int32 pa = alongOf(p), pb = inwardOf(p);
					for (int32 da = -1; da <= 1; da++)
						for (int32 db = -1; db <= 1; db++)
						{
							if (da == 0 && db == 0)
								continue;
							const int32 na = pa + da, nb = pb + db;
							if (na < a0 || na > a1 || nb < 0 || nb > c_rerouteBand)
								continue;
							const int32 n = toPixel(na, nb);
							// Off the border itself (the soft walls) but for the two ends; never through the sea or a lake.
							if (n != oidx && nb == 0)
								continue;
							if (!g.mask[n] || g.sea[n] || d.lakeOf[n] >= 0)
								continue;
							const float step = (da != 0 && db != 0) ? 1.41421356f : 1.0f;
							const float cost = top.first + step * (1.0f + c_climbCost * oc::max(g.elev[n] - baseElev, 0.0f));
							const size_t ln = (size_t)local(n);
							if (cost < dist[ln])
							{
								dist[ln] = cost;
								prev[ln] = p;
								open.push({ cost, n });
							}
						}
				}
				if (!reached)
					continue;
				route.clear();
				float rim = baseElev;
				for (int32 p = oidx; p >= 0; p = prev[(size_t)local(p)])
				{
					route.push_back(p);
					rim = oc::max(rim, g.elev[p]);
					if (p == exitIdx)
						break;
				}
				// The breach keeps the exit's level through the rim: past c_rerouteMaxRim the gorge (and the neighbour's
				// matched gorge downstream) would be a canyon - the fine and coarse drainage really disagree there; leave it.
				if (rim - baseElev > c_rerouteMaxRim)
					continue;
				oc::reverse(route.begin(), route.end());
				const int32 exitRank = oc::max(d.rank[exitIdx], 0);
				for (size_t k = 0; k + 1 < route.size(); k++)
					d.receiver[route[k]] = route[k + 1];
				for (size_t k = 1; k < route.size(); k++)
				{
					d.outflow[route[k]] += exitQ;
					if (route[k] != oidx)
						d.rank[route[k]] = exitRank;
				}
			}
		}

		// --- Channels.
		oc::vector<uint8> inC(NP, 0);
		for (size_t i = 0; i < NP; i++)
			inC[i] = g.mask[i] && !g.sea[i] && d.lakeOf[i] < 0 && d.rank[i] >= 0 && d.outflow[i] >= cfg.channelMinQ;
		oc::vector<uint8> donors(NP, 0);
		for (size_t i = 0; i < NP; i++)
			if (inC[i] && d.receiver[i] >= 0 && inC[d.receiver[i]] && donors[d.receiver[i]] < 255)
				donors[d.receiver[i]]++;
		oc::vector<int32> starts;
		oc::vector<uint8> isStart(NP, 0);
		for (size_t i = 0; i < NP; i++)
			if (inC[i] && (donors[i] != 1 || inlets.count((int32)i)))
			{
				isStart[i] = 1;
				starts.push_back((int32)i);
			}
		// UPSTREAM first (the flood pops downstream first, so the highest rank is the farthest up): every segment that
		// ends on a start is profiled before the segment that starts there.
		oc::sort(starts.begin(), starts.end(), [&d](int32 a, int32 b) { return d.rank[a] > d.rank[b]; });

		oc::unordered_map<int32, float> arriving; // a start pixel -> the lowest water level the segments ending on it bring
		oc::vector<int32> chain;
		oc::vector<float> wUp, qChain;
		oc::vector<ChainPoint> pts, simplified;
		for (const int32 s : starts)
		{
			if (cancelled())
				return nullptr;
			chain.clear();
			chain.push_back(s);
			ERiverEnd end = ERiverEnd::Dry;
			int32 c = s;
			for (;;)
			{
				const int32 r = d.receiver[c];
				if (r < 0)
				{
					end = outlets.count(c) ? ERiverEnd::Outlet : softSeeds.count(c) ? ERiverEnd::Edge : ERiverEnd::Sink;
					break;
				}
				if (g.sea[r])
				{
					chain.push_back(r);
					end = ERiverEnd::Sea;
					break;
				}
				if (d.lakeOf[r] >= 0)
				{
					chain.push_back(r);
					end = ERiverEnd::Lake;
					break;
				}
				if (!inC[r])
				{
					end = ERiverEnd::Dry;
					break;
				}
				chain.push_back(r);
				if (isStart[r])
				{
					end = ERiverEnd::Junction;
					break;
				}
				c = r;
			}

			// The profile, pure BREACH semantics, profiled upstream first: the water starts at the lowest of its own
			// bed + depth, the water its tributaries bring and (an inlet) its crossing's level, then follows bed + depth
			// downstream but NEVER rises - a rim in its way is cut through (the carve's gorge), never filled up to. Only
			// the sea (level 0) and a lake (its level) floor it. The first version floored every segment at the level
			// its downstream segment STARTED at: in a filled basin that start is the rim, so whole rivers stood at rim
			// height over the basin floor (seen 2026-10-08).
			const size_t n = chain.size();
			const int32 last = chain.back();
			const CrossingEnd* inlet = nullptr;
			if (auto it = inlets.find(s); it != inlets.end())
				inlet = &it->second;
			const CrossingEnd* outlet = nullptr;
			if (end == ERiverEnd::Outlet)
				outlet = &outlets[last];

			// OUTLET AGREEMENT: the downstream unit's inlet carries the coarse link's Q, this side its own fine Q - and
			// width, depth and the valley follow Q, so the bed jumped at the unit edge. The last stretch blends into
			// the crossing's Q, so both sides reach the boundary with the same one. ONLY where the two are close (the fine
			// Q at the outlet at least half the coarse one): blended from far below, a small stream swelled into a big river
			// in 24 px at the unit edge (2026-10-09) - it keeps its own size instead, and the neighbour's inlet grows from
			// it (RiverTerrain's inlet match).
			constexpr float c_crossBlendPx = 24.0f;
			const bool blendQ = outlet && d.outflow[last] >= 0.5f * outlet->q;
			qChain.resize(n);
			for (size_t k = 0; k < n; k++)
			{
				qChain[k] = inC[chain[k]] ? d.outflow[chain[k]] : d.outflow[chain[k - 1]];
				if (blendQ)
				{
					const float t = 1.0f - smoothstep01((float)(n - 1 - k) / c_crossBlendPx);
					qChain[k] += (outlet->q - qChain[k]) * t;
				}
			}
			const auto qAt = [&](size_t k) { return qChain[k]; };
			wUp.resize(n);
			wUp[0] = g.elev[chain[0]] + depthOf(qAt(0));
			if (inlet)
				wUp[0] = oc::min(wUp[0], inlet->water);
			if (auto it = arriving.find(s); it != arriving.end())
				wUp[0] = oc::min(wUp[0], it->second);
			for (size_t k = 1; k < n; k++)
				wUp[k] = oc::min(wUp[k - 1], g.elev[chain[k]] + depthOf(qAt(k)));
			if (outlet)
				wUp[n - 1] = oc::min(wUp[n - 1], outlet->water); // not above the crossing's level (only ever lowers)
			if (end == ERiverEnd::Sea || end == ERiverEnd::Lake)
			{
				const float floor = end == ERiverEnd::Sea ? 0.0f : d.lakes[d.lakeOf[last]].level;
				for (size_t k = 0; k < n; k++)
					wUp[k] = oc::max(wUp[k], floor);
			}
			if (end == ERiverEnd::Junction)
			{
				auto it = arriving.find(last);
				if (it == arriving.end())
					arriving[last] = wUp[n - 1];
				else
					it->second = oc::min(it->second, wUp[n - 1]);
			}

			pts.clear();
			float maxQ = 0.0f;
			// A crossing's segment starts / ends ON the tile boundary (the shared point both units compute), so the two
			// sides meet there instead of a pixel apart, with the crossing's Q.
			if (inlet)
				pts.push_back(ChainPoint{ inlet->edgeX, inlet->edgeZ, wUp[0], inlet->q });
			for (size_t k = 0; k < n; k++)
			{
				const int32 p = chain[k];
				const float q = qAt(k);
				maxQ = oc::max(maxQ, q);
				pts.push_back(ChainPoint{ (float)(p % W), (float)(p / W), wUp[k], q });
			}
			if (outlet)
				pts.push_back(ChainPoint{ outlet->edgeX, outlet->edgeZ, wUp[n - 1], blendQ ? outlet->q : qChain[n - 1] });
			smoothPath(pts, cfg.pathSmoothing);
			{
				// The phase from the segment's start in GLOBAL native pixels: the same river meanders the same way in
				// every build.
				const uint32 gx = (uint32)(uj * W + chain[0] % W), gz = (uint32)(ui * W + chain[0] / W);
				meander(pts, cfg, g.elev, g.mask, W, gx * 0x9E3779B1u ^ gz * 0x85EBCA77u, nr);
			}
			chaikin(pts, 2);
			// THE CROSSING'S DIRECTION: both units put a crossing segment's end on the same boundary point, but each came
			// in at its own angle - the channel kinked there. Both can agree on the EDGE'S NORMAL there, so the stretch
			// within c_alignPx of the crossing is pulled onto the line through the boundary point along it (fully at the
			// point, fading out with the distance): the two sides meet in one direction.
			{
				constexpr float c_alignPx = 16.0f;
				const auto align = [&](bool atStart, int32 borderPixel, float edgeX, float edgeZ)
				{
					const glm::vec2 boundary(edgeX, edgeZ);
					// Inward: from the boundary point (half a pixel outside) to the border pixel's centre.
					const glm::vec2 inward = glm::normalize(glm::vec2((float)(borderPixel % W), (float)(borderPixel / W)) - boundary);
					const size_t n = pts.size();
					for (size_t i = 0; i < n; i++)
					{
						ChainPoint& p = pts[atStart ? i : n - 1 - i];
						const glm::vec2 rel = glm::vec2(p.x, p.z) - boundary;
						const float d = glm::length(rel);
						if (d >= c_alignPx)
							break;
						const glm::vec2 onLine = boundary + inward * glm::max(glm::dot(rel, inward), 0.0f);
						const float w = 1.0f - smoothstep01(d / c_alignPx);
						p.x += (onLine.x - p.x) * w;
						p.z += (onLine.y - p.z) * w;
					}
				};
				if (inlet)
					align(true, chain.front(), inlet->edgeX, inlet->edgeZ);
				if (outlet)
					align(false, last, outlet->edgeX, outlet->edgeZ);
			}
			clampToGround(pts, cfg, g.elev, g.mask, W);
			simplify(pts, 0.1f, 0.25f, simplified); // 0.1 px in plan, 0.25 model m in W

			RiverSegment seg;
			seg.first = (uint32)unit->points.size();
			seg.count = (uint32)simplified.size();
			seg.end = end;
			seg.ephemeral = maxQ < cfg.perennialQ ? 1 : 0;
			for (size_t k = 0; k < simplified.size(); k++)
			{
				const ChainPoint& cp = simplified[k];
				RiverPoint rp;
				rp.x = cp.x;
				rp.z = cp.z;
				rp.water = cp.w;
				rp.q = cp.q;
				rp.halfWidth = halfWidthOf(cp.q);
				rp.depth = depthOf(cp.q);
				if (k + 1 < simplified.size())
				{
					const ChainPoint& nx = simplified[k + 1];
					const float dist = std::sqrt((nx.x - cp.x) * (nx.x - cp.x) + (nx.z - cp.z) * (nx.z - cp.z)) * nr;
					const float slope = dist > 1e-3f ? (cp.w - nx.w) / dist : 0.0f;
					if (slope >= cfg.fallSlope)
						rp.flags |= RiverPoint_Fall;
					else if (slope >= cfg.rapidsSlope)
						rp.flags |= RiverPoint_Rapids;
				}
				unit->points.push_back(rp);
			}
			unit->segments.push_back(seg);
		}

		// --- Lakes: the wet pixels (a pan's salt) as row runs, the lakes renumbered to the ones that have any.
		oc::vector<int32> lakeIndex(d.lakes.size(), -1);
		for (int32 r = 0; r < W; r++)
		{
			int32 c = 0;
			while (c < W)
			{
				const size_t i = (size_t)r * W + c;
				const ERiverWater w = d.water[i];
				const int32 lake = d.lakeOf[i];
				if (lake < 0 || (w != ERiverWater::Lake && w != ERiverWater::TerminalLake && w != ERiverWater::Pan))
				{
					c++;
					continue;
				}
				int32 e = c + 1;
				while (e < W && d.lakeOf[(size_t)r * W + e] == lake && d.water[(size_t)r * W + e] == w)
					e++;
				if (lakeIndex[lake] < 0)
				{
					lakeIndex[lake] = (int32)unit->lakes.size();
					RiverLake l;
					l.level = d.lakes[lake].level;
					l.kind = d.lakes[lake].kind;
					unit->lakes.push_back(l);
				}
				RiverLakeRun run;
				run.row = (uint16)r;
				run.x0 = (uint16)c;
				run.len = (uint16)(e - c);
				run.lake = (uint16)lakeIndex[lake];
				unit->lakeRuns.push_back(run);
				c = e;
			}
		}

		if (cacheable && !cancelled())
			saveUnit(path, hash, *unit);
		return unit;
	}
}
