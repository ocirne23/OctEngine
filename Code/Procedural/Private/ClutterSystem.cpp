module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Log;
import Settings;
import Settings.Tweaks;

import RendererVK;
import File;
import Threading;

import :ClutterSystem;
import :ClutterType;
import :ClutterGenerator;
import :TreeSystem;
import :TreeWorld;
import :TreeGenerator; // treeHash
import :TerrainSampler;

namespace
{
	using namespace Procedural;
	using RendererVKLayout::ClutterTypeGpu;
	using RendererVKLayout::CLUTTER_FLOOR_DIM;
	using RendererVKLayout::CLUTTER_FLOOR_TEXEL;
	using RendererVKLayout::CLUTTER_MAX_TYPES;
	using RendererVKLayout::CLUTTER_MAX_MESHES;

	constexpr float FLOOR_HALF = 0.5f * (float)CLUTTER_FLOOR_DIM * CLUTTER_FLOOR_TEXEL;
	// The farthest a record reaches into the map from outside it (m): the largest crown / trunk reach / rock apron.
	constexpr float FLOOR_REACH = 24.0f;

	glm::vec3 srgbToLinear(const glm::vec3& srgb) { return glm::pow(glm::clamp(srgb, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(2.2f)); }
	float termMax(const glm::vec2& term) { return glm::max(term.x, term.y); }

	// One Placement block of a type as the GPU's type (RendererVKLayout::ClutterTypeGpu).
	ClutterTypeGpu gpuType(const ClutterTypeDesc& desc, const ClutterPlacementDesc& p, uint32 firstMesh, uint32 numMeshes)
	{
		ClutterTypeGpu t{};
		t.climate = glm::vec4(temperatureTo01(p.temperature.x), temperatureTo01(p.temperature.y), precipTo01(p.precipitation.x), precipTo01(p.precipitation.y));
		t.placement = glm::vec4(p.density, 1.0f / p.climateWidth, p.clusterSize > 0.0f ? 1.0f / p.clusterSize : 0.0f, p.clusterCoverage);
		t.terms0 = glm::vec4(p.grass, p.crag);
		t.terms1 = glm::vec4(p.beach, p.canopy);
		t.terms2 = glm::vec4(p.trunk, p.rock);
		t.terms3 = glm::vec4(p.wet, p.maxSlope, p.minAltitude);
		t.terms4 = glm::vec4(p.river, p.flow);
		t.terms5 = glm::vec4(p.riverCurve, 0.0f, 0.0f, 0.0f);
		t.ring = p.ring;
		t.shape = glm::vec4(desc.scale, desc.range, desc.sink);
		t.albedo0 = glm::vec4(srgbToLinear(desc.color), desc.roughness);
		t.albedo1 = glm::vec4(srgbToLinear(desc.color2), desc.align);
		t.flower = glm::vec4(desc.stemHeight, desc.headSize, desc.petalWidth, glm::radians(desc.open));
		// The most this block can place anywhere (every term at its largest): the ranks the cull evaluates.
		const float maxDensity = p.density * termMax(p.grass) * termMax(p.crag) * termMax(p.beach) * termMax(p.canopy)
			* termMax(p.trunk) * termMax(p.rock) * termMax(p.wet) * termMax(p.river) * termMax(p.flow) * termMax(p.water);
		t.bound = glm::vec4(maxDensity, desc.spots, p.water);
		t.info = glm::uvec4(firstMesh, numMeshes, (uint32)desc.kind, (uint32)desc.head | ((uint32)glm::min(desc.petals, 255) << 8));
		return t;
	}

	uint64 hashCombine(uint64 h, uint64 v) { return (h ^ v) * 0x100000001B3ull; }

	// A closed mesh uses every edge exactly twice: the edges used once (holes) and more than twice (non-manifold fans,
	// folds). The branch tubes are open on purpose at their twigs' roots and thin ends, so only the stones and the
	// mushrooms' parts are expected to be closed.
	void countBadEdges(const Renderer::ClutterMesh& mesh, uint32 lod, uint32& open, uint32& nonManifold)
	{
		open = nonManifold = 0;
		const oc::vector<uint32>& idx = mesh.indices[lod];
		// By index: the stones' meshes share every vertex (no seams).
		const auto key = [&](uint32 i) { return (uint64)i; };
		oc::unordered_map<uint64, uint32> uses;
		for (size_t t = 0; t + 2 < idx.size(); t += 3)
			for (int e = 0; e < 3; ++e)
			{
				const uint64 a = key(idx[t + e]), b = key(idx[t + (e + 1) % 3]);
				if (a == b)
					continue; // a degenerate edge
				++uses[glm::min(a, b) * 0x9E3779B97F4A7C15ull ^ glm::max(a, b)];
			}
		for (const auto& [edge, count] : uses)
		{
			open += count == 1 ? 1u : 0u;
			nonManifold += count > 2 ? 1u : 0u;
		}
	}
}

namespace Procedural
{
	ClutterSystem::~ClutterSystem()
	{
		Globals::jobSystem.wait(m_genCounter);
		Globals::jobSystem.wait(m_floorCounter);
	}

