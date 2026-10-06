module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Tweaks;
import Core.Log;

import RendererVK;
import File;
import Threading;

import :RockSystem;
import :RockType;
import :RockGenerator;
import :TreeGenerator; // treeHash
import :TerrainSampler;

namespace
{
	using namespace Procedural;

	// Raytraced: the LOD chain shares ONE BLAS (RendererVK "RT/BLAS LOD level"), so RT shadows, GI and RTAO see the rock.
	RenderMesh uploadRockMesh(Renderer& renderer, const RockMesh& mesh, const char* name)
	{
		if (mesh.indices.empty())
			return {};
		MeshGeometryDesc geometry;
		geometry.positions = mesh.positions.data();
		geometry.normals = mesh.normals.data();
		geometry.tangents = mesh.tangents.data();
		geometry.bitangents = mesh.bitangents.data();
		geometry.texCoords = mesh.texCoords.data();
		geometry.numVertices = mesh.numVertices();
		geometry.indices = mesh.indices.data();
		geometry.numIndices = (uint32)mesh.indices.size();
		geometry.name = name;
		RenderMeshData data;
		data.build(geometry);
		return renderer.createMesh(data, true);
	}
}

namespace Procedural
{
	Transform RockSystem::groundTransform(glm::vec2 p, const float ground[5], float scale, float height, float sink, float align, const glm::quat& yaw)
	{
		const float r = FOOTPRINT * scale;
		const float h0 = ground[0], hx0 = ground[1], hx1 = ground[2], hz0 = ground[3], hz1 = ground[4];
		const glm::vec3 normal = glm::normalize(glm::vec3(hx0 - hx1, 2.0f * r, hz0 - hz1));
		// `align` of the turn from straight up to the normal, about their common perpendicular (cross(up, normal)).
		const glm::vec3 axis(normal.z, 0.0f, -normal.x);
		const float axisLength = glm::length(axis);
		const glm::quat lean = axisLength > 1e-4f
			? glm::angleAxis(std::acos(glm::clamp(normal.y, -1.0f, 1.0f)) * glm::clamp(align, 0.0f, 1.0f), axis / axisLength)
			: glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		const glm::vec3 up = lean * glm::vec3(0.0f, 1.0f, 0.0f);
		// The ground the footprint rests on: the plane through the four outer points, lowered to the centre where the
		// centre is a dip and by the saddle the plane cannot follow - no edge of the rock floats.
		const float average = 0.25f * (hx0 + hx1 + hz0 + hz1);
		const float saddle = 0.25f * std::abs((hx0 + hx1) - (hz0 + hz1));
		const float base = glm::min(h0, average - saddle);
		// The part of the slope the lean does NOT follow lifts the rock's downhill edge off the ground by r x the
		// tangent of the angle left: it sinks that much more (an upright rock on a slope is buried on its uphill side).
		const float cosLeft = glm::clamp(glm::dot(up, normal), 0.05f, 1.0f);
		const float lift = glm::min(r * std::sqrt(1.0f - cosLeft * cosLeft) / cosLeft, 2.0f * r);
		return Transform(glm::vec3(p.x, base, p.y) - up * (sink * height * scale + lift), scale, lean * yaw);
	}

	RockSystem::~RockSystem()
	{
		m_beforeFree = nullptr; // its owner may be gone already (the procedural globals' order is undefined)
		clearAll();
	}

	void RockSystem::clearAll()
	{
		Globals::jobSystem.wait(m_genCounter); // the generation jobs write into m_types
		m_generating = false;
		// The world's set draws these meshes: it goes first.
		if (!m_worldTypes.empty())
		{
			if (m_beforeFree)
				m_beforeFree();
			m_worldTypes.clear();
			++m_worldGeneration;
		}
		m_nodes.clear();
		for (const Type& type : m_types)
			for (const Variant& variant : type.variants)
				if (variant.chain != UINT32_MAX)
					Globals::rendererVK.freeMeshLodChain(variant.chain); // before its meshes
		m_types.clear();
	}

