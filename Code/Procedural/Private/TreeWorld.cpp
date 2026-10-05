module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Tweaks;
import Core.Log;

import File;
import Threading;
import RendererVK;

import :TreeWorld;
import :TreeSpecies;
import :TreeGenerator;
import :TerrainSampler;
import :TerrainStreamer;
import :Noise;

namespace
{
	using namespace Procedural;

	uint64 chunkKey(glm::ivec2 c) { return (uint64)(uint32)c.x | ((uint64)(uint32)c.y << 32); }
	glm::ivec2 chunkCoord(uint64 key) { return glm::ivec2((int32)(uint32)key, (int32)(uint32)(key >> 32)); }
	int chebyshev(glm::ivec2 a, glm::ivec2 b) { return glm::max(glm::abs(a.x - b.x), glm::abs(a.y - b.y)); }

	// The climate / height grid a chunk is placed from: about one sample per GRID_STEP metres, one-sample halo.
	constexpr float GRID_STEP = 8.0f;
}

namespace Procedural
{
	TreeWorld::~TreeWorld()
	{
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_requests.clear(); // starve the pumps: they exit once the queue is empty
		}
		Globals::jobSystem.wait(m_pumpCounter);
	}

	void TreeWorld::initialize()
	{
		auto dirty = [this]() { m_configDirty = true; };
		Tweak::boolean("Trees/World", "Enabled", &m_enabled, dirty);
		Tweak::intVar("Trees/World", "Seed", &m_seed, 0, 1000000, 1.0f, dirty);
		Tweak::floatVar("Trees/World", "Candidate cell (m)", &m_cellSize, 2.0f, 20.0f, 0.1f, dirty);
		Tweak::floatVar("Trees/World", "Density scale", &m_densityScale, 0.0f, 4.0f, 0.01f, dirty);
		Tweak::floatVar("Trees/World", "Climate sharpness", &m_climateSharpness, 0.0f, 16.0f, 0.1f, dirty);
		Tweak::floatVar("Trees/World", "Climate fade start", &m_climateFadeStart, 0.0f, 1.0f, 0.01f, dirty);
		Tweak::floatVar("Trees/World", "Climate fade end", &m_climateFadeEnd, 0.0f, 1.0f, 0.01f, dirty);
		Tweak::intVar("Trees/World", "Gen jobs", &m_maxGenJobs, 1, 8, 1.0f);
		Tweak::intVar("Trees/World", "GPU pool (MB)", &m_poolMB, 1, 1024, 1.0f, dirty);
		Tweak::intVar("Trees/World", "Upload KB per frame", &m_uploadKB, 16, 65536, 16.0f);
		Tweak::intVar("Trees/World", "CPU keep radius (chunks)", &m_keepRadius, 0, 128, 1.0f, [this]() { m_ringCam = glm::ivec2(INT32_MAX); });
		Tweak::boolean("Trees/World", "Reload species", &m_reloadSpecies);
		Tweak::boolean("Trees/World", "Log stats", &m_logStats);
	}

	oc::string_view TreeWorld::typeName(uint32 type) const
	{
		return type < m_typeNames.size() ? oc::string_view(m_typeNames[type]) : oc::string_view();
	}

	void TreeWorld::loadSpecies()
	{
		m_typeNames.clear();
		m_species.clear();
		m_speciesLoaded = true;
		oc::vector<FileSystem::DirEntry> entries;
		{
			const FileSystem::AllowMainThreadIO allowIo; // explicit user action: enable / reload
			if (!FileSystem::listDirectory("Trees", entries))
			{
				Log::warning("Trees/World: no Assets/Trees directory");
				return;
			}
		}
		// Name-sorted, as TreeSystem loads them: the record type is the index in this list.
		oc::sort(entries.begin(), entries.end(), [](const FileSystem::DirEntry& a, const FileSystem::DirEntry& b) { return a.name < b.name; });
		for (const FileSystem::DirEntry& entry : entries)
		{
			if (entry.isDirectory || entry.extension != ".tree")
				continue;
			TreeSpeciesDesc desc;
			oc::string error;
			{
				const FileSystem::AllowMainThreadIO allowIo;
				if (!loadTreeSpecies(entry.path, desc, error))
				{
					Log::warning(oc::format("Trees/World: failed to load '{}': {}", entry.path, error));
					continue;
				}
			}
			if (m_typeNames.size() > 255)
			{
				Log::warning(oc::format("Trees/World: more than 256 species, '{}' is not placed", desc.name));
				continue;
			}
			const uint8 type = (uint8)m_typeNames.size();
			m_typeNames.push_back(desc.name);
			if (desc.bush || desc.placement.density <= 0.0f)
				continue;
			Species& species = m_species.emplace_back();
			species.name = desc.name;
			species.type = type;
			species.placement = desc.placement;
			species.climateMin = glm::vec2(temperatureTo01(desc.placement.temperature.x), precipTo01(desc.placement.precipitation.x));
			species.climateMax = glm::vec2(temperatureTo01(desc.placement.temperature.y), precipTo01(desc.placement.precipitation.y));
		}
		Log::info(oc::format("Trees/World: {} species with a Placement block", m_species.size()));
	}

	// Drops every chunk, CPU and GPU (the pool at `poolBytes`; 0 frees it).
	void TreeWorld::clear(Renderer& renderer, uint64 poolBytes)
	{
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_requests.clear();
			m_results.clear();
			m_pumpConfig = nullptr;
		}
		renderer.resetTreeRecords(poolBytes, (uint32)glm::max(m_configRingR, 0));
		m_chunks.clear();
		m_inFlight.clear();
		m_uploadQueue.clear();
		m_numRecords = 0;
		m_cpuRecords = 0;
		m_poolRefusedLogged = 0;
		m_config = nullptr;
		m_ringCam = glm::ivec2(INT32_MAX);
	}

	void TreeWorld::restart(Renderer& renderer, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		const uint32 generation = ++m_generation;
		clear(renderer, (uint64)glm::max(m_poolMB, 1) << 20);
		auto config = oc::make_shared<GenConfig>();
		config->maps = maps;
		config->species = m_species;
		config->seed = (uint32)m_seed;
		config->generation = generation;
		config->chunkSize = m_chunkSize;
		config->cellSize = glm::max(m_cellSize, 1.0f);
		config->densityScale = glm::max(m_densityScale, 0.0f);
		config->sharpness = glm::max(m_climateSharpness, 0.0f);
		config->fadeStart = glm::clamp(m_climateFadeStart, 0.0f, 1.0f);
		config->fadeEnd = glm::clamp(glm::max(m_climateFadeEnd, config->fadeStart + 1e-3f), 0.0f, 1.0f);
		m_config = config;
		std::lock_guard<std::mutex> lk(m_mutex);
		m_pumpConfig = m_config;
	}

	void TreeWorld::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		if (m_logStats)
		{
			m_logStats = false;
			logStats(renderer);
		}
		if (!m_enabled || !maps)
		{
			if (m_config || !m_chunks.empty())
			{
				clear(renderer, 0); // frees the pool
				renderer.updateTreeRecords();
			}
			m_lastMaps = nullptr;
			return;
		}
		ProfileScope profileScope("TreeWorld", EProfileCategory::Procedural);

		// Anything the records are a function of: a new generation.
		if (!m_speciesLoaded || m_reloadSpecies)
		{
			m_reloadSpecies = false;
			loadSpecies();
			m_configDirty = true;
		}
		const float chunkSize = (float)Globals::terrain.chunkSize();
		const int ringR = Globals::terrain.ringRadius();
		glm::vec2 boundsMin, boundsMax;
		const bool bounded = Globals::terrain.generatedBounds(boundsMin, boundsMax);
		if (m_configDirty || !m_config || maps.get() != m_lastMaps || chunkSize != m_chunkSize || ringR != m_configRingR
			|| bounded != m_bounded || (bounded && (boundsMin != m_boundsMin || boundsMax != m_boundsMax)))
		{
			m_configDirty = false;
			m_configRingR = ringR;
			m_lastMaps = maps.get();
			m_chunkSize = chunkSize;
			m_bounded = bounded;
			m_boundsMin = boundsMin;
			m_boundsMax = boundsMax;
			restart(renderer, maps);
		}

		// The finished chunks (a dropped or old-generation one only releases its key). A chunk that exists already came
		// back for its CPU records (it re-entered the keep radius): the same records, nothing to upload.
		oc::vector<Result> results;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			results.swap(m_results);
		}
		for (Result& result : results)
		{
			if (result.generation != m_config->generation)
				continue;
			const uint64 key = chunkKey(result.coord);
			m_inFlight.erase(key);
			if (result.dropped || chebyshev(result.coord, m_ringCam) > m_ringR + 1)
				continue;
			result.records.shrink_to_fit();
			if (const auto it = m_chunks.find(key); it != m_chunks.end())
			{
				if (!it->second.hasCpu)
				{
					m_cpuRecords += result.records.size();
					it->second.records = oc::move(result.records);
					it->second.hasCpu = true;
				}
				continue;
			}
			Chunk& chunk = m_chunks[key];
			chunk.count = (uint32)result.records.size();
			chunk.records = oc::move(result.records);
			chunk.ground = oc::move(result.ground);
			m_numRecords += chunk.count;
			m_cpuRecords += chunk.count;
			m_uploadQueue.push_back(key);
		}

		const glm::ivec2 cam((int32)std::floor(camera.position.x / chunkSize), (int32)std::floor(camera.position.z / chunkSize));
		if (cam != m_ringCam || ringR != m_ringR)
		{
			m_ringCam = cam;
			m_ringR = ringR;
			rescanRing(renderer);
		}
		uploadChunks(renderer);
		renderer.updateTreeRecords();
	}

	bool TreeWorld::insideKeepRadius(glm::ivec2 coord) const
	{
		return chebyshev(coord, m_ringCam) <= glm::max(m_keepRadius, m_minKeepRadius);
	}

	const oc::vector<TreeRecord>* TreeWorld::cpuRecords(glm::ivec2 coord) const
	{
		const auto it = m_chunks.find(chunkKey(coord));
		return it != m_chunks.end() && it->second.hasCpu ? &it->second.records : nullptr;
	}

	void TreeWorld::dropCpu(Chunk& chunk)
	{
		m_cpuRecords -= chunk.records.size();
		chunk.records = {};
		chunk.hasCpu = false;
	}

	// The generated chunks to the GPU pool, within "Upload KB per frame". Uploaded chunks outside the keep radius drop
	// their CPU records.
	void TreeWorld::uploadChunks(Renderer& renderer)
	{
		size_t budget = (size_t)glm::max(m_uploadKB, 1) * 1024;
		while (!m_uploadQueue.empty() && budget > 0)
		{
			const uint64 key = m_uploadQueue.front();
			m_uploadQueue.pop_front();
			const auto it = m_chunks.find(key);
			if (it == m_chunks.end() || it->second.uploaded)
				continue;
			Chunk& chunk = it->second;
			chunk.uploaded = true;
			// Every chunk, also without trees: its ground serves its neighbours' trees and columns.
			chunk.gpu = renderer.addTreeRecordChunk(chunkCoord(key), chunk.ground,
				oc::span<const uint32>((const uint32*)chunk.records.data(), chunk.records.size()));
			budget -= glm::min(budget, chunk.records.size() * sizeof(TreeRecord) + chunk.ground.size() * sizeof(uint32));
			chunk.ground = {};
			if (!insideKeepRadius(chunkCoord(key)))
				dropCpu(chunk);
		}
		const uint32 refused = renderer.treeRecordStats().refused;
		if (refused > m_poolRefusedLogged)
		{
			if (m_poolRefusedLogged == 0)
				Log::warning(oc::format("Trees/World: the GPU record pool ({} MB) is full - raise 'GPU pool (MB)'", m_poolMB));
			m_poolRefusedLogged = refused;
		}
	}

	// The ring moved: drop the chunks that left it (one chunk of hysteresis), keep CPU records inside the keep radius only
	// (re-requesting the uploaded chunks that come back into it), request the missing chunks, and re-sort the whole queue
	// nearest first (the pumps take from the front).
	void TreeWorld::rescanRing(Renderer& renderer)
	{
		const int R = m_ringR;
		oc::vector<Request> missing;
		for (auto it = m_chunks.begin(); it != m_chunks.end(); )
		{
			const glm::ivec2 coord = chunkCoord(it->first);
			Chunk& chunk = it->second;
			if (chebyshev(coord, m_ringCam) > R + 1)
			{
				if (chunk.gpu != UINT32_MAX)
					renderer.removeTreeRecordChunk(chunk.gpu);
				m_numRecords -= chunk.count;
				m_cpuRecords -= chunk.records.size();
				it = m_chunks.erase(it);
				continue;
			}
			if (chunk.uploaded)
			{
				const bool keep = insideKeepRadius(coord);
				if (chunk.hasCpu && !keep)
					dropCpu(chunk);
				else if (!chunk.hasCpu && keep && chunk.count > 0 && m_inFlight.find(it->first) == m_inFlight.end())
				{
					m_inFlight.insert(it->first);
					missing.push_back({ coord, m_config->generation });
				}
			}
			++it;
		}

		for (int z = -R; z <= R; ++z)
			for (int x = -R; x <= R; ++x)
			{
				const glm::ivec2 coord = m_ringCam + glm::ivec2(x, z);
				if (m_bounded)
				{
					const glm::vec2 lo = glm::vec2(coord) * m_chunkSize;
					if (lo.x + m_chunkSize <= m_boundsMin.x || lo.x >= m_boundsMax.x || lo.y + m_chunkSize <= m_boundsMin.y || lo.y >= m_boundsMax.y)
						continue;
				}
				const uint64 key = chunkKey(coord);
				if (m_chunks.find(key) != m_chunks.end() || m_inFlight.find(key) != m_inFlight.end())
					continue;
				m_inFlight.insert(key);
				missing.push_back({ coord, m_config->generation });
			}

		const glm::ivec2 cam = m_ringCam;
		auto nearer = [cam](const Request& a, const Request& b)
		{
			const glm::ivec2 da = a.coord - cam, db = b.coord - cam;
			return da.x * da.x + da.y * da.y < db.x * db.x + db.y * db.y;
		};
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_pumpRingCam = m_ringCam;
			m_pumpRingR = m_ringR;
			for (const Request& request : missing)
				m_requests.push_back(request);
			oc::sort(m_requests.begin(), m_requests.end(), nearer);
		}
		kickPump(missing.size());
	}

	void TreeWorld::kickPump(size_t numNew)
	{
		const int32 cap = glm::clamp(m_maxGenJobs, 1, 8);
		for (size_t spawned = 0; spawned < numNew; )
		{
			int32 cur = m_numPumps.load(oc::memory_order_relaxed);
			if (cur >= cap)
				return;
			if (m_numPumps.compare_exchange_weak(cur, cur + 1, oc::memory_order_acq_rel))
			{
				Globals::jobSystem.submit([this] { pumpJob(); }, { "treeWorldPump", EProfileCategory::Procedural }, EJobPriority::Low, &m_pumpCounter);
				++spawned;
			}
		}
	}

	void TreeWorld::pumpJob()
	{
		for (;;)
		{
			Globals::jobSystem.preemptionPoint();
			Request request;
			oc::shared_ptr<const GenConfig> config;
			bool haveWork = false;
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				// Lazy staleness, as the terrain / scatter pumps: a chunk that left the ring while queued goes back as a
				// dropped result (the main thread releases its key).
				while (!m_requests.empty())
				{
					request = m_requests.front();
					m_requests.pop_front();
					if (m_pumpConfig && request.generation == m_pumpConfig->generation && chebyshev(request.coord, m_pumpRingCam) <= m_pumpRingR)
					{
						config = m_pumpConfig;
						haveWork = true;
						break;
					}
					Result drop;
					drop.coord = request.coord;
					drop.generation = request.generation;
					drop.dropped = true;
					m_results.push_back(oc::move(drop));
				}
			}
			if (!haveWork)
			{
				// The same claim / exit-recheck protocol as the terrain pump.
				m_numPumps.fetch_sub(1, oc::memory_order_release);
				{
					std::lock_guard<std::mutex> lk(m_mutex);
					if (m_requests.empty())
						return;
				}
				const int32 cap = glm::clamp(m_maxGenJobs, 1, 8);
				int32 cur = m_numPumps.load(oc::memory_order_relaxed);
				for (;;)
				{
					if (cur >= cap)
						return;
					if (m_numPumps.compare_exchange_weak(cur, cur + 1, oc::memory_order_acq_rel))
						break;
				}
				continue;
			}

			Result result;
			result.coord = request.coord;
			result.generation = request.generation;
			{
				ProfileScope scope("TreeWorld::placeChunk", EProfileCategory::Procedural);
				placeChunk(*config, request.coord, result.records, result.ground);
			}
			std::lock_guard<std::mutex> lk(m_mutex);
			m_results.push_back(oc::move(result));
		}
	}

	// THE PLACEMENT FUNCTION. One candidate per cell of a lattice of ~cellSize metres over the chunk, jittered over the
	// whole cell. Each species' CLIMATE FIT - 1 inside its ideal climate box (normalized temperature / precipitation,
	// as the scatter rules), outside a Gaussian of the distance to the box, faded to 0 below "Climate fade end" of the
	// peak, 0 outside its slope and altitude band - gives it a density there: its own density x its fit x its cluster
	// noise x (fit / best fit) ^ "Climate sharpness". A tree exists with probability (the summed densities) x the
	// cell's area, its species picked by share. Pure: every random choice hashes (seed, chunk, cell).
	void TreeWorld::placeChunk(const GenConfig& config, glm::ivec2 coord, oc::vector<TreeRecord>& out, oc::vector<uint32>& outGround)
	{
		out.clear();
		outGround.clear();
		if (!config.maps)
			return;
		const float cs = config.chunkSize;
		const double ox = (double)coord.x * cs, oz = (double)coord.y * cs;

		// The grid: res + 1 points over the chunk plus a one-point halo (the slope at the border).
		const uint32 res = (uint32)glm::max(2.0f, std::round(cs / GRID_STEP));
		const float step = cs / (float)res;
		const uint32 gpr = res + 3;
		oc::vector<TerrainPoint> field((size_t)gpr * gpr);
		config.maps->sampleGrid(ox - step, oz - step, step, gpr, gpr, field, ESampleDetail::Full);
		Globals::jobSystem.preemptionPoint();

		// THE GROUND for the far volume (TreeRecordPool's encoding): TREE_RECORD_HEIGHT_RES^2 heights over the chunk,
		// corners included, bilinear in the grid (point (i, j) of the grid sits at local (i - 1, j - 1) x step), as
		// 16-bit steps above the chunk's minimum.
		{
			constexpr uint32 N = TREE_RECORD_HEIGHT_RES;
			float heights[N * N];
			float lo = FLT_MAX, hi = -FLT_MAX;
			for (uint32 j = 0; j < N; ++j)
				for (uint32 i = 0; i < N; ++i)
				{
					const glm::vec2 g = glm::vec2((float)i, (float)j) * (cs / (float)(N - 1)) / step + 1.0f;
					const glm::uvec2 i0 = glm::min(glm::uvec2(g), glm::uvec2(gpr - 2));
					const glm::vec2 f = g - glm::vec2(i0);
					const float h00 = field[(size_t)i0.y * gpr + i0.x].height, h10 = field[(size_t)i0.y * gpr + i0.x + 1].height;
					const float h01 = field[(size_t)(i0.y + 1) * gpr + i0.x].height, h11 = field[(size_t)(i0.y + 1) * gpr + i0.x + 1].height;
					const float h = glm::mix(glm::mix(h00, h10, f.x), glm::mix(h01, h11, f.x), f.y);
					heights[i + j * N] = h;
					lo = glm::min(lo, h);
					hi = glm::max(hi, h);
				}
			const float unit = glm::max(hi - lo, 1e-3f) / 65535.0f;
			outGround.assign(TREE_RECORD_HEIGHT_WORDS, 0u);
			outGround[0] = oc::bitCast<uint32>(lo);
			outGround[1] = oc::bitCast<uint32>(unit);
			for (uint32 k = 0; k < N * N; ++k)
			{
				const uint32 q = (uint32)glm::clamp(std::round((heights[k] - lo) / unit), 0.0f, 65535.0f);
				outGround[2 + k / 2] |= (k & 1) ? q << 16 : q;
			}
		}
		if (config.species.empty())
			return;

		oc::small_vector<NoiseField, 8> clusterNoise;
		for (const Species& species : config.species)
			clusterNoise.push_back(NoiseField(treeHash(config.seed, 1000u + species.type)));
		oc::small_vector<float, 8> weights(config.species.size(), 0.0f);
		oc::small_vector<float, 8> fits(config.species.size(), 0.0f);

		const uint32 n = (uint32)glm::max(1.0f, std::round(cs / config.cellSize));
		const float cell = cs / (float)n;
		const float cellArea = cell * cell * (1.0f / 10000.0f) * config.densityScale; // hectares x the scale
		constexpr float TEMP_RANGE = TEMPERATURE_MAX_C - TEMPERATURE_MIN_C;
		const uint32 chunkSeed = treeHash(treeHash(config.seed, (uint32)coord.x), (uint32)coord.y);
		for (uint32 cj = 0; cj < n; ++cj)
		{
			for (uint32 ci = 0; ci < n; ++ci)
			{
				const uint32 h = treeHash(chunkSeed, cj * n + ci);
				const float lx = ((float)ci + treeHash01(treeHash(h, 1u))) * cell;
				const float lz = ((float)cj + treeHash01(treeHash(h, 2u))) * cell;
				const uint32 qx = glm::min((uint32)(lx / cs * (float)TREE_RECORD_STEPS), TREE_RECORD_STEPS - 1);
				const uint32 qz = glm::min((uint32)(lz / cs * (float)TREE_RECORD_STEPS), TREE_RECORD_STEPS - 1);
				// Evaluated at the RECORD's position (the quantized one), as everything that reads the record later.
				const glm::vec2 local = treeRecordLocal(makeTreeRecord(qx, qz, 0), cs);

				// Bilinear in the grid (point (i, j) of the grid sits at local (i - 1, j - 1) x step).
				const glm::vec2 g = local / step + 1.0f;
				const glm::uvec2 i0 = glm::min(glm::uvec2(g), glm::uvec2(gpr - 2));
				const glm::vec2 f = g - glm::vec2(i0);
				const TerrainPoint& p00 = field[(size_t)i0.y * gpr + i0.x];
				const TerrainPoint& p10 = field[(size_t)i0.y * gpr + i0.x + 1];
				const TerrainPoint& p01 = field[(size_t)(i0.y + 1) * gpr + i0.x];
				const TerrainPoint& p11 = field[(size_t)(i0.y + 1) * gpr + i0.x + 1];
				auto lerp2 = [&](float a, float b, float c, float d) { return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y); };
				const float height = lerp2(p00.height, p10.height, p01.height, p11.height);
				const float water = lerp2(p00.waterLevel, p10.waterLevel, p01.waterLevel, p11.waterLevel);
				const float temperature = lerp2(p00.temperature, p10.temperature, p01.temperature, p11.temperature);
				const float humidity = lerp2(p00.humidity, p10.humidity, p01.humidity, p11.humidity);
				// The bilinear patch's gradient.
				const float dx = glm::mix(p10.height - p00.height, p11.height - p01.height, f.y) / step;
				const float dz = glm::mix(p01.height - p00.height, p11.height - p10.height, f.x) / step;
				const float slope2 = dx * dx + dz * dz;
				const float altitude = height - water;
				const glm::vec2 climate(glm::clamp((temperature - TEMPERATURE_MIN_C) / TEMP_RANGE, 0.0f, 1.0f), glm::clamp(humidity, 0.0f, 1.0f));
				const glm::vec2 world((float)(ox + local.x), (float)(oz + local.y));

				// Each species' CLIMATE FIT: 1 inside its ideal climate box, outside a Gaussian of the distance to the box,
				// faded to 0 between "Climate fade start" and "end" (fractions of the peak - no long tail of lone trees far
				// from its forests), 0 where its slope / altitude gates exclude it.
				float best = 0.0f;
				for (size_t s = 0; s < config.species.size(); ++s)
				{
					const TreePlacementDesc& p = config.species[s].placement;
					fits[s] = 0.0f;
					if (altitude < p.altitude.x || altitude > p.altitude.y || slope2 > p.maxSlope * p.maxSlope)
						continue;
					const glm::vec2 d = glm::max(config.species[s].climateMin - climate, glm::vec2(0.0f))
						+ glm::max(climate - config.species[s].climateMax, glm::vec2(0.0f));
					float fit = std::exp(-glm::dot(d, d) / (2.0f * p.climateWidth * p.climateWidth));
					fit *= glm::smoothstep(config.fadeStart, config.fadeEnd, fit);
					fits[s] = fit;
					best = glm::max(best, fit);
				}
				if (best <= 0.0f)
					continue;
				// Each species' density here: its own density x its fit x its forest patches, suppressed by its fit
				// RELATIVE TO THE BEST-FITTING species, sharpened ("Climate sharpness": (fit / best)^k) - a dense
				// species' tail does not reach into a sparse one's core (pines among the acacias), and species
				// sharing a climate ADD (a rare willow among the oaks takes no oaks away).
				float total = 0.0f;
				for (size_t s = 0; s < config.species.size(); ++s)
				{
					weights[s] = 0.0f;
					if (fits[s] <= 0.0f)
						continue;
					const TreePlacementDesc& p = config.species[s].placement;
					float density = p.density * fits[s] * std::pow(fits[s] / best, config.sharpness);
					if (density > 0.0f && p.clusterSize > 1.0f)
					{
						const float noise = clusterNoise[s].fbm(world.x / p.clusterSize, world.y / p.clusterSize, 2) * 0.5f + 0.5f;
						const float threshold = 1.0f - p.clusterCoverage;
						density *= glm::smoothstep(threshold - 0.08f, threshold + 0.08f, noise);
					}
					weights[s] = density;
					total += density;
				}
				// Whether a tree exists (the summed densities), then which one (by its share).
				if (total <= 0.0f || treeHash01(treeHash(h, 3u)) >= total * cellArea)
					continue;
				float pick = treeHash01(treeHash(h, 4u)) * total;
				size_t chosen = config.species.size() - 1;
				for (size_t s = 0; s < config.species.size(); ++s)
				{
					pick -= weights[s];
					if (pick < 0.0f && weights[s] > 0.0f)
					{
						chosen = s;
						break;
					}
				}
				out.push_back(makeTreeRecord(qx, qz, config.species[chosen].type));
			}
		}
	}

	void TreeWorld::logStats(const Renderer& renderer) const
	{
		uint32 maxPerChunk = 0;
		oc::vector<size_t> perType(m_typeNames.size(), 0);
		for (const auto& [key, chunk] : m_chunks)
		{
			maxPerChunk = glm::max(maxPerChunk, chunk.count);
			for (TreeRecord record : chunk.records)
				if (treeRecordType(record) < perType.size())
					++perType[treeRecordType(record)];
		}
		size_t queued = 0;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			queued = m_requests.size();
		}
		constexpr double MB = 1.0 / (1024.0 * 1024.0);
		Log::info(oc::format("Trees/World: {} chunks ({} in flight, {} queued, {} to upload), {} trees, {:.0f} per chunk on average, {} at most",
			m_chunks.size(), m_inFlight.size(), queued, m_uploadQueue.size(), m_numRecords,
			m_chunks.empty() ? 0.0 : (double)m_numRecords / (double)m_chunks.size(), maxPerChunk));
		const auto gpu = renderer.treeRecordStats();
		Log::info(oc::format("Trees/World: CPU {:.1f} MB of records ({} trees); GPU pool {:.1f} / {:.1f} MB used ({} chunks, {} trees; {:.1f} MB awaiting reuse, {} refused)",
			(double)(m_cpuRecords * sizeof(TreeRecord)) * MB, m_cpuRecords, (double)gpu.usedBytes * MB, (double)gpu.poolBytes * MB,
			gpu.chunks, gpu.records, (double)gpu.pendingBytes * MB, gpu.refused));
		Log::info("Trees/World: species of the CPU-held records:");
		for (size_t t = 0; t < perType.size(); ++t)
			if (perType[t] > 0)
				Log::info(oc::format("Trees/World:   {}: {}", m_typeNames[t], perType[t]));
	}
}
