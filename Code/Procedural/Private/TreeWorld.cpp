module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Log;
import Settings;
import Settings.Tweaks;

import File;
import Threading;
import RendererVK;

import :TreeWorld;
import :TreeSpecies;
import :TreeGenerator;
import :RockType;
import :TerrainSampler;
import :TerrainStreamer;
import :Noise;

namespace
{
	using namespace Procedural;

	uint64 chunkKey(glm::ivec2 c) { return (uint64)(uint32)c.x | ((uint64)(uint32)c.y << 32); }
	glm::ivec2 chunkCoord(uint64 key) { return glm::ivec2((int32)(uint32)key, (int32)(uint32)(key >> 32)); }
	int chebyshev(glm::ivec2 a, glm::ivec2 b) { return glm::max(glm::abs(a.x - b.x), glm::abs(a.y - b.y)); }

	// The climate / height grid a chunk is placed from: about one sample per GRID_STEP metres, with a halo of points
	// around the chunk. Trees only: 1 point (the slope at the border). With ROCKS: GRID_HALO, for the rocks of the
	// neighbour chunks that reach into this one - their fit is evaluated at their own position, up to ROCK_REACH + 2 m
	// outside (3 cells), plus the talus probe's TALUS_PROBE uphill of it and the rugged measure's RUGGED_CELLS around it.
	constexpr float GRID_STEP = 8.0f;
	constexpr uint32 GRID_HALO = 5;
	constexpr float RIVER_GRID_STEP = 1.0f; // m: the river influence's own grid (channels are ~1-3 m wide at mpp 5)
	constexpr float ROCK_REACH = 0.5f * ROCK_MAX_SIZE; // m: the largest rock radius (the halo allows up to 15)
	constexpr float TALUS_PROBE = 6.0f; // m
	constexpr uint32 RUGGED_CELLS = 2;  // RUGGED ground = the steepest grid cell within this many cells (16-24 m)
	// The rocks' VALLEY measure has its own COARSE grid (only when a rule has a `Valley`): a point per ~VALLEY_STEP
	// metres, the lowest and the highest within VALLEY_CELLS points (~160 m), and a halo of VALLEY_CELLS + 1 points for
	// the neighbours' rocks. A valley = the low end of that range: none above VALLEY_LOW_START of the way down, full
	// below VALLEY_LOW_FULL.
	constexpr float VALLEY_STEP = 32.0f;
	constexpr uint32 VALLEY_CELLS = 5;
	constexpr float VALLEY_LOW_START = 0.5f, VALLEY_LOW_FULL = 0.9f;

