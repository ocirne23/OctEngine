module Procedural;

import Core;

import :GeneratorV3;
import :RiverRouting;
import :RiverNetwork;

namespace
{
	uint64 tileKey(int32 ti, int32 tj)
	{
		return ((uint64)(uint32)ti << 32) | (uint64)(uint32)tj;
	}
}

namespace Procedural
{
	CoarseRiverNetwork::CoarseRiverNetwork(oc::shared_ptr<const TerrainGenV3> generator, const RiverConfig& cfg)
		: m_generator(oc::move(generator))
		, m_cfg(cfg)
	{
	}

	oc::shared_ptr<const CoarseFieldPlanes> CoarseRiverNetwork::planes(int32 ti, int32 tj) const
	{
		const uint64 key = tileKey(ti, tj);
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_planes.find(key);
			if (it != m_planes.end())
				return it->second;
		}
		auto p = oc::make_shared<CoarseFieldPlanes>();
		if (!m_generator->fetchCoarseTilePlanes(ti, tj, *p))
			return nullptr;
		std::lock_guard<std::mutex> lk(m_mutex);
		m_planes[key] = p;
		return p;
	}

	oc::shared_ptr<const CoarseRiverTile> CoarseRiverNetwork::tile(int32 ti, int32 tj) const
	{
		const uint64 key = tileKey(ti, tj);
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_tiles.find(key);
			if (it != m_tiles.end())
				return it->second;
		}
		oc::shared_ptr<const CoarseRiverTile> t = compute(ti, tj);
		if (!t)
			return nullptr;
		std::lock_guard<std::mutex> lk(m_mutex);
		m_tiles[key] = t;
		return t;
	}

	oc::shared_ptr<const CoarseRiverTile> CoarseRiverNetwork::compute(int32 ti, int32 tj) const
	{
		const int32 S = TerrainGenV3::coarseTilePixels();
		const int32 R = oc::clamp(m_cfg.coarseDomain, 0, 4);
		const int32 T = 2 * R + 1;
		const int32 W = T * S;
		const size_t N = (size_t)W * W;

		const float cellM = TerrainGenV3::nativeResolution() * (float)TerrainGenV3::nativePerCoarsePixel(); // model m
		const float mmToQ = cellM * cellM * 0.001f / 3.156e7f; // mm/yr over one coarse pixel -> m3/s

		DrainageGrid g;
		g.w = W;
		g.h = W;
		g.d8 = false;
		g.elev.resize(N);
		g.runoff.resize(N);
		g.lakeNet.resize(N);
		g.aridity.resize(N);
		g.cellKm = cellM * 0.001f;
		g.loss = m_cfg.loss;
		g.seaDepth = m_cfg.seaDepth;
		g.breachDepth = m_cfg.breachDepth;
		g.lakeMinCells = m_cfg.lakeMinCells;
		for (int32 dt = 0; dt < T; dt++)
			for (int32 dj = 0; dj < T; dj++)
			{
				const oc::shared_ptr<const CoarseFieldPlanes> p = planes(ti - R + dt, tj - R + dj);
				if (!p)
					return nullptr;
				for (int32 r = 0; r < S; r++)
					for (int32 c = 0; c < S; c++)
					{
						const size_t src = (size_t)r * S + c;
						const size_t idx = (size_t)(dt * S + r) * W + (size_t)(dj * S + c);
						g.elev[idx] = p->elev[src];
						riverPixelClimate(m_generator->config(), m_cfg.petPerC, m_cfg.budykoW, m_cfg.lakeEvap,
							p->elev[src], p->tempSea[src], p->precip[src], mmToQ, g.runoff[idx], g.lakeNet[idx], g.aridity[idx]);
					}
			}
		markSea(g);

		// The domain edge drains off it: water that reaches the edge leaves.
		oc::vector<DrainageSeed> seeds;
		for (int32 z = 0; z < W; z++)
			for (int32 x = 0; x < W; x++)
			{
				const int32 i = z * W + x;
				if ((x == 0 || z == 0 || x == W - 1 || z == W - 1) && !g.sea[i])
					seeds.push_back(DrainageSeed{ i, g.elev[i], 0 });
			}
		DrainageResult d;
		routeDrainage(g, seeds, d);

		// --- The tile's own pixels.
		auto t = oc::make_shared<CoarseRiverTile>();
		t->ti = ti;
		t->tj = tj;
		t->size = S;
		const size_t n = (size_t)S * S;
		t->dir.resize(n);
		t->q.resize(n);
		t->runoff.resize(n);
		t->area.resize(n);
		t->filled.resize(n);
		t->water.resize(n);
		t->level.resize(n);
		for (int32 r = 0; r < S; r++)
			for (int32 c = 0; c < S; c++)
			{
				const int32 x = R * S + c, z = R * S + r;
				const int32 i = z * W + x;
				const size_t dst = (size_t)r * S + c;
				const bool sea = g.sea[i] != 0;
				const ERiverWater water = d.water[i];
				ECoarseDir dir = ECoarseDir::None;
				const int32 rcv = d.receiver[i];
				if (!sea && rcv >= 0 && water != ERiverWater::TerminalLake && water != ERiverWater::Pan)
				{
					const int32 rx = rcv % W, rz = rcv / W;
					dir = rx > x ? ECoarseDir::PosX : rx < x ? ECoarseDir::NegX : rz > z ? ECoarseDir::PosZ : ECoarseDir::NegZ;
				}
				const int32 lake = d.lakeOf[i];
				t->dir[dst] = dir;
				t->q[dst] = sea ? 0.0f : d.outflow[i];
				t->runoff[dst] = (sea || lake >= 0) ? 0.0f : g.runoff[i];
				t->area[dst] = d.area[i];
				t->filled[dst] = d.filled[i];
				t->water[dst] = water;
				t->level[dst] = lake >= 0 ? d.lakes[lake].level : 0.0f;
			}
		return t;
	}
}
