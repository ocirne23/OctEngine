module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Tweaks;
import Core.Log;

import RendererVK;
import File;
import Spatial;

import :Scattering;
import :TerrainSampler;
import :Noise;

namespace
{
	using namespace Procedural;

	// --- Scatter configuration ---------------------------------------------------------------------------
	// THE place to add scatterable content. An asset describes one model (with optional node variants) and
	// how it sits on the ground; a rule places one asset in one CLIMATE, given in real units (mean annual
	// temperature C / annual precipitation mm/yr) with density falling off around it — add as many rules as
	// you like, and reference one asset from any number of them. Function-local statics so the tables build
	// on first use (no global-init order dependency on the engine allocator).
	//
	// The climates below are the ones these rules were tuned against, carried over verbatim from the named
	// biomes they used to reference. They are NOT the terrain textures' climate boxes: those were retuned
	// onto the Whittaker diagram for the diffusion generator's real climate, while these still sit where the
	// old noise generator's attractors were. So trees can disagree with the ground they stand on — worth
	// fixing, but it is a tuning pass with nothing to do with removing the generator.

	oc::span<const ScatterAsset> scatterAssets()
	{
		static const ScatterAsset assets[] =
		{
			{ .name = "tree_small", .ocPath = "ObjectContainers/tree_small_02.oc",
			  .nodes = { "tree_small_02_LOD0" },
			  .footprintRadius = 1.6f, .minScale = 0.75f, .maxScale = 1.5f, .slopeAlign = 0.1f, .sinkDepth = 0.10f },
			{ .name = "boulder", .ocPath = "ObjectContainers/namaqualand_boulder_04.oc",
			  .nodes = { "boulder_04" },
			  .footprintRadius = 1.4f, .minScale = 0.6f, .maxScale = 2.4f, .slopeAlign = 0.6f, .sinkDepth = 0.25f },
			{ .name = "rocks", .ocPath = "ObjectContainers/namaqualand_rocks_01.oc",
			  .nodes = { "namaqualand_rocks_02_a", "namaqualand_rocks_02_b", "namaqualand_rocks_02_c", "namaqualand_rocks_02_d" },
			  .footprintRadius = 0.7f, .minScale = 0.7f, .maxScale = 1.8f, .slopeAlign = 0.8f, .sinkDepth = 0.10f },
			{ .name = "stone", .ocPath = "ObjectContainers/stone_01.oc",
			  .nodes = { "stone_01" },
			  .footprintRadius = 0.35f, .minScale = 0.6f, .maxScale = 1.6f, .slopeAlign = 0.9f, .sinkDepth = 0.04f },
		};
		return assets;
	}