	void RockSystem::initialize()
	{
		auto respawn = [this]() { m_respawn = true; };
		Tweak::boolean("Rocks", "Enabled", &m_enabled);
		Tweak::boolean("Rocks", "Reload types", &m_reload);
		Tweak::boolean("Rocks", "Respawn preview", &m_respawn);
		Tweak::boolean("Rocks", "Show preview", &m_showPreview, respawn);
		// The WORLD's rocks (TreeWorld's records + TreeSystem's world set; they need "Trees/World/Enabled" too). A
		// change regenerates every record chunk (the trees give way to the rocks).
		Tweak::boolean("Rocks/World", "Enabled", &m_worldEnabled);
		Tweak::floatVar("Rocks/World", "Density scale", &m_worldRules.densityScale, 0.0f, 8.0f, 0.01f);
		// What the types' `Plains` / `Rugged` (.rock Placement) mean: the steepest ground within ~20 m of a rock - at
		// or below "start" it lies on plains, at or above "full" on fully rugged ground.
		Tweak::floatVar("Rocks/World", "Rugged slope start", &m_worldRules.ruggedSlope.x, 0.0f, 2.0f, 0.01f);
		Tweak::floatVar("Rocks/World", "Rugged slope full", &m_worldRules.ruggedSlope.y, 0.0f, 2.0f, 0.01f);
		// What their `Valley` means: low ground, where the heights within ~160 m of the rock range over "start" (no
		// valley) .. "full" metres.
		Tweak::floatVar("Rocks/World", "Valley relief start (m)", &m_worldRules.valleyRelief.x, 0.0f, 500.0f, 0.5f);
		Tweak::floatVar("Rocks/World", "Valley relief full (m)", &m_worldRules.valleyRelief.y, 0.0f, 500.0f, 0.5f);
		// (The LOD level each rock draws is the GPU's pick: the "LOD" tweaks - "Force LOD" shows one level.)
		// The rock material (RendererVK EPipelineIndex::LitRock, "Rocks/Material": the climate's terrain bedrock), or
		// flat grey on LitOpaque - the shape alone.
		static constexpr oc::string_view PREVIEW_SHADINGS[] = { "Rock material (climate)", "Flat grey" };
		Tweak::enumVar("Rocks", "Preview shading", &m_previewShading, PREVIEW_SHADINGS, respawn);
		Tweak::intVar("Rocks", "Grid resolution", &m_gridResolution, 16, 256, 1.0f, [this]() { m_reload = true; });
		Tweak::intVar("Rocks", "Seed", &m_seed, 0, 1000000, 1.0f, respawn);
		Tweak::floatVar("Rocks", "Spacing", &m_spacing, 1.0f, 4.0f, 0.01f, respawn);
	}

	void RockSystem::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		if (!m_enabled)
		{
			// Disabled frees everything, so enabling again re-reads the .rock files.
			if (m_loaded)
			{
				clearAll();
				m_loaded = false;
				m_spawned = false;
			}
			return;
		}