	void ClutterSystem::initialize()
	{
		Tweak::onChange(m_settings.meshResolution, this, [this]() { m_remesh = true; });
	}

	void ClutterSystem::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		(void)maps; // the clutter stands on the drawn terrain chunks (the renderer's ground table), not on the sampler
		if (!m_settings.enabled || !renderer.isInitialized())
		{
			// The types stay loaded (an enable draws at once); the floor map goes (the grass's canopy thinning reads it).
			if (m_floorSet)
			{
				renderer.setClutterFloorMap(glm::vec2(0.0f), {});
				m_floorSet = false;
				m_floorCentre = glm::vec2(FLT_MAX);
			}
			return;
		}
		ProfileScope profileScope("Clutter", EProfileCategory::Procedural);
		if (!m_loaded || m_settings.reload)
		{
			m_settings.reload = false;
			m_remesh = false;
			Globals::jobSystem.wait(m_genCounter); // the jobs write into m_types
			m_types.clear();
			loadTypes();
			kickGeneration();
			m_loaded = true;
		}
		else if (m_remesh)
		{
			m_remesh = false;
			Globals::jobSystem.wait(m_genCounter);
			for (Type& type : m_types)
				type.meshes.clear();
			kickGeneration();
		}
		if (m_generating && m_genInFlight.load(oc::memory_order_acquire) == 0)
			finishGeneration(renderer);
		updateFloorMap(renderer, camera);
	}

	void ClutterSystem::loadTypes()
	{
		oc::vector<FileSystem::DirEntry> entries;
		{
			const FileSystem::AllowMainThreadIO allowIo; // explicit user action: enable / reload
			if (!FileSystem::listDirectory("Clutter", entries))
			{
				Log::warning("Clutter: no Assets/Clutter directory");
				return;
			}
		}
		oc::sort(entries.begin(), entries.end(), [](const FileSystem::DirEntry& a, const FileSystem::DirEntry& b) { return a.name < b.name; });
		for (const FileSystem::DirEntry& entry : entries)
		{
			if (entry.isDirectory || entry.extension != ".clutter")
				continue;
			ClutterTypeDesc desc;
			oc::string error;
			{
				const FileSystem::AllowMainThreadIO allowIo;
				if (!loadClutterType(entry.path, desc, error))
				{
					Log::warning(oc::format("Clutter: failed to load '{}': {}", entry.path, error));
					continue;
				}
			}
			if (desc.name.empty())
				desc.name = entry.name;
			if (desc.placements.empty())
			{
				Log::warning(oc::format("Clutter: '{}' has no Placement block with a Density - never placed", desc.name));
				continue;
			}
			m_types.emplace_back().desc = oc::move(desc);
		}
	}

	void ClutterSystem::kickGeneration()
	{
		// One Low job per type (each single-threaded inside), in the BACKGROUND: finishGeneration hands the meshes over
		// once the last one is done. m_types is not resized until the counter is joined.
		const uint32 resolution = (uint32)glm::clamp(m_settings.meshResolution, 8, 64);
		for (uint32 t = 0; t < (uint32)m_types.size(); ++t)
		{
			m_genInFlight.fetch_add(1, oc::memory_order_relaxed);
			Globals::jobSystem.submit([this, t, resolution]
			{
				Type& type = m_types[t];
				generateClutterMeshes(type.desc, resolution, type.meshes);
				m_genInFlight.fetch_sub(1, oc::memory_order_release);
			}, { "Clutter generate", EProfileCategory::Procedural }, EJobPriority::Low, &m_genCounter);
		}
		m_generating = true;
	}

