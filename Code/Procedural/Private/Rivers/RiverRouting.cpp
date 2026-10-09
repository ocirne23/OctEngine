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

	// THE SIZE LIMIT / EDGE CAP on one depression (`id` in depOf): the level goes between its lowest `keep` pixels and the
	// next (never over maxLevel); of the pools under it the LARGEST is the lake (the others stay dry hollows - the one around the lowest pixel
	// was a thin strip in the outflow's gorge, 2026-10-09). Then the basin's DRAIN TREE is rebuilt: the flood's tree ran from the
	// spill through the whole basin, so its paths left the smaller lake over a dry stretch and came back into it, or ran
	// from a pool into the lake - rivers that started and ended in the same lake (2026-10-09). Now ONE path leaves: the
	// old one from the lowest pixel, from where it last leaves the lake; every other basin pixel drains into the lake or
	// onto that path (a priority flood from the path, lowest first, so the whole lake fills from its exit before any dry
	// pixel). The basin keeps its rank slots, reassigned in the new tree's order (a receiver always ranks lower).
	void capDepression(const DrainageGrid& g, Depression& dep, int32 id, const oc::vector<int32>& depOf, DrainageResult& out,
	                   size_t keep, float maxLevel)
	{
		const int32 W = g.w, H = g.h;
		const int32 nDirs = g.d8 ? 8 : 4;
		if (keep >= dep.cells.size())
		{
			dep.level = oc::min(dep.level, maxLevel); // every pixel stays under: only the level drops
			return;
		}
		oc::vector<int32> region = dep.cells;
		oc::sort(region.begin(), region.end(), [&g](int32 a, int32 b)
		{
			return g.elev[a] != g.elev[b] ? g.elev[a] < g.elev[b] : a < b;
		});
		dep.level = oc::min(0.5f * (g.elev[region[keep - 1]] + g.elev[region[keep]]), maxLevel);

		// 0 = above the level, 1 = under it, 2 = the lake, 3 = placed in the new tree. The pools under the level are
		// labelled (`pool`, from 4 up); the LARGEST is the lake.
		oc::unordered_map<int32, uint8> state;
		oc::unordered_map<int32, int32> pool;
		state.reserve(region.size());
		pool.reserve(keep);
		for (size_t k = 0; k < region.size(); k++)
			state[region[k]] = k < keep ? 1 : 0;
		oc::vector<int32> stack;
		int32 bestPool = -1, bestLowest = -1;
		size_t bestSize = 0;
		for (size_t k = 0; k < keep; k++)
		{
			if (pool.count(region[k]))
				continue;
			// region is sorted lowest first: region[k] is this pool's lowest pixel.
			const int32 label = (int32)k;
			size_t size = 0;
			stack.push_back(region[k]);
			pool[region[k]] = label;
			while (!stack.empty())
			{
				const int32 c = stack.back();
				stack.pop_back();
				size++;
				const int32 x = c % W, z = c / W;
				for (int32 d = 0; d < nDirs; d++)
				{
					const int32 nx = x + c_dx[d], nz = z + c_dz[d];
					if (nx < 0 || nz < 0 || nx >= W || nz >= H)
						continue;
					const int32 n = nz * W + nx;
					const auto it = state.find(n);
					if (it != state.end() && it->second == 1 && !pool.count(n))
					{
						pool[n] = label;
						stack.push_back(n);
					}
				}
			}
			if (size > bestSize)
			{
				bestSize = size;
				bestPool = label;
				bestLowest = region[k];
			}
		}
		dep.cells.clear();
		for (size_t k = 0; k < keep; k++)
			if (pool[region[k]] == bestPool)
			{
				state[region[k]] = 2;
				dep.cells.push_back(region[k]);
			}

		// The way out: the old path from the lake's lowest pixel, from where it last leaves the lake.
		oc::vector<int32> path;
		for (int32 c = bestLowest; c >= 0 && depOf[c] == id; c = out.receiver[c])
			path.push_back(c);
		size_t lastWet = 0;
		for (size_t k = 0; k < path.size(); k++)
			if (state[path[k]] == 2)
				lastWet = k;
		path.erase(path.begin(), path.begin() + (ptrdiff_t)lastWet);

		oc::vector<int32> slots;
		slots.reserve(region.size());
		for (const int32 c : region)
			slots.push_back(out.rank[c]);
		oc::sort(slots.begin(), slots.end());
		oc::vector<int32> newOrder;
		newOrder.reserve(region.size());
		oc::priority_queue<FloodNode, oc::vector<FloodNode>, FloodGreater> open;
		uint32 seq = 0;
		for (size_t k = path.size(); k-- > 0;)
		{
			newOrder.push_back(path[k]); // the rim end first: down the path the ranks fall
			state[path[k]] = 3;
			open.push(FloodNode{ g.elev[path[k]], seq++, path[k] });
		}
		while (!open.empty())
		{
			const int32 c = open.top().idx;
			open.pop();
			const int32 x = c % W, z = c / W;
			for (int32 d = 0; d < nDirs; d++)
			{
				const int32 nx = x + c_dx[d], nz = z + c_dz[d];
				if (nx < 0 || nz < 0 || nx >= W || nz >= H)
					continue;
				const auto it = state.find(nz * W + nx);
				if (it == state.end() || it->second == 3)
					continue;
				it->second = 3;
				out.receiver[it->first] = c;
				newOrder.push_back(it->first);
				open.push(FloodNode{ g.elev[it->first], seq++, it->first });
			}
		}
		assert(newOrder.size() == region.size());
		for (size_t k = 0; k < newOrder.size() && k < slots.size(); k++)
		{
			out.rank[newOrder[k]] = slots[k];
			out.order[(size_t)slots[k]] = newOrder[k];
		}
	}

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
			// A lake: deeper than the breach depth AND big enough - or, whatever its size, deeper than TWICE the breach
			// depth (a pond). Cut through, a deep small basin left the river a gorge tens of metres under its rim, reaching
			// a unit edge far below the crossing's level: the neighbour floated, or carried the gorge on as a canyon
			// (the user, 2026-10-09).
			const bool deepPond = dep.maxDepth > 2.0f * g.breachDepth;
			if (!(dep.maxDepth > g.breachDepth && ((int32)dep.cells.size() >= oc::max(g.lakeMinCells, 1) || deepPond)))
				continue;
			// Spilling through a soft wall: the grid's edge cut the basin off, so its level is not the ground's.
			if (softSeed[out.root[dep.cells.front()]])
				continue;
			// THE SIZE LIMIT (the user, 2026-10-09: a deep valley basin filled to its spill was a 7 km2 lake): over
			// lakeMaxCells only the lowest that many pixels are the lake, its level between the last of them and the next
			// (a terminal lake's hypsometry); the pixels above it are land again - the water runs over them as channels,
			// and the outflow's profile cuts down through the rim to the lower level (a gorge, at most the basin's depth).
			// THE EDGE CAP (units): the neighbour unit cannot see this lake, so a lake over or beside a grid-edge pixel
			// stood in the air where the neighbour's ground fell away (a lake on a clifftop at a unit edge, beside an
			// inlet where no soft wall holds the border, 2026-10-09). Its level stays 0.1 model m under the lowest such
			// pixel - only the pixels under that stay lake, as with the size limit.
			const int32 id = depOf[dep.cells.front()];
			size_t keep = dep.cells.size();
			float maxLevel = dep.level;
			if (g.edgeHoldsNoLake)
			{
				const auto edgePixel = [&](int32 i)
				{
					const int32 x = i % W, z = i / W;
					if (x == 0 || z == 0 || x == W - 1 || z == H - 1)
						return true;
					for (int32 d = 0; d < nDirs; d++)
					{
						const int32 nx = x + c_dx[d], nz = z + c_dz[d];
						if (!inGrid(nz * W + nx))
							return true;
					}
					return false;
				};
				float wall = FLT_MAX;
				for (const int32 c : dep.cells)
				{
					if (edgePixel(c))
						wall = oc::min(wall, g.elev[c]);
					const int32 x = c % W, z = c / W;
					for (int32 d = 0; d < nDirs; d++)
					{
						const int32 nx = x + c_dx[d], nz = z + c_dz[d];
						if (nx < 0 || nz < 0 || nx >= W || nz >= H)
							continue;
						const int32 n = nz * W + nx;
						if (depOf[n] != id && inGrid(n) && edgePixel(n))
							wall = oc::min(wall, g.elev[n]);
					}
				}
				if (wall - 0.1f < dep.level)
				{
					maxLevel = wall - 0.1f;
					keep = 0;
					for (const int32 c : dep.cells)
						if (g.elev[c] < maxLevel)
							keep++;
				}
			}
			if (g.lakeMaxCells > 0)
				keep = oc::min(keep, (size_t)g.lakeMaxCells);
			if (keep == 0)
				continue;
			if (keep < dep.cells.size() || maxLevel < dep.level)
				capDepression(g, dep, id, depOf, out, keep, maxLevel);
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