	oc::span<const ScatterRule> scatterRules()
	{
		static const ScatterRule rules[] =
		{
			// Forest belt: clustered woods with mossy ground stones between the trunks.
			{ .temperatureC =  11.0f, .precipMm = 1320.0f, .asset = "tree_small", .density = 90.0f,  .viewDistance = 500.0f, .clusterSize = 60.0f,  .clusterCoverage = 0.65f, .maxSlope = 0.55f },
			{ .temperatureC =  11.0f, .precipMm = 1320.0f, .asset = "stone",      .density = 25.0f,  .viewDistance = 160.0f },
			{ .temperatureC =  26.0f, .precipMm = 2090.0f, .asset = "tree_small", .density = 140.0f, .viewDistance = 500.0f, .clusterSize = 90.0f,  .clusterCoverage = 0.80f, .maxSlope = 0.6f },
			{ .temperatureC =  -1.0f, .precipMm = 1210.0f, .asset = "tree_small", .density = 45.0f,  .viewDistance = 500.0f, .clusterSize = 80.0f,  .clusterCoverage = 0.55f },
			{ .temperatureC =  -1.0f, .precipMm = 1210.0f, .asset = "boulder",    .density = 2.5f,   .viewDistance = 600.0f },
			// Open country: lone trees, stones in the grass.
			{ .temperatureC =   9.0f, .precipMm =  990.0f, .asset = "tree_small", .density = 2.5f,   .viewDistance = 500.0f, .climateWidth = 0.09f },
			{ .temperatureC =   9.0f, .precipMm =  990.0f, .asset = "stone",      .density = 30.0f,  .viewDistance = 160.0f },
			{ .temperatureC =  24.0f, .precipMm =  550.0f, .asset = "tree_small", .density = 6.0f,   .viewDistance = 500.0f, .clusterSize = 120.0f, .clusterCoverage = 0.40f },
			{ .temperatureC =  24.0f, .precipMm =  550.0f, .asset = "rocks",      .density = 12.0f,  .viewDistance = 260.0f },
			// Arid: namaqualand boulder fields.
			{ .temperatureC =  24.0f, .precipMm =  220.0f, .asset = "boulder",    .density = 4.0f,   .viewDistance = 600.0f, .clusterSize = 150.0f, .clusterCoverage = 0.35f },
			{ .temperatureC =  24.0f, .precipMm =  220.0f, .asset = "rocks",      .density = 18.0f,  .viewDistance = 300.0f, .clusterSize = 100.0f, .clusterCoverage = 0.50f },
			// Wet lowland: sparse trees on the flats above the waterline.
			{ .temperatureC =  17.0f, .precipMm = 2090.0f, .asset = "tree_small", .density = 18.0f,  .viewDistance = 400.0f, .maxSlope = 0.4f },
			{ .temperatureC =  12.0f, .precipMm = 2200.0f, .asset = "stone",      .density = 20.0f,  .viewDistance = 160.0f },
			// Cold/high: bare rock.
			{ .temperatureC =  -8.0f, .precipMm =  990.0f, .asset = "rocks",      .density = 20.0f,  .viewDistance = 300.0f },
			{ .temperatureC =  -8.0f, .precipMm =  990.0f, .asset = "boulder",    .density = 2.5f,   .viewDistance = 600.0f },
			{ .temperatureC =   2.0f, .precipMm =  990.0f, .asset = "boulder",    .density = 5.0f,   .viewDistance = 600.0f, .maxSlope = 0.8f },
			{ .temperatureC =   2.0f, .precipMm =  990.0f, .asset = "rocks",      .density = 15.0f,  .viewDistance = 300.0f, .maxSlope = 0.8f },
			{ .temperatureC = -15.0f, .precipMm =  990.0f, .asset = "rocks",      .density = 6.0f,   .viewDistance = 300.0f },
		};
		return rules;
	}

	// ------------------------------------------------------------------------------------------------------

	// Deterministic per-candidate RNG (splitmix64): every random choice a placement makes streams from a
	// hash of (seed, cell, rule, subcell), so a cell regenerates bit-identical wherever/whenever it's asked.
	struct ScatterRng
	{
		uint64 state;
		explicit ScatterRng(uint64 seed) : state(seed) {}
		uint64 next()
		{
			state += 0x9E3779B97F4A7C15ull;
			uint64 z = state;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}
		float next01() { return (float)(next() >> 40) * (1.0f / 16777216.0f); }
		float range(float lo, float hi) { return lo + (hi - lo) * next01(); }
	};

	uint64 candidateSeed(uint32 seed, glm::ivec2 cell, uint32 ruleIdx, uint32 subIdx)
	{
		uint64 h = seed;
		h = h * 0x100000001B3ull ^ (uint32)cell.x;
		h = h * 0x100000001B3ull ^ (uint32)cell.y;
		h = h * 0x100000001B3ull ^ ruleIdx;
		h = h * 0x100000001B3ull ^ subIdx;
		return h;
	}

	// Pack a cell coordinate into a stable map key (and back).
	uint64 cellKey(glm::ivec2 coord)
	{
		return ((uint64)(uint32)coord.x << 32) | (uint64)(uint32)coord.y;
	}
	glm::ivec2 cellCoord(uint64 key)
	{
		return glm::ivec2((int)(uint32)(key >> 32), (int)(uint32)key);
	}

	Sphere mergeSpheres(const Sphere& a, const Sphere& b)
	{
		const glm::vec3 d = b.pos - a.pos;
		const float dist = glm::length(d);
		if (dist + b.radius <= a.radius)
			return a;
		if (dist + a.radius <= b.radius)
			return b;
		const float radius = (dist + a.radius + b.radius) * 0.5f;
		return Sphere(a.pos + d * ((radius - a.radius) / glm::max(dist, 1e-6f)), radius);
	}
}

