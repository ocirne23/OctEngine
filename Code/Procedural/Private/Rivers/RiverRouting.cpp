module Procedural;

import Core;

import :GeneratorV3;
import :RiverRouting;

namespace
{
	using namespace Procedural;

	// D4 first (+x, +z, -x, -z: ECoarseDir order), then the diagonals.
	constexpr int32 c_dx[8] = { 1, 0, -1, 0, 1, -1, -1, 1 };
	constexpr int32 c_dz[8] = { 0, 1, 0, -1, 1, 1, -1, -1 };

	struct FloodNode
	{
		float h;
		uint32 seq; // insertion order: equal heights pop first-in first-out, so the order is deterministic
		int32 idx;
	};
	struct FloodGreater
	{
		bool operator()(const FloodNode& a, const FloodNode& b) const { return a.h != b.h ? a.h > b.h : a.seq > b.seq; }
	};

	struct Depression
	{
		float level = 0.0f;
		float maxDepth = 0.0f;
		oc::vector<int32> cells;
		int32 lake = -1;     // index into DrainageResult::lakes, -1 = cut through
		int32 exitsLeft = 0; // basin cells that drain out of it, not yet accumulated
		double inflow = 0.0;
		double net = 0.0;
		float areaIn = 0.0f;
	};

	// Annual runoff, mm/yr: the rain minus the Budyko (Fu) evaporation. phi = PET / P is the aridity;
	// ET / P = 1 + phi - (1 + phi^w)^(1/w) runs from 0 (phi = 0: all rain runs off) to 1 (phi large: all evaporates).
	float budykoRunoff(float precip, float pet, float w)
	{
		if (precip <= 1.0f)
			return 0.0f;
		const float phi = pet / precip;
		const float etRatio = 1.0f + phi - std::pow(1.0f + std::pow(phi, w), 1.0f / w);
		return precip * oc::clamp(1.0f - etRatio, 0.0f, 1.0f);
	}
}

namespace Procedural
{
	void riverPixelClimate(const TerrainConfigV3& gen, float petPerC, float budykoW, float lakeEvap,
	                       float elev, float tempSea, float precip, float mmToQ,
	                       float& outRunoff, float& outLakeNet, float& outAridity)
	{
		// The generator's own shaping, so the rivers follow the climate the biomes show.
		const float temp = tempSea + gen.temperatureOffset + oc::min(gen.lapseRate, 0.0f) * oc::max(0.0f, elev);
		const float p = oc::max(0.0f, precip + gen.humidityOffset * gen.precipForFullHumidity);
		const float pet = petPerC * oc::max(0.0f, temp + 5.0f);
		outRunoff = budykoRunoff(p, pet, oc::max(budykoW, 1.01f)) * mmToQ;
		outLakeNet = (p - lakeEvap * pet) * mmToQ;
		outAridity = p > 1.0f ? pet / p : 1000.0f;
	}

	void markSea(DrainageGrid& g)
	{
		const int32 W = g.w, H = g.h;
		const size_t N = (size_t)W * H;
		const bool masked = !g.mask.empty();
		g.sea.assign(N, 0);
		oc::vector<int32> stack;
		for (size_t i = 0; i < N; i++)
			if ((!masked || g.mask[i]) && g.elev[i] < -g.seaDepth)
			{
				g.sea[i] = 1;
				stack.push_back((int32)i);
			}
		while (!stack.empty())
		{
			const int32 c = stack.back();
			stack.pop_back();
			const int32 x = c % W, z = c / W;
			for (int32 d = 0; d < 4; d++)
			{
				const int32 nx = x + c_dx[d], nz = z + c_dz[d];
				if (nx < 0 || nz < 0 || nx >= W || nz >= H)
					continue;
				const int32 n = nz * W + nx;
				if (!g.sea[n] && (!masked || g.mask[n]) && g.elev[n] < 0.0f)
				{
					g.sea[n] = 1;
					stack.push_back(n);
				}
			}
		}
	}

