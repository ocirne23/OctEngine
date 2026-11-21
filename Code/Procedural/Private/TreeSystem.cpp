module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Log;
import Settings;
import Settings.Tweaks;
import Core.Sphere;

import RendererVK;
import File;
import Physics;

import :TreeSystem;
import :TreeSpecies;
import :TreeGenerator;
import :TreeLeafTexture;
import :TreeBarkTexture;
import :TreeImpostor;
import :TerrainSampler;
import :TerrainStreamer;
import :RockSystem;
import :RockGenerator; // ROCK_DENSITY_RES
import :MeshCache;

namespace
{
	using namespace Procedural;

	// `raytraced`: whether RT (GI, RT shadows, RTAO, reflections) sees it - a BLAS only then (TreeSystem::uploadSpecies:
	// the trees' shadow billboards; bushes never).
	RenderMesh uploadTreeMesh(Renderer& renderer, const TreeMesh& mesh, bool raytraced)
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
		geometry.name = "TreePiece";
		RenderMeshData data;
		data.build(geometry);
		return renderer.createMesh(data, raytraced);
	}

	size_t numTriangles(const TreeMesh& mesh) { return mesh.indices.size() / 3; }

	// Uploads LEVEL 0 only, no GPU LOD chain (a one-level RenderMesh): trees switch representation through their own tiers
	// (the voxels, the far volume), and the mesh LOD steps underneath popped visibly (fewer, larger leaf cards per
	// level, no crossfade). The generator still builds every level (the bakes read level 0); outMeshes[1..] stay empty.
	void uploadLevelZero(Renderer& renderer, const TreeMesh (&levels)[TREE_PIECE_LODS], RenderMesh (&outMeshes)[TREE_PIECE_LODS], bool raytraced)
	{
		outMeshes[0] = uploadTreeMesh(renderer, levels[0], raytraced);
	}

	// Species textures are generated ONCE into Assets/Local/Trees/Textures (TREE_TEXTURE_DIR: <species>_bark.tex,
	// _leaves.tex, the shadow masks, the voxel grids) in their UPLOAD encoding (MeshCache's CachedTexture) and read back on later loads.
	// Generated output: not in git.

	constexpr uint32 TREE_TEXTURE_SIZE = 1024;
	constexpr uint32 TREE_DENSITY_RES = 32; // the far-tree volume's per-variant extinction grid (bakeTreeDensity)
	constexpr uint32 SHADOW_MASK_VERSION = 5; // in the shadow masks' cache key: bump when bakeBillboardMask's output changes

	// A bush's `Climate` may list several names (space-separated): it grows around the trees of ANY of them (case-
	// insensitive). A tree's climate is one name.
	bool bushClimateMatches(const oc::string& bushClimates, const oc::string& treeClimate)
	{
		size_t i = 0;
		while (i < bushClimates.size())
		{
			while (i < bushClimates.size() && bushClimates[i] == ' ')
				++i;
			size_t end = i;
			while (end < bushClimates.size() && bushClimates[end] != ' ')
				++end;
			if (end - i == treeClimate.size() && end > i)
			{
				bool same = true;
				for (size_t k = 0; k < treeClimate.size() && same; ++k)
					same = std::tolower((unsigned char)bushClimates[i + k]) == std::tolower((unsigned char)treeClimate[k]);
				if (same)
					return true;
			}
			i = end;
		}
		return false;
	}

	// The LINEAR mean albedo of an RGBA8 leaf image, alpha-weighted: albedo textures upload as sRGB, so each texel
	// is decoded before the sum (the far-tree volume's leaf colour must match what the meshes show).
	glm::vec3 meanLeafAlbedo(const oc::vector<uint8>& rgba, const glm::vec3& fallback)
	{
		auto decode = [](uint8 v)
		{
			const float c = (float)v / 255.0f;
			return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		};
		glm::vec3 sum(0.0f);
		float weight = 0.0f;
		for (size_t i = 0; i + 3 < rgba.size(); i += 4)
		{
			const float a = (float)rgba[i + 3] / 255.0f;
			sum += glm::vec3(decode(rgba[i]), decode(rgba[i + 1]), decode(rgba[i + 2])) * a;
			weight += a;
		}
		return weight > 0.0f ? sum / weight : fallback;
	}

	// "Trees/Compress textures": an RGBA8 mip chain (levels halving from width x height) as BC blocks, one buffer per
	// level, every level's block rows spread over the job system.
	void encodeChain(const oc::vector<oc::vector<uint8>>& rgba, uint32 width, uint32 height, TextureConvert::EBlockFormat format,
		oc::vector<oc::vector<uint8>>& out)
	{
		out.resize(rgba.size());
		oc::vector<glm::uvec2> tasks; // (level, block row)
		for (uint32 k = 0; k < (uint32)rgba.size(); ++k)
		{
			const uint32 w = glm::max(width >> k, 1u), h = glm::max(height >> k, 1u);
			out[k].resize(TextureConvert::compressedSize(w, h, format));
			for (uint32 row = 0; row < (h + 3) / 4; ++row)
				tasks.push_back({ k, row });
		}
		Globals::jobSystem.parallelFor(0, (uint32)tasks.size(), 4, { "Tree texture BC", EProfileCategory::Procedural },
			[&](uint32 begin, uint32 end)
		{
			for (uint32 i = begin; i < end; ++i)
			{
				const uint32 k = tasks[i].x;
				const uint32 w = glm::max(width >> k, 1u), h = glm::max(height >> k, 1u);
				TextureConvert::compressBlockRows(rgba[k].data(), w, h, format, tasks[i].y, tasks[i].y + 1, out[k].data());
			}
		});
	}

	RenderMaterial::ETextureEncoding textureEncoding(TextureConvert::EBlockFormat format)
	{
		switch (format)
		{
		case TextureConvert::EBlockFormat::BC1: return RenderMaterial::ETextureEncoding::BC1;
		case TextureConvert::EBlockFormat::BC3: return RenderMaterial::ETextureEncoding::BC3;
		case TextureConvert::EBlockFormat::BC4: return RenderMaterial::ETextureEncoding::BC4;
		default:                                return RenderMaterial::ETextureEncoding::BC5;
		}
	}

	// A generated pair of RGBA8 mip chains into its UPLOAD encoding (the .tex cache's content): BC in each map's format when
	// `compress`, else the RGBA8 chains as they are (moved out of `albedo` / `normal`).
	void encodeTexture(bool compress, uint32 width, uint32 height, oc::vector<oc::vector<uint8>>& albedo, TextureConvert::EBlockFormat albedoFormat,
		oc::vector<oc::vector<uint8>>& normal, TextureConvert::EBlockFormat normalFormat, CachedTexture& out)
	{
		out.width = width;
		out.height = height;
		if (!compress)
		{
			out.albedo = oc::move(albedo);
			out.normal = oc::move(normal);
			out.albedoEncoding = out.normalEncoding = RenderMaterial::ETextureEncoding::Rgba8;
			return;
		}
		encodeChain(albedo, width, height, albedoFormat, out.albedo);
		encodeChain(normal, width, height, normalFormat, out.normal);
		out.albedoEncoding = textureEncoding(albedoFormat);
		out.normalEncoding = textureEncoding(normalFormat);
	}

	// A SHADOW BILLBOARD's mask (bakeBillboardMask's two-channel levels, taken: the cover / bark share, the depth) into its
	// UPLOAD encoding: BC5 when `compress`, else RG8.
	void encodeMask(bool compress, uint32 size, oc::vector<oc::vector<uint8>>& levels, CachedTexture& out)
	{
		out = {};
		out.width = size;
		out.height = size;
		if (!compress)
		{
			out.albedo = oc::move(levels);
			out.albedoEncoding = RenderMaterial::ETextureEncoding::Rg8;
			return;
		}
		// The block compressor reads RGBA texels (BC5: their red and green).
		oc::vector<oc::vector<uint8>> rgba(levels.size());
		for (size_t k = 0; k < levels.size(); ++k)
		{
			const size_t texels = levels[k].size() / 2;
			rgba[k].assign(texels * 4, 0);
			for (size_t i = 0; i < texels; ++i)
			{
				rgba[k][i * 4] = levels[k][i * 2];
				rgba[k][i * 4 + 1] = levels[k][i * 2 + 1];
			}
		}
		levels = {};
		encodeChain(rgba, size, size, TextureConvert::EBlockFormat::BC5, out.albedo);
		out.albedoEncoding = RenderMaterial::ETextureEncoding::BC5;
	}

	// The VOXEL tier's bake (TreeImpostor.ixx) into its UPLOAD encoding, two .tex files: `atlases` = the opacity atlas (the
	// albedo: BC4, else R8) and the surface atlas (the normal: BC3, else RGBA8 - half the opacity atlas each way), `index` =
	// the index texture (RGBA8: its bytes are exact). Takes the bake's images.
	void encodeVoxelTextures(bool compress, TreeVoxelBake& bake, CachedTexture& atlases, CachedTexture& index)
	{
		atlases = {};
		atlases.width = bake.width;
		atlases.height = bake.height;
		if (compress)
		{
			// The block compressor reads RGBA texels (BC4: their red).
			oc::vector<oc::vector<uint8>> rgba(1);
			rgba[0].assign(bake.opacity.size() * 4, 0);
			for (size_t i = 0; i < bake.opacity.size(); ++i)
				rgba[0][i * 4] = bake.opacity[i];
			bake.opacity = {};
			encodeChain(rgba, bake.width, bake.height, TextureConvert::EBlockFormat::BC4, atlases.albedo);
			rgba[0] = oc::move(bake.surface);
			encodeChain(rgba, bake.width / 2, bake.height / 2, TextureConvert::EBlockFormat::BC3, atlases.normal);
			atlases.albedoEncoding = RenderMaterial::ETextureEncoding::BC4;
			atlases.normalEncoding = RenderMaterial::ETextureEncoding::BC3;
		}
		else
		{
			atlases.albedo.push_back(oc::move(bake.opacity));
			atlases.normal.push_back(oc::move(bake.surface));
			atlases.albedoEncoding = RenderMaterial::ETextureEncoding::R8;
			atlases.normalEncoding = RenderMaterial::ETextureEncoding::Rgba8;
		}
		index = {};
		index.width = bake.indexWidth;
		index.height = bake.indexHeight;
		index.albedo.push_back(oc::move(bake.index));
	}

	// The .tex file's key: the species file's text, "Compress textures" and the level-0 size.
	uint64 textureCacheHash(uint64 speciesTextHash, bool compress, uint32 size)
	{
		return meshCacheMix(meshCacheMix(speciesTextHash, compress ? 1u : 0u), size);
	}

	// On the species' load job (TreeSystem::loadSpecies).
	void saveTexture(const oc::string& path, uint64 hash, const CachedTexture& texture)
	{
		saveCachedTexture(path, hash, texture);
		Log::info(oc::format("Trees: generated '{}'", path));
	}

	RenderMaterial createTreeMaterial(CachedTexture& texture, float alphaCutoff, const char* debugName, uint32 extraFlags)
	{
		oc::vector<oc::span<uint8>> albedo, normal;
		for (oc::vector<uint8>& level : texture.albedo)
			albedo.push_back(oc::span<uint8>(level.data(), level.size()));
		for (oc::vector<uint8>& level : texture.normal)
			normal.push_back(oc::span<uint8>(level.data(), level.size()));
		RenderMaterial::TextureDesc desc;
		desc.width = texture.width;
		desc.height = texture.height;
		desc.albedoMips = &albedo;
		desc.normalMips = normal.empty() ? nullptr : &normal;
		desc.albedoEncoding = texture.albedoEncoding;
		desc.normalEncoding = texture.normalEncoding;
		desc.alphaCutoff = alphaCutoff;
		desc.extraFlags = extraFlags;
		desc.debugName = debugName;
		return RenderMaterial::createTextured(desc);
	}

	// A SHADOW BILLBOARD's material (RendererVK MATERIAL_FLAG_BILLBOARD): its mask in the albedo slot, alpha-tested at the
	// mask's own threshold; its two colours for the rays (linear: the bark's, the leaves') as a 2 x 1 texture in the normal
	// slot, stored as their square roots (tree_card.inc.glsl squares the tap).
	RenderMaterial createBillboardMaterial(CachedTexture& mask, const glm::vec3& barkColour, const glm::vec3& leafColour, const char* debugName)
	{
		oc::vector<oc::span<uint8>> maskMips, colourMips;
		for (oc::vector<uint8>& level : mask.albedo)
			maskMips.push_back(oc::span<uint8>(level.data(), level.size()));
		uint8 colours[8];
		const glm::vec3 pair[2] = { barkColour, leafColour };
		for (uint32 t = 0; t < 2; ++t)
		{
			for (uint32 c = 0; c < 3; ++c)
				colours[t * 4 + c] = (uint8)(std::sqrt(glm::clamp(pair[t][(int)c], 0.0f, 1.0f)) * 255.0f + 0.5f);
			colours[t * 4 + 3] = 255;
		}
		colourMips.push_back(oc::span<uint8>(colours, sizeof(colours)));
		RenderMaterial::TextureDesc desc;
		desc.width = mask.width;
		desc.height = mask.height;
		desc.albedoMips = &maskMips;
		desc.albedoEncoding = mask.albedoEncoding;
		desc.normalMips = &colourMips;
		desc.normalWidth = 2;
		desc.normalHeight = 1;
		desc.alphaCutoff = 0.5f;
		desc.extraFlags = RendererVKLayout::MATERIAL_FLAG_BILLBOARD;
		desc.debugName = debugName;
		return RenderMaterial::createTextured(desc);
	}

	// The VOXEL tier's material (the opacity atlas in the albedo slot, the surface atlas in the normal slot) and its index
	// texture (`outIndex`, linear: exact bytes). The opacity atlas's HEADER - slot 0, no brick: its first two 4 x 4 blocks -
	// takes the index texture's bindless id, low byte then high: the main pass and the shadow casters, which get the opacity
	// atlas alone, both find the index through it (tree_voxel.inc.glsl voxelGrid).
	RenderMaterial createVoxelMaterial(CachedTexture& atlases, CachedTexture& index, RenderTexture& outIndex, const char* debugName, uint32 extraFlags)
	{
		oc::vector<oc::span<uint8>> indexMips, opacityMips, surfaceMips;
		indexMips.push_back(oc::span<uint8>(index.albedo[0].data(), index.albedo[0].size()));
		outIndex = RenderTexture::create(index.width, index.height, indexMips, oc::format("{}/index", debugName).c_str(),
			RenderMaterial::ETextureEncoding::Rgba8, false);
		const uint8 lo = (uint8)(outIndex.index() & 255u), hi = (uint8)(outIndex.index() >> 8);
		oc::vector<uint8>& opacity = atlases.albedo[0];
		if (atlases.albedoEncoding == RenderMaterial::ETextureEncoding::BC4)
		{
			// A BC4 block of two equal endpoints whose selectors are all 0 decodes to exactly that value.
			const uint8 blocks[16] = { lo, lo, 0, 0, 0, 0, 0, 0, hi, hi, 0, 0, 0, 0, 0, 0 };
			memcpy(opacity.data(), blocks, sizeof(blocks));
		}
		else
		{
			for (uint32 y = 0; y < 4; ++y)
				for (uint32 x = 0; x < 8; ++x)
					opacity[(size_t)y * atlases.width + x] = x < 4 ? lo : hi;
		}
		opacityMips.push_back(oc::span<uint8>(opacity.data(), opacity.size()));
		surfaceMips.push_back(oc::span<uint8>(atlases.normal[0].data(), atlases.normal[0].size()));
		RenderMaterial::TextureDesc desc;
		desc.width = atlases.width;
		desc.height = atlases.height;
		desc.albedoMips = &opacityMips;
		desc.normalMips = &surfaceMips;
		desc.normalWidth = atlases.width / 2;
		desc.normalHeight = atlases.height / 2;
		desc.albedoEncoding = atlases.albedoEncoding;
		desc.normalEncoding = atlases.normalEncoding;
		desc.extraFlags = extraFlags;
		desc.debugName = debugName;
		return RenderMaterial::createTextured(desc);
	}

	// A tree species' UNDERGROWTH: x = its bushes per tree, y = how far out they go (m, at least the trunk's 1.5 m). The
	// species' `Undergrowth` block, else "Bushes per tree" and DEFAULT_BUSH_RADIUS. The world expansion and the far
	// volume's record types (TreeRecordTypeGpu, per type) both read it.
	constexpr float DEFAULT_BUSH_RADIUS = 8.25f;
	glm::vec2 bushUndergrowth(const TreeSpeciesDesc& desc, const TreeSettings& settings)
	{
		const float perTree = desc.undergrowthBushes >= 0.0f ? desc.undergrowthBushes : settings.bushesPerTree;
		const float radius = desc.undergrowthRadius >= 0.0f ? desc.undergrowthRadius : DEFAULT_BUSH_RADIUS;
		return glm::vec2(glm::max(perTree, 0.0f), glm::max(radius, 1.5f));
	}
}