	// The lowest and the highest value of an n x n grid within +-radius points of each point (a square, separable).
	void windowRange(const oc::vector<float>& src, uint32 n, uint32 radius, oc::vector<float>& lo, oc::vector<float>& hi)
	{
		oc::vector<float> rowLo(src.size()), rowHi(src.size());
		for (uint32 j = 0; j < n; ++j)
			for (uint32 i = 0; i < n; ++i)
			{
				float l = FLT_MAX, h = -FLT_MAX;
				for (uint32 k = i - glm::min(i, radius); k <= glm::min(i + radius, n - 1); ++k)
				{
					l = glm::min(l, src[(size_t)j * n + k]);
					h = glm::max(h, src[(size_t)j * n + k]);
				}
				rowLo[(size_t)j * n + i] = l;
				rowHi[(size_t)j * n + i] = h;
			}
		lo.resize(src.size());
		hi.resize(src.size());
		for (uint32 j = 0; j < n; ++j)
			for (uint32 i = 0; i < n; ++i)
			{
				float l = FLT_MAX, h = -FLT_MAX;
				for (uint32 k = j - glm::min(j, radius); k <= glm::min(j + radius, n - 1); ++k)
				{
					l = glm::min(l, rowLo[(size_t)k * n + i]);
					h = glm::max(h, rowHi[(size_t)k * n + i]);
				}
				lo[(size_t)j * n + i] = l;
				hi[(size_t)j * n + i] = h;
			}
	}
	// An n x n grid at the grid coordinate g, bilinear, clamped to it.
	float gridAt(const oc::vector<float>& grid, uint32 n, glm::vec2 g)
	{
		g = glm::clamp(g, glm::vec2(0.0f), glm::vec2((float)(n - 1)));
		const glm::uvec2 i0 = glm::min(glm::uvec2(g), glm::uvec2(n - 2));
		const glm::vec2 f = g - glm::vec2(i0);
		const float* r = grid.data() + (size_t)i0.y * n + i0.x;
		return glm::mix(glm::mix(r[0], r[1], f.x), glm::mix(r[n], r[n + 1], f.x), f.y);
	}
	// A soft band: 0 below .x, 1 over .y .. .z, 0 above .w; a hard edge where two are equal.
	float bandWeight(const glm::vec4& band, float v)
	{
		if (v < band.x || v > band.w)
			return 0.0f;
		const float in = v >= band.y ? 1.0f : glm::smoothstep(band.x, band.y, v);
		const float out = v <= band.z ? 1.0f : 1.0f - glm::smoothstep(band.z, band.w, v);
		return in * out;
	}
	// The terrain shader's rock coverage (terrain_splat.inc.glsl terrainLayers: steep, or far above the macro
	// altitude) at the terrain's DEFAULT tweaks ("Terrain/Textures": slope rock start / full, crag start / full) - a
	// mirror for the rocks' crag fit. Approximate on purpose: it places rocks, it does not shade.
	constexpr float CRAG_SLOPE_START = 0.25f, CRAG_SLOPE_FULL = 0.60f, CRAG_RELIEF_START = 34.0f, CRAG_RELIEF_FULL = 400.0f;
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
		const TreeWorldSettings& s = m_settings;
		Tweak::onChange(s.enabled, this, dirty);
		Tweak::onChange(s.seed, this, dirty);
		Tweak::onChange(s.cellSize, this, dirty);
		Tweak::onChange(s.densityScale, this, dirty);
		Tweak::onChange(s.climateSharpness, this, dirty);
		Tweak::onChange(s.climateFadeStart, this, dirty);
		Tweak::onChange(s.climateFadeEnd, this, dirty);
		Tweak::onChange(s.poolMB, this, dirty);
		Tweak::onChange(s.keepRadius, this, [this]() { m_ringCam = glm::ivec2(INT32_MAX); });
	}

	oc::string_view TreeWorld::typeName(uint32 type) const
	{
		return type < m_typeNames.size() ? oc::string_view(m_typeNames[type]) : oc::string_view();
	}

	void TreeWorld::loadSpecies()
	{
		m_typeNames.clear();
		m_floorTypes.clear();
		m_species.clear();
		m_rocks.clear();
		m_firstRockType = 0;
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
			FloorType& floor = m_floorTypes.emplace_back();
			if (desc.bush || desc.placement.density <= 0.0f)
				continue;
			const float meanScale = 0.5f * (desc.scale.x + desc.scale.y);
			floor.kind = 1;
			floor.crown = desc.crownRadius * meanScale;
			floor.trunk = desc.trunkRadius * (1.0f + desc.trunkFlare) * meanScale;
			Species& species = m_species.emplace_back();
			species.name = desc.name;
			species.type = type;
			species.placement = desc.placement;
			species.climateMin = glm::vec2(temperatureTo01(desc.placement.temperature.x), precipTo01(desc.placement.precipitation.x));
			species.climateMax = glm::vec2(temperatureTo01(desc.placement.temperature.y), precipTo01(desc.placement.precipitation.y));
		}
		Log::info(oc::format("Trees/World: {} species with a Placement block", m_species.size()));

		// THE ROCK TYPES, after the trees: every .rock, name-sorted (as RockSystem loads them).
		m_firstRockType = (uint32)m_typeNames.size();
		if (!m_rocksEnabled)
			return;
		entries.clear();
		{
			const FileSystem::AllowMainThreadIO allowIo;
			if (!FileSystem::listDirectory("Rocks", entries))
				return;
		}
		oc::sort(entries.begin(), entries.end(), [](const FileSystem::DirEntry& a, const FileSystem::DirEntry& b) { return a.name < b.name; });
		for (const FileSystem::DirEntry& entry : entries)
		{
			if (entry.isDirectory || entry.extension != ".rock")
				continue;
			RockTypeDesc desc;
			oc::string error;
			{
				const FileSystem::AllowMainThreadIO allowIo;
				if (!loadRockType(entry.path, desc, error))
				{
					Log::warning(oc::format("Trees/World: failed to load '{}': {}", entry.path, error));
					continue;
				}
			}
			if (desc.name.empty())
				desc.name = entry.name;
			if (m_typeNames.size() > 255)
			{
				Log::warning(oc::format("Trees/World: more than 256 record types, rock '{}' is not placed", desc.name));
				continue;
			}
			const uint8 type = (uint8)m_typeNames.size();
			m_typeNames.push_back(desc.name);
			FloorType& floor = m_floorTypes.emplace_back();
			if (desc.placements.empty())
				continue;
			floor.kind = desc.surface == ERockSurface::Wood ? 3 : 2;
			floor.rockScale = desc.scale;
			floor.footprint = rockFootprint(desc);
			RockSpecies& rock = m_rocks.emplace_back();
			rock.name = desc.name;
			rock.type = type;
			rock.footprint = floor.footprint;
			for (const RockPlacementDesc& placement : desc.placements)
				rock.rules.push_back({ placement,
					glm::vec2(temperatureTo01(placement.temperature.x), precipTo01(placement.precipitation.x)),
					glm::vec2(temperatureTo01(placement.temperature.y), precipTo01(placement.precipitation.y)) });
			rock.scale = desc.scale;
			// One rock of the type per cell at most: a cell holds its largest rock with room to spare.
			rock.cell = glm::max(desc.scale.y * 1.5f, 6.0f);
		}
		Log::info(oc::format("Trees/World: {} rock types with a Placement block", m_rocks.size()));
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
		clear(renderer, (uint64)glm::max(m_settings.poolMB, 1) << 20);
		auto config = oc::make_shared<GenConfig>();
		config->maps = maps;
		config->species = m_species;
		config->rocks = m_rocks;
		config->rockRules = m_rockRules;
		config->rockRules.densityScale = glm::max(m_rockRules.densityScale, 0.0f);
		config->rockRules.ruggedSlope.y = glm::max(m_rockRules.ruggedSlope.y, m_rockRules.ruggedSlope.x + 0.01f);
		config->rockRules.valleyRelief.y = glm::max(m_rockRules.valleyRelief.y, m_rockRules.valleyRelief.x + 0.1f);
		config->seaLevel = Globals::terrain.seaLevel();
		config->seed = (uint32)m_settings.seed;
		config->generation = generation;
		config->chunkSize = m_chunkSize;
		config->cellSize = glm::max(m_settings.cellSize, 1.0f);
		config->densityScale = glm::max(m_settings.densityScale, 0.0f);
		config->sharpness = glm::max(m_settings.climateSharpness, 0.0f);
		config->fadeStart = glm::clamp(m_settings.climateFadeStart, 0.0f, 1.0f);
		config->fadeEnd = glm::clamp(glm::max(m_settings.climateFadeEnd, config->fadeStart + 1e-3f), 0.0f, 1.0f);
		config->riverClear = Globals::settings.terrain.riverVegetationClear;
		m_config = config;
		std::lock_guard<std::mutex> lk(m_mutex);
		m_pumpConfig = m_config;
	}

	void TreeWorld::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		if (m_settings.logStats)
		{
			m_settings.logStats = false;
			logStats(renderer);
		}
		if (!m_settings.enabled || !maps)
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
		if (!m_speciesLoaded || m_settings.reloadSpecies)
		{
			m_settings.reloadSpecies = false;
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
		return chebyshev(coord, m_ringCam) <= glm::max(m_settings.keepRadius, m_minKeepRadius);
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
		size_t budget = (size_t)glm::max(m_settings.uploadKB, 1) * 1024;
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
				Log::warning(oc::format("Trees/World: the GPU record pool ({} MB) is full - raise 'GPU pool (MB)'", m_settings.poolMB));
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
		const int32 cap = glm::clamp(m_settings.maxGenJobs, 1, 8);
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
				// Lazy staleness, as the terrain pumps: a chunk that left the ring while queued goes back as a
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
				const int32 cap = glm::clamp(m_settings.maxGenJobs, 1, 8);
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
	// temperatureTo01 / precipTo01), outside a Gaussian of the distance to the box, faded to 0 below "Climate fade end" of the
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

		// The grid: res + 1 points over the chunk plus the halo (the slope at the border; the neighbours' rocks). With
		// chunk origins on the grid's own lattice, two neighbour chunks sample the same world points: a rock near
		// their border gets the same fit from both.
		const uint32 res = (uint32)glm::max(2.0f, std::round(cs / GRID_STEP));
		const float step = cs / (float)res;
		const uint32 halo = config.rocks.empty() ? 1u : GRID_HALO;
		const uint32 gpr = res + 1 + 2 * halo;
		oc::vector<TerrainPoint> field((size_t)gpr * gpr);
		config.maps->sampleGrid(ox - step * (float)halo, oz - step * (float)halo, step, gpr, gpr, field, ESampleDetail::Full);
		Globals::jobSystem.preemptionPoint();

		// The river influence on its OWN fine grid over the same extent: most channels are narrower than GRID_STEP, so
		// the field's points miss them. Cheap - it reads only the river units, not the terrain.
		const float riverPad = step * (float)halo;
		const uint32 rpr = (uint32)std::ceil((cs + 2.0f * riverPad) / RIVER_GRID_STEP) + 1;
		oc::vector<float> riverField((size_t)rpr * rpr);
		config.maps->sampleRiverGrid(ox - riverPad, oz - riverPad, RIVER_GRID_STEP, rpr, rpr, riverField);
		const auto riverAt = [&](glm::vec2 local)
		{
			// The MAX of the cell's corners: a bed a little narrower than the step still counts across its cell.
			const glm::vec2 g = glm::clamp((local + riverPad) / RIVER_GRID_STEP, glm::vec2(0.0f), glm::vec2((float)(rpr - 1)));
			const glm::uvec2 i0 = glm::min(glm::uvec2(g), glm::uvec2(rpr - 2));
			const size_t a = (size_t)i0.y * rpr + i0.x;
			return glm::max(glm::max(riverField[a], riverField[a + 1]), glm::max(riverField[a + rpr], riverField[a + rpr + 1]));
		};

		// The field at a chunk-local point: bilinear in the grid (point (i, j) sits at local (i - halo, j - halo) x
		// step), clamped to it, with the bilinear patch's gradient.
		struct FieldSample
		{
			float height, water, altitude, temperature, humidity;
			float dx, dz; // the height's gradient (rise / run)
			float river;  // the river influence, from the fine river grid (riverAt)
		};
		auto fieldAt = [&](glm::vec2 local)
		{
			const glm::vec2 g = glm::clamp(local / step + (float)halo, glm::vec2(0.0f), glm::vec2((float)(gpr - 1)));
			const glm::uvec2 i0 = glm::min(glm::uvec2(g), glm::uvec2(gpr - 2));
			const glm::vec2 f = g - glm::vec2(i0);
			const TerrainPoint& p00 = field[(size_t)i0.y * gpr + i0.x];
			const TerrainPoint& p10 = field[(size_t)i0.y * gpr + i0.x + 1];
			const TerrainPoint& p01 = field[(size_t)(i0.y + 1) * gpr + i0.x];
			const TerrainPoint& p11 = field[(size_t)(i0.y + 1) * gpr + i0.x + 1];
			auto lerp2 = [&](float a, float b, float c, float d) { return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y); };
			FieldSample s;
			s.height = lerp2(p00.height, p10.height, p01.height, p11.height);
			// The water the ground is measured from: the sea, or a river / lake surface above it - so nothing places in
			// inland water (its altitude goes negative there).
			const auto water = [](const TerrainPoint& p)
			{
				const bool inland = p.waterKind == ETerrainWater::River || p.waterKind == ETerrainWater::Lake;
				return inland ? glm::max(p.waterLevel, p.inlandWater) : p.waterLevel;
			};
			s.water = lerp2(water(p00), water(p10), water(p01), water(p11));
			s.altitude = lerp2(p00.altitude, p10.altitude, p01.altitude, p11.altitude);
			s.temperature = lerp2(p00.temperature, p10.temperature, p01.temperature, p11.temperature);
			s.humidity = lerp2(p00.humidity, p10.humidity, p01.humidity, p11.humidity);
			s.dx = glm::mix(p10.height - p00.height, p11.height - p01.height, f.y) / step;
			s.dz = glm::mix(p01.height - p00.height, p11.height - p10.height, f.x) / step;
			s.river = riverAt(local);
			return s;
		};

		// THE GROUND for the far volume (TreeRecordPool's encoding): TREE_RECORD_HEIGHT_RES^2 heights over the chunk,
		// corners included, bilinear in the grid, as 16-bit steps above the chunk's minimum.
		{
			constexpr uint32 N = TREE_RECORD_HEIGHT_RES;
			float heights[N * N];
			float lo = FLT_MAX, hi = -FLT_MAX;
			for (uint32 j = 0; j < N; ++j)
				for (uint32 i = 0; i < N; ++i)
				{
					const float h = fieldAt(glm::vec2((float)i, (float)j) * (cs / (float)(N - 1))).height;
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
		constexpr float TEMP_RANGE = TEMPERATURE_MAX_C - TEMPERATURE_MIN_C;

		// THE TREES' DENSITY at a point, per species (into `weights`, per ha; the sum returned): each species' CLIMATE FIT
		// - 1 inside its ideal climate box, outside a Gaussian of the distance to the box, faded to 0 between "Climate fade
		// start" and "end" (fractions of the peak - no long tail of lone trees far from its forests), 0 where its slope /
		// altitude gates exclude it - then its own density x its fit x its forest patches, suppressed by its fit RELATIVE
		// TO THE BEST-FITTING species, sharpened ("Climate sharpness": (fit / best)^k) - a dense species' tail does not
		// reach into a sparse one's core (pines among the acacias), and species sharing a climate ADD (a rare willow among
		// the oaks takes no oaks away). The tree pass picks by it; the rocks' FOREST term reads the sum.
		oc::small_vector<NoiseField, 8> clusterNoise;
		for (const Species& species : config.species)
			clusterNoise.push_back(NoiseField(treeHash(config.seed, 1000u + species.type)));
		oc::small_vector<float, 8> weights(config.species.size(), 0.0f);
		oc::small_vector<float, 8> fits(config.species.size(), 0.0f);
		const auto speciesWeights = [&](const FieldSample& at, glm::vec2 world) -> float
		{
			const float slope2 = at.dx * at.dx + at.dz * at.dz;
			if (at.river > config.riverClear)
				return 0.0f; // a river's bed (Terrain/Rivers/Vegetation clear)
			const float altitude = at.height - at.water;
			const glm::vec2 climate(glm::clamp((at.temperature - TEMPERATURE_MIN_C) / TEMP_RANGE, 0.0f, 1.0f), glm::clamp(at.humidity, 0.0f, 1.0f));
			float best = 0.0f;
			for (size_t s = 0; s < config.species.size(); ++s)
			{
				const TreePlacementDesc& p = config.species[s].placement;
				fits[s] = 0.0f;
				weights[s] = 0.0f;
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
				return 0.0f;
			float total = 0.0f;
			for (size_t s = 0; s < config.species.size(); ++s)
			{
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
			return total;
		};

		// THE ROCKS FIRST (Docs/RockRenderingPlan.md 6). Each rock type has its own WORLD lattice (RockSpecies::cell,
		// anchored at the world origin - NOT at the chunk): a cell's rock is a pure function of (seed, type, cell), so
		// this chunk computes its neighbours' rocks exactly as they do. Every cell that can put a rock within
		// ROCK_REACH of the chunk is evaluated: the chunk's own rocks become records, the others only keep the trees
		// out of them. A type's RULES (its Placement blocks) add. A rule's fit, at the rock's (quantized) record position:
		// its optional climate box (as a tree's), its altitude and (soft) slope bands, its CRAG fit (the terrain's own
		// rock coverage: steep ground, or ground far above the macro altitude), its TALUS fit (gentle ground just below
		// a steep slope), the ground around it (PLAINS / RUGGED low and high end / VALLEY), the FOREST there (the trees'
		// own density: dead wood lies where trees grow) and its cluster noise. Rocks do not give way to each other:
		// boulders lie against boulders.
		// A placed rock keeps its FOOTPRINT, a capsule (a round rock's has no length; a fallen log's runs along its yaw).
		struct PlacedRock
		{
			glm::vec2 local{ 0.0f }; // in THIS chunk's frame (a neighbour's rock: outside [0, cs])
			glm::vec2 half{ 0.0f };  // the capsule's half axis (m)
			float radius = 0.0f;
		};
		oc::vector<PlacedRock> placedRocks;

		// THE GROUND AROUND A ROCK, for the rules' `Plains` / `Rugged` / `Valley` multipliers (RockPlacementDesc). The
		// slope under the rock cannot tell: a boulder lies on the gentle ground NEXT to the cliff, and on a valley's
		// flat floor.
		const RockWorldDesc& worldRules = config.rockRules;
		bool ruggedUsed = false, valleyUsed = false;
		for (const RockSpecies& rock : config.rocks)
			for (const RockRule& rule : rock.rules)
			{
				ruggedUsed |= rule.placement.plains != 1.0f || rule.placement.rugged != glm::vec2(1.0f);
				valleyUsed |= rule.placement.valley >= 0.0f;
			}
		// RUGGED: each grid cell's slope, then the steepest cell within RUGGED_CELLS of it, read between the cell
		// centres (cell (i, j)'s centre sits at local (i - halo + 0.5, j - halo + 0.5) x step). Its LOW and HIGH end:
		// the lowest and the highest grid point within RUGGED_CELLS, read between the points.
		const uint32 cpr = gpr - 1;
		oc::vector<float> steepest, nearLo, nearHi;
		if (ruggedUsed)
		{
			oc::vector<float> cellSlope((size_t)cpr * cpr), heights((size_t)gpr * gpr), unused;
			for (uint32 j = 0; j < cpr; ++j)
				for (uint32 i = 0; i < cpr; ++i)
				{
					const float h00 = field[(size_t)j * gpr + i].height, h10 = field[(size_t)j * gpr + i + 1].height;
					const float h01 = field[(size_t)(j + 1) * gpr + i].height, h11 = field[(size_t)(j + 1) * gpr + i + 1].height;
					const float dx = (h10 - h00 + h11 - h01) * (0.5f / step), dz = (h01 - h00 + h11 - h10) * (0.5f / step);
					cellSlope[(size_t)j * cpr + i] = std::sqrt(dx * dx + dz * dz);
				}
			windowRange(cellSlope, cpr, RUGGED_CELLS, unused, steepest);
			for (size_t k = 0; k < heights.size(); ++k)
				heights[k] = field[k].height;
			windowRange(heights, gpr, RUGGED_CELLS, nearLo, nearHi);
		}
		// VALLEY: the coarse grid (VALLEY_STEP; on the chunks' common lattice, as the fine one) - the lowest and the
		// highest point within VALLEY_CELLS of each point, read between the points (point (i, j) sits at local
		// (i - valleyHalo, j - valleyHalo) x valleyStep).
		constexpr uint32 valleyHalo = VALLEY_CELLS + 1;
		const uint32 valleyRes = (uint32)glm::max(1.0f, std::round(cs / VALLEY_STEP));
		const float valleyStep = cs / (float)valleyRes;
		const uint32 vpr = valleyRes + 1 + 2 * valleyHalo;
		oc::vector<float> farLo, farHi;
		if (valleyUsed)
		{
			oc::vector<TerrainPoint> coarse((size_t)vpr * vpr);
			config.maps->sampleGrid(ox - valleyStep * (float)valleyHalo, oz - valleyStep * (float)valleyHalo, valleyStep, vpr, vpr, coarse, ESampleDetail::Full);
			Globals::jobSystem.preemptionPoint();
			oc::vector<float> heights(coarse.size());
			for (size_t k = 0; k < heights.size(); ++k)
				heights[k] = coarse[k].height;
			windowRange(heights, vpr, VALLEY_CELLS, farLo, farHi);
		}

		for (const RockSpecies& rock : config.rocks)
		{
			oc::small_vector<NoiseField, 4> ruleNoise; // per rule
			for (uint32 k = 0; k < (uint32)rock.rules.size(); ++k)
				ruleNoise.push_back(NoiseField(treeHash(treeHash(config.seed, 3000u + rock.type), k)));
			const double c = (double)rock.cell;
			const float cellArea = rock.cell * rock.cell * (1.0f / 10000.0f) * worldRules.densityScale; // hectares x the scale
			const int32 cx0 = (int32)std::floor((ox - ROCK_REACH) / c), cx1 = (int32)std::floor((ox + cs + ROCK_REACH) / c);
			const int32 cz0 = (int32)std::floor((oz - ROCK_REACH) / c), cz1 = (int32)std::floor((oz + cs + ROCK_REACH) / c);
			const uint32 typeSeed = treeHash(config.seed, 7000u + rock.type);
			for (int32 cz = cz0; cz <= cz1; ++cz)
				for (int32 cx = cx0; cx <= cx1; ++cx)
				{
					const uint32 h = treeHash(treeHash(typeSeed, (uint32)cx), (uint32)cz);
					// The rock's world position in its cell, then its OWNER chunk and its record there.
					const double wx = ((double)cx + (double)treeHash01(treeHash(h, 1u))) * c;
					const double wz = ((double)cz + (double)treeHash01(treeHash(h, 2u))) * c;
					const glm::ivec2 owner((int32)std::floor(wx / cs), (int32)std::floor(wz / cs));
					const uint32 qx = glm::min((uint32)((wx - (double)owner.x * cs) / cs * (double)TREE_RECORD_STEPS), TREE_RECORD_STEPS - 1);
					const uint32 qz = glm::min((uint32)((wz - (double)owner.y * cs) / cs * (double)TREE_RECORD_STEPS), TREE_RECORD_STEPS - 1);
					const TreeRecord record = makeTreeRecord(qx, qz, rock.type);
					// Evaluated at the RECORD's position (the quantized one), in this chunk's frame.
					const glm::vec2 local = glm::vec2((float)((double)owner.x * cs - ox), (float)((double)owner.y * cs - oz)) + treeRecordLocal(record, cs);
					const uint32 recordSeed = treeRecordSeed(config.seed, owner, record);
					const float scale = rockRecordScale(recordSeed, rock.scale);
					const float radius = 0.5f * scale; // its reach (the longest axis)
					const bool own = owner == coord;
					if (!own && (local.x < -radius - 2.0f || local.x > cs + radius + 2.0f || local.y < -radius - 2.0f || local.y > cs + radius + 2.0f))
						continue; // a neighbour's rock that does not reach this chunk

					const FieldSample s = fieldAt(local);
					if (s.river > config.riverClear)
						continue; // a river's bed (Terrain/Rivers/Vegetation clear)
					const float altitude = s.height - s.water;
					const float slope = std::sqrt(s.dx * s.dx + s.dz * s.dz);
					const glm::vec2 climate(glm::clamp((s.temperature - TEMPERATURE_MIN_C) / TEMP_RANGE, 0.0f, 1.0f), glm::clamp(s.humidity, 0.0f, 1.0f));
					// The ground terms, shared by the type's rules: each on its first use.
					float crag = -1.0f, talus = -1.0f, ruggedness = -1.0f, lowEnd = 0.0f, valley = -1.0f, forest = -1.0f;
					float density = 0.0f; // the rules ADD
					for (uint32 k = 0; k < (uint32)rock.rules.size(); ++k)
					{
						const RockRule& rule = rock.rules[k];
						const RockPlacementDesc& p = rule.placement;
						if (altitude < p.altitude.x || altitude > p.altitude.y)
							continue;
						const glm::vec2 d = glm::max(rule.climateMin - climate, glm::vec2(0.0f)) + glm::max(climate - rule.climateMax, glm::vec2(0.0f));
						float fit = std::exp(-glm::dot(d, d) / (2.0f * p.climateWidth * p.climateWidth));
						if (fit < 0.05f)
							continue;
						fit *= bandWeight(p.slope, slope);
						if (fit <= 0.0f)
							continue;
						if (p.crag > 0.0f)
						{
							if (crag < 0.0f)
							{
								const float slopeN = 1.0f - 1.0f / std::sqrt(1.0f + slope * slope); // the shader's 1 - normal.y
								const float relief = (s.height - config.seaLevel) - s.altitude;
								crag = glm::max(glm::smoothstep(CRAG_SLOPE_START, CRAG_SLOPE_FULL, slopeN),
									glm::smoothstep(CRAG_RELIEF_START, CRAG_RELIEF_FULL, relief) * 0.85f);
							}
							fit *= glm::mix(1.0f, crag, p.crag);
						}
						if (p.talus > 0.0f)
						{
							// Gentle ground with a steep slope TALUS_PROBE uphill of it: the foot of a cliff.
							if (talus < 0.0f)
							{
								talus = 0.0f;
								if (slope > 1e-3f)
								{
									const FieldSample up = fieldAt(local + glm::vec2(s.dx, s.dz) * (TALUS_PROBE / slope));
									talus = glm::smoothstep(0.35f, 0.8f, std::sqrt(up.dx * up.dx + up.dz * up.dz)) * (1.0f - glm::smoothstep(0.25f, 0.6f, slope));
								}
							}
							fit *= glm::mix(1.0f, talus, p.talus);
						}
						// The ground around it: plains .. rugged (its low end .. its high end), and in a valley the valley's own.
						float ground = 1.0f;
						if (p.plains != 1.0f || p.rugged != glm::vec2(1.0f))
						{
							if (ruggedness < 0.0f)
							{
								const glm::vec2 g = local / step + (float)halo;
								ruggedness = glm::smoothstep(worldRules.ruggedSlope.x, worldRules.ruggedSlope.y, gridAt(steepest, cpr, g - 0.5f));
								const float lo = gridAt(nearLo, gpr, g), hi = gridAt(nearHi, gpr, g);
								lowEnd = glm::smoothstep(0.25f, 0.75f, (hi - s.height) / glm::max(hi - lo, 0.01f));
							}
							ground = glm::mix(p.plains, glm::mix(p.rugged.y, p.rugged.x, lowEnd), ruggedness);
						}
						if (p.valley >= 0.0f)
						{
							if (valley < 0.0f)
							{
								const glm::vec2 g = local / valleyStep + (float)valleyHalo;
								const float lo = gridAt(farLo, vpr, g), hi = gridAt(farHi, vpr, g);
								valley = glm::smoothstep(VALLEY_LOW_START, VALLEY_LOW_FULL, (hi - s.height) / glm::max(hi - lo, 0.01f))
									* glm::smoothstep(worldRules.valleyRelief.x, worldRules.valleyRelief.y, hi - lo);
							}
							ground = glm::mix(ground, p.valley, valley);
						}
						if (p.forest != glm::vec2(1.0f))
						{
							if (forest < 0.0f)
								forest = glm::clamp(speciesWeights(s, glm::vec2((float)wx, (float)wz)) * config.densityScale / ROCK_FOREST_FULL, 0.0f, 1.0f);
							ground *= glm::mix(p.forest.x, p.forest.y, forest);
						}
						fit *= ground;
						float ruleDensity = p.density * fit;
						if (ruleDensity > 0.0f && p.clusterSize > 1.0f)
						{
							const float noise = ruleNoise[k].fbm((float)(wx / p.clusterSize), (float)(wz / p.clusterSize), 2) * 0.5f + 0.5f;
							const float threshold = 1.0f - p.clusterCoverage;
							ruleDensity *= glm::smoothstep(threshold - 0.08f, threshold + 0.08f, noise);
						}
						density += ruleDensity;
					}
					if (treeHash01(treeHash(h, 3u)) >= density * cellArea)
						continue;
					// Its footprint along its yaw (the expansion's: hash 104 about up, rock-local X turned to (cos, -sin)).
					const float yaw = treeHash01(treeHash(recordSeed, 104u)) * 6.28318531f;
					placedRocks.push_back({ local, glm::vec2(std::cos(yaw), -std::sin(yaw)) * (rock.footprint.halfLength * scale),
						rock.footprint.radius * scale });
					if (own)
						out.push_back(record);
				}
		}
		if (config.species.empty())
			return;
		Globals::jobSystem.preemptionPoint();

		const uint32 n = (uint32)glm::max(1.0f, std::round(cs / config.cellSize));
		const float cell = cs / (float)n;
		const float cellArea = cell * cell * (1.0f / 10000.0f) * config.densityScale; // hectares x the scale
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

				const FieldSample at = fieldAt(local);
				const float total = speciesWeights(at, glm::vec2((float)(ox + local.x), (float)(oz + local.y)));
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
				// The trees give way to the rocks (this chunk's and the neighbours' that reach in): out of each footprint
				// capsule (a fallen log's runs along it - a disc over its length would clear the forest around it).
				bool inRock = false;
				for (const PlacedRock& rock : placedRocks)
				{
					const glm::vec2 rel = local - rock.local;
					const float along = glm::dot(rock.half, rock.half) > 1e-8f ? glm::clamp(glm::dot(rel, rock.half) / glm::dot(rock.half, rock.half), -1.0f, 1.0f) : 0.0f;
					const glm::vec2 toRock = rel - rock.half * along;
					const float clear = rock.radius * 0.9f + (rock.half == glm::vec2(0.0f) ? 0.5f : 0.9f);
					if (glm::dot(toRock, toRock) < clear * clear)
					{
						inRock = true;
						break;
					}
				}
				if (inRock)
					continue;
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