		ProfileScope profileScope("Rocks", EProfileCategory::Procedural);
		if (!m_loaded || m_reload)
		{
			if (m_loaded)
				++m_typesRevision; // a RE-load (the first load follows the enable, which TreeWorld sees itself)
			m_reload = false;
			clearAll();
			reload();
			m_loaded = true;
			m_spawned = false;
		}
		if (m_generating)
		{
			if (m_genInFlight.load(oc::memory_order_acquire) > 0)
				return; // still generating: nothing to draw yet
			finishLoad(renderer);
		}
		if (!m_spawned || m_respawn)
		{
			m_respawn = false;
			m_nodes.clear();
			if (m_showPreview)
				spawnPreview(renderer, camera, maps.get());
			m_spawned = true;
		}
		for (const RenderNode& node : m_nodes)
			renderer.renderNode(node);
	}

	void RockSystem::reload()
	{
		oc::vector<FileSystem::DirEntry> entries;
		{
			const FileSystem::AllowMainThreadIO allowIo; // explicit user action: enable / reload
			if (!FileSystem::listDirectory("Rocks", entries))
			{
				Log::warning("Rocks: no Assets/Rocks directory");
				return;
			}
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
					Log::warning(oc::format("Rocks: failed to load '{}': {}", entry.path, error));
					continue;
				}
			}
			if (desc.name.empty())
				desc.name = entry.name;
			Type& type = m_types.emplace_back();
			type.desc = oc::move(desc);
			type.variants.resize((size_t)type.desc.variantCount);
		}

		// One Low job per variant (each single-threaded inside), in the BACKGROUND: the main thread never waits for
		// generation (seconds in Debug), finishLoad uploads once the last one is done. m_types is not resized until
		// clearAll has joined them.
		for (uint32 t = 0; t < (uint32)m_types.size(); ++t)
		{
			// A type with thin features asks for more cells (`Resolution`).
			const uint32 resolution = (uint32)glm::clamp((float)m_gridResolution * m_types[t].desc.resolution, 16.0f, 256.0f);
			for (uint32 v = 0; v < (uint32)m_types[t].variants.size(); ++v)
			{
				m_genInFlight.fetch_add(1, oc::memory_order_relaxed);
				Globals::jobSystem.submit([this, t, v, resolution]
				{
					Type& type = m_types[t];
					generateRockVariant(type.desc, treeHash(5000u, v), resolution, type.variants[v].data);
					m_genInFlight.fetch_sub(1, oc::memory_order_release);
				}, { "Rock generate", EProfileCategory::Procedural }, EJobPriority::Low, &m_genCounter);
			}
		}
		m_generating = true;
	}

	void RockSystem::finishLoad(Renderer& renderer)
	{
		Globals::jobSystem.wait(m_genCounter); // every job has decremented; this only retires the counter
		m_generating = false;

		m_material = renderer.getOrCreateSolidColorMaterial(glm::vec3(0.42f, 0.40f, 0.38f));
		for (Type& type : m_types)
		{
			size_t triangles[ROCK_MAX_LODS] = {};
			for (Variant& variant : type.variants)
			{
				const RenderMesh* levels[ROCK_MAX_LODS] = {};
				uint32 numLevels = 0;
				for (uint32 k = 0; k < variant.data.lodCount; ++k)
				{
					variant.lods[k] = uploadRockMesh(renderer, variant.data.lods[k], "Rock");
					if (!variant.lods[k].isValid())
						break;
					levels[numLevels++] = &variant.lods[k];
					triangles[k] += variant.data.lods[k].indices.size() / 3;
				}
				// The GPU LOD chain (the cull picks each instance's level by its screen-space error).
				if (numLevels >= 2)
					variant.chain = renderer.createMeshLodChain(oc::span<const RenderMesh* const>(levels, numLevels),
						oc::span<const float>(variant.data.lodError, numLevels));
			}
			const size_t n = glm::max(type.variants.size(), (size_t)1);
			Log::info(oc::format("Rocks: '{}' - {} variants, triangles per variant per LOD: {} / {} / {} / {}",
				type.desc.name, type.variants.size(), triangles[0] / n, triangles[1] / n, triangles[2] / n, triangles[3] / n));
		}

		// The world's view: every type with its uploaded variants (level 0 + its rock-local bounds, as RenderMeshData's:
		// the box centre and half diagonal).
		for (const Type& type : m_types)
		{
			WorldType world;
			world.name = type.desc.name;
			world.scale = type.desc.scale;
			world.sink = type.desc.sink;
			world.align = type.desc.align;
			for (const Variant& variant : type.variants)
			{
				if (!variant.lods[0].isValid())
					continue;
				glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
				for (const glm::vec3& p : variant.data.lods[0].positions)
				{
					lo = glm::min(lo, p);
					hi = glm::max(hi, p);
				}
				world.variants.push_back({ &variant.lods[0], (lo + hi) * 0.5f, glm::length(hi - lo) * 0.5f, variant.data.height,
					variant.data.density.empty() ? nullptr : variant.data.density.data(), variant.data.densityMin, variant.data.densityMax });
			}
			if (!world.variants.empty())
				m_worldTypes.push_back(oc::move(world));
		}
		++m_worldGeneration;
	}

	void RockSystem::spawnPreview(Renderer& renderer, const Camera& camera, const ITerrainSampler* maps)
	{
		if (m_types.empty())
			return;

		// Rows in front of the camera, aligned to its heading: one row per type, one rock per variant.
		glm::vec2 fwd(-camera.viewMatrix[0][2], -camera.viewMatrix[2][2]);
		fwd = glm::dot(fwd, fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec2(0.0f, -1.0f);
		const glm::vec2 right(-fwd.y, fwd.x);
		const glm::vec2 origin = glm::vec2(camera.position.x, camera.position.z) + fwd * 10.0f;
		const FileSystem::AllowMainThreadIO groundIo; // explicit user action: enable / respawn
		auto groundAt = [&](glm::vec2 p) { return maps ? maps->sampleHeight(p.x, p.y) : 0.0f; };

		// LitRock does not read the material (the instance still needs one).
		const RendererVKLayout::EPipelineIndex pipeline = m_previewShading == 0
			? RendererVKLayout::EPipelineIndex::LitRock : RendererVKLayout::EPipelineIndex::LitOpaque;
		float rowOffset = 0.0f;
		for (uint32 t = 0; t < (uint32)m_types.size(); ++t)
		{
			const Type& type = m_types[t];
			const float cell = type.desc.scale.y * m_spacing;
			rowOffset += cell * 0.5f;
			const float rowWidth = cell * (float)type.variants.size();
			for (uint32 v = 0; v < (uint32)type.variants.size(); ++v)
			{
				const Variant& variant = type.variants[v];
				if (!variant.lods[0].isValid())
					continue;
				const uint32 seed = treeHash(treeHash((uint32)m_seed, t), v);
				const float scale = glm::mix(type.desc.scale.x, type.desc.scale.y, treeHash01(treeHash(seed, 1u)));
				const glm::quat yaw = glm::angleAxis(treeHash01(treeHash(seed, 2u)) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
				const glm::vec2 p = origin + fwd * rowOffset + right * (cell * ((float)v + 0.5f) - rowWidth * 0.5f);
				const float r = FOOTPRINT * scale;
				const float ground[5] = { groundAt(p), groundAt(p - glm::vec2(r, 0.0f)), groundAt(p + glm::vec2(r, 0.0f)),
					groundAt(p - glm::vec2(0.0f, r)), groundAt(p + glm::vec2(0.0f, r)) };
				// Level 0: the cull redirects the instance to its LOD through the chain.
				m_nodes.push_back(renderer.spawnMeshNode(variant.lods[0], m_material, pipeline,
					groundTransform(p, ground, scale, variant.data.height, type.desc.sink, type.desc.align, yaw)));
			}
			rowOffset += cell * 0.5f;
		}
	}
}
