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
	constexpr uint32 RIVER_UNIT_VERSION = 2; // 2: soft unit walls, ERiverEnd::Edge

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
		hashValue(h, uc.perennialQ);
		hashValue(h, uc.widthA);
		hashValue(h, uc.depthC);
		hashValue(h, uc.rapidsSlope);
		hashValue(h, uc.fallSlope);
		hashValue(h, uc.edgeWall);
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

	// Douglas-Peucker on the plan view: drops the points a straight reach does not need.
	void simplify(const oc::vector<ChainPoint>& in, float tolerancePx, oc::vector<ChainPoint>& out)
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
		const float tol2 = tolerancePx * tolerancePx;
		while (!stack.empty())
		{
			const size_t a = stack.back().first, b = stack.back().second;
			stack.pop_back();
			const float dx = in[b].x - in[a].x, dz = in[b].z - in[a].z;
			const float len2 = dx * dx + dz * dz;
			float worst = -1.0f;
			size_t worstAt = a;
			for (size_t i = a + 1; i < b; i++)
			{
				const float px = in[i].x - in[a].x, pz = in[i].z - in[a].z;
				float d2;
				if (len2 > 1e-12f)
				{
					const float cr = px * dz - pz * dx;
					d2 = cr * cr / len2;
				}
				else
					d2 = px * px + pz * pz;
				if (d2 > worst)
				{
					worst = d2;
					worstAt = i;
				}
			}
			if (worst > tol2)
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

		const auto depthOf = [&cfg](float q) { return cfg.depthC * std::pow(oc::max(q, 0.0f), 0.4f); };
		const auto halfWidthOf = [&cfg](float q) { return 0.5f * cfg.widthA * std::sqrt(oc::max(q, 0.0f)); };

		// --- Crossings: every coarse link across the unit's edge (or into one of its missing tiles).
		oc::vector<DrainageSeed> seeds;
		oc::unordered_map<int32, float> outletWater, inletWater;
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
					if (outlet)
					{
						seeds.push_back(DrainageSeed{ idx, g.elev[idx], 0 });
						outletWater[idx] = x.water;
					}
					else
					{
						g.inject[idx] += q;
						inletWater[idx] = x.water;
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
				if (!g.mask[i] || g.sea[i] || hardSeeds.count(i) || inletWater.count(i))
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
			if (inC[i] && (donors[i] != 1 || inletWater.count((int32)i)))
			{
				isStart[i] = 1;
				starts.push_back((int32)i);
			}
		// Downstream first: a junction's lower segment is profiled before the segments that end on it.
		oc::sort(starts.begin(), starts.end(), [&d](int32 a, int32 b) { return d.rank[a] < d.rank[b]; });

		oc::unordered_map<int32, float> startWater; // segment start pixel -> its water level
		oc::vector<int32> chain;
		oc::vector<float> wUp;
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
					end = outletWater.count(c) ? ERiverEnd::Outlet : softSeeds.count(c) ? ERiverEnd::Edge : ERiverEnd::Sink;
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

			// The profile: walking up from the pinned end, the water never drops below bed + depth and never rises
			// downstream. An inlet then pins the start at the crossing's level.
			const size_t n = chain.size();
			const auto qAt = [&](size_t k) { return inC[chain[k]] ? d.outflow[chain[k]] : d.outflow[chain[k - 1]]; };
			const int32 last = chain.back();
			float endW = 0.0f;
			switch (end)
			{
			case ERiverEnd::Junction:
			{
				auto it = startWater.find(last);
				endW = it != startWater.end() ? it->second : g.elev[last] + depthOf(qAt(n - 1));
				break;
			}
			case ERiverEnd::Outlet: endW = outletWater[last]; break;
			case ERiverEnd::Sea:    endW = 0.0f; break;
			case ERiverEnd::Lake:   endW = d.lakes[d.lakeOf[last]].level; break;
			default:                endW = g.elev[last] + depthOf(qAt(n - 1)); break;
			}
			wUp.resize(n);
			wUp[n - 1] = endW;
			for (size_t k = n - 1; k-- > 0;)
				wUp[k] = oc::max(wUp[k + 1], g.elev[chain[k]] + depthOf(qAt(k)));
			auto inletIt = inletWater.find(s);
			if (inletIt != inletWater.end())
			{
				wUp[0] = inletIt->second;
				for (size_t k = 1; k < n; k++)
					wUp[k] = oc::min(wUp[k], wUp[k - 1]);
			}
			startWater[s] = wUp[0];

			pts.clear();
			float maxQ = 0.0f;
			for (size_t k = 0; k < n; k++)
			{
				const int32 p = chain[k];
				const float q = qAt(k);
				maxQ = oc::max(maxQ, q);
				pts.push_back(ChainPoint{ (float)(p % W), (float)(p / W), wUp[k], q });
			}
			chaikin(pts, 2);
			simplify(pts, 0.2f, simplified);

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