namespace Procedural
{
	ScatterSystem::~ScatterSystem()
	{
		{
			// starve the pump: it exits once the pool is empty, and nothing re-kicks it
			std::lock_guard<std::mutex> lk(m_mutex);
			m_requests.clear();
		}
		Globals::jobSystem.wait(m_pumpCounter); // main thread helps while waiting
		clearResidents(); // main thread: free RenderNodes while the renderer is still alive
	}

	void ScatterSystem::initialize()
	{
		auto dirty = [this]() { m_configDirty = true; };
		Tweak::boolean("Scatter", "Enabled", &m_enabled);
		Tweak::intVar("Scatter", "Seed", &m_seed, 0, 1000000, 1.0f, dirty);
		Tweak::floatVar("Scatter", "Cell size (m)", &m_cellSize, 16.0f, 256.0f, 1.0f, dirty);
		Tweak::floatVar("Scatter", "Density scale", &m_densityScale, 0.0f, 8.0f, 0.05f, dirty);
		Tweak::intVar("Scatter", "Gen jobs", &m_maxGenJobs, 1, 16, 1.0f);
		Tweak::floatVar("Scatter", "View distance scale", &m_viewScale, 0.1f, 4.0f, 0.05f); // live: spawn range only, no regen
		Tweak::intVar("Scatter", "Spawns/frame", &m_maxSpawnsPerFrame, 32, 8192, 1.0f);

		// Load every asset's container up front, importing through its .oc so the model path and import
		// options (incl. DecimationFactor) are shared with World's containers — both serve one cooked
		// .vsc + converted textures per model. A failed asset drops its rules with a warning.
		const MeshLodParams& lodParams = Globals::rendererVK.getLodParams();
		SceneCookOptions cookOptions;
		cookOptions.generateLods = lodParams.generate;
		cookOptions.lodLevels = lodParams.generateLevels;
		cookOptions.lodReduction = lodParams.generateReduction;
		cookOptions.lodMinIndices = lodParams.minIndices;

		const oc::span<const ScatterAsset> assets = scatterAssets();
		m_assets.resize(assets.size());
		for (size_t i = 0; i < assets.size(); ++i)
		{
			const ScatterAsset& desc = assets[i];
			ObjectContainerDesc ocDesc;
			oc::string ocError;
			if (!loadObjectContainerDesc(desc.ocPath, ocDesc, ocError))
			{
				Log::warning(oc::format("Scatter: failed to load '{}' for asset '{}': {}", desc.ocPath, desc.name, ocError));
				continue;
			}
			cookOptions.decimationFactor = ocDesc.decimationFactor;
			oc::unique_ptr<ISceneData> scene = ISceneData::loadCached(ocDesc.path.c_str(), ocDesc.mergeNodes, ocDesc.preTransformVertices, cookOptions);
			if (!scene)
			{
				Log::warning(oc::format("Scatter: failed to load '{}' for asset '{}'", ocDesc.path, desc.name));
				continue;
			}
			auto container = oc::make_unique<ObjectContainer>();
			if (!container->initialize(*scene))
			{
				Log::warning(oc::format("Scatter: failed to initialize container for asset '{}'", desc.name));
				continue;
			}
			oc::vector<NodeSpawnIdx> variants;
			for (const char* node : desc.nodes)
			{
				const NodeSpawnIdx idx = container->getSpawnIdxForPath(node);
				if (idx == NodeSpawnIdx_INVALID)
					Log::warning(oc::format("Scatter: node '{}' not found in '{}'", node, ocDesc.path));
				else
					variants.push_back(idx);
			}
			if (desc.nodes.empty())
				variants.push_back(NodeSpawnIdx_ROOT);
			if (variants.empty())
			{
				Log::warning(oc::format("Scatter: asset '{}' has no spawnable variants, dropping", desc.name));
				continue;
			}
			m_assets[i].container = oc::move(container);
			m_assets[i].variants = oc::move(variants);
		}

		// Resolve rules against the loaded assets; generation order is footprint-descending so large
		// objects claim ground first and small ones fill the gaps around them.
		const oc::span<const ScatterRule> rules = scatterRules();
		m_rules.resize(rules.size());
		// Constructed at size, not resize()d: vector relocation would instantiate the map's copy ctor
		// (unordered_map's move isn't noexcept), which the move-only RuleGroup value forbids.
		m_ruleGroups = decltype(m_ruleGroups)(rules.size());
		for (size_t i = 0; i < rules.size(); ++i)
		{
			const ScatterRule& rule = rules[i];
			uint16 assetIdx = UINT16_MAX;
			for (size_t a = 0; a < assets.size(); ++a)
				if (strcmp(assets[a].name, rule.asset) == 0) { assetIdx = (uint16)a; break; }
			if (assetIdx == UINT16_MAX)
			{
				Log::warning(oc::format("Scatter: rule {} references unknown asset '{}', dropping", i, rule.asset));
				continue;
			}
			if (!m_assets[assetIdx].container)
				continue; // asset failed to load, already warned
			m_rules[i].assetIdx = assetIdx;
			m_rules[i].numVariants = (uint16)m_assets[assetIdx].variants.size();
			// Real units -> the normalized (t01, h01) space the density falloff and the terrain's texture
			// picking both work in.
			m_rules[i].climateCoords = glm::vec2(temperatureTo01(rule.temperatureC), precipTo01(rule.precipMm));
			m_ruleOrder.push_back((uint16)i);
			m_maxViewDistance = glm::max(m_maxViewDistance, rule.viewDistance);
		}
		oc::sort(m_ruleOrder.begin(), m_ruleOrder.end(), [&](uint16 a, uint16 b)
		{
			return scatterAssets()[m_rules[a].assetIdx].footprintRadius > scatterAssets()[m_rules[b].assetIdx].footprintRadius;
		});

	}