	// Every type's meshes into one list, a GPU type per Placement block (their densities add), to the renderer. The CPU
	// meshes are freed after.
	void ClutterSystem::finishGeneration(Renderer& renderer)
	{
		Globals::jobSystem.wait(m_genCounter); // every job has decremented; this only retires the counter
		m_generating = false;
		oc::vector<ClutterTypeGpu> types;
		oc::vector<Renderer::ClutterMesh> meshes;
		size_t flowers = 0;
		for (Type& type : m_types)
		{
			const bool flower = type.desc.kind == EClutterKind::Flower;
			if (!flower && type.meshes.empty())
				continue;
			if (meshes.size() + type.meshes.size() > CLUTTER_MAX_MESHES)
			{
				Log::warning(oc::format("Clutter: more than {} meshes - '{}' and the types after it are not placed", CLUTTER_MAX_MESHES, type.desc.name));
				break;
			}
			const uint32 firstMesh = (uint32)meshes.size();
			const uint32 numMeshes = (uint32)type.meshes.size();
			if (type.desc.kind == EClutterKind::Pebble)
				for (uint32 v = 0; v < numMeshes; ++v)
					for (uint32 lod = 0; lod < RendererVKLayout::CLUTTER_LODS; ++lod)
					{
						uint32 open, nonManifold;
						countBadEdges(type.meshes[v], lod, open, nonManifold);
						if (open + nonManifold > 0)
							Log::warning(oc::format("Clutter: '{}' variant {} level {} ({} triangles): {} open edges, {} non-manifold edges",
								type.desc.name, v, lod, type.meshes[v].indices[lod].size() / 3, open, nonManifold));
					}
			for (Renderer::ClutterMesh& mesh : type.meshes)
				meshes.push_back(oc::move(mesh));
			type.meshes.clear();
			for (const ClutterPlacementDesc& placement : type.desc.placements)
			{
				if (types.size() >= CLUTTER_MAX_TYPES)
				{
					Log::warning(oc::format("Clutter: more than {} placement blocks - '{}' is cut short", CLUTTER_MAX_TYPES, type.desc.name));
					break;
				}
				types.push_back(gpuType(type.desc, placement, firstMesh, numMeshes));
			}
			flowers += flower ? 1 : 0;
		}
		renderer.setClutterAssets(types, meshes);
		Log::info(oc::format("Clutter: {} types ({} flowers), {} placement rules, {} meshes", m_types.size(), flowers, types.size(), meshes.size()));
	}