namespace Procedural
{
	TreeSystem::~TreeSystem()
	{
		// Globals::terrain may be gone already (the procedural globals' order is undefined): its walk ended with the
		// frame loop and never calls the sink again.
		m_vegHooked = false;
		stopExpansion();
		clearAll();
	}

	void TreeSystem::clearAll()
	{
		stopLoad(); // its jobs write only their slots: joined before the species they upload into go
		destroyTreeSet();
		m_species.clear(); // the meshes and the materials (Species: the derived ones before their sources)
	}

	void TreeSystem::destroyTreeSet()
	{
		stopExpansion();
		m_near.clear(); // the collider bodies first: they use the rock meshes below
		m_colliderBodies = 0;
		m_rockColliderMeshes.clear();
		m_expandContext = nullptr;
		// Without the world set the far volume has no record types (it ignores the records).
		if (m_recordTypesSet)
		{
			m_world.joinPool(); // the pool job writes the same record pool
			Globals::rendererVK.setTreeRecordTypes({}, 256.0f, 1, RenderTreeSet());
		}
		m_recordTypesSet = false;
		// Unhook from the terrain first: setVegetation joins its walk, the last caller of the sink.
		if (m_vegHooked)
			Globals::terrain.setVegetation({}, {}, 0);
		m_vegHooked = false;
		m_vegChunkOf.clear();
		m_prevSetKeys.clear();
		Globals::rendererVK.setTreeNearChunks({});
		m_treeSet.destroy();
		m_setHasRocks = false;
	}

	void TreeSystem::applyFadeBands(Renderer& renderer)
	{
		constexpr uint32 FADE_BITS = RendererVKLayout::MATERIAL_FADE_BAND_MASK | RendererVKLayout::MATERIAL_FLAG_DISTANCE_FADE
			| RendererVKLayout::MATERIAL_FLAG_FADE_IN;
		for (Species& species : m_species)
		{
			if (!species.barkFadeMaterial.isValid())
				continue;
			// Sets a material's fade band, keeping its other flags; an invalid material (none) is skipped.
			auto setFade = [&](RenderMaterial& material, uint32 fade)
			{
				if (material.isValid())
					material.setFlags((material.getFlags() & ~FADE_BITS) | fade);
			};
			const float width = fadeWidth(species);
			const float start = glm::max(farDistance(species) * m_settings.farDistanceScale - width * 0.5f, 0.0f);
			const uint32 fadeOut = RendererVKLayout::makeDistanceFadeFlags(start, width, false);
			const uint32 fadeIn = RendererVKLayout::makeDistanceFadeFlags(start, width, true);
			setFade(species.barkFadeMaterial, fadeOut);
			setFade(species.leafFadeMaterial, fadeOut);
			// Force far draws the voxels at every distance: no fade-in then, or they would vanish nearby.
			const uint32 voxelFade = m_settings.forceFar ? 0u : fadeIn;
			for (PieceMeshes& meshes : species.variantMeshes)
				setFade(meshes.voxelMaterial, voxelFade);
		}
	}

	float TreeSystem::fadeWidth(const Species& species) const
	{
		return glm::clamp(species.desc.billboardFadeWidth * m_settings.fadeWidthScale, 1.0f, 1023.0f); // the material packs 10 bits
	}

	// "Voxel start (m)" puts a TREE's band start there in metres: unscaled, so the scale the culls and the fade bands apply
	// lands it back on that start (a "Far distance scale" change respawns then, initialize).
	float TreeSystem::farDistance(const Species& species) const
	{
		if (!species.desc.bush && m_settings.voxelStart > 0.0f && m_settings.farDistanceScale > 0.0f)
			return (m_settings.voxelStart + 0.5f * fadeWidth(species)) / m_settings.farDistanceScale;
		return species.desc.billboardDistance;
	}

	void TreeSystem::initialize()
	{
		// The "Trees" buttons and respawn / reload triggers are the settings' own (Settings::registerTrees); the fade
		// bands are this system's.
		const auto fadeBandsDirty = [this]() { m_fadeBandsDirty = true; };
		// With "Voxel start" the trees' distances are stored unscaled for the scale of their spawn: a new scale respawns.
		Tweak::onChange(m_settings.farDistanceScale, this, [this]() { m_fadeBandsDirty = true; if (m_settings.voxelStart > 0.0f) m_settings.respawn = true; });
		Tweak::onChange(m_settings.fadeWidthScale, this, fadeBandsDirty);
		Tweak::onChange(m_settings.voxelStart, this, fadeBandsDirty);
		Tweak::onChange(m_settings.forceFar, this, fadeBandsDirty);
		m_world.initialize();
		// The world set draws RockSystem's meshes (spawnWorld): before RockSystem frees them (a reload, a disable), the
		// set goes - the next update spawns it again, without them until they are back.
		Globals::rocks.setMeshUser([this]()
		{
			if (!m_setHasRocks)
				return;
			destroyTreeSet();
			m_spawned = false;
		});
	}