	void ScatterSystem::clearResidents()
	{
		m_residentCells.clear();
		for (auto& groups : m_ruleGroups)
			groups.clear();
		m_pending.clear();
		m_active.clear();
		m_ringDirty = true;
		m_spawnScan = true;
	}

	// Places one cell's instances. Pure function of (maps, coord, params, config tables): candidates come
	// from a per-rule jittered grid sized to the rule's density, then survive a filter chain — climate
	// weight (Gaussian falloff around the biome attractor, cheap first), cluster noise, altitude band
	// above the water level, slope — and finally dart-throw against everything already accepted in the
	// cell (footprint discs; big-footprint rules run first). Overlap is exact within a cell; across cell
	// borders only the jittered grid's spacing separates instances, which in practice keeps footprints
	// from stacking without the cost of neighbor-cell regeneration.
	void ScatterSystem::generateCell(const ITerrainSampler& maps, glm::ivec2 coord, const GenParams& p,
		oc::span<const RuleRuntime> ruleRt, oc::span<const uint16> order, oc::vector<GroupResult>& outGroups)
	{
		const oc::span<const ScatterAsset> assets = scatterAssets();
		const oc::span<const ScatterRule> rules = scatterRules();
		const double cs = p.cellSize;
		const double ox = coord.x * cs, oz = coord.y * cs;
		constexpr float TEMP_RANGE = TEMPERATURE_MAX_C - TEMPERATURE_MIN_C;

		oc::vector<glm::vec3> accepted; // (x, z, footprint radius) of everything placed so far in this cell

		for (uint16 ruleIdx : order)
		{
			const ScatterRule& rule = rules[ruleIdx];
			const ScatterSystem::RuleRuntime& rt = ruleRt[ruleIdx];
			const ScatterAsset& asset = assets[rt.assetIdx];

			const float expected = rule.density * p.densityScale * (float)(cs * cs) * (1.0f / 10000.0f);
			if (expected <= 0.0f)
				continue;
			const int nSub = (int)std::ceil(std::sqrt(expected));
			const float keepProb = expected / (float)(nSub * nSub);
			const double sub = cs / nSub;
			const NoiseField clusterNoise(p.seed * 0x9E3779B9u + (uint32)ruleIdx * 747796405u);
			const float sigma = glm::max(rule.climateWidth, 0.01f);
			const float invS2 = 1.0f / (2.0f * sigma * sigma);

			ScatterSystem::GroupResult group;
			group.ruleIdx = ruleIdx;
			for (int sj = 0; sj < nSub; ++sj)
			{
				for (int si = 0; si < nSub; ++si)
				{
					ScatterRng rng(candidateSeed(p.seed, coord, ruleIdx, (uint32)(sj * nSub + si)));
					if (rng.next01() > keepProb)
						continue;
					const double wx = ox + ((double)si + rng.next01()) * sub;
					const double wz = oz + ((double)sj + rng.next01()) * sub;

					// ONE evaluation for climate AND height: samplePoint returns both from a single tile
					// resolve, where sampleTemperature + sampleHumidity + sampleHeightAndWater is three of
					// them for the same point. Worth it even though the altitude test below rejects some
					// candidates that never needed a height — under V3 the height falls out of the same
					// resolve for free, so the only way to not pay for it is to resolve the tile twice.
					TerrainPoint tp;
					maps.samplePoint(wx, wz, tp);

					// Climate weight: density fades with distance to the biome attractor in the same
					// (t01, h01) space the terrain blends in. The reported temperature carries the altitude
					// lapse, so growth thins toward peaks on its own.
					const float t01 = glm::clamp((tp.temperature - TEMPERATURE_MIN_C) / TEMP_RANGE, 0.0f, 1.0f);
					const float h01 = glm::clamp(tp.humidity, 0.0f, 1.0f);
					const glm::vec2 dClim = glm::vec2(t01, h01) - rt.climateCoords;
					const float w = std::exp(-(dClim.x * dClim.x + dClim.y * dClim.y) * invS2);
					if (w < 0.02f || rng.next01() > w)
						continue;

					if (rule.clusterSize > 1.0f)
					{
						const float freq = 1.0f / rule.clusterSize;
						const float n = clusterNoise.fbm((float)(wx * freq), (float)(wz * freq), 2) * 0.5f + 0.5f;
						const float threshold = 1.0f - glm::clamp(rule.clusterCoverage, 0.0f, 1.0f);
						const float inPatch = glm::smoothstep(threshold - 0.08f, threshold + 0.08f, n);
						if (inPatch < 0.001f || rng.next01() > inPatch)
							continue;
					}

					const float h = tp.height;
					const float waterLevel = tp.waterLevel;
					const float altitude = h - waterLevel;
					if (altitude < rule.minAltitude || altitude > rule.maxAltitude)
						continue;

					const float gx = (maps.sampleHeight(wx + 1.0, wz) - maps.sampleHeight(wx - 1.0, wz)) * 0.5f;
					const float gz = (maps.sampleHeight(wx, wz + 1.0) - maps.sampleHeight(wx, wz - 1.0)) * 0.5f;
					if (gx * gx + gz * gz > rule.maxSlope * rule.maxSlope)
						continue;

					const float scale = rng.range(asset.minScale, asset.maxScale);
					const float radius = asset.footprintRadius * scale;
					if (radius > 0.0f)
					{
						bool blocked = false;
						for (const glm::vec3& a : accepted)
						{
							const float dx = (float)(wx - a.x), dz = (float)(wz - a.y);
							const float rr = radius + a.z;
							if (dx * dx + dz * dz < rr * rr) { blocked = true; break; }
						}
						if (blocked)
							continue;
						accepted.push_back(glm::vec3((float)wx, (float)wz, radius));
					}

					glm::quat rot = glm::angleAxis(rng.range(0.0f, 6.2831853f), glm::vec3(0.0f, 1.0f, 0.0f));
					if (asset.slopeAlign > 0.001f)
					{
						const glm::vec3 normal = glm::normalize(glm::vec3(-gx, 1.0f, -gz));
						const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
						const float axisLen = glm::length(axis);
						if (axisLen > 1e-5f)
						{
							const float angle = std::acos(glm::clamp(normal.y, -1.0f, 1.0f));
							rot = glm::angleAxis(angle * glm::clamp(asset.slopeAlign, 0.0f, 1.0f), axis / axisLen) * rot;
						}
					}

					ScatterSystem::Placed placed;
					placed.pos = glm::vec3((float)wx, h - asset.sinkDepth * scale, (float)wz);
					placed.rot = rot;
					placed.scale = scale;
					placed.assetIdx = rt.assetIdx;
					placed.variantIdx = (uint16)(rng.next() % glm::max<uint32>(rt.numVariants, 1));
					group.instances.push_back(placed);
				}
			}
			if (!group.instances.empty())
				outGroups.push_back(oc::move(group));
		}
	}