	// THE FOREST FLOOR MAP: kick a bake when the camera left the map's centre or the records under it changed; hand a
	// finished bake to the renderer.
	void ClutterSystem::updateFloorMap(Renderer& renderer, const Camera& camera)
	{
		if (m_floorBake && m_floorInFlight.load(oc::memory_order_acquire) == 0)
		{
			Globals::jobSystem.wait(m_floorCounter);
			renderer.setClutterFloorMap(m_floorBake->centre, m_floorBake->texels);
			m_floorSet = true;
			m_floorBake.reset();
		}
		if (m_floorBake)
			return; // one bake at a time

		const TreeWorld& world = Globals::trees.world();
		if (!world.enabled())
		{
			if (m_floorSet)
			{
				renderer.setClutterFloorMap(glm::vec2(0.0f), {});
				m_floorSet = false;
				m_floorCentre = glm::vec2(FLT_MAX);
			}
			return;
		}
		const float chunkSize = world.chunkSize();
		const glm::vec2 cam(camera.position.x, camera.position.z);
		// The records under a map at `centre`: which chunks the CPU holds, and how many records each - a chunk that
		// arrives, leaves or regenerates changes it.
		const auto signatureAt = [&](glm::vec2 centre, glm::ivec2& c0, glm::ivec2& c1) {
			c0 = glm::ivec2(glm::floor((centre - FLOOR_HALF - FLOOR_REACH) / chunkSize));
			c1 = glm::ivec2(glm::floor((centre + FLOOR_HALF + FLOOR_REACH) / chunkSize));
			uint64 h = 0xCBF29CE484222325ull;
			for (int z = c0.y; z <= c1.y; ++z)
				for (int x = c0.x; x <= c1.x; ++x)
				{
					const oc::vector<TreeRecord>* records = world.cpuRecords(glm::ivec2(x, z));
					h = hashCombine(h, ((uint64)(uint32)x << 32) | (uint32)z);
					h = hashCombine(h, records ? (uint64)records->size() : 0xFFFFFFFFull);
				}
			return h;
		};
		// The camera may move this far before the map no longer covers the clutter's range around it.
		const float maxShift = glm::max(FLOOR_HALF - renderer.groundRange() - 2.0f, 4.0f);
		const float rebake = glm::min(glm::max(m_settings.floorRebakeDistance, 1.0f), maxShift);
		glm::ivec2 c0, c1;
		const bool moved = m_floorCentre.x == FLT_MAX || glm::distance(cam, m_floorCentre) > rebake;
		const glm::vec2 centre = moved ? glm::round(cam / CLUTTER_FLOOR_TEXEL) * CLUTTER_FLOOR_TEXEL : m_floorCentre;
		const uint64 signature = signatureAt(centre, c0, c1);
		if (!moved && signature == m_floorSignature && world.generation() == m_floorGeneration)
			return;

		// A snapshot for the job: the records under the map and what their types mean.
		auto bake = oc::make_unique<FloorBake>();
		bake->centre = centre;
		bake->chunkSize = chunkSize;
		bake->worldSeed = world.seed();
		for (const TreeWorld::FloorType& type : world.floorTypes())
		{
			bake->kind.push_back(type.kind);
			bake->crown.push_back(type.crown);
			bake->trunk.push_back(type.trunk);
			bake->rockScale.push_back(type.rockScale);
			bake->footprint.push_back(glm::vec2(type.footprint.halfLength, type.footprint.radius));
		}
		for (int z = c0.y; z <= c1.y; ++z)
			for (int x = c0.x; x <= c1.x; ++x)
				if (const oc::vector<TreeRecord>* records = world.cpuRecords(glm::ivec2(x, z)); records && !records->empty())
				{
					FloorRecords& chunk = bake->chunks.emplace_back();
					chunk.coord = glm::ivec2(x, z);
					chunk.records.resize(records->size());
					memcpy(chunk.records.data(), records->data(), records->size() * sizeof(uint32));
				}
		m_floorCentre = centre;
		m_floorSignature = signature;
		m_floorGeneration = world.generation();
		m_floorBake = oc::move(bake);
		m_floorInFlight.fetch_add(1, oc::memory_order_relaxed);
		Globals::jobSystem.submit([this]
		{
			bakeFloor(*m_floorBake);
			m_floorInFlight.fetch_sub(1, oc::memory_order_release);
		}, { "Clutter floor map", EProfileCategory::Procedural }, EJobPriority::Low, &m_floorCounter);
	}