	void TreeSystem::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		// The world's ROCK records (RockSystem's "Rocks/World" tweaks; a change is a new TreeWorld generation).
		m_world.setRocks(Globals::rocks.worldEnabled(), Globals::rocks.worldRules(), Globals::rocks.typesRevision());
		m_world.update(renderer, camera, maps);
		if (!m_settings.enabled)
		{
			// Disabled frees everything, so enabling again re-reads the .tree files.
			if (m_loaded || m_loading)
			{
				clearAll();
				m_loaded = false;
				m_spawned = false;
			}
			return;
		}

		ProfileScope profileScope("Trees", EProfileCategory::Procedural);
		if ((!m_loaded && !m_loading) || m_settings.reload)
		{
			m_settings.reload = false;
			clearAll(); // joins a load in flight
			kickLoad();
			m_loaded = false;
			m_spawned = false;
		}
		// The species load on its jobs: each finished species uploads here; nothing spawns until all are in.
		if (m_loading)
		{
			if (!pumpLoad(renderer))
				return;
			m_settings.regenerateTextures = false;
			m_loaded = true;
			m_spawned = false;
		}
		// The terrain's chunk size changed: the set's chunks are the old size.
		if (m_vegHooked && m_vegChunkSize != Globals::terrain.chunkSize())
			m_settings.respawn = true;
		// WORLD MODE: TreeWorld's records on the terrain; without either nothing draws. Switching, or new records (a
		// TreeWorld restart), respawns.
		const bool worldMode = m_world.enabled() && maps != nullptr;
		m_world.requireKeepRadius(worldMode ? m_settings.nearRadius + 1 : 0);
		if (worldMode != m_worldMode || (worldMode && m_world.generation() != m_worldGeneration))
			m_settings.respawn = true;
		// The rock meshes arrived, changed or went (RockSystem loads in the background): the set's types are fixed, so a
		// new set.
		if (worldMode && Globals::rocks.worldGeneration() != m_rockGeneration)
			m_settings.respawn = true;
		// A respawn hooks the new set into the terrain AFTER this frame's walk (setVegetation joins it): this frame
		// draws every chunk itself.
		const bool respawned = !m_spawned || m_settings.respawn;
		if (!m_spawned || m_settings.respawn)
		{
			m_settings.respawn = false;
			destroyTreeSet();
			m_worldMode = worldMode;
			if (worldMode)
				spawnWorld(renderer, maps);
			m_spawned = true;
		}
		if (m_worldMode)
			updateWorld(renderer, camera);
		if (m_fadeBandsDirty)
		{
			m_fadeBandsDirty = false;
			applyFadeBands(renderer);
		}

