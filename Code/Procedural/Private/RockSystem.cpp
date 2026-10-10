module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Log;
import Settings;
import Settings.Tweaks;

import RendererVK;
import File;
import Threading;

import :RockSystem;
import :RockType;
import :RockGenerator;
import :TreeGenerator; // treeHash
import :TreeSpecies;
import :TreeBarkTexture;
import :TerrainSampler;
import :MeshCache;

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

	constexpr uint32 BARK_TEXTURE_SIZE = 1024; // TreeSystem's generated textures

	// A WOOD type's bark: its `Bark` species' texture pair - the files TreeSystem writes once to Assets/Local/Trees/Textures,
	// else generated from Assets/Trees/<bark>.tree (not saved: TreeSystem owns those files) - x the type's tint, with the
	// mip chains BC1 / BC5-compressed. A job: the IO, and seconds of work in Debug. False without a bark.
	bool loadBark(const RockTypeDesc& desc, oc::vector<oc::vector<uint8>>& outAlbedo, oc::vector<oc::vector<uint8>>& outNormal,
		uint32& outSize, glm::vec3& outMean)
	{
		oc::vector<uint8> albedo, normal;
		uint32 w = 0, h = 0, nw = 0, nh = 0;
		const bool loaded = ImageIO::readImageRgba8(oc::format("Local/Trees/Textures/{}_bark.png", desc.bark), w, h, albedo)
			&& ImageIO::readImageRgba8(oc::format("Local/Trees/Textures/{}_bark_normal.png", desc.bark), nw, nh, normal)
			&& w == h && nw == w && nh == h && w > 0 && (w & (w - 1)) == 0;
		uint32 size = w;
		if (!loaded)
		{
			TreeSpeciesDesc species;
			oc::string error;
			if (!loadTreeSpecies(oc::format("Trees/{}.tree", desc.bark), species, error))
			{
				Log::warning(oc::format("Rocks: '{}' - no bark '{}': {}", desc.name, desc.bark, error));
				return false;
			}
			size = BARK_TEXTURE_SIZE;
			generateBarkImages(species, size, albedo, normal);
		}

		// The tint (sRGB), and the far volume's colour: the tinted texture's LINEAR mean (it uploads as sRGB).
		float toLinear[256];
		for (int i = 0; i < 256; ++i)
		{
			const float c = (float)i / 255.0f;
			toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}
		glm::dvec3 sum(0.0);
		for (size_t i = 0; i + 3 < albedo.size(); i += 4)
			for (int c = 0; c < 3; ++c)
			{
				albedo[i + c] = (uint8)glm::min((float)albedo[i + c] * desc.color[c] + 0.5f, 255.0f);
				sum[c] += toLinear[albedo[i + c]];
			}
		outMean = glm::vec3(sum / (double)glm::max(albedo.size() / 4, (size_t)1));

		TreeBarkTexture bark;
		buildBarkMips(albedo, normal, size, bark);
		const auto encode = [size](const oc::vector<oc::vector<uint8>>& mips, TextureConvert::EBlockFormat format, oc::vector<oc::vector<uint8>>& out)
		{
			out.resize(mips.size());
			for (uint32 k = 0; k < (uint32)mips.size(); ++k)
			{
				const uint32 s = glm::max(size >> k, 1u);
				out[k].resize(TextureConvert::compressedSize(s, s, format));
				TextureConvert::compressBlockRows(mips[k].data(), s, s, format, 0, (s + 3) / 4, out[k].data());
			}
		};
		encode(bark.albedoMips, TextureConvert::EBlockFormat::BC1, outAlbedo);
		encode(bark.normalMips, TextureConvert::EBlockFormat::BC5, outNormal);
		outSize = size;
		return true;
	}
}

namespace Procedural
{
	Transform RockSystem::groundTransform(glm::vec2 p, const float ground[5], float r, float scale, float height, float sink, float align, const glm::quat& yaw)
	{
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
		clearMeshes();
		for (const Type& type : m_types)
			if (type.woodMaterial != UINT16_MAX)
				Globals::rendererVK.destroyTextureMaterial(type.woodMaterial);
		m_types.clear();
	}