	// Every record under the map splats its footprint (MAX blend per channel):
	//   a TREE - the canopy (1 inside 0.75 x its crown, none past 1.1 x), the trunk proximity (1 at the trunk, none at
	//            max(3 m, 0.8 x the crown)), occupied inside the trunk;
	//   a ROCK - the rock proximity (1 up to 0.85 x its radius, none at 1.6 x + 1.5 m: the apron at its foot), occupied
	//            inside 0.85 x its radius (its scale is the record's, rockRecordScale);
	//   DEAD WOOD - a fallen trunk's footprint is a capsule along its yaw (TreeWorld::FloorType::footprint): the TRUNK
	//            proximity around it (mushrooms and fallen branches gather at dead wood too), occupied inside it.
	// A rock's distance is to its capsule too (a round rock's has no length).
	void ClutterSystem::bakeFloor(FloorBake& bake)
	{
		const uint32 N = CLUTTER_FLOOR_DIM;
		oc::vector<uint8> channels[4];
		for (oc::vector<uint8>& c : channels)
			c.assign((size_t)N * N, 0);
		const glm::vec2 origin = bake.centre - FLOOR_HALF;
		const float invTexel = 1.0f / CLUTTER_FLOOR_TEXEL;
		// Within `reach` metres of the segment p -/+ half: value(distance) into the channel, max-blended.
		const auto splat = [&](oc::vector<uint8>& channel, glm::vec2 p, glm::vec2 half, float reach, auto&& value) {
			const glm::vec2 extent = glm::abs(half) + reach;
			const glm::ivec2 lo = glm::max(glm::ivec2(glm::floor((p - extent - origin) * invTexel)), glm::ivec2(0));
			const glm::ivec2 hi = glm::min(glm::ivec2(glm::floor((p + extent - origin) * invTexel)), glm::ivec2((int)N - 1));
			const float half2 = glm::dot(half, half);
			for (int y = lo.y; y <= hi.y; ++y)
				for (int x = lo.x; x <= hi.x; ++x)
				{
					const glm::vec2 texel = origin + (glm::vec2((float)x, (float)y) + 0.5f) * CLUTTER_FLOOR_TEXEL;
					const glm::vec2 rel = texel - p;
					const float along = half2 > 1e-8f ? glm::clamp(glm::dot(rel, half) / half2, -1.0f, 1.0f) : 0.0f;
					const float d = glm::length(rel - half * along);
					if (d > reach)
						continue;
					const uint8 v = (uint8)(glm::clamp(value(d), 0.0f, 1.0f) * 255.0f + 0.5f);
					uint8& dst = channel[(size_t)y * N + (size_t)x];
					dst = glm::max(dst, v);
				}
		};
		for (const FloorRecords& chunk : bake.chunks)
		{
			const glm::vec2 chunkOrigin = glm::vec2(chunk.coord) * bake.chunkSize;
			for (const uint32 bits : chunk.records)
			{
				const TreeRecord record{ bits };
				const uint32 type = treeRecordType(record);
				if (type >= bake.kind.size() || bake.kind[type] == 0)
					continue;
				const glm::vec2 p = chunkOrigin + treeRecordLocal(record, bake.chunkSize);
				if (glm::any(glm::lessThan(p, origin - FLOOR_REACH)) || glm::any(glm::greaterThan(p, origin + 2.0f * FLOOR_HALF + FLOOR_REACH)))
					continue;
				if (bake.kind[type] == 1)
				{
					const float crown = glm::max(bake.crown[type], 0.5f);
					const float trunk = glm::max(bake.trunk[type], 0.05f);
					splat(channels[0], p, glm::vec2(0.0f), 1.1f * crown, [&](float d) { return 1.0f - glm::smoothstep(0.75f * crown, 1.1f * crown, d); });
					const float trunkReach = glm::max(3.0f, 0.8f * crown);
					splat(channels[1], p, glm::vec2(0.0f), trunkReach, [&](float d) { const float f = 1.0f - glm::max(d - trunk, 0.0f) / (trunkReach - trunk); return f * f; });
					splat(channels[3], p, glm::vec2(0.0f), trunk + 0.15f, [](float) { return 1.0f; });
					continue;
				}
				// A rock's or dead wood's capsule: the record's scale and yaw (TreeSystem's placeRock: hashes 103, 104).
				const uint32 seed = treeRecordSeed(bake.worldSeed, chunk.coord, record);
				const float scale = rockRecordScale(seed, bake.rockScale[type]);
				const float yaw = treeHash01(treeHash(seed, 104u)) * 6.28318531f;
				const glm::vec2 half = glm::vec2(std::cos(yaw), -std::sin(yaw)) * (bake.footprint[type].x * scale);
				const float radius = bake.footprint[type].y * scale;
				if (bake.kind[type] == 3)
				{
					const float reach = glm::max(radius + 2.5f, 3.0f * radius);
					splat(channels[1], p, half, reach, [&](float d) { const float f = 1.0f - glm::max(d - radius, 0.0f) / (reach - radius); return f * f; });
					splat(channels[3], p, half, radius, [](float) { return 1.0f; });
					continue;
				}
				const float apron = 1.6f * radius + 1.5f;
				splat(channels[2], p, half, apron, [&](float d) { return 1.0f - glm::smoothstep(0.85f * radius, apron, d); });
				splat(channels[3], p, half, 0.85f * radius, [](float) { return 1.0f; });
			}
		}
		bake.texels.resize((size_t)N * N);
		for (size_t i = 0; i < bake.texels.size(); ++i)
			bake.texels[i] = (uint32)channels[0][i] | ((uint32)channels[1][i] << 8) | ((uint32)channels[2][i] << 16) | ((uint32)channels[3][i] << 24);
	}
}