	void routeDrainage(const DrainageGrid& g, oc::span<const DrainageSeed> seeds, DrainageResult& out)
	{
		const int32 W = g.w, H = g.h;
		const size_t N = (size_t)W * H;
		const int32 nDirs = g.d8 ? 8 : 4;
		const bool masked = !g.mask.empty();
		const auto inGrid = [&](int32 i) { return !masked || g.mask[i] != 0; };
		const oc::vector<uint8>& sea = g.sea;

		// --- Priority-flood (Barnes 2014, with the pit queue). A pixel reached at or below its receiver's filled
		// height is raised to it (a depression or a flat) and goes through the FIFO pit queue, so flats drain
		// breadth-first toward their outlet. Seeds: the coast, then the caller's.
		out.filled.assign(N, 0.0f);
		out.receiver.assign(N, -1);
		out.rank.assign(N, -1);
		out.root.assign(N, -1);
		out.order.clear();
		out.order.reserve(N);
		oc::vector<uint8> visited(N, 0);
		oc::vector<uint8> softSeed(N, 0);
		oc::priority_queue<FloodNode, oc::vector<FloodNode>, FloodGreater> open;
		oc::vector<int32> pit;
		size_t pitHead = 0;
		uint32 seq = 0;
		// The filled height is the pixel's own (the water spills over the real ground); the priority only orders it.
		const auto seed = [&](int32 i, float priority)
		{
			if (visited[i])
				return;
			visited[i] = 1;
			out.filled[i] = g.elev[i];
			out.root[i] = i;
			open.push(FloodNode{ priority, seq++, i });
		};
		for (int32 z = 0; z < H; z++)
			for (int32 x = 0; x < W; x++)
			{
				const int32 i = z * W + x;
				if (!sea[i])
					continue;
				visited[i] = 1;
				out.filled[i] = g.elev[i];
				out.root[i] = i;
				bool coast = false;
				for (int32 d = 0; d < nDirs && !coast; d++)
				{
					const int32 nx = x + c_dx[d], nz = z + c_dz[d];
					coast = nx >= 0 && nz >= 0 && nx < W && nz < H && inGrid(nz * W + nx) && !sea[nz * W + nx];
				}
				if (coast)
					open.push(FloodNode{ g.elev[i], seq++, i });
			}
		for (const DrainageSeed& s : seeds)
			if (inGrid(s.idx) && !visited[s.idx])
			{
				softSeed[s.idx] = s.soft;
				seed(s.idx, s.priority);
			}

		// A region no seed reaches drains to its own lowest pixel: walk the unvisited pixels lowest first.
		oc::vector<int32> fallback;
		size_t fallbackHead = 0;
		bool fallbackBuilt = false;
		for (;;)
		{
			while (pitHead < pit.size() || !open.empty())
			{
				int32 c;
				if (pitHead < pit.size())
					c = pit[pitHead++];
				else
				{
					c = open.top().idx;
					open.pop();
				}
				out.rank[c] = (int32)out.order.size();
				out.order.push_back(c);
				const int32 x = c % W, z = c / W;
				for (int32 d = 0; d < nDirs; d++)
				{
					const int32 nx = x + c_dx[d], nz = z + c_dz[d];
					if (nx < 0 || nz < 0 || nx >= W || nz >= H)
						continue;
					const int32 n = nz * W + nx;
					if (visited[n] || !inGrid(n))
						continue;
					visited[n] = 1;
					out.receiver[n] = c;
					out.root[n] = out.root[c];
					if (g.elev[n] <= out.filled[c])
					{
						out.filled[n] = out.filled[c];
						pit.push_back(n);
					}
					else
					{
						out.filled[n] = g.elev[n];
						open.push(FloodNode{ g.elev[n], seq++, n });
					}
				}
			}
			if (!fallbackBuilt)
			{
				fallbackBuilt = true;
				for (size_t i = 0; i < N; i++)
					if (!visited[i] && inGrid((int32)i))
						fallback.push_back((int32)i);
				oc::sort(fallback.begin(), fallback.end(), [&g](int32 a, int32 b)
				{
					return g.elev[a] != g.elev[b] ? g.elev[a] < g.elev[b] : a < b;
				});
			}
			while (fallbackHead < fallback.size() && visited[fallback[fallbackHead]])
				fallbackHead++;
			if (fallbackHead >= fallback.size())
				break;
			const int32 f = fallback[fallbackHead++];
			seed(f, g.elev[f]);
		}

		// --- Depressions: connected raised pixels at one spill level. Deep and big enough = a lake; the rest are cut
		// through (the water passes, no lake).
		constexpr float c_raisedEps = 1e-3f;
		const auto raised = [&](int32 i) { return !sea[i] && out.rank[i] >= 0 && out.filled[i] - g.elev[i] > c_raisedEps; };
		oc::vector<Depression> deps;
		oc::vector<int32> depOf(N, -1);
		oc::vector<int32> stack;
		for (size_t i = 0; i < N; i++)
		{
			if (depOf[i] >= 0 || !raised((int32)i))
				continue;
			const int32 id = (int32)deps.size();
			deps.emplace_back();
			Depression& dep = deps.back();
			dep.level = out.filled[i];
			depOf[i] = id;
			stack.push_back((int32)i);
			while (!stack.empty())
			{
				const int32 c = stack.back();
				stack.pop_back();
				dep.cells.push_back(c);
				dep.maxDepth = oc::max(dep.maxDepth, out.filled[c] - g.elev[c]);
				const int32 x = c % W, z = c / W;
				for (int32 d = 0; d < nDirs; d++)
				{
					const int32 nx = x + c_dx[d], nz = z + c_dz[d];
					if (nx < 0 || nz < 0 || nx >= W || nz >= H)
						continue;
					const int32 n = nz * W + nx;
					if (depOf[n] >= 0 || !raised(n) || std::abs(out.filled[n] - dep.level) > c_raisedEps)
						continue;
					depOf[n] = id;
					stack.push_back(n);
				}
			}
		}
		out.lakes.clear();
		out.lakeOf.assign(N, -1);
		for (Depression& dep : deps)
		{
			if (!(dep.maxDepth > g.breachDepth && (int32)dep.cells.size() >= oc::max(g.lakeMinCells, 1)))
				continue;
			// Spilling through a soft wall: the grid's edge cut the basin off, so its level is not the ground's.
			if (softSeed[out.root[dep.cells.front()]])
				continue;
			dep.lake = (int32)out.lakes.size();
			DrainageLake lake;
			lake.spill = dep.level;
			lake.level = dep.level;
			out.lakes.push_back(lake);
			for (const int32 c : dep.cells)
				out.lakeOf[c] = dep.lake;
		}
		oc::vector<int32> depOfLake(out.lakes.size(), -1);
		for (int32 id = 0; id < (int32)deps.size(); id++)
		{
			Depression& dep = deps[id];
			if (dep.lake < 0)
				continue;
			depOfLake[dep.lake] = id;
			for (const int32 c : dep.cells)
			{
				dep.net += (double)g.lakeNet[c];
				if (out.lakeOf[out.receiver[c]] != dep.lake) // a raised pixel always has a receiver
					dep.exitsLeft++;
			}
		}

		// --- Accumulation, upstream first. A lake gathers everything that reaches it and releases at its exits once
		// the last one is in: what its own rain and evaporation leave of the inflow, or nothing (terminal).
		out.water.assign(N, ERiverWater::Land);
		out.outflow.assign(N, 0.0f);
		out.area.assign(N, 0.0f);
		oc::vector<float> inflow(N, 0.0f);
		if (!g.inject.empty())
			for (size_t i = 0; i < N; i++)
				inflow[i] = g.inject[i];
		for (size_t i = 0; i < N; i++)
			if (sea[i])
				out.water[i] = ERiverWater::Sea;
		oc::vector<int32> sorted;
		for (size_t k = out.order.size(); k-- > 0;)
		{
			const int32 c = out.order[k];
			if (sea[c])
				continue;
			const int32 lake = out.lakeOf[c];
			out.area[c] += 1.0f;
			float flow;
			if (lake >= 0)
				flow = inflow[c]; // the basin's own rain is in its balance
			else
			{
				flow = inflow[c] + g.runoff[c];
				const float dry = g.aridity[c] - 1.0f;
				if (dry > 0.0f && flow > 0.0f)
					flow = oc::max(0.0f, flow - g.loss * dry * g.cellKm * std::sqrt(flow));
			}
			out.outflow[c] = flow;
			const int32 r = out.receiver[c];
			if (r < 0)
				continue; // a seed: off the grid
			if (lake < 0 || out.lakeOf[r] == lake)
			{
				inflow[r] += flow;
				out.area[r] += out.area[c];
				continue;
			}

			Depression& dep = deps[depOfLake[lake]];
			DrainageLake& dl = out.lakes[lake];
			dep.inflow += (double)flow;
			dep.areaIn += out.area[c];
			if (--dep.exitsLeft > 0)
				continue;
			const double total = dep.inflow + dep.net;
			if (total >= 0.0)
			{
				// Full: spills the rest through this exit.
				dl.kind = ERiverWater::Lake;
				for (const int32 lc : dep.cells)
					out.water[lc] = ERiverWater::Lake;
				out.outflow[c] = (float)total;
				inflow[r] += (float)total;
				out.area[r] += dep.areaIn;
				continue;
			}
			// Terminal: the open water shrinks until its evaporation takes the whole inflow. The wet cells are the
			// lowest ones (the hypsometry of the basin); under one cell of water the floor is a salt pan.
			out.outflow[c] = 0.0f;
			sorted = dep.cells;
			oc::sort(sorted.begin(), sorted.end(), [&g](int32 a, int32 b)
			{
				return g.elev[a] != g.elev[b] ? g.elev[a] < g.elev[b] : a < b;
			});
			const double deficitPerCell = -dep.net / (double)dep.cells.size(); // > 0 here
			const double wetCells = dep.inflow / oc::max(deficitPerCell, 1e-9);
			const size_t wet = (size_t)oc::min(wetCells, (double)sorted.size());
			if (wet == 0)
			{
				dl.kind = ERiverWater::Pan;
				dl.level = g.elev[sorted[0]];
				out.water[sorted[0]] = ERiverWater::Pan;
				continue;
			}
			dl.kind = ERiverWater::TerminalLake;
			dl.level = wet < sorted.size() ? 0.5f * (g.elev[sorted[wet - 1]] + g.elev[sorted[wet]]) : dep.level;
			for (size_t s = 0; s < wet; s++)
				out.water[sorted[s]] = ERiverWater::TerminalLake;
		}
	}
}