	void RockSystem::clearMeshes()
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
		for (Type& type : m_types)
		{
			for (const Variant& variant : type.variants)
				if (variant.chain != UINT32_MAX)
					Globals::rendererVK.freeMeshLodChain(variant.chain); // before its meshes
			const size_t count = type.variants.size();
			type.variants.clear(); // the meshes
			type.variants.resize(count);
		}
	}

	void RockSystem::initialize()
	{
		// "Grid resolution": the meshes only - the types (and so TreeWorld's placement) stay as read.
		Tweak::onChange(m_settings.gridResolution, this, [this]() { m_remesh = true; });
	}

	void RockSystem::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		if (!m_settings.enabled)
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
		if (!m_loaded || m_settings.reload)
		{
			if (m_loaded)
				++m_typesRevision; // a RE-load (the first load follows the enable, which TreeWorld sees itself)
			m_settings.reload = false;
			m_remesh = false;
			clearAll();
			reload();
			m_loaded = true;
			m_spawned = false;
		}
		else if (m_remesh)
		{
			m_remesh = false;
			clearMeshes();
			kickGeneration();
			m_spawned = false;
		}
		if (m_generating)
		{
			if (m_genInFlight.load(oc::memory_order_acquire) > 0)
				return; // still generating: nothing to draw yet
			finishLoad(renderer);
		}
		if (!m_spawned || m_settings.respawn)
		{
			m_settings.respawn = false;
			m_nodes.clear();
			if (m_settings.showPreview)
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
			uint64 meshHash = 0;
			{
				const FileSystem::AllowMainThreadIO allowIo;
				if (!loadRockType(entry.path, desc, error))
				{
					Log::warning(oc::format("Rocks: failed to load '{}': {}", entry.path, error));
					continue;
				}
				meshHash = meshCacheHash(FileSystem::readFileStr(entry.path));
			}
			if (desc.name.empty())
				desc.name = entry.name;
			Type& type = m_types.emplace_back();
			type.desc = oc::move(desc);
			type.meshHash = meshHash;
			type.variants.resize((size_t)type.desc.variantCount);
		}
		kickGeneration();
	}

	void RockSystem::kickGeneration()
	{
		// One Low job per variant (each single-threaded inside), in the BACKGROUND: the main thread never waits for
		// generation (seconds in Debug), finishLoad uploads once the last one is done. m_types is not resized until
		// clearAll has joined them.
		for (uint32 t = 0; t < (uint32)m_types.size(); ++t)
		{
			// A type with thin features asks for more cells (`Resolution`).
			const uint32 resolution = (uint32)glm::clamp((float)m_settings.gridResolution * m_types[t].desc.resolution, 16.0f, 256.0f);
			for (uint32 v = 0; v < (uint32)m_types[t].variants.size(); ++v)
			{
				m_genInFlight.fetch_add(1, oc::memory_order_relaxed);
				Globals::jobSystem.submit([this, t, v, resolution]
				{
					// THE MESH CACHE (MeshCache): keyed by the .rock file's text, the grid resolution and the variant's seed.
					Type& type = m_types[t];
					const uint32 seed = treeHash(5000u, v);
					const uint64 hash = meshCacheMix(meshCacheMix(type.meshHash, resolution), seed);
					const oc::string path = oc::format("{}/{}_v{}.rockmesh", ROCK_MESH_DIR, type.desc.name, v);
					if (!loadRockVariant(path, hash, type.variants[v].data))
					{
						type.variants[v].data = {};
						generateRockVariant(type.desc, seed, resolution, type.variants[v].data);
						saveRockVariant(path, hash, type.variants[v].data);
					}
					m_genInFlight.fetch_sub(1, oc::memory_order_release);
				}, { "Rock generate", EProfileCategory::Procedural }, EJobPriority::Low, &m_genCounter);
			}
			// A wood type's bark texture (writes only the type's bark fields; the variant jobs write their variants).
			if (m_types[t].desc.surface == ERockSurface::Wood && m_types[t].woodMaterial == UINT16_MAX)
			{
				m_genInFlight.fetch_add(1, oc::memory_order_relaxed);
				Globals::jobSystem.submit([this, t]
				{
					Type& type = m_types[t];
					if (!loadBark(type.desc, type.barkAlbedo, type.barkNormal, type.barkSize, type.barkMean))
						type.barkSize = 0;
					m_genInFlight.fetch_sub(1, oc::memory_order_release);
				}, { "Rock bark", EProfileCategory::Procedural }, EJobPriority::Low, &m_genCounter);
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
			// DEAD WOOD's material: the bark its job loaded. GI and the RT hits sample the same texture: wood-coloured
			// bounce light.
			if (type.woodMaterial == UINT16_MAX && type.barkSize > 0)
			{
				oc::vector<oc::span<uint8>> albedoMips, normalMips;
				for (oc::vector<uint8>& level : type.barkAlbedo)
					albedoMips.push_back(oc::span<uint8>(level.data(), level.size()));
				for (oc::vector<uint8>& level : type.barkNormal)
					normalMips.push_back(oc::span<uint8>(level.data(), level.size()));
				type.woodMaterial = renderer.createTextureMaterial(type.barkSize, type.barkSize, albedoMips, 0.0f,
					oc::format("Rocks/{}", type.desc.name).c_str(), &normalMips, 0u, Renderer::ETextureEncoding::BC1, Renderer::ETextureEncoding::BC5);
				type.barkAlbedo = {};
				type.barkNormal = {};
			}
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
			world.footprint = rockFootprint(type.desc).flat;
			world.wood = type.woodMaterial != UINT16_MAX;
			world.material = world.wood ? type.woodMaterial : m_material;
			world.woodAlbedo = type.barkMean; // the far volume's colour
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
		// The CPU meshes are uploaded: only the shape's numbers and the density grid are read from here on.
		for (Type& type : m_types)
			for (Variant& variant : type.variants)
				for (RockMesh& mesh : variant.data.lods)
					mesh = {};
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

		// LitRock reads only a wood type's material (its bark); a rock's instance still needs one.
		const RendererVKLayout::EPipelineIndex pipeline = m_settings.previewShading == 0
			? RendererVKLayout::EPipelineIndex::LitRock : RendererVKLayout::EPipelineIndex::LitOpaque;
		float rowOffset = 0.0f;
		for (uint32 t = 0; t < (uint32)m_types.size(); ++t)
		{
			const Type& type = m_types[t];
			const float cell = type.desc.scale.y * m_settings.spacing;
			rowOffset += cell * 0.5f;
			const float rowWidth = cell * (float)type.variants.size();
			for (uint32 v = 0; v < (uint32)type.variants.size(); ++v)
			{
				const Variant& variant = type.variants[v];
				if (!variant.lods[0].isValid())
					continue;
				const uint32 seed = treeHash(treeHash((uint32)m_settings.seed, t), v);
				const float scale = glm::mix(type.desc.scale.x, type.desc.scale.y, treeHash01(treeHash(seed, 1u)));
				const glm::quat yaw = glm::angleAxis(treeHash01(treeHash(seed, 2u)) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
				const glm::vec2 p = origin + fwd * rowOffset + right * (cell * ((float)v + 0.5f) - rowWidth * 0.5f);
				const float r = FOOTPRINT * scale * rockFootprint(type.desc).flat;
				const float ground[5] = { groundAt(p), groundAt(p - glm::vec2(r, 0.0f)), groundAt(p + glm::vec2(r, 0.0f)),
					groundAt(p - glm::vec2(0.0f, r)), groundAt(p + glm::vec2(0.0f, r)) };
				// Level 0: the cull redirects the instance to its LOD through the chain.
				const bool woodColours = type.woodMaterial != UINT16_MAX && pipeline == RendererVKLayout::EPipelineIndex::LitRock;
				m_nodes.push_back(renderer.spawnMeshNode(variant.lods[0], woodColours ? type.woodMaterial : m_material, pipeline,
					groundTransform(p, ground, r, scale, variant.data.height, type.desc.sink, type.desc.align, yaw)));
			}
			rowOffset += cell * 0.5f;
		}
	}
}