		// The culls decide per tree (tree_cull.inc.glsl). The terrain's walk lists the chunks (the sink, hookTerrain);
		// without it - no walk this frame, or the frame of a respawn - every chunk draws.
		m_sinkDistanceScale.store(m_settings.farDistanceScale, oc::memory_order_relaxed);
		m_sinkForceFar.store(m_settings.forceFar, oc::memory_order_relaxed);
		if (m_treeSet.isValid())
		{
			m_treeSet.bind();
			if (!m_vegHooked || !Globals::terrain.vegetationRouted() || respawned)
			{
				m_vegFallback.clear();
				for (const auto& [key, chunk] : m_vegChunkOf)
					m_vegFallback.push_back({ (uint32)chunk, RendererVKLayout::PASS_ALL });
				m_treeSet.render(m_vegFallback, m_settings.farDistanceScale, m_settings.forceFar);
			}
		}
	}

	void TreeSystem::kickLoad()
	{
		m_loading = true; // also with no species: pumpLoad then finishes at once
		oc::vector<FileSystem::DirEntry> entries;
		{
			const FileSystem::AllowMainThreadIO allowIo; // explicit user action: enable / reload
			if (!FileSystem::listDirectory("Trees", entries))
			{
				Log::warning("Trees: no Assets/Trees directory");
				return;
			}
		}
		oc::sort(entries.begin(), entries.end(), [](const FileSystem::DirEntry& a, const FileSystem::DirEntry& b) { return a.name < b.name; });
		LoadSettings settings;
		settings.compressTextures = m_settings.compressTextures;
		settings.regenerateTextures = m_settings.regenerateTextures;
		settings.voxelRes = m_settings.voxelImpostors ? 32u << glm::clamp(m_settings.voxelResolution, 0, 3) : 0u;
		for (const FileSystem::DirEntry& entry : entries)
		{
			if (entry.isDirectory || entry.extension != ".tree")
				continue;
			auto slot = oc::make_unique<LoadSlot>();
			slot->load.path = entry.path;
			slot->load.fileName = entry.name;
			m_loadSlots.push_back(oc::move(slot));
		}
		m_species.clear();
		m_species.resize(m_loadSlots.size());
		for (oc::unique_ptr<LoadSlot>& slot : m_loadSlots)
			Globals::jobSystem.submit([s = slot.get(), settings]
			{
				loadSpecies(settings, s->load);
				s->done.store(true, oc::memory_order_release);
			}, { "Tree species load", EProfileCategory::Procedural }, EJobPriority::Low, &m_loadCounter);
	}

	bool TreeSystem::pumpLoad(Renderer& renderer)
	{
		bool all = true;
		for (size_t i = 0; i < m_loadSlots.size(); ++i)
		{
			LoadSlot& slot = *m_loadSlots[i];
			if (slot.uploaded)
				continue;
			if (!slot.done.load(oc::memory_order_acquire))
			{
				all = false;
				continue;
			}
			const bool valid = slot.load.valid;
			if (valid)
				uploadSpecies(renderer, slot.load, m_species[i]);
			slot.load = SpeciesLoad{}; // the CPU meshes and textures go now
			slot.load.valid = valid;
			slot.uploaded = true;
		}
		if (!all)
			return false;
		Globals::jobSystem.wait(m_loadCounter); // every job has stored `done`: this only retires the counter
		oc::vector<Species> loaded;
		for (size_t i = 0; i < m_loadSlots.size(); ++i)
			if (m_loadSlots[i]->load.valid)
				loaded.push_back(oc::move(m_species[i]));
		m_species = oc::move(loaded);
		m_loadSlots.clear();
		m_loading = false;
		applyFadeBands(renderer);
		return true;
	}

	void TreeSystem::stopLoad()
	{
		if (!m_loading)
			return;
		Globals::jobSystem.wait(m_loadCounter); // main helps
		m_loadSlots.clear();
		m_loading = false;
	}

	// On a JOB: everything of the species but its GPU objects (uploadSpecies). Reads only `out` and `settings`.
	void TreeSystem::loadSpecies(const LoadSettings& settings, SpeciesLoad& out)
	{
		oc::string error;
		if (!loadTreeSpecies(out.path, out.desc, error))
		{
			Log::warning(oc::format("Trees: failed to load '{}': {}", out.path, error));
			return;
		}
		out.name = out.desc.name.empty() ? out.fileName : out.desc.name;
		const oc::string& name = out.name;
		const TreeSpeciesDesc& desc = out.desc;

		// THE MESH CACHE (MeshCache): the baked variants and their far-volume grids, keyed by the species file's text. A
		// miss generates the piece library, bakes the variants from it and writes the file; the library is not kept. Main
		// frees the variants once uploaded (and baked into the far tier below): nothing reads them after - a few hundred MB
		// over all species.
		const float leafCoverage = desc.leafType == ETreeLeafType::Cluster ? 0.5f : 1.0f; // cluster cards ~half opaque
		const uint64 textHash = meshCacheHash(FileSystem::readFileStr(out.path)); // the mesh and texture caches' key
		{
			const uint64 meshHash = meshCacheMix(textHash, TREE_DENSITY_RES);
			const oc::string meshPath = oc::format("{}/{}.treemesh", TREE_MESH_DIR, name);
			if (!loadTreeMeshes(meshPath, meshHash, out.variants, out.density))
			{
				TreeLibrary library;
				generateTreeLibrary(desc, library);
				out.variants.clear();
				out.variants.resize((size_t)desc.variantCount);
				for (int v = 0; v < desc.variantCount; ++v)
					bakeTreeVariant(desc, library, treeHash(desc.seed, 5000u + (uint32)v), out.variants[(size_t)v]);
				// The far-tree volume's view of every baked tree (RendererVK TreeVolumePipeline).
				out.density.resize(out.variants.size());
				for (size_t v = 0; v < out.variants.size(); ++v)
				{
					TreeBillboardBox box;
					bakeTreeDensity(out.variants[v], TREE_DENSITY_RES, leafCoverage, out.density[v].values, box);
					out.density[v].min = box.min;
					out.density[v].max = box.max;
				}
				saveTreeMeshes(meshPath, meshHash, out.variants, out.density);
			}
		}
		// Each variant's bounds: the sphere around its box (the culls' band test, the RT range).
		out.variantBounds.resize(out.variants.size());
		for (size_t v = 0; v < out.variants.size(); ++v)
		{
			const TreeBillboardBox box = billboardBox(out.variants[v]);
			out.variantBounds[v] = glm::vec4((box.min + box.max) * 0.5f, glm::length(box.max - box.min) * 0.5f);
		}

		// Level-0 images for the far tier's bakes: only those generated here (a .tex holds no RGBA8 level 0), so
		// buildFarTier generates any it lacks when a bake misses.
		oc::vector<uint8> barkAlbedo, leafImage;
		uint32 barkSize = 0, leafSize = 0;
		glm::vec3 barkMean(0.0f), leafMean(0.0f);
		const uint64 textureHash = textureCacheHash(textHash, settings.compressTextures, TREE_TEXTURE_SIZE);
		{
			const oc::string path = oc::format("{}/{}_bark.tex", TREE_TEXTURE_DIR, name);
			CachedTexture& texture = out.bark;
			if (settings.regenerateTextures || !loadCachedTexture(path, textureHash, texture))
			{
				oc::vector<uint8> normal;
				barkSize = TREE_TEXTURE_SIZE;
				generateBarkImages(desc, barkSize, barkAlbedo, normal);
				TreeBarkTexture bark;
				buildBarkMips(barkAlbedo, normal, barkSize, bark);
				texture = {};
				encodeTexture(settings.compressTextures, barkSize, barkSize, bark.albedoMips, TextureConvert::EBlockFormat::BC1,
					bark.normalMips, TextureConvert::EBlockFormat::BC5, texture);
				texture.mean = meanLeafAlbedo(barkAlbedo, desc.barkColor);
				saveTexture(path, textureHash, texture);
				// The level-0 PNGs this file replaces.
				FileSystem::remove(oc::format("{}/{}_bark.png", TREE_TEXTURE_DIR, name));
				FileSystem::remove(oc::format("{}/{}_bark_normal.png", TREE_TEXTURE_DIR, name));
			}
			barkMean = texture.mean;
		}
		if (desc.leafType == ETreeLeafType::Cluster)
		{
			const oc::string path = oc::format("{}/{}_leaves.tex", TREE_TEXTURE_DIR, name);
			CachedTexture& texture = out.leaves;
			if (settings.regenerateTextures || !loadCachedTexture(path, textureHash, texture))
			{
				// One generation draws both: the normal map must match the leaves drawn in the colour image.
				oc::vector<uint8> normalImage;
				leafSize = TREE_TEXTURE_SIZE;
				generateLeafClusterImage(desc, leafSize, leafImage, &normalImage);
				TreeLeafTexture albedoChain;
				buildLeafClusterMips(leafImage, leafSize, albedoChain);
				oc::vector<oc::vector<uint8>> normalChain;
				buildNormalMips(normalImage, leafSize, normalChain);
				texture = {};
				// The normal map: cupped, tilted leaves, BC3 for its A - the RIM (0 at the midrib .. 1 at the edge: the
				// transmission's leaf thickness).
				encodeTexture(settings.compressTextures, leafSize, leafSize, albedoChain.mips, TextureConvert::EBlockFormat::BC3,
					normalChain, TextureConvert::EBlockFormat::BC3, texture);
				texture.mean = meanLeafAlbedo(leafImage, desc.leafColor);
				saveTexture(path, textureHash, texture);
				FileSystem::remove(oc::format("{}/{}_leaves.png", TREE_TEXTURE_DIR, name));
				FileSystem::remove(oc::format("{}/{}_leaves_normal.png", TREE_TEXTURE_DIR, name));
			}
			leafMean = texture.mean;
		}
		else
		{
			// Single leaves are solid-coloured: the bake samples a 1x1 opaque image of that colour.
			leafSize = 1;
			leafImage = { (uint8)(glm::clamp(desc.leafColor.x, 0.0f, 1.0f) * 255.0f + 0.5f),
				(uint8)(glm::clamp(desc.leafColor.y, 0.0f, 1.0f) * 255.0f + 0.5f),
				(uint8)(glm::clamp(desc.leafColor.z, 0.0f, 1.0f) * 255.0f + 0.5f), 255 };
			leafMean = meanLeafAlbedo(leafImage, desc.leafColor);
		}
		// `Color Far` (sRGB, decoded) overrides the leaf texture's mean; x `Billboard FarAlbedoScale`. A species WITHOUT
		// leaves (a succulent: bark blades) shows in the volume as its bark (bakeTreeDensity splats it): the bark's mean.
		const glm::vec3 far = desc.farColor;
		const bool leafless = out.variants.empty() || out.variants[0].leaves[0].indices.empty();
		out.volumeAlbedo = (far.x >= 0.0f ? glm::pow(far, glm::vec3(2.2f)) : leafless ? barkMean : leafMean) * desc.farAlbedoScale;
		// What a ray sees of the shadow billboard: the two textures' means (a leafless species: its bark for both).
		out.barkColour = barkMean;
		out.leafColour = (leafless ? barkMean : leafMean) * desc.billboardAlbedoScale;
		out.farTier = desc.billboardDistance > 0.0f;
		if (out.farTier)
			buildFarTier(settings, out, barkAlbedo, barkSize, leafImage, leafSize);
		out.valid = true;
	}

	// On main: the species' GPU objects from its load (loadSpecies).
	void TreeSystem::uploadSpecies(Renderer& renderer, SpeciesLoad& load, Species& species)
	{
		species.desc = oc::move(load.desc);
		species.volumeAlbedo = load.volumeAlbedo;
		const oc::string& name = load.name;

		size_t barkTris[TREE_PIECE_LODS] = {}, leafTris[TREE_PIECE_LODS] = {};
		// RT sees the MESHES only of a species with no shadow billboard (`Billboard Distance` 0: its meshes cast and stand
		// in the TLAS); else the billboard is the tree's only RT representation. Bushes never: too small to matter to GI /
		// RT shadows, and too many for the TLAS.
		const bool meshesRaytraced = !load.farTier && !species.desc.bush;
		// THE BARK of a tree always has a BLAS: near the scene focus it stands in the TLAS beside the billboard, for RTAO's
		// rays only (RendererVK tree_cull.inc.glsl treeCullAoPiece) - the contact occlusion at the trunk and in the forks.
		const bool barkRaytraced = !species.desc.bush;
		// The baked whole trees only: the piece library (its trunks and modules) is the bakes' input, never drawn.
		species.variantMeshes.resize(load.variants.size());
		for (size_t v = 0; v < load.variants.size(); ++v)
		{
			const TreePiece& variant = load.variants[v];
			PieceMeshes& meshes = species.variantMeshes[v];
			uploadLevelZero(renderer, variant.bark, meshes.bark, barkRaytraced);
			uploadLevelZero(renderer, variant.leaves, meshes.leaves, meshesRaytraced);
			meshes.farCentre = glm::vec3(load.variantBounds[v]);
			meshes.farRadius = load.variantBounds[v].w;
			for (uint32 k = 0; k < TREE_PIECE_LODS; ++k)
			{
				barkTris[k] += numTriangles(variant.bark[k]);
				leafTris[k] += numTriangles(variant.leaves[k]);
			}
		}
		for (size_t v = 0; v < load.variants.size() && v < load.density.size(); ++v)
		{
			PieceMeshes& meshes = species.variantMeshes[v];
			meshes.density = oc::move(load.density[v].values);
			meshes.densityMin = load.density[v].min;
			meshes.densityMax = load.density[v].max;
		}

		// FACTION_TINT: the near bark draws on LitOpaque (early depth), which tints only a material that asks for it.
		species.ownBarkMaterial = createTreeMaterial(load.bark, 0.0f, oc::format("TreeBark/{}", name).c_str(),
			RendererVKLayout::MATERIAL_FLAG_FACTION_TINT);
		species.barkMaterial = species.ownBarkMaterial.index();
		if (species.desc.leafType == ETreeLeafType::Cluster)
		{
			// LEAF: the sun shines through the cluster cards (LitTree's leaf terms).
			species.ownLeafMaterial = createTreeMaterial(load.leaves, TREE_LEAF_ALPHA_CUTOFF, oc::format("TreeLeafCluster/{}", name).c_str(),
				RendererVKLayout::MATERIAL_FLAG_LEAF);
			species.leafMaterial = species.ownLeafMaterial.index();
			species.leafPipeline = RendererVKLayout::EPipelineIndex::LitTree;
		}
		else
			species.leafMaterial = RenderMaterial::getOrCreateSolidColor(species.desc.leafColor);

		if (load.farTier)
		{
			// The SHADOW BILLBOARDS. "TreeBillboard/<variant>": one box per billboard in the VRAM view.
			for (size_t v = 0; v < load.variantBillboards.size() && v < species.variantMeshes.size(); ++v)
			{
				VariantBillboard& billboard = load.variantBillboards[v];
				PieceMeshes& meshes = species.variantMeshes[v];
				meshes.billboardMaterial = createBillboardMaterial(billboard.mask, load.barkColour, load.leafColour,
					oc::format("TreeBillboard/{}_tree{}", name, v).c_str());
				// RT: never a bush's.
				meshes.billboard = uploadTreeMesh(renderer, billboard.cards, !species.desc.bush);
			}
			// The VOXEL tier (main pass only: no BLAS). LEAF: the leaf terms (transmission, the N.V bend, the sky wrap).
			for (size_t v = 0; v < load.voxelTextures.size() && v < species.variantMeshes.size(); ++v)
			{
				if (load.voxelTextures[v].albedo.empty() || v >= load.voxelBoxes.size() || v >= load.voxelIndex.size() || load.voxelIndex[v].albedo.empty())
					continue;
				PieceMeshes& meshes = species.variantMeshes[v];
				meshes.voxelMaterial = createVoxelMaterial(load.voxelTextures[v], load.voxelIndex[v], meshes.voxelIndex,
					oc::format("TreeVoxel/{}_{}", name, v).c_str(), RendererVKLayout::MATERIAL_FLAG_LEAF);
				meshes.voxelBox = uploadTreeMesh(renderer, load.voxelBoxes[v], false);
				// THE GRID AS THE MESH LEAVES FIND IT (buildGpuTypes -> RenderTreeSet::Type's leaf grid): its box, from the box
				// mesh (treeVoxelBoxMesh: a vertex's normal = its grid corner, its tangent = the box size), and the index
				// texture's header texel (log2 of the resolution, the levels, log2 of the bricks per atlas row).
				const TreeMesh& box = load.voxelBoxes[v];
				const oc::vector<uint8>& header = load.voxelIndex[v].albedo[0];
				if (!box.positions.empty() && header.size() >= 4)
				{
					meshes.voxelGridSize = box.tangents[0];
					meshes.voxelGridMin = box.positions[0] - box.normals[0] * meshes.voxelGridSize;
					meshes.voxelGridLog2Res = header[0];
					meshes.voxelGridLevels = header[1];
					meshes.voxelGridLog2Cols = header[2];
				}
			}
			// The crossfade's fade-out copies of the bark and the leaf material (bands set by applyFadeBands).
			species.barkFadeMaterial = RenderMaterial::derive(species.barkMaterial, 0);
			species.leafFadeMaterial = RenderMaterial::derive(species.leafMaterial, 0);
		}
		Log::info(oc::format("Trees: '{}' - {} variants, triangles bark/leaf per LOD: {}/{} {}/{} {}/{} {}/{}",
			species.desc.name, load.variants.size(),
			barkTris[0], leafTris[0], barkTris[1], leafTris[1], barkTris[2], leafTris[2], barkTris[3], leafTris[3]));
	}

	// On the species' load job (loadSpecies).
	void TreeSystem::buildFarTier(const LoadSettings& settings, SpeciesLoad& out,
		oc::vector<uint8>& barkAlbedo, uint32& barkSize, oc::vector<uint8>& leafImage, uint32& leafSize)
	{
		const TreeSpeciesDesc& desc = out.desc;
		const oc::vector<TreePiece>& variants = out.variants;
		const oc::string& name = out.name;
		// The bakes' inputs, generated on the first miss when their .tex was loaded (the same images: the generators are
		// deterministic).
		const auto bakeInputs = [&]()
		{
			if (barkAlbedo.empty())
			{
				oc::vector<uint8> normal;
				barkSize = TREE_TEXTURE_SIZE;
				generateBarkImages(desc, barkSize, barkAlbedo, normal);
			}
			if (leafImage.empty())
			{
				oc::vector<uint8> normal; // drawn as for the .tex, so the colour image is the same
				leafSize = TREE_TEXTURE_SIZE;
				generateLeafClusterImage(desc, leafSize, leafImage, &normal);
			}
		};
		const uint32 size = (uint32)desc.billboardSize;

		oc::vector<FileSystem::DirEntry> existing;
		FileSystem::listDirectory(TREE_TEXTURE_DIR, existing);
		// The cache key's source: the species file's text (every piece's geometry comes from it and the generator code,
		// which BAKE_VERSION covers).
		uint32 speciesHash = 2166136261u;
		for (const char c : FileSystem::readFileStr(desc.path))
			speciesHash = (speciesHash ^ (uint8)c) * 16777619u;

		// THE SHADOW BILLBOARDS: per baked variant its MASK (bakeBillboardMask: two channels, a level per mip) in
		// "<species>_shadowmask<v>_<hash>.tex", and its three cards. The cache: the bake's inputs in the name, a variant's
		// stale files removed.
		const uint32 numMips = billboardLayout(size).numMips;
		out.variantBillboards.resize(variants.size());
		for (size_t v = 0; v < variants.size(); ++v)
		{
			const oc::string prefix = oc::format("{}_shadowmask{}_", name, v);
			const oc::string stem = oc::format("{}{:08x}", prefix, treeBakeHash(treeHash(speciesHash, treeHash(2u, (uint32)v)), SHADOW_MASK_VERSION, size));
			const oc::string path = oc::format("{}/{}.tex", TREE_TEXTURE_DIR, stem);
			const uint64 textureHash = meshCacheMix(meshCacheHash(stem), settings.compressTextures ? 1u : 0u);
			VariantBillboard& billboard = out.variantBillboards[v];
			CachedTexture& mask = billboard.mask;
			if (settings.regenerateTextures || !loadCachedTexture(path, textureHash, mask) || mask.width != size || mask.height != size
				|| mask.albedo.size() != numMips)
			{
				for (const FileSystem::DirEntry& entry : existing)
					if (entry.name.rfind(prefix.c_str(), 0) == 0)
						FileSystem::remove(entry.path);
				// The leaf atlas alone (its alpha): the bark is opaque, and the mask holds no colour.
				if (leafImage.empty())
				{
					oc::vector<uint8> normal; // drawn as for the .tex, so the image is the same
					leafSize = TREE_TEXTURE_SIZE;
					generateLeafClusterImage(desc, leafSize, leafImage, &normal);
				}
				oc::vector<oc::vector<uint8>> levels;
				bakeBillboardMask(variants[v], TreeBakeImage{ leafImage, leafSize }, size, RendererVKLayout::TREE_CARD_BARK,
					desc.billboardTopCardHeight, levels);
				encodeMask(settings.compressTextures, size, levels, mask);
				saveTexture(path, textureHash, mask);
			}
			billboardMesh(billboardBox(variants[v]), size, desc.billboardTopCardHeight, billboard.cards);
		}

		// THE VOXEL TIER: per baked variant its prefiltered voxel grid as SPARSE BRICKS (bakeTreeVoxels): the opacity +
		// surface atlases in "<species>_treevoxel<v>r<res>_<hash>.tex" and the index in "..._index.tex" beside the masks,
		// keyed as they are plus TREE_VOXEL_BAKE_VERSION. Its box comes from the geometry on every load (treeVoxelBox). A
		// BUSH takes a quarter of the resolution (at least 32): it is about a metre, and its band starts a few tens of
		// metres out - a voxel of the full grid would be far under a pixel there.
		if (settings.voxelRes > 0)
		{
			const uint32 res = desc.bush ? glm::max(settings.voxelRes / 4u, 32u) : settings.voxelRes;
			out.voxelTextures.resize(variants.size());
			out.voxelIndex.resize(variants.size());
			out.voxelBoxes.resize(variants.size());
			for (size_t v = 0; v < variants.size(); ++v)
			{
				const oc::string prefix = oc::format("{}_treevoxel{}r{}_", name, v, res);
				const uint32 key = treeBakeHash(treeHash(speciesHash, treeHash(7u, (uint32)v)), TREE_VOXEL_BAKE_VERSION, res);
				const oc::string stem = oc::format("{}{:08x}", prefix, key);
				const oc::string path = oc::format("{}/{}.tex", TREE_TEXTURE_DIR, stem);
				const oc::string indexPath = oc::format("{}/{}_index.tex", TREE_TEXTURE_DIR, stem);
				const uint64 textureHash = meshCacheMix(meshCacheHash(stem), settings.compressTextures ? 1u : 0u);
				CachedTexture& texture = out.voxelTextures[v];
				CachedTexture& index = out.voxelIndex[v];
				if (settings.regenerateTextures || !loadCachedTexture(path, textureHash, texture) || !loadCachedTexture(indexPath, textureHash, index)
					|| texture.albedo.size() != 1 || texture.normal.size() != 1 || index.albedo.size() != 1)
				{
					for (const FileSystem::DirEntry& entry : existing)
						if (entry.name.rfind(prefix.c_str(), 0) == 0)
							FileSystem::remove(entry.path);
					bakeInputs();
					TreeVoxelBake bake;
					bakeTreeVoxels(variants[v], TreeBakeImage{ barkAlbedo, barkSize }, TreeBakeImage{ leafImage, leafSize }, res,
						desc.billboardAlbedoScale, bake);
					const uint32 bricks = bake.bricks, bricksTotal = bake.bricksTotal;
					// BC4 / BC3: a brick's tiles are whole blocks (8 x 8 opacity, 4 x 4 surface), so no block mixes two slices.
					encodeVoxelTextures(settings.compressTextures, bake, texture, index);
					saveTexture(path, textureHash, texture);
					saveCachedTexture(indexPath, textureHash, index);
					Log::info(oc::format("Trees: '{}' voxels {} at {}: {} of {} bricks stored ({:.0f} %), {:.1f} MB", name, v, res, bricks, bricksTotal,
						100.0f * (float)bricks / (float)glm::max(bricksTotal, 1u),
						(float)(texture.albedo[0].size() + texture.normal[0].size() + index.albedo[0].size()) / (1024.0f * 1024.0f)));
				}
				const TreeBillboardBox box = treeVoxelBox(variants[v], res);
				treeVoxelBoxMesh(box, out.voxelBoxes[v]);
			}
		}
	}

	void TreeSystem::buildGpuTypes(oc::vector<RenderTreeSet::Type>& gpuTypes, oc::unordered_map<const PieceMeshes*, uint32>& typeOf) const
	{
		for (const Species& species : m_species)
			for (const PieceMeshes& meshes : species.variantMeshes)
			{
				RenderTreeSet::Type type;
				const RenderMesh* bark = meshes.bark[0].isValid() ? &meshes.bark[0] : nullptr;
				const RenderMesh* leaves = meshes.leaves[0].isValid() ? &meshes.leaves[0] : nullptr;
				type.bark = { bark, species.barkMaterial, RendererVKLayout::EPipelineIndex::LitOpaque };
				type.leaves = { leaves, species.leafMaterial, species.leafPipeline };
				// THE VARIANT'S VOXEL GRID, in its type: the voxel tier's fragment shader takes the box from it, and the mesh
				// leaves of a cluster-leaf species (MATERIAL_FLAG_LEAF) march it toward the sun for their self-shadow
				// (RendererVK tree_leaf_sun.inc.glsl). A variant without a grid has none.
				if (meshes.voxelMaterial.isValid() && meshes.voxelIndex.isValid())
				{
					type.voxelGridMin = meshes.voxelGridMin;
					type.voxelGridSize = meshes.voxelGridSize;
					type.voxelGridOpacityTexture = meshes.voxelMaterial.albedoTexture();
					type.voxelGridIndexTexture = (uint16)meshes.voxelIndex.index();
					type.voxelGridLog2Res = meshes.voxelGridLog2Res;
					type.voxelGridLevels = meshes.voxelGridLevels;
					type.voxelGridLog2Cols = meshes.voxelGridLog2Cols;
				}
				if (!meshes.density.empty())
				{
					type.density = meshes.density.data();
					type.densityRes = TREE_DENSITY_RES;
					type.densityMin = meshes.densityMin;
					type.densityMax = meshes.densityMax;
					type.albedo = species.volumeAlbedo;
				}
				// The SHADOW BILLBOARD: the shadow passes and the rays only (its pipeline is not read).
				if (meshes.billboard.isValid() && meshes.billboardMaterial.isValid())
					type.shadowBillboard = { &meshes.billboard, meshes.billboardMaterial.index(), RendererVKLayout::EPipelineIndex::LitOpaque };
				// The VOXEL tier past the species' band, the meshes fading out over it on their derived materials.
				if (meshes.voxelBox.isValid() && meshes.voxelMaterial.isValid() && species.barkFadeMaterial.isValid() && species.leafFadeMaterial.isValid())
				{
					type.voxel = { &meshes.voxelBox, meshes.voxelMaterial.index(), RendererVKLayout::EPipelineIndex::LitTreeVoxel };
					type.barkFade = { bark, species.barkFadeMaterial.index(), RendererVKLayout::EPipelineIndex::LitTree };
					type.leavesFade = { leaves, species.leafFadeMaterial.index(), RendererVKLayout::EPipelineIndex::LitTree };
					type.farDistance = farDistance(species);
					type.fadeWidth = fadeWidth(species);
				}
				// The far voxel trees (past the near set) draw the trees only.
				type.farRing = !species.desc.bush;
				if (species.desc.bush)
					type.shadowDistance = m_settings.bushShadowDistance;
				type.windTrunk = species.desc.windTrunk;
				type.windBranch = species.desc.windBranch;
				typeOf.emplace(&meshes, (uint32)gpuTypes.size());
				gpuTypes.push_back(type);
			}
	}

	void TreeSystem::hookTerrain(uint32 numChunks)
	{
		auto lookup = [this](glm::ivec2 coord)
		{
			const auto it = m_vegChunkOf.find((uint64)(uint32)coord.x | ((uint64)(uint32)coord.y << 32));
			return it != m_vegChunkOf.end() ? it->second : -1;
		};
		// On the terrain's walk job (between beginFrame and present): the band settings through atomics.
		auto sink = [this](oc::span<const TerrainStreamer::VegetationDraw> draws)
		{
			static_assert(sizeof(TerrainStreamer::VegetationDraw) == sizeof(RenderTreeSet::ChunkDraw));
			m_treeSet.render(oc::span<const RenderTreeSet::ChunkDraw>((const RenderTreeSet::ChunkDraw*)draws.data(), draws.size()),
				m_sinkDistanceScale.load(oc::memory_order_relaxed), m_sinkForceFar.load(oc::memory_order_relaxed));
		};
		Globals::terrain.setVegetation(lookup, sink, numChunks);
		m_vegHooked = true;
	}

	// WORLD MODE's spawn: the set (every type, no pieces yet), the expansion context, the terrain hook.
	void TreeSystem::spawnWorld(Renderer& renderer, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		m_worldGeneration = m_world.generation();
		m_rockGeneration = Globals::rocks.worldGeneration();
		m_worldFullLogged = false;
		if (m_species.empty())
			return;
		oc::vector<RenderTreeSet::Type> gpuTypes;
		oc::unordered_map<const PieceMeshes*, uint32> typeOf;
		buildGpuTypes(gpuTypes, typeOf);

		auto context = oc::make_shared<ExpandContext>();
		// THE ROCK TYPES (RockSystem's loaded types, once their meshes are up): a GPU type per variant - ONE mesh, the
		// LOD chain's level 0 in the bark slot, on LitRock (the culls pick its level; no billboard, no fade) - and the
		// record types they answer to, by name among TreeWorld's rock types.
		if (Globals::rocks.worldEnabled())
		{
			const oc::span<const RockSystem::WorldType> rockTypes = Globals::rocks.worldTypes();
			for (const RockSystem::WorldType& rockType : rockTypes)
			{
				ExpandRock& rock = context->rocks.emplace_back();
				rock.scale = rockType.scale;
				rock.sink = rockType.sink;
				rock.align = rockType.align;
				rock.footprint = rockType.footprint;
				context->rockReach = glm::max(context->rockReach, 0.5f * rockType.scale.y);
				for (const RockSystem::WorldVariant& variant : rockType.variants)
				{
					RenderTreeSet::Type type;
					type.bark = { variant.mesh, rockType.material, RendererVKLayout::EPipelineIndex::LitRock };
					if (variant.density)
					{
						// The FAR VOLUME's view (R5): the occupancy over its box, a SOLID. The volume stands a piece on the
						// ground: the sink goes into the box. The lean is left out - kilometres away. Dead wood keeps its
						// own colour (not the climate's bedrock).
						const glm::vec3 sink(0.0f, rockType.sink * variant.height, 0.0f);
						type.density = variant.density;
						type.densityRes = ROCK_DENSITY_RES;
						type.densityMin = variant.densityMin - sink;
						type.densityMax = variant.densityMax - sink;
						type.solid = true;
						type.solidOwnColour = rockType.ownColour;
						type.albedo = rockType.ownAlbedo;
					}
					// The COLLIDER: a BVH of the LOD chain's last level (its own copy: RockSystem may free its meshes first).
					int32 colliderMesh = -1;
					if (!variant.collisionIndices.empty())
						if (PhysicsMesh mesh = Globals::physics.createCollisionMesh(variant.collisionPositions, variant.collisionIndices); mesh.isValid())
						{
							colliderMesh = (int32)m_rockColliderMeshes.size();
							m_rockColliderMeshes.push_back(oc::move(mesh));
						}
					rock.variants.push_back({ (uint32)gpuTypes.size(), variant.centre, variant.radius, variant.height, colliderMesh });
					gpuTypes.push_back(type);
				}
			}
			for (uint32 type = 0; type < 256; ++type)
			{
				const oc::string_view name = m_world.typeName(type);
				if (name.empty())
					break;
				int32 index = -1;
				if (type >= m_world.firstRockType())
					for (size_t r = 0; r < rockTypes.size(); ++r)
						if (oc::string_view(rockTypes[r].name) == name)
							index = (int32)r;
				context->rockOfRecordType.push_back(index);
			}
			m_setHasRocks = !context->rocks.empty();
		}
		context->maps = maps;
		context->worldSeed = m_world.seed();
		context->chunkSize = (float)Globals::terrain.chunkSize();
		context->sizeVariation = m_settings.sizeVariation;
		context->bushRadius = 1.5f;
		context->riverClear = Globals::settings.terrain.riverVegetationClear;
		// Every loaded species (its baked variants; a tree its undergrowth), then per TREE species the bushes of its climate
		// (case-insensitive, each `Weight` times; a tree whose climate no bush shares takes every bush).
		oc::vector<uint32> bushes;
		for (const Species& species : m_species)
		{
			ExpandSpecies& out = context->species.emplace_back();
			out.scale = species.desc.scale;
			for (const PieceMeshes& meshes : species.variantMeshes)
				out.variants.push_back({ typeOf[&meshes], meshes.farCentre, meshes.farRadius });
			if (species.desc.bush && !species.variantMeshes.empty())
				for (int w = 0; w < species.desc.weight; ++w)
					bushes.push_back((uint32)(context->species.size() - 1));
			if (!species.desc.bush)
			{
				const glm::vec2 undergrowth = bushUndergrowth(species.desc, m_settings);
				out.bushesPerTree = undergrowth.x;
				out.bushRadius = undergrowth.y;
				if (undergrowth.x > 0.0f)
					context->bushRadius = glm::max(context->bushRadius, undergrowth.y);
				// The COLLIDER: the trunk as an 8-sided cylinder at scale 1, from just under the ground to its mean height.
				const float radius = species.desc.trunkRadius * (1.0f + 0.5f * species.desc.trunkFlare);
				const float height = 0.5f * (species.desc.trunkHeight.x + species.desc.trunkHeight.y);
				if (radius > 0.0f && height > 0.0f)
					for (int i = 0; i < 8; ++i)
					{
						const float a = (float)i * (6.28318531f / 8.0f);
						const glm::vec2 c = glm::vec2(std::cos(a), std::sin(a)) * radius;
						out.colliderPoints.push_back(glm::vec3(c.x, -0.3f, c.y));
						out.colliderPoints.push_back(glm::vec3(c.x, height, c.y));
					}
			}
		}
		for (size_t s = 0; s < m_species.size(); ++s)
		{
			if (m_species[s].desc.bush)
				continue;
			for (uint32 b : bushes)
				if (bushClimateMatches(m_species[b].desc.climate, m_species[s].desc.climate))
					context->species[s].bushes.push_back(b);
			if (context->species[s].bushes.empty())
				context->species[s].bushes = bushes;
		}
		// The record types are TreeWorld's (its name-sorted .tree list): matched by name. A bush's record (a bush placed on
		// its own) expands to the bush alone: it has no undergrowth.
		for (uint32 type = 0; type < 256; ++type)
		{
			const oc::string_view name = m_world.typeName(type);
			if (name.empty())
				break;
			int32 index = -1;
			if (type < m_world.firstRockType())
				for (size_t s = 0; s < m_species.size(); ++s)
					if (oc::string_view(m_species[s].desc.name) == name && !m_species[s].variantMeshes.empty())
						index = (int32)s;
			context->speciesOfRecordType.push_back(index);
		}
		m_expandContext = context;

		m_treeSet = RenderTreeSet::create(gpuTypes, (uint32)glm::max(m_settings.worldCapacity, 1));

		// The far volume's view of the RECORDS (RendererVK TreeRecordTypeGpu), per record type (TreeWorld's name-sorted
		// species, bushes too): how a record EXPANDS - the same rules as expandChunk (its variants as this set's types,
		// the scale range, the size variation, a tree's bushes in expandChunk's order) - and, for the mass far out, per
		// variant its extinction's integral at scale 1, plus the species' profile over height (the variants' mean at
		// the mean of its Scale range), crown radius (scale 1) and leaf colour.
		oc::vector<int32> recordTypeOfSpecies(m_species.size(), -1);
		oc::vector<int32> speciesOfType;
		for (uint32 type = 0; type < 256; ++type)
		{
			const oc::string_view name = m_world.typeName(type);
			if (name.empty())
				break;
			int32 index = -1; // a rock type stays -1 here: the rock types follow below
			if (type < m_world.firstRockType())
				for (size_t s = 0; s < m_species.size(); ++s)
					if (oc::string_view(m_species[s].desc.name) == name && !m_species[s].variantMeshes.empty())
						index = (int32)s;
			speciesOfType.push_back(index);
			if (index >= 0)
				recordTypeOfSpecies[(size_t)index] = (int32)type;
		}
		oc::vector<TreeRecordTypeGpu> recordTypes(speciesOfType.size());
		bool clipped = false;
		for (size_t t = 0; t < recordTypes.size(); ++t)
		{
			if (speciesOfType[t] < 0)
				continue;
			const size_t s = (size_t)speciesOfType[t];
			const Species& species = m_species[s];
			TreeRecordTypeGpu& out = recordTypes[t];
			out.scale = species.desc.scale;
			out.sizeVariation = m_settings.sizeVariation;
			out.albedo = glm::vec4(species.volumeAlbedo, 1.0f);
			clipped |= species.variantMeshes.size() > TREE_RECORD_MAX_VARIANTS;
			out.numVariants = (uint32)glm::min(species.variantMeshes.size(), (size_t)TREE_RECORD_MAX_VARIANTS);
			if (!species.desc.bush)
			{
				const oc::vector<uint32>& climateBushes = context->species[s].bushes;
				clipped |= climateBushes.size() > TREE_RECORD_MAX_BUSHES;
				for (uint32 bush : climateBushes)
					if (out.numBushes < TREE_RECORD_MAX_BUSHES && recordTypeOfSpecies[bush] >= 0)
						out.bushTypes[out.numBushes++] = (uint32)recordTypeOfSpecies[bush];
				out.bushesPerTree = context->species[s].bushesPerTree;
				out.bushRadius = context->species[s].bushRadius;
			}
			const float meanScale = 0.5f * (species.desc.scale.x + species.desc.scale.y);
			for (uint32 v = 0; v < out.numVariants; ++v)
			{
				const PieceMeshes& meshes = species.variantMeshes[v];
				out.variantType[v] = typeOf[&meshes];
				if (meshes.density.empty())
					continue;
				out.height = glm::max(out.height, meshes.densityMax.y * meanScale);
				out.radius = glm::max(out.radius, 0.5f * glm::max(meshes.densityMax.x - meshes.densityMin.x, meshes.densityMax.z - meshes.densityMin.z));
			}
			if (out.height <= 0.0f)
				continue;
			double profile[TREE_RECORD_PROFILE_BINS] = {};
			uint32 variants = 0;
			for (uint32 v = 0; v < out.numVariants; ++v)
			{
				const PieceMeshes& meshes = species.variantMeshes[v];
				if (meshes.density.empty())
					continue;
				++variants;
				const uint32 res = TREE_DENSITY_RES;
				const glm::vec3 voxel = (meshes.densityMax - meshes.densityMin) / (float)res;
				// Extinction (1/m) in tree space -> world: / scale; the voxel's volume x scale^3: the mass grows with scale^2.
				const double voxelVolume = (double)voxel.x * voxel.y * voxel.z;
				double variantMass = 0.0;
				for (uint32 z = 0; z < res; ++z)
					for (uint32 y = 0; y < res; ++y)
					{
						const float height = (meshes.densityMin.y + ((float)y + 0.5f) * voxel.y) * meanScale;
						const uint32 bin = (uint32)glm::clamp(height / out.height * (float)TREE_RECORD_PROFILE_BINS, 0.0f, (float)TREE_RECORD_PROFILE_BINS - 1.0f);
						for (uint32 x = 0; x < res; ++x)
						{
							const double m = meshes.density[x + res * (y + res * z)] * voxelVolume;
							variantMass += m;
							profile[bin] += m * meanScale * meanScale;
						}
					}
				out.variantMass[v] = (float)variantMass;
			}
			double mass = 0.0;
			for (double p : profile)
				mass += p;
			mass /= (double)glm::max(variants, 1u);
			if (mass <= 0.0)
				continue;
			out.mass = (float)mass;
			const double binH = out.height / (double)TREE_RECORD_PROFILE_BINS;
			for (uint32 b = 0; b < TREE_RECORD_PROFILE_BINS; ++b)
				out.shape[b] = (float)(profile[b] / (double)variants / (mass * binH));
		}
		// The ROCK record types (RendererVK "Far-tree volume" ROCKS): a record expands as placeRock - its variant and
		// yaw from the same hashes, its scale rockRecordScale (the tree rule without size variation) - into the set's rock
		// types (the occupancy, the sink in the box, no lean). A SOLID (albedo.w 0): the volume gives it "Far rock
		// extinction" at any size, so its mass grows with scale^3 (variantMass = its occupied volume at scale 1), and its
		// colour is the climate's bedrock. The profile as a tree's, over the sunk box.
		if (m_setHasRocks)
		{
			const oc::span<const RockSystem::WorldType> rockTypes = Globals::rocks.worldTypes();
			for (size_t t = 0; t < recordTypes.size() && t < context->rockOfRecordType.size(); ++t)
			{
				const int32 r = context->rockOfRecordType[t];
				if (r < 0 || (size_t)r >= rockTypes.size())
					continue;
				const ExpandRock& rock = context->rocks[(size_t)r];
				const RockSystem::WorldType& world = rockTypes[(size_t)r];
				TreeRecordTypeGpu& out = recordTypes[t];
				out.scale = rock.scale;
				out.sizeVariation = 0.0f;
				out.albedo = world.ownColour ? glm::vec4(world.ownAlbedo, 0.5f) : glm::vec4(0.0f); // w: 0.5 a solid in its own colour
				clipped |= rock.variants.size() > TREE_RECORD_MAX_VARIANTS;
				out.numVariants = (uint32)glm::min(rock.variants.size(), (size_t)TREE_RECORD_MAX_VARIANTS);
				const float meanScale = 0.5f * (rock.scale.x + rock.scale.y);
				for (uint32 v = 0; v < out.numVariants; ++v)
				{
					out.variantType[v] = rock.variants[v].type;
					const RockSystem::WorldVariant& variant = world.variants[v];
					if (!variant.density)
						continue;
					out.height = glm::max(out.height, (variant.densityMax.y - rock.sink * variant.height) * meanScale);
					out.radius = glm::max(out.radius, 0.5f * glm::max(variant.densityMax.x - variant.densityMin.x, variant.densityMax.z - variant.densityMin.z));
				}
				if (out.height <= 0.0f)
					continue;
				double profile[TREE_RECORD_PROFILE_BINS] = {};
				uint32 variants = 0;
				for (uint32 v = 0; v < out.numVariants; ++v)
				{
					const RockSystem::WorldVariant& variant = world.variants[v];
					if (!variant.density)
						continue;
					++variants;
					const uint32 res = ROCK_DENSITY_RES;
					const glm::vec3 voxel = (variant.densityMax - variant.densityMin) / (float)res;
					const double voxelVolume = (double)voxel.x * voxel.y * voxel.z;
					const float baseY = variant.densityMin.y - rock.sink * variant.height;
					double variantMass = 0.0;
					for (uint32 z = 0; z < res; ++z)
						for (uint32 y = 0; y < res; ++y)
						{
							const float height = (baseY + ((float)y + 0.5f) * voxel.y) * meanScale;
							const uint32 bin = (uint32)glm::clamp(height / out.height * (float)TREE_RECORD_PROFILE_BINS, 0.0f, (float)TREE_RECORD_PROFILE_BINS - 1.0f);
							for (uint32 x = 0; x < res; ++x)
							{
								const double m = variant.density[x + res * (y + res * z)] * voxelVolume;
								variantMass += m;
								profile[bin] += m * meanScale * meanScale * meanScale;
							}
						}
					out.variantMass[v] = (float)variantMass;
				}
				double mass = 0.0;
				for (double p : profile)
					mass += p;
				mass /= (double)glm::max(variants, 1u);
				if (mass <= 0.0)
					continue;
				out.mass = (float)mass;
				const double binH = out.height / (double)TREE_RECORD_PROFILE_BINS;
				for (uint32 b = 0; b < TREE_RECORD_PROFILE_BINS; ++b)
					out.shape[b] = (float)(profile[b] / (double)variants / (mass * binH));
			}
		}
		if (clipped)
			Log::warning(oc::format("Trees: a species has more than {} variants or a tree more than {} bush species - the far volume's "
				"records then pick differently from the meshes", TREE_RECORD_MAX_VARIANTS, TREE_RECORD_MAX_BUSHES));
		m_world.joinPool(); // the pool job writes the same record pool
		renderer.setTreeRecordTypes(recordTypes, context->chunkSize, context->worldSeed, m_treeSet);
		m_recordTypesSet = true;
		m_treeSet.bind();
		m_vegChunkSize = Globals::terrain.chunkSize();
		// The set's chunk indices are recycled: at most the near square (+1 of hysteresis) plus one frame of removals.
		const uint32 side = 2u * (uint32)m_settings.nearRadius + 3u;
		hookTerrain(side * side * 2u);
	}

	// WORLD MODE, every frame: drop the chunks that left the near radius (+1 of hysteresis), start the expansion of the
	// ones inside it that TreeWorld holds records for, and add up to "Expand per frame" finished ones to the set.
	void TreeSystem::updateWorld(Renderer& renderer, const Camera& camera)
	{
		if (!m_expandContext || !m_treeSet.isValid())
			return;
		const float chunkSize = m_expandContext->chunkSize;
		const glm::ivec2 cam((int32)std::floor(camera.position.x / chunkSize), (int32)std::floor(camera.position.z / chunkSize));
		auto key = [](glm::ivec2 c) { return (uint64)(uint32)c.x | ((uint64)(uint32)c.y << 32); };
		auto chebyshev = [&](glm::ivec2 c) { return glm::max(glm::abs(c.x - cam.x), glm::abs(c.y - cam.y)); };

		for (auto it = m_near.begin(); it != m_near.end(); )
		{
			const glm::ivec2 coord((int32)(uint32)it->first, (int32)(uint32)(it->first >> 32));
			if (chebyshev(coord) <= m_settings.nearRadius + 1)
			{
				++it;
				continue;
			}
			if (it->second.setChunk != UINT32_MAX)
			{
				m_treeSet.removeChunk(it->second.setChunk);
				m_vegChunkOf.erase(it->first);
				Globals::terrain.restampVegetation(coord);
			}
			m_colliderBodies -= it->second.liveBodies; // its bodies go with it
			it = m_near.erase(it); // a pending one: its result finds no entry
		}

		// The finished expansions (this generation's, still wanted), nearest first is the jobs' order already.
		oc::vector<ExpandResult> results;
		{
			std::lock_guard<std::mutex> lk(m_expandMutex);
			const size_t take = glm::min(m_expandResults.size(), (size_t)glm::max(m_settings.expandPerFrame, 1));
			for (size_t i = 0; i < take; ++i)
				results.push_back(oc::move(m_expandResults[i]));
			m_expandResults.erase(m_expandResults.begin(), m_expandResults.begin() + take);
		}
		for (ExpandResult& result : results)
		{
			const auto it = m_near.find(key(result.coord));
			if (result.generation != m_expandGeneration || it == m_near.end() || !it->second.pending)
				continue;
			it->second.pending = false;
			it->second.colliders = oc::move(result.colliders); // their bodies: updateColliders
			const uint32 setChunk = m_treeSet.addChunk(result.pieces);
			if (setChunk == UINT32_MAX)
			{
				if (!m_worldFullLogged)
					Log::warning(oc::format("Trees: the world set is full ({} pieces) - raise 'Trees/World/Set capacity (pieces)'", m_settings.worldCapacity));
				m_worldFullLogged = true;
				continue;
			}
			it->second.setChunk = setChunk;
			m_vegChunkOf[it->first] = (int32)setChunk;
			Globals::terrain.restampVegetation(result.coord);
		}

		// New expansions, nearest first, a few in flight at a time.
		constexpr int32 MAX_IN_FLIGHT = 4;
		for (int r = 0; r <= m_settings.nearRadius && m_expandInFlight.load(oc::memory_order_relaxed) < MAX_IN_FLIGHT; ++r)
			for (int z = -r; z <= r && m_expandInFlight.load(oc::memory_order_relaxed) < MAX_IN_FLIGHT; ++z)
				for (int x = -r; x <= r; ++x)
				{
					if (glm::max(glm::abs(x), glm::abs(z)) != r)
						continue; // the ring at Chebyshev distance r only
					const glm::ivec2 coord = cam + glm::ivec2(x, z);
					if (m_near.find(key(coord)) != m_near.end())
						continue;
					const oc::vector<TreeRecord>* records = m_world.cpuRecords(coord);
					if (!records)
						continue; // not generated yet: next frame
					m_near[key(coord)] = NearChunk{};
					m_expandInFlight.fetch_add(1, oc::memory_order_relaxed);
					Globals::jobSystem.submit([this, context = m_expandContext, coord, chunkRecords = *records, generation = m_expandGeneration]
					{
						ExpandResult result;
						result.coord = coord;
						result.generation = generation;
						expandChunk(*context, coord, chunkRecords, result.pieces, result.colliders);
						std::lock_guard<std::mutex> lk(m_expandMutex);
						m_expandResults.push_back(oc::move(result));
						m_expandInFlight.fetch_sub(1, oc::memory_order_relaxed);
					}, { "treeExpandChunk", EProfileCategory::Procedural }, EJobPriority::Low, &m_expandCounter);
					if (m_expandInFlight.load(oc::memory_order_relaxed) >= MAX_IN_FLIGHT)
						break;
				}

		// The chunks the set draws: the renderer's far voxel trees expand every OTHER record chunk (RendererVK Scene.md). Only
		// the ones the set held LAST frame too: the terrain walk that lists the set's drawn chunks was kicked before this
		// update, so a chunk added now draws from the next frame on - excluded at once, its trees vanished for a frame (a
		// dark flash at the near radius). Drawn twice for that frame instead (the same voxels).
		oc::vector<uint64> keys;
		keys.reserve(m_vegChunkOf.size());
		for (const auto& [k, setChunk] : m_vegChunkOf)
			keys.push_back(k);
		oc::sort(keys.begin(), keys.end());
		oc::vector<glm::ivec2> setCoords;
		setCoords.reserve(keys.size());
		for (const uint64 k : keys)
			if (oc::binary_search(m_prevSetKeys.begin(), m_prevSetKeys.end(), k))
				setCoords.push_back(glm::ivec2((int32)(uint32)k, (int32)(uint32)(k >> 32)));
		m_prevSetKeys = oc::move(keys);
		renderer.setTreeNearChunks(setCoords);

		updateColliders(camera);
	}

	void TreeSystem::stopExpansion()
	{
		Globals::jobSystem.wait(m_expandCounter);
		++m_expandGeneration;
		std::lock_guard<std::mutex> lk(m_expandMutex);
		m_expandResults.clear();
	}

	void TreeSystem::updateColliders(const Camera& camera)
	{
		if (!m_settings.colliders)
		{
			if (m_colliderBodies > 0)
				for (auto& [k, chunk] : m_near)
				{
					chunk.bodies.clear();
					chunk.liveBodies = 0;
				}
			m_colliderBodies = 0;
			return;
		}
		ProfileScope scope("Tree colliders", EProfileCategory::Procedural);
		const float radius = m_settings.colliderRadius;
		const float out2 = (radius * 1.1f) * (radius * 1.1f), in2 = radius * radius;
		// The focus points (setColliderFocus), or the camera with none. A distance is the one to the NEAREST focus.
		glm::vec2 focus[MaxColliderFocus];
		int focusCount = m_colliderFocusCount;
		for (int i = 0; i < focusCount; ++i)
			focus[i] = m_colliderFocus[i];
		if (focusCount == 0)
			focus[focusCount++] = glm::vec2(camera.position.x, camera.position.z);
		const float chunkSize = m_expandContext->chunkSize;
		int32 budget = glm::max(m_settings.collidersPerFrame, 1);
		const uint64 layer = PhysicsLayers::bit("Vegetation");

		for (auto& [k, chunk] : m_near)
		{
			if (chunk.colliders.empty())
				continue;
			const glm::vec2 lo = glm::vec2((float)(int32)(uint32)k, (float)(int32)(uint32)(k >> 32)) * chunkSize;
			float chunkDist2 = FLT_MAX;
			for (int f = 0; f < focusCount; ++f)
			{
				const glm::vec2 d = glm::clamp(focus[f], lo - 8.0f, lo + chunkSize + 8.0f) - focus[f]; // a rock's centre may sit past the border
				chunkDist2 = glm::min(chunkDist2, glm::dot(d, d));
			}
			if (chunkDist2 > out2)
			{
				if (chunk.liveBodies > 0)
				{
					m_colliderBodies -= chunk.liveBodies;
					chunk.bodies.clear();
					chunk.liveBodies = 0;
				}
				continue;
			}
			if (chunk.bodies.size() != chunk.colliders.size())
				chunk.bodies.resize(chunk.colliders.size());
			for (size_t i = 0; i < chunk.colliders.size(); ++i)
			{
				const ExpandCollider& c = chunk.colliders[i];
				float dist2 = FLT_MAX;
				for (int f = 0; f < focusCount; ++f)
				{
					const glm::vec2 e = glm::vec2(c.transform.pos.x, c.transform.pos.z) - focus[f];
					dist2 = glm::min(dist2, glm::dot(e, e));
				}
				PhysicsBody& body = chunk.bodies[i];
				if (body.isValid())
				{
					if (dist2 > out2)
					{
						body.destroy();
						--chunk.liveBodies;
						--m_colliderBodies;
					}
					continue;
				}
				if (dist2 > in2 || budget <= 0)
					continue;
				PhysicsShape shape;
				shape.categoryBits = layer;
				if (c.rockMesh >= 0)
				{
					shape.type = EPhysicsShapeType::Mesh;
					shape.mesh = &m_rockColliderMeshes[(size_t)c.rockMesh];
				}
				else
				{
					shape.type = EPhysicsShapeType::Hull;
					shape.hullPoints = m_expandContext->species[c.species].colliderPoints;
					shape.maxHullVertices = 16;
				}
				PhysicsBodyDesc desc;
				desc.type = EPhysicsBodyType::Static;
				desc.transform = c.transform;
				body = Globals::physics.createBody(desc, oc::span<const PhysicsShape>(&shape, 1));
				--budget;
				if (body.isValid())
				{
					++chunk.liveBodies;
					++m_colliderBodies;
				}
			}
		}
	}

	// One chunk's trees + bushes from its records: a BAKED variant per plant - variant, scale and yaw from its seed (the
	// record's for the tree) - on the ground from one Full grid at 2 m (the terrain's LOD 0 spacing) over the chunk plus
	// the bushes' reach. MIRRORED on the GPU by the far volume (tree_volume_splat.cs, tree_volume_records.cs) and the far
	// voxel trees (tree_voxel_far.cs) - keep the hashes and the order of the choices in step.
	void TreeSystem::expandChunk(const ExpandContext& context, glm::ivec2 coord, const oc::vector<TreeRecord>& records,
		oc::vector<RenderTreeSet::Piece>& out, oc::vector<ExpandCollider>& outColliders)
	{
		out.clear();
		outColliders.clear();
		if (records.empty() || !context.maps)
			return;
		const float cs = context.chunkSize;
		constexpr float STEP = 2.0f;
		const float margin = std::ceil((glm::max(context.bushRadius, context.rockReach) + 2.0f) / STEP) * STEP;
		const glm::vec2 origin = glm::vec2(coord) * cs - margin;
		const uint32 res = (uint32)std::ceil((cs + 2.0f * margin) / STEP) + 1;
		oc::vector<TerrainPoint> field((size_t)res * res);
		context.maps->sampleGrid(origin.x, origin.y, STEP, res, res, field, ESampleDetail::Full);
		Globals::jobSystem.preemptionPoint();
		auto groundAt = [&](glm::vec2 p)
		{
			const glm::vec2 g = glm::clamp((p - origin) / STEP, glm::vec2(0.0f), glm::vec2((float)(res - 1) - 1e-3f));
			const glm::uvec2 i0 = glm::uvec2(g);
			const glm::vec2 f = g - glm::vec2(i0);
			const float h00 = field[(size_t)i0.y * res + i0.x].height, h10 = field[(size_t)i0.y * res + i0.x + 1].height;
			const float h01 = field[(size_t)(i0.y + 1) * res + i0.x].height, h11 = field[(size_t)(i0.y + 1) * res + i0.x + 1].height;
			return glm::mix(glm::mix(h00, h10, f.x), glm::mix(h01, h11, f.x), f.y);
		};
		// A bush lands anywhere around its tree: not in WATER - under a river's / lake's surface or the sea, or in a river's
		// bed past "Vegetation clear" (the trees' own rule, TreeWorld) - at the nearest grid point. The far volume's GPU
		// mirror has no water data and keeps them (kilometres away).
		auto inWater = [&](glm::vec2 p)
		{
			const glm::uvec2 i = glm::uvec2(glm::clamp((p - origin) / STEP + 0.5f, glm::vec2(0.0f), glm::vec2((float)(res - 1))));
			const TerrainPoint& t = field[(size_t)i.y * res + i.x];
			return t.waterKind == ETerrainWater::River || t.waterKind == ETerrainWater::Lake || t.height < t.waterLevel
				|| t.river > context.riverClear;
		};
		// `collider`: the species index of a TREE (its trunk cylinder, when it has one); -1 = a bush (no collider).
		auto placeVariant = [&](const ExpandSpecies& species, uint32 seed, glm::vec2 p, int32 collider)
		{
			if (species.variants.empty())
				return;
			const ExpandVariant& variant = species.variants[treeHash(seed, 102u) % (uint32)species.variants.size()];
			// The species' own range, then "Size variation": x 2^(+-v), log-uniform, so halving and doubling are as likely.
			const float scale = glm::mix(species.scale.x, species.scale.y, treeHash01(treeHash(seed, 103u)))
				* std::exp2(context.sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0f - 1.0f));
			const glm::quat yaw = glm::angleAxis(treeHash01(treeHash(seed, 104u)) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
			RenderTreeSet::Piece& piece = out.emplace_back();
			piece.transform = Transform(glm::vec3(p.x, groundAt(p) - 0.05f, p.y), scale, yaw);
			piece.centre = piece.transform.transformPoint(variant.farCentre);
			piece.radius = variant.farRadius * scale;
			piece.type = variant.type;
			if (collider >= 0 && !species.colliderPoints.empty())
				outColliders.push_back({ piece.transform, -1, (uint32)collider });
		};

		// A ROCK record: one piece. The variant and the yaw from the seed as a tree's, the scale from rockRecordScale
		// (TreeWorld's placement keeps the trees out of the same footprint). It lies on the ground under its footprint,
		// turned to the ground's normal by `align`: RockSystem::groundTransform, the rule the preview uses too.
		auto placeRock = [&](const ExpandRock& rock, uint32 seed, glm::vec2 p)
		{
			if (rock.variants.empty())
				return;
			const ExpandRockVariant& variant = rock.variants[treeHash(seed, 102u) % (uint32)rock.variants.size()];
			const float scale = rockRecordScale(seed, rock.scale);
			const glm::quat yaw = glm::angleAxis(treeHash01(treeHash(seed, 104u)) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
			const float r = RockSystem::FOOTPRINT * scale * rock.footprint;
			const float ground[5] = { groundAt(p), groundAt(p - glm::vec2(r, 0.0f)), groundAt(p + glm::vec2(r, 0.0f)),
				groundAt(p - glm::vec2(0.0f, r)), groundAt(p + glm::vec2(0.0f, r)) };
			RenderTreeSet::Piece& piece = out.emplace_back();
			piece.transform = RockSystem::groundTransform(p, ground, r, scale, variant.height, rock.sink, rock.align, yaw);
			piece.centre = piece.transform.transformPoint(variant.centre);
			piece.radius = variant.radius * scale;
			piece.type = variant.type;
			if (variant.colliderMesh >= 0)
				outColliders.push_back({ piece.transform, variant.colliderMesh, 0 });
		};

		const glm::vec2 chunkOrigin = glm::vec2(coord) * cs;
		for (TreeRecord record : records)
		{
			const uint32 type = treeRecordType(record);
			const uint32 seed = treeRecordSeed(context.worldSeed, coord, record);
			const glm::vec2 p = chunkOrigin + treeRecordLocal(record, cs);
			if (const int32 r = type < context.rockOfRecordType.size() ? context.rockOfRecordType[type] : -1; r >= 0)
			{
				placeRock(context.rocks[(size_t)r], seed, p);
				continue;
			}
			const int32 s = type < context.speciesOfRecordType.size() ? context.speciesOfRecordType[type] : -1;
			if (s < 0)
				continue;
			const ExpandSpecies& tree = context.species[(size_t)s];
			placeVariant(tree, seed, p, s); // a bush placed on its own has no collider points: none
			// The species' undergrowth (bushUndergrowth): the whole part always, the fraction by chance, out of the trunk's
			// way (1.5 m) to its bush radius (area-uniform), a bush species of its climate by weight.
			if (tree.bushes.empty() || tree.bushesPerTree <= 0.0f)
				continue;
			const float whole = std::floor(tree.bushesPerTree);
			const uint32 count = (uint32)whole + (treeHash01(treeHash(seed, 110u)) < tree.bushesPerTree - whole ? 1u : 0u);
			for (uint32 b = 0; b < count; ++b)
			{
				const uint32 bushSeed = treeHash(seed, 200u + b);
				const float angle = treeHash01(treeHash(bushSeed, 111u)) * 6.28318531f;
				const float radius = glm::mix(1.5f, tree.bushRadius, std::sqrt(treeHash01(treeHash(bushSeed, 112u))));
				const ExpandSpecies& bush = context.species[tree.bushes[treeHash(bushSeed, 113u) % (uint32)tree.bushes.size()]];
				const glm::vec2 at = p + glm::vec2(std::cos(angle), std::sin(angle)) * radius;
				if (!inWater(at))
					placeVariant(bush, bushSeed, at, -1);
			}
		}
	}
}