	void ScatterSystem::kickPump(size_t numNew)
	{
		const int32 cap = glm::clamp(m_maxGenJobs, 1, 16);
		for (size_t spawned = 0; spawned < numNew; )
		{
			int32 cur = m_numPumps.load(oc::memory_order_relaxed);
			if (cur >= cap)
				return;
			if (m_numPumps.compare_exchange_weak(cur, cur + 1, oc::memory_order_acq_rel))
			{
				Globals::jobSystem.submit([this] { pumpJob(); }, { "scatterCellPump", EProfileCategory::Procedural }, EJobPriority::Low, &m_pumpCounter);
				++spawned;
			}
		}
	}

	void ScatterSystem::pumpJob()
	{
		for (;;)
		{
			Request req;
			bool haveWork = false;
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				// Lazy staleness at dequeue, like the terrain worker: cells that left the ring while
				// queued bounce back as dropped results so the main thread releases their pending keys.
				while (!m_requests.empty())
				{
					req = oc::move(m_requests.front());
					m_requests.pop_front();
					const glm::ivec2 d = req.coord - m_ringCam;
					if (glm::max(glm::abs(d.x), glm::abs(d.y)) <= m_ringR)
					{
						haveWork = true;
						break;
					}
					Result drop;
					drop.key = req.key;
					drop.generation = req.generation;
					drop.coord = req.coord;
					drop.dropped = true;
					m_results.push_back(oc::move(drop));
				}
			}
			if (!haveWork)
			{
				// same claim/exit-recheck protocol as the terrain pump
				m_numPumps.fetch_sub(1, oc::memory_order_release);
				{
					std::lock_guard<std::mutex> lk(m_mutex);
					if (m_requests.empty())
						return;
				}
				const int32 cap = glm::clamp(m_maxGenJobs, 1, 16);
				int32 cur = m_numPumps.load(oc::memory_order_relaxed);
				for (;;)
				{
					if (cur >= cap)
						return; // an appender's kicks refilled the pool; those jobs take over
					if (m_numPumps.compare_exchange_weak(cur, cur + 1, oc::memory_order_acq_rel))
						break;
				}
				continue;
			}

			Result res;
			res.key = req.key;
			res.generation = req.generation;
			res.coord = req.coord;
			if (req.maps)
				generateCell(*req.maps, req.coord, req.params, m_rules, m_ruleOrder, res.groups);

			{
				std::lock_guard<std::mutex> lk(m_mutex);
				m_results.push_back(oc::move(res));
			}
		}
	}

	void ScatterSystem::spawnGroup(RuleGroup& group)
	{
		AssetRuntime& asset = m_assets[m_rules[group.ruleIdx].assetIdx];
		group.nodes.reserve(group.instances.size());
		Sphere bounds;
		bool first = true;
		for (const Placed& p : group.instances)
		{
			RenderNode node = asset.container->spawnNodeForIdx(asset.variants[p.variantIdx], Transform(p.pos, p.scale, p.rot));
			if (!node.isValid())
				continue;
			const Sphere b = node.getWorldBounds();
			bounds = first ? b : mergeSpheres(bounds, b);
			first = false;
			group.nodes.push_back(oc::move(node));
		}
		if (group.nodes.empty())
			return;
		// Like terrain chunks: own layer (gameplay queries must not see scatter), registered once
		// (instances never move), no spawn-visibility guard (groups stream in off-screen constantly).
		group.spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(
			glm::dvec3(bounds.pos), bounds.radius, 0ull, SpatialLayer_Terrain, false));
	}

	void ScatterSystem::despawnGroup(RuleGroup& group)
	{
		group.nodes.clear();
		group.spatialEntry.reset();
	}

	void ScatterSystem::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		ProfileScope profileScope("Scatter", EProfileCategory::Procedural);
		// The sampler's identity doubles as the terrain's config generation: a rebuilt field (or terrain
		// toggling off -> nullptr) invalidates every placement.
		const bool mapsChanged = maps.get() != m_lastMaps;
		m_lastMaps = maps.get();

		if (!m_enabled || !maps || m_ruleOrder.empty())
		{
			if (!m_residentCells.empty() || !m_pending.empty())
			{
				clearResidents();
				std::lock_guard<std::mutex> lk(m_mutex);
				++m_generation;
				m_requests.clear();
			}
			return;
		}
		if (m_configDirty || mapsChanged)
		{
			m_configDirty = false;
			clearResidents();
			std::lock_guard<std::mutex> lk(m_mutex);
			++m_generation;
			m_requests.clear();
		}

		const float cellSize = glm::clamp(m_cellSize, 16.0f, 256.0f);
		const int camCX = (int)std::floor(camera.position.x / cellSize);
		const int camCZ = (int)std::floor(camera.position.z / cellSize);
		const int R = glm::clamp((int)std::ceil(m_maxViewDistance * m_viewScale / cellSize) + 1, 1, 128);

		uint32 generation;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			generation = m_generation;
		}

		const bool ringMoved = camCX != m_lastRingCX || camCZ != m_lastRingCZ || R != m_lastRingR;
		m_lastRingCX = camCX; m_lastRingCZ = camCZ; m_lastRingR = R;

		// --- Enqueue any ring cell that is neither resident nor in flight (nearest-first, like terrain).
		// A stationary ring's contents can't change, so the scan only runs when it moved (or a reset/
		// dropped result flagged it dirty).
		oc::vector<Request> newRequests;
		if (ringMoved || m_ringDirty)
		{
			m_ringDirty = false;
			for (int dz = -R; dz <= R; ++dz)
			{
				for (int dx = -R; dx <= R; ++dx)
				{
					const glm::ivec2 coord(camCX + dx, camCZ + dz);
					const uint64 key = cellKey(coord);
					if (m_residentCells.count(key) || m_pending.count(key))
						continue;
					Request req;
					req.key = key;
					req.generation = generation;
					req.coord = coord;
					req.params = GenParams{ cellSize, m_densityScale, (uint32)m_seed };
					req.maps = maps;
					newRequests.push_back(oc::move(req));
					m_pending.insert(key);
				}
			}
		}
		if (ringMoved || !newRequests.empty())
		{
			const glm::ivec2 cam(camCX, camCZ);
			oc::sort(newRequests.begin(), newRequests.end(), [&](const Request& a, const Request& b)
			{
				const glm::ivec2 da = a.coord - cam, db = b.coord - cam;
				return da.x * da.x + da.y * da.y < db.x * db.x + db.y * db.y;
			});
			std::lock_guard<std::mutex> lk(m_mutex);
			m_ringCam = cam;
			m_ringR = R;
			for (Request& r : newRequests)
				m_requests.push_back(oc::move(r));
		}
		if (!newRequests.empty())
			kickPump(newRequests.size());

		// --- Drain generated cells. Admission is just moving vectors into the resident map (node
		// spawning is budgeted separately below), so no per-frame cap is needed here.
		oc::vector<Result> ready;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			ready.swap(m_results);
		}
		for (Result& res : ready)
		{
			m_pending.erase(res.key);
			if (res.dropped || res.generation != generation || m_residentCells.count(res.key))
			{
				m_ringDirty = true; // a still-wanted cell must re-enqueue even if the ring stands still
				continue;
			}
			const glm::ivec2 d = res.coord - glm::ivec2(camCX, camCZ);
			if (glm::max(glm::abs(d.x), glm::abs(d.y)) > R)
			{
				m_ringDirty = true;
				continue;
			}
			m_residentCells.insert(res.key);
			for (GroupResult& g : res.groups)
			{
				RuleGroup group;
				group.ruleIdx = g.ruleIdx;
				group.instances = oc::move(g.instances);
				m_ruleGroups[g.ruleIdx].emplace(res.key, oc::move(group));
			}
			m_spawnScan = true;
		}

		// --- Evict cells outside the ring (RuleGroup RAII frees nodes + spatial entries; stale m_active
		// entries drop out in the loop below). Cells can only leave the ring when it moved.
		if (ringMoved)
		{
			auto outsideRing = [&](uint64 key) {
				const glm::ivec2 d = cellCoord(key) - glm::ivec2(camCX, camCZ);
				return glm::max(glm::abs(d.x), glm::abs(d.y)) > R;
			};
			oc::erase_if(m_residentCells, outsideRing);
			for (auto& groups : m_ruleGroups)
				oc::erase_if(groups, [&](const auto& kv) { return outsideRing(kv.first); });
		}

		// --- Spawn scan: per rule, probe only the cells inside that rule's OWN view-distance ring — the
		// residency ring is sized by the largest rule, and short-range rules never touch it. Gated besides:
		// eligibility only changes on new admissions, a deferred over-budget spawn, or half a hysteresis of
		// camera travel since the last scan (a group spawns at most that late; despawns stay exact, handled
		// every frame below). Once the frame's spawn budget is gone the scan stops and retries next frame.
		// m_ruleOrder is footprint-descending, so big objects also claim the budget first.
		const oc::span<const ScatterRule> rules = scatterRules();
		const glm::vec2 camXZ((float)camera.position.x, (float)camera.position.z);
		const float hysteresis = glm::max(10.0f, cellSize * 0.25f);
		auto cellDist = [&](glm::ivec2 coord) {
			const glm::vec2 lo((float)coord.x * cellSize, (float)coord.y * cellSize);
			return glm::distance(camXZ, glm::clamp(camXZ, lo, lo + cellSize));
		};
		if (m_spawnScan || ringMoved || m_viewScale != m_lastScanViewScale
			|| glm::distance(camXZ, m_lastScanPos) > hysteresis * 0.5f)
		{
			m_spawnScan = false;
			m_lastScanPos = camXZ;
			m_lastScanViewScale = m_viewScale;
			int spawnBudget = m_maxSpawnsPerFrame;
			for (uint16 ruleIdx : m_ruleOrder)
			{
				auto& groups = m_ruleGroups[ruleIdx];
				if (groups.empty())
					continue;
				const float viewDist = rules[ruleIdx].viewDistance * m_viewScale;
				const int Rr = glm::min(R, (int)(viewDist / cellSize) + 1);
				for (int dz = -Rr; dz <= Rr && !m_spawnScan; ++dz)
				{
					for (int dx = -Rr; dx <= Rr; ++dx)
					{
						const glm::ivec2 coord(camCX + dx, camCZ + dz);
						if (cellDist(coord) > viewDist)
							continue;
						const auto it = groups.find(cellKey(coord));
						if (it == groups.end() || !it->second.nodes.empty())
							continue;
						if (spawnBudget <= 0)
						{
							m_spawnScan = true;
							break;
						}
						spawnGroup(it->second);
						spawnBudget -= (int)it->second.nodes.size();
						if (!it->second.nodes.empty())
							m_active.push_back({ it->first, ruleIdx });
					}
				}
				if (m_spawnScan)
					break;
			}
		}

		// --- Despawn + render push over the spawned groups only (hysteresis keeps the deactivation
		// boundary from thrashing). Entries whose cell was evicted or reset drop out here. Pass mask via
		// the spatial gate, same policy as terrain chunks: main-culled groups keep shadow/GI so off-screen
		// trees still shadow and bounce light; MainOnly debug drops them.
		const SpatialCullingConfig& culling = Globals::spatialIndex.getCullingConfig();
		const bool gate = culling.mode >= int(ESpatialCullMode::Cull);
		for (size_t i = 0; i < m_active.size(); )
		{
			auto& groups = m_ruleGroups[m_active[i].ruleIdx];
			const auto it = groups.find(m_active[i].key);
			RuleGroup* pGroup = it != groups.end() ? &it->second : nullptr;
			if (!pGroup || pGroup->nodes.empty()
				|| cellDist(cellCoord(m_active[i].key)) > rules[m_active[i].ruleIdx].viewDistance * m_viewScale + hysteresis)
			{
				if (pGroup)
					despawnGroup(*pGroup);
				m_active[i] = m_active.back();
				m_active.pop_back();
				continue;
			}
			uint32 passMask = RendererVKLayout::PASS_ALL;
			if (gate && pGroup->spatialEntry.isValid()
				&& !(Globals::spatialIndex.getPassMask(pGroup->spatialEntry.handle()) & SpatialPassBit_Main))
			{
				if (culling.mode == int(ESpatialCullMode::MainOnly))
				{
					++i;
					continue;
				}
				passMask = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI;
			}
			for (RenderNode& node : pGroup->nodes)
				renderer.renderNode(node, passMask);
			++i;
		}
	}
}
