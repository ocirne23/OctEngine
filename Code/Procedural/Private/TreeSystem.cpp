module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Tweaks;
import Core.Log;
import Core.Sphere;

import RendererVK;
import File;

import :TreeSystem;
import :TreeSpecies;
import :TreeGenerator;
import :TreeLeafTexture;
import :TreeBarkTexture;
import :TreeImpostor;
import :TerrainSampler;
import :TerrainStreamer;

namespace
{
	using namespace Procedural;

	// `raytraced`: whether RT (GI, RT shadows, RTAO, reflections) sees it - a BLAS only then (TreeSystem::reload:
	// with billboards only the trees' whole billboards; bushes never).
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

	// Uploads LEVEL 0 only, no GPU LOD chain: trees switch representation through their own tiers (mid tier, billboard,
	// far volume), and the mesh LOD steps underneath popped visibly (fewer, larger leaf cards per level, no crossfade).
	// The generator still builds every level (the bakes read level 0); outMeshes[1..] stay empty, outChain none.
	void uploadLodChain(Renderer& renderer, const TreeMesh (&levels)[TREE_PIECE_LODS], const float (&errors)[TREE_PIECE_LODS],
		RenderMesh (&outMeshes)[TREE_PIECE_LODS], uint32& outChain, bool raytraced)
	{
		(void)errors;
		outMeshes[0] = uploadTreeMesh(renderer, levels[0], raytraced);
		outChain = UINT32_MAX;
	}

	// Species textures are generated ONCE into Assets/Local/Trees/Textures (<species>_bark.png, _bark_normal.png,
	// _leaves.png, the billboard bakes) and read back on later loads. Generated output: not in git.
	constexpr const char* TREE_TEXTURE_DIR = "Local/Trees/Textures";

	// Trees/Grove type: "Mixed" alternates every loaded species; the rest select one by its TreeSpecies name.
	// The names are fixed here (a tweak enum is registered before the species load) - add a new species' name.
	constexpr oc::string_view GROVE_TYPES[] = { "Mixed", "Oak", "Pine", "Acacia" };

	// Far-representation cache name infix per piece set (modules keep the original, unprefixed names).
	constexpr const char* PIECE_KINDS[3] = { "", "trunk", "tree" };
	constexpr uint32 TREE_TEXTURE_SIZE = 1024;
	constexpr uint32 TREE_DENSITY_RES = 32; // the far-tree volume's per-variant extinction grid (bakeTreeDensity)

	// A square power-of-two RGBA8 image (the mip chain halves down to 1x1). False when missing or unusable.
	bool loadSquareImage(const oc::string& path, uint32& outSize, oc::vector<uint8>& outRgba)
	{
		uint32 w = 0, h = 0;
		if (!ImageIO::readImageRgba8(path, w, h, outRgba, true)) // explicit user action: enable / reload
			return false;
		if (w != h || w == 0 || (w & (w - 1)) != 0)
		{
			Log::warning(oc::format("Trees: '{}' is {}x{}, needs a square power of two - regenerating", path, w, h));
			return false;
		}
		outSize = w;
		return true;
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

	void saveImage(const oc::string& path, uint32 size, const oc::vector<uint8>& rgba)
	{
		FileSystem::createDirectories(TREE_TEXTURE_DIR, true);
		if (ImageIO::writePngRgba8(path, size, size, rgba, true))
			Log::info(oc::format("Trees: generated '{}'", path));
		else
			Log::warning(oc::format("Trees: failed to write '{}'", path));
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
		destroyTreeSet();
		m_pieces.clear();
		for (const Species& species : m_species)
		{
			for (const oc::vector<PieceMeshes>* pieces : { &species.trunkMeshes, &species.moduleMeshes, &species.variantMeshes })
				for (const PieceMeshes& meshes : *pieces)
				{
					for (uint32 chain : { meshes.barkChain, meshes.leafChain, meshes.trunkChain, meshes.branchChain })
						if (chain != UINT32_MAX)
							Globals::rendererVK.freeMeshLodChain(chain);
					if (meshes.billboardMaterial != UINT16_MAX)
						Globals::rendererVK.destroyTextureMaterial(meshes.billboardMaterial);
				}
			if (species.barkFadeMaterial != UINT16_MAX)
				Globals::rendererVK.releaseMaterial(species.barkFadeMaterial); // derived: textures stay with the source
			if (species.leafFadeMaterial != UINT16_MAX)
				Globals::rendererVK.releaseMaterial(species.leafFadeMaterial);
			// The mid tier's derived materials first (the card ones share the atlas' textures), then the atlas.
			for (uint16 mid : { species.leafMidFadeMaterial, species.branchMidFadeMaterial, species.cardInMaterial, species.cardOutMaterial })
				if (mid != UINT16_MAX)
					Globals::rendererVK.releaseMaterial(mid);
			if (species.cardAtlasMaterial != UINT16_MAX)
				Globals::rendererVK.destroyTextureMaterial(species.cardAtlasMaterial);
			if (species.ownsLeafMaterial)
				Globals::rendererVK.destroyTextureMaterial(species.leafMaterial);
			if (species.ownsBarkMaterial)
				Globals::rendererVK.destroyTextureMaterial(species.barkMaterial);
		}
		m_species.clear();
	}

	void TreeSystem::destroyTreeSet()
	{
		stopExpansion();
		m_near.clear();
		m_expandContext = nullptr;
		// Without world mode the far volume has no record types (it ignores the records).
		if (m_recordTypesSet)
			Globals::rendererVK.setTreeRecordTypes({}, 256.0f, 1, UINT32_MAX);
		m_recordTypesSet = false;
		// Unhook from the terrain first: setVegetation joins its walk, the last caller of the sink.
		if (m_vegHooked)
			Globals::terrain.setVegetation({}, {}, 0);
		m_vegHooked = false;
		m_vegChunkOf.clear();
		m_vegNumChunks = 0;
		if (m_treeSet != UINT32_MAX)
			Globals::rendererVK.destroyTreeInstanceSet(m_treeSet);
		m_treeSet = UINT32_MAX;
	}

	void TreeSystem::applyFadeBands(Renderer& renderer)
	{
		constexpr uint32 FADE_BITS = RendererVKLayout::MATERIAL_FADE_BAND_MASK | RendererVKLayout::MATERIAL_FLAG_DISTANCE_FADE
			| RendererVKLayout::MATERIAL_FLAG_FADE_IN;
		for (Species& species : m_species)
		{
			if (species.barkFadeMaterial == UINT16_MAX)
				continue;
			const float width = species.desc.billboardFadeWidth;
			const float start = glm::max(species.desc.billboardDistance * m_farDistanceScale - width * 0.5f, 0.0f);
			const uint32 fadeOut = RendererVKLayout::makeDistanceFadeFlags(start, width, false);
			const uint32 fadeIn = RendererVKLayout::makeDistanceFadeFlags(start, width, true);
			for (uint16 material : { species.barkFadeMaterial, species.leafFadeMaterial })
				renderer.setMaterialFlags(material, (renderer.getMaterialFlags(material) & ~FADE_BITS) | fadeOut);
			// Force far draws the billboards at every distance: no fade-in then, or they would vanish nearby.
			const uint32 billboardFade = m_forceFar ? 0u : fadeIn;
			for (const oc::vector<PieceMeshes>* pieces : { &species.moduleMeshes, &species.trunkMeshes, &species.variantMeshes })
				for (const PieceMeshes& meshes : *pieces)
					if (meshes.billboardMaterial != UINT16_MAX)
						renderer.setMaterialFlags(meshes.billboardMaterial, (renderer.getMaterialFlags(meshes.billboardMaterial) & ~FADE_BITS) | billboardFade);
			// The MID tier: the leaves and the branch bark fade OUT and the card mesh IN over the mid band; the card mesh
			// fades OUT over the far band with the trunk (barkFadeMaterial) while the billboard fades in
			// (tree_cull.inc.glsl picks the side per tree).
			if (species.leafMidFadeMaterial == UINT16_MAX)
				continue;
			const float midStart = glm::max(midDistance(species) * m_farDistanceScale - width * 0.5f, 0.0f);
			auto setFade = [&](uint16 material, uint32 fade)
			{
				if (material != UINT16_MAX)
					renderer.setMaterialFlags(material, (renderer.getMaterialFlags(material) & ~FADE_BITS) | fade);
			};
			setFade(species.leafMidFadeMaterial, RendererVKLayout::makeDistanceFadeFlags(midStart, width, false));
			setFade(species.branchMidFadeMaterial, RendererVKLayout::makeDistanceFadeFlags(midStart, width, false));
			setFade(species.cardInMaterial, RendererVKLayout::makeDistanceFadeFlags(midStart, width, true));
			setFade(species.cardOutMaterial, fadeOut);
		}
	}

	float TreeSystem::midDistance(const Species& species) const
	{
		return species.desc.billboardDistance * m_branchCardDistance;
	}

	void TreeSystem::initialize()
	{
		auto respawn = [this]() { m_respawn = true; };
		Tweak::boolean("Trees", "Enabled", &m_enabled);
		Tweak::boolean("Trees", "Reload species", &m_reload);
		Tweak::boolean("Trees", "Respawn preview", &m_respawn);
		Tweak::boolean("Trees", "Regenerate textures", &m_regenerateTextures, [this]() { if (m_regenerateTextures) m_reload = true; });
		Tweak::boolean("Trees", "Show piece library", &m_showLibrary, respawn);
		Tweak::intVar("Trees", "Grove size", &m_gridSize, 1, 512, 1.0f, respawn);
		Tweak::floatVar("Trees", "Spacing (m)", &m_spacing, 2.0f, 50.0f, 0.1f, respawn);
		Tweak::floatVar("Trees", "Position jitter", &m_positionJitter, 0.0f, 2.0f, 0.01f, respawn);
		Tweak::floatVar("Trees", "Size variation", &m_sizeVariation, 0.0f, 2.0f, 0.01f, respawn);
		Tweak::floatVar("Trees", "Bushes per tree", &m_bushesPerTree, 0.0f, 8.0f, 0.05f, respawn);
		// GPU path: a bush farther than this from the shadow cascades' centre casts no sun shadow. 0 = no limit.
		Tweak::floatVar("Trees", "Bush shadow distance (m)", &m_bushShadowDistance, 0.0f, 5000.0f, 1.0f, respawn);
		Tweak::intVar("Trees", "Seed", &m_seed, 0, 1000000, 1.0f, respawn);
		Tweak::enumVar("Trees", "Grove type", &m_groveType, GROVE_TYPES, respawn);
		// World mode ("Trees/World/Enabled" with the GPU expansion): the near chunks' records as trees + bushes.
		Tweak::intVar("Trees/World", "Near radius (chunks)", &m_nearRadius, 1, 32, 1.0f, respawn);
		Tweak::intVar("Trees/World", "Set capacity (pieces)", &m_worldCapacity, 10000, 8000000, 1000.0f, respawn);
		Tweak::intVar("Trees/World", "Expand per frame", &m_expandPerFrame, 1, 32, 1.0f);
		static constexpr oc::string_view FAR_MODES[] = { "Billboards", "None" };
		Tweak::enumVar("Trees", "Far mode", &m_farMode, FAR_MODES, [this]() { m_reload = true; });
		static constexpr oc::string_view BILLBOARD_VIEWS[] = { "2 (side + top)", "4 (+ other side + bottom)" };
		Tweak::enumVar("Trees", "Billboard views", &m_billboardViews, BILLBOARD_VIEWS, [this]() { m_reload = true; });
		Tweak::floatVar("Trees", "Far distance scale", &m_farDistanceScale, 0.0f, 10.0f, 0.01f, [this]() { m_fadeBandsDirty = true; });
		// The MID tier: from this fraction of each species' billboard distance the leaves mesh gives way to one billboard
		// card per branch module (the bark mesh stays). 0 = off.
		Tweak::floatVar("Trees", "Branch card distance", &m_branchCardDistance, 0.0f, 1.0f, 0.01f,
			[this]() { m_fadeBandsDirty = true; m_respawn = true; });
		Tweak::boolean("Trees", "Force far", &m_forceFar, [this]() { m_fadeBandsDirty = true; });
		Tweak::boolean("Trees", "GPU expansion", &m_gpuExpansion, respawn);
		m_world.initialize();
	}

	void TreeSystem::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		m_world.update(renderer, camera, maps);
		if (!m_enabled)
		{
			// Disabled frees everything, so enabling again re-reads the .tree files.
			if (m_loaded)
			{
				clearAll();
				m_loaded = false;
				m_spawned = false;
			}
			return;
		}

		ProfileScope profileScope("Trees", EProfileCategory::Procedural);
		if (!m_loaded || m_reload)
		{
			m_reload = false;
			clearAll();
			reload(renderer);
			m_regenerateTextures = false;
			m_loaded = true;
			m_spawned = false;
		}
		// The terrain's chunk size changed: the pieces sort into the old chunks.
		if (m_vegHooked && m_vegChunkSize != Globals::terrain.chunkSize())
			m_respawn = true;
		// WORLD MODE: TreeWorld's records instead of the preview grove (the GPU path only). Switching, or new records
		// (a TreeWorld restart), respawns.
		const bool worldMode = m_world.enabled() && m_gpuExpansion && maps != nullptr;
		m_world.requireKeepRadius(worldMode ? m_nearRadius + 1 : 0);
		if (worldMode != m_worldMode || (worldMode && m_world.generation() != m_worldGeneration))
			m_respawn = true;
		// A respawn hooks the new set into the terrain AFTER this frame's walk (setVegetation joins it): this frame
		// draws every chunk itself.
		const bool respawned = !m_spawned || m_respawn;
		if (!m_spawned || m_respawn)
		{
			m_respawn = false;
			destroyTreeSet();
			m_pieces.clear();
			m_worldMode = worldMode;
			if (worldMode)
				spawnWorld(renderer, maps);
			else
				spawnPreview(renderer, camera, maps.get());
			m_spawned = true;
		}
		if (m_worldMode)
			updateWorld(renderer, camera);
		if (m_fadeBandsDirty)
		{
			m_fadeBandsDirty = false;
			applyFadeBands(renderer);
		}

		// The GPU path: the expansion decides per piece (the same rules as the CPU loop below). The terrain's walk lists
		// the chunks (the sink, spawnPreview); without it - terrain off, or no walk this frame - every chunk draws.
		m_sinkDistanceScale.store(m_farDistanceScale, oc::memory_order_relaxed);
		m_sinkForceFar.store(m_forceFar, oc::memory_order_relaxed);
		if (m_treeSet != UINT32_MAX)
		{
			renderer.bindTreeInstanceSet(m_treeSet);
			if (!m_vegHooked || !Globals::terrain.vegetationRouted() || respawned)
			{
				// Every chunk of the set (a dynamic set: the live ones).
				m_vegFallback.clear();
				if (m_worldMode)
					for (const auto& [key, chunk] : m_vegChunkOf)
						m_vegFallback.push_back({ (uint32)chunk, RendererVKLayout::PASS_ALL });
				else
					for (uint32 c = 0; c < glm::max(m_vegNumChunks, 1u); ++c)
						m_vegFallback.push_back({ c, RendererVKLayout::PASS_ALL });
				renderer.renderTreeInstanceSet(m_treeSet, m_vegFallback, m_farDistanceScale, m_forceFar);
			}
		}
		// The far-tree volume lays its cells out by the camera's height above the ground under it.
		if (maps)
		{
			const FileSystem::AllowMainThreadIO cameraGroundIo; // the tile under the camera (TerrainStreamer read it already)
			renderer.setFarTreeCameraGround(maps->sampleHeight(camera.position.x, camera.position.z));
		}

		// The CPU path (GPU expansion off): per piece, its billboard crossfade (see PlacedPiece); without a far
		// representation (Far mode None) the meshes alone.
		constexpr uint32 SHADOW_AND_GI = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI;
		for (PlacedPiece& piece : m_pieces)
		{
			if (piece.far.isValid())
			{
				// The same band the materials carry (applyFadeBands). A pixel's distance varies by up to the
				// module's radius from the centre's, so both sides draw while any of it can be in the band.
				// The billboard stands in for the piece in every pass but MAIN (shadow, GI, RT): the mesh draws in
				// MAIN only, the cards always draw, in MAIN too only while they show (as tree_cull.inc.glsl).
				const float switchDistance = piece.farDistance * m_farDistanceScale;
				const float bandStart = glm::max(switchDistance - piece.fadeWidth * 0.5f, 0.0f);
				const float bandEnd = bandStart + piece.fadeWidth;
				const float distance = glm::distance(camera.position, piece.centre);
				if (m_forceFar)
					renderer.renderNode(piece.far);
				else if (switchDistance <= 0.0f || distance + piece.radius < bandStart)
				{
					renderer.renderNode(piece.bark, RendererVKLayout::PASS_MAIN);
					renderer.renderNode(piece.leaves, RendererVKLayout::PASS_MAIN);
					renderer.renderNode(piece.far, SHADOW_AND_GI);
				}
				else if (distance - piece.radius > bandEnd)
					renderer.renderNode(piece.far);
				else
				{
					renderer.renderNode(piece.barkFade, RendererVKLayout::PASS_MAIN);
					renderer.renderNode(piece.leavesFade, RendererVKLayout::PASS_MAIN);
					renderer.renderNode(piece.far);
				}
				continue;
			}
			renderer.renderNode(piece.bark);
			renderer.renderNode(piece.leaves);
		}
	}

	void TreeSystem::reload(Renderer& renderer)
	{
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
					Log::warning(oc::format("Trees: failed to load '{}': {}", entry.path, error));
					continue;
				}
			}

			Species& species = m_species.emplace_back();
			species.desc = oc::move(desc);
			generateTreeLibrary(species.desc, species.library);

			size_t barkTris[TREE_PIECE_LODS] = {}, leafTris[TREE_PIECE_LODS] = {};
			// RT sees the MESHES only without billboards (Far mode None: the meshes stand in for the tree in
			// shadow + GI); with billboards the whole billboard is the tree's only RT representation (buildBillboards).
			// Bushes never: too small to matter to GI / RT shadows, and too many for the TLAS.
			const bool meshesRaytraced = m_farMode != 0 && !species.desc.bush;
			auto uploadPieces = [&](const oc::vector<TreePiece>& pieces, oc::vector<PieceMeshes>& out)
			{
				out.resize(pieces.size());
				for (size_t i = 0; i < pieces.size(); ++i)
				{
					uploadLodChain(renderer, pieces[i].bark, pieces[i].lodError, out[i].bark, out[i].barkChain, meshesRaytraced);
					uploadLodChain(renderer, pieces[i].leaves, pieces[i].lodError, out[i].leaves, out[i].leafChain, meshesRaytraced);
					// Baked variants: the bark split as well (the mid tier's trunk / branches, main pass only); empty
					// elsewhere - no meshes.
					uploadLodChain(renderer, pieces[i].trunkBark, pieces[i].lodError, out[i].trunkBark, out[i].trunkChain, false);
					uploadLodChain(renderer, pieces[i].branchBark, pieces[i].lodError, out[i].branchBark, out[i].branchChain, false);
					for (uint32 k = 0; k < TREE_PIECE_LODS; ++k)
					{
						barkTris[k] += numTriangles(pieces[i].bark[k]);
						leafTris[k] += numTriangles(pieces[i].leaves[k]);
					}
				}
			};
			uploadPieces(species.library.trunks, species.trunkMeshes);
			uploadPieces(species.library.modules, species.moduleMeshes);
			// The baked whole trees the grove places (counted into the same triangle totals).
			species.variants.resize((size_t)species.desc.variantCount);
			for (int v = 0; v < species.desc.variantCount; ++v)
				bakeTreeVariant(species.desc, species.library, treeHash(species.desc.seed, 5000u + (uint32)v), species.variants[(size_t)v]);
			uploadPieces(species.variants, species.variantMeshes);
			// The far-tree volume's view of every baked tree (RendererVK TreeVolumePipeline). Cluster cards are about
			// half opaque; a single leaf diamond fully.
			const float leafCoverage = species.desc.leafType == ETreeLeafType::Cluster ? 0.5f : 1.0f;
			for (size_t v = 0; v < species.variants.size(); ++v)
			{
				PieceMeshes& meshes = species.variantMeshes[v];
				TreeBillboardBox box;
				bakeTreeDensity(species.variants[v], TREE_DENSITY_RES, leafCoverage, meshes.density, box);
				meshes.densityMin = box.min;
				meshes.densityMax = box.max;
			}
			const oc::string& name = species.desc.name.empty() ? entry.name : species.desc.name;
			// Level-0 images, kept for the billboard bake.
			oc::vector<uint8> barkAlbedo, leafImage;
			uint32 barkSize = 0, leafSize = 0;
			{
				const oc::string albedoPath = oc::format("{}/{}_bark.png", TREE_TEXTURE_DIR, name);
				const oc::string normalPath = oc::format("{}/{}_bark_normal.png", TREE_TEXTURE_DIR, name);
				uint32 normalSize = 0;
				oc::vector<uint8>& albedo = barkAlbedo;
				uint32& size = barkSize;
				oc::vector<uint8> normal;
				const bool loaded = !m_regenerateTextures && loadSquareImage(albedoPath, size, albedo)
					&& loadSquareImage(normalPath, normalSize, normal) && normalSize == size;
				if (!loaded)
				{
					size = TREE_TEXTURE_SIZE;
					generateBarkImages(species.desc, size, albedo, normal);
					saveImage(albedoPath, size, albedo);
					saveImage(normalPath, size, normal);
				}
				TreeBarkTexture bark;
				buildBarkMips(albedo, normal, size, bark);
				oc::vector<oc::span<uint8>> albedoMips, normalMips;
				for (oc::vector<uint8>& level : bark.albedoMips)
					albedoMips.push_back(oc::span<uint8>(level.data(), level.size()));
				for (oc::vector<uint8>& level : bark.normalMips)
					normalMips.push_back(oc::span<uint8>(level.data(), level.size()));
				species.barkMaterial = renderer.createTextureMaterial(bark.size, bark.size, albedoMips, 0.0f,
					oc::format("TreeBark/{}", name).c_str(), &normalMips);
				species.ownsBarkMaterial = true;
			}
			if (species.desc.leafType == ETreeLeafType::Cluster)
			{
				const oc::string leavesPath = oc::format("{}/{}_leaves.png", TREE_TEXTURE_DIR, name);
				uint32& size = leafSize;
				oc::vector<uint8>& image = leafImage;
				if (m_regenerateTextures || !loadSquareImage(leavesPath, size, image))
				{
					size = TREE_TEXTURE_SIZE;
					generateLeafClusterImage(species.desc, size, image);
					saveImage(leavesPath, size, image);
				}
				TreeLeafTexture texture;
				buildLeafClusterMips(image, size, texture);
				oc::vector<oc::span<uint8>> mips;
				for (oc::vector<uint8>& level : texture.mips)
					mips.push_back(oc::span<uint8>(level.data(), level.size()));
				// LEAF: the sun shines through the cluster cards (the lit FS's transmission).
				species.leafMaterial = renderer.createTextureMaterial(texture.size, texture.size, mips, TREE_LEAF_ALPHA_CUTOFF,
					oc::format("TreeLeafCluster/{}", name).c_str(), nullptr, RendererVKLayout::MATERIAL_FLAG_LEAF);
				species.ownsLeafMaterial = true;
				species.leafPipeline = RendererVKLayout::EPipelineIndex::LitMasked;
			}
			else
			{
				species.leafMaterial = renderer.getOrCreateSolidColorMaterial(species.desc.leafColor);
				// Single leaves are solid-coloured: the bake samples a 1x1 opaque image of that colour.
				leafSize = 1;
				leafImage = { (uint8)(glm::clamp(species.desc.leafColor.x, 0.0f, 1.0f) * 255.0f + 0.5f),
					(uint8)(glm::clamp(species.desc.leafColor.y, 0.0f, 1.0f) * 255.0f + 0.5f),
					(uint8)(glm::clamp(species.desc.leafColor.z, 0.0f, 1.0f) * 255.0f + 0.5f), 255 };
			}
			species.volumeAlbedo = meanLeafAlbedo(leafImage, species.desc.leafColor);
			if (m_farMode == 0 && species.desc.billboardDistance > 0.0f)
			{
				buildBillboards(renderer, species, name, barkAlbedo, barkSize, leafImage, leafSize);
				// The crossfade's fade-out copies of the module mesh materials (bands set by applyFadeBands).
				species.barkFadeMaterial = renderer.deriveMaterial(species.barkMaterial, 0);
				species.leafFadeMaterial = renderer.deriveMaterial(species.leafMaterial, 0);
				// The MID tier's (bands set by applyFadeBands): the leaves and the branch bark fading out over the mid band,
				// the card mesh fading in over it (from the atlas buildBillboards made) and out over the far band.
				species.leafMidFadeMaterial = renderer.deriveMaterial(species.leafMaterial, 0);
				species.branchMidFadeMaterial = renderer.deriveMaterial(species.barkMaterial, 0);
				if (species.cardAtlasMaterial != UINT16_MAX)
				{
					species.cardInMaterial = renderer.deriveMaterial(species.cardAtlasMaterial, 0);
					species.cardOutMaterial = renderer.deriveMaterial(species.cardAtlasMaterial, 0);
				}
			}
			Log::info(oc::format("Trees: '{}' - {} trunks, {} modules, library triangles bark/leaf per LOD: {}/{} {}/{} {}/{} {}/{}",
				species.desc.name, species.library.trunks.size(), species.library.modules.size(),
				barkTris[0], leafTris[0], barkTris[1], leafTris[1], barkTris[2], leafTris[2], barkTris[3], leafTris[3]));
		}
		applyFadeBands(renderer);
	}

	void TreeSystem::buildBillboards(Renderer& renderer, Species& species, const oc::string& name,
		oc::span<const uint8> barkAlbedo, uint32 barkSize, oc::span<const uint8> leafImage, uint32 leafSize)
	{
		const uint32 size = (uint32)species.desc.billboardSize;
		const uint32 numViews = m_billboardViews == 0 ? 2u : 4u;

		oc::vector<FileSystem::DirEntry> existing;
		FileSystem::listDirectory(TREE_TEXTURE_DIR, existing, true);

		// Modules, trunks and the baked whole trees alike. A trunk's / tree's +Y is world up, so its "top" card
		// (normal +Z) stands vertical too: two crossed vertical cards through the trunk axis. A whole tree also gets
		// a HORIZONTAL card for the top-down view (a module's "top" card already lies flat; a bare trunk from
		// above is a dot).
		const oc::vector<TreePiece>* pieceSets[3] = { &species.library.modules, &species.library.trunks, &species.variants };
		oc::vector<PieceMeshes>* meshSets[3] = { &species.moduleMeshes, &species.trunkMeshes, &species.variantMeshes };
		// The modules' level-0.. mip chains and axis-coded cards, kept for the mid tier's atlas + card meshes (below).
		const size_t numModules = species.library.modules.size();
		oc::vector<oc::vector<oc::vector<uint8>>> moduleAlbedo(numModules), moduleNormal(numModules);
		oc::vector<TreeMesh> moduleCards(numModules);
		for (uint32 set = 0; set < 3; ++set)
		for (size_t i = 0; i < pieceSets[set]->size(); ++i)
		{
			const TreePiece& piece = (*pieceSets[set])[i];
			PieceMeshes& meshes = (*meshSets[set])[i];
			const TreeBillboardBox box = billboardBox(piece);
			const bool horizontal = set == 2;
			// Mips down to ~4 px per strip, while the strip boundaries stay on texel boundaries (billboardLayout).
			const uint32 numMips = billboardLayout(size, numViews, horizontal).numMips;

			// The cache: geometry + settings in the name, stale slot files removed.
			// The view count is in the name, so both Billboard views settings keep their own cache.
			const oc::string prefix = oc::format("{}_{}billboard{}v{}_", name, PIECE_KINDS[set], i, numViews);
			// Before the view count was named.
			const oc::string legacyPrefix = oc::format("{}_{}billboard{}_", name, PIECE_KINDS[set], i);
			const float bend = species.desc.billboardNormalBend;
			const uint32 settingsHash = treeHash(0xB1B0A2D5u ^ numViews, (uint32)std::round(bend * 1000.0f));
			const oc::string stem = oc::format("{}{:08x}", prefix, treeBakeHash(piece, 2u, size) ^ settingsHash);
			const oc::string albedoPath = oc::format("{}/{}.png", TREE_TEXTURE_DIR, stem);
			const oc::string normalPath = oc::format("{}/{}_normal.png", TREE_TEXTURE_DIR, stem);

			oc::vector<uint8> albedo, normal;
			uint32 albedoSize = 0, normalSize = 0;
			const bool loaded = !m_regenerateTextures && loadSquareImage(albedoPath, albedoSize, albedo)
				&& loadSquareImage(normalPath, normalSize, normal) && albedoSize == size && normalSize == size;
			if (!loaded)
			{
				for (const FileSystem::DirEntry& entry : existing)
					if (entry.name.rfind(prefix.c_str(), 0) == 0 || entry.name.rfind(legacyPrefix.c_str(), 0) == 0)
						FileSystem::remove(entry.path, true);
				bakeBillboards(piece, TreeBakeImage{ barkAlbedo, barkSize }, TreeBakeImage{ leafImage, leafSize }, size, bend, numViews,
					horizontal, albedo, normal);
				saveImage(albedoPath, size, albedo);
				saveImage(normalPath, size, normal);
			}

			// `Billboard AlbedoScale` (whole trees only): x the colour in LINEAR space (the texture is sRGB), applied here -
			// after the cache - so a change needs no re-bake.
			const float albedoScale = species.desc.billboardAlbedoScale;
			if (horizontal && albedoScale != 1.0f)
			{
				uint8 lut[256];
				for (uint32 v = 0; v < 256; ++v)
				{
					const float c = (float)v / 255.0f;
					const float lin = (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f)) * albedoScale;
					const float enc = lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(glm::min(lin, 1.0f), 1.0f / 2.4f) - 0.055f;
					lut[v] = (uint8)(glm::clamp(enc, 0.0f, 1.0f) * 255.0f + 0.5f);
				}
				for (size_t t = 0; t + 3 < albedo.size(); t += 4)
					for (size_t c = 0; c < 3; ++c)
						albedo[t + c] = lut[albedo[t + c]];
			}

			// Coverage-preserving albedo mips (alpha scaled up per level to keep the level-0 coverage).
			TreeLeafTexture albedoChain;
			buildLeafClusterMips(albedo, size, albedoChain);
			oc::vector<oc::vector<uint8>> normalChain;
			buildNormalMips(normal, size, normalChain); // RGB renormalized, A (the interior) averaged
			oc::vector<oc::span<uint8>> albedoMips, normalMips;
			for (uint32 k = 0; k < numMips; ++k)
			{
				albedoMips.push_back(oc::span<uint8>(albedoChain.mips[k].data(), albedoChain.mips[k].size()));
				normalMips.push_back(oc::span<uint8>(normalChain[k].data(), normalChain[k].size()));
			}
			// FOLIAGE: the sun shadow is not rejected by the flat card normal (see instanced_indirect.fs.glsl).
			// "TreeBillboard/<piece>": one box per billboard in the VRAM view.
			meshes.billboardMaterial = renderer.createTextureMaterial(size, size, albedoMips, TREE_LEAF_ALPHA_CUTOFF,
				oc::format("TreeBillboard/{}_{}{}", name, PIECE_KINDS[set], i).c_str(), &normalMips, RendererVKLayout::MATERIAL_FLAG_BILLBOARD | RendererVKLayout::MATERIAL_FLAG_LEAF
				| (horizontal ? RendererVKLayout::MATERIAL_FLAG_BILLBOARD_TOP_CARD : 0u));

			TreeMesh cards;
			billboardMesh(box, size, numViews, horizontal, cards);
			// RT: the WHOLE-TREE billboards only (the variants' - the library rows' module / trunk billboards are debug
			// views), and never a bush's.
			meshes.billboard = uploadTreeMesh(renderer, cards, horizontal && !species.desc.bush);
			meshes.farCentre = (box.min + box.max) * 0.5f;
			meshes.farRadius = glm::length(box.max - box.min) * 0.5f;
			if (set == 0)
			{
				// Kept for the atlas (the material above has copied its levels) and the card meshes.
				moduleAlbedo[i].assign(albedoChain.mips.begin(), albedoChain.mips.begin() + numMips);
				moduleNormal[i].assign(normalChain.begin(), normalChain.begin() + numMips);
				billboardMesh(box, size, numViews, false, moduleCards[i], true);
			}
		}

		// THE MID TIER's BRANCH CARDS. One ATLAS per species: the module billboards stacked top to bottom (module m in
		// rows [m, m + 1) x size), each level the stack of the modules' own levels - exact (every module block halves
		// cleanly) and every module keeps its coverage-preserving alpha. Then per baked variant ONE card mesh: each
		// module placement's cards in the variant's space, v remapped into the module's block, the card axis code
		// (texCoords.z, billboardMesh) remapped with it.
		if (numModules == 0 || species.variants.empty())
			return;
		const uint32 numMips = billboardLayout(size, numViews, false).numMips;
		oc::vector<oc::vector<uint8>> atlasAlbedo(numMips), atlasNormal(numMips);
		for (uint32 k = 0; k < numMips; ++k)
			for (size_t m = 0; m < numModules; ++m)
			{
				atlasAlbedo[k].insert(atlasAlbedo[k].end(), moduleAlbedo[m][k].begin(), moduleAlbedo[m][k].end());
				atlasNormal[k].insert(atlasNormal[k].end(), moduleNormal[m][k].begin(), moduleNormal[m][k].end());
			}
		oc::vector<oc::span<uint8>> albedoMips, normalMips;
		for (uint32 k = 0; k < numMips; ++k)
		{
			albedoMips.push_back(oc::span<uint8>(atlasAlbedo[k].data(), atlasAlbedo[k].size()));
			normalMips.push_back(oc::span<uint8>(atlasNormal[k].data(), atlasNormal[k].size()));
		}
		// Foliage cards like the whole-tree billboards (LitFoliage: the same shading and tweaks), but no edge-on fade.
		species.cardAtlasMaterial = renderer.createTextureMaterial(size, size * (uint32)numModules, albedoMips, TREE_LEAF_ALPHA_CUTOFF,
			oc::format("TreeCardAtlas/{}", name).c_str(), &normalMips, RendererVKLayout::MATERIAL_FLAG_BILLBOARD
			| RendererVKLayout::MATERIAL_FLAG_LEAF | RendererVKLayout::MATERIAL_FLAG_NO_EDGE_FADE);

		const float invModules = 1.0f / (float)numModules;
		for (size_t v = 0; v < species.variants.size(); ++v)
		{
			TreeMesh merged;
			for (const TreePiecePlacement& placement : species.variants[v].placements)
			{
				if (placement.trunk || placement.pieceIdx >= numModules)
					continue;
				const TreeMesh& src = moduleCards[placement.pieceIdx];
				const Transform& t = placement.local;
				const float block = (float)placement.pieceIdx;
				const uint32 base = merged.numVertices();
				for (uint32 k = 0; k < src.numVertices(); ++k)
				{
					merged.positions.push_back(t.transformPoint(src.positions[k]));
					merged.normals.push_back(t.quat * src.normals[k]); // uniform scale: directions only rotate
					merged.tangents.push_back(t.quat * src.tangents[k]);
					merged.bitangents.push_back(t.quat * src.bitangents[k]);
					const glm::vec3 uv = src.texCoords[k];
					const float axisZ = uv.z >= 2.0f ? TREE_CARD_AXIS_CODE + (block + (uv.z - TREE_CARD_AXIS_CODE)) * invModules : 0.0f;
					merged.texCoords.push_back(glm::vec3(uv.x, (block + uv.y) * invModules, axisZ));
					merged.bones.push_back(0);
				}
				for (uint32 index : src.indices)
					merged.indices.push_back(base + index);
			}
			species.variantMeshes[v].cards = uploadTreeMesh(renderer, merged, false); // main pass only
		}
	}

	void TreeSystem::spawnPiece(Renderer& renderer, const Species& species, const PieceMeshes& meshes, const Transform& transform)
	{
		PlacedPiece& piece = m_pieces.emplace_back();
		// Level 0: the GPU cull redirects each instance to its LOD through the chain.
		if (meshes.bark[0].isValid())
			piece.bark = renderer.spawnMeshNode(meshes.bark[0], species.barkMaterial, RendererVKLayout::EPipelineIndex::LitOpaque, transform);
		if (meshes.leaves[0].isValid())
			piece.leaves = renderer.spawnMeshNode(meshes.leaves[0], species.leafMaterial, species.leafPipeline, transform);
		if (meshes.billboard.isValid())
		{
			piece.far = renderer.spawnMeshNode(meshes.billboard, meshes.billboardMaterial, RendererVKLayout::EPipelineIndex::LitFoliage, transform);
			piece.farDistance = species.desc.billboardDistance;
			piece.fadeWidth = species.desc.billboardFadeWidth;
			piece.radius = meshes.farRadius * transform.scale;
			// The crossfade copies: the same LOD-chain meshes on LitMasked (the dither discards) with the fade-out
			// materials. Drawn only inside the band, so the bark keeps LitOpaque's early depth elsewhere.
			if (meshes.bark[0].isValid())
				piece.barkFade = renderer.spawnMeshNode(meshes.bark[0], species.barkFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked, transform);
			if (meshes.leaves[0].isValid())
				piece.leavesFade = renderer.spawnMeshNode(meshes.leaves[0], species.leafFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked, transform);
		}
		piece.centre = transform.transformPoint(meshes.farCentre);
	}

	void TreeSystem::buildGpuTypes(oc::vector<Renderer::TreeInstanceType>& gpuTypes, oc::unordered_map<const PieceMeshes*, uint32>& typeOf) const
	{
		for (const Species& species : m_species)
			for (const oc::vector<PieceMeshes>* set : { &species.trunkMeshes, &species.moduleMeshes, &species.variantMeshes })
				for (const PieceMeshes& meshes : *set)
				{
					Renderer::TreeInstanceType type;
					const RenderMesh* bark = meshes.bark[0].isValid() ? &meshes.bark[0] : nullptr;
					const RenderMesh* leaves = meshes.leaves[0].isValid() ? &meshes.leaves[0] : nullptr;
					type.bark = { bark, species.barkMaterial, RendererVKLayout::EPipelineIndex::LitOpaque };
					type.leaves = { leaves, species.leafMaterial, species.leafPipeline };
					if (!meshes.density.empty())
					{
						type.density = meshes.density.data();
						type.densityRes = TREE_DENSITY_RES;
						type.densityMin = meshes.densityMin;
						type.densityMax = meshes.densityMax;
						type.albedo = species.volumeAlbedo;
					}
					if (meshes.billboard.isValid())
					{
						type.billboard = { &meshes.billboard, meshes.billboardMaterial, RendererVKLayout::EPipelineIndex::LitFoliage };
						type.barkFade = { bark, species.barkFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked };
						type.leavesFade = { leaves, species.leafFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked };
						type.farDistance = species.desc.billboardDistance;
						type.fadeWidth = species.desc.billboardFadeWidth;
						// The MID tier of a baked variant: the TRUNK on its own (its own mesh, fading only in the far band),
						// the BRANCH bark and the leaves fading out over the mid band while the CARD mesh (every module
						// placement's billboard, merged; the species' atlas) fades in - and out over the far band.
						if (set == &species.variantMeshes && midDistance(species) > 0.0f && meshes.cards.isValid()
							&& meshes.trunkBark[0].isValid() && species.cardInMaterial != UINT16_MAX)
						{
							const RenderMesh* branches = meshes.branchBark[0].isValid() ? &meshes.branchBark[0] : nullptr;
							type.trunk = { &meshes.trunkBark[0], species.barkMaterial, RendererVKLayout::EPipelineIndex::LitOpaque };
							type.trunkFade = { &meshes.trunkBark[0], species.barkFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked };
							type.bark = { branches, species.barkMaterial, RendererVKLayout::EPipelineIndex::LitOpaque };
							type.barkFade = { branches, species.branchMidFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked };
							type.leavesFade = { leaves, species.leafMidFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked };
							type.cardsIn = { &meshes.cards, species.cardInMaterial, RendererVKLayout::EPipelineIndex::LitFoliage };
							type.cardsOut = { &meshes.cards, species.cardOutMaterial, RendererVKLayout::EPipelineIndex::LitFoliage };
							type.midDistance = midDistance(species);
							type.midFadeWidth = species.desc.billboardFadeWidth;
						}
					}
					if (species.desc.bush)
						type.shadowDistance = m_bushShadowDistance;
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
			static_assert(sizeof(TerrainStreamer::VegetationDraw) == sizeof(Renderer::TreeChunkDraw));
			Globals::rendererVK.renderTreeInstanceSet(m_treeSet,
				oc::span<const Renderer::TreeChunkDraw>((const Renderer::TreeChunkDraw*)draws.data(), draws.size()),
				m_sinkDistanceScale.load(oc::memory_order_relaxed), m_sinkForceFar.load(oc::memory_order_relaxed));
		};
		Globals::terrain.setVegetation(lookup, sink, numChunks);
		m_vegHooked = true;
		m_vegNumChunks = numChunks;
	}

	void TreeSystem::spawnPreview(Renderer& renderer, const Camera& camera, const ITerrainSampler* maps)
	{
		if (m_species.empty())
			return;

		// Grove centred in front of the camera, aligned to its heading.
		glm::vec2 fwd(-camera.viewMatrix[0][2], -camera.viewMatrix[2][2]);
		fwd = glm::dot(fwd, fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec2(0.0f, -1.0f);
		const glm::vec2 right(-fwd.y, fwd.x);
		const int grid = glm::max(m_gridSize, 1);
		const float half = (float)(grid - 1) * 0.5f * m_spacing;
		const glm::vec2 center = glm::vec2(camera.position.x, camera.position.z) + fwd * (half + 15.0f);
		auto groundAt = [&](glm::vec2 p) { return maps ? maps->sampleHeight(p.x, p.y) : 0.0f; };

		// Grove type: one species by name, or all alternating (also when the named one is not loaded).
		const Species* only = nullptr;
		if (m_groveType > 0 && m_groveType < (int)std::size(GROVE_TYPES))
		{
			for (const Species& species : m_species)
				if (oc::string_view(species.desc.name) == GROVE_TYPES[m_groveType])
					only = &species;
			if (!only)
				Log::warning(oc::format("Trees: grove type '{}' has no loaded species of that name - using a mixed grove",
					GROVE_TYPES[m_groveType]));
		}

		// G4: on the GPU path every placed piece goes into ONE baked set (RendererVK tree_cull.inc.glsl): a
		// piece TYPE per library piece, then per frame one call instead of a renderNode per piece node.
		const bool gpu = m_gpuExpansion;
		oc::vector<Renderer::TreeInstanceType> gpuTypes;
		oc::unordered_map<const PieceMeshes*, uint32> typeOf;
		oc::vector<Renderer::TreeInstancePiece> gpuPieces;
		if (gpu)
			buildGpuTypes(gpuTypes, typeOf);
		auto place =[&](const Species& species, const PieceMeshes& meshes, const Transform& transform)
		{
			if (!gpu)
			{
				spawnPiece(renderer, species, meshes, transform);
				return;
			}
			Renderer::TreeInstancePiece& piece = gpuPieces.emplace_back();
			piece.transform = transform;
			piece.centre = transform.transformPoint(meshes.farCentre);
			piece.radius = meshes.farRadius * transform.scale;
			piece.type = typeOf[&meshes];
		};

		// A BAKED variant per plant (no runtime composite): variant, scale and yaw from the seed.
		auto placeVariant = [&](const Species& species, uint32 seed, glm::vec2 p)
		{
			if (species.variantMeshes.empty())
				return;
			const uint32 variant = treeHash(seed, 102u) % (uint32)species.variantMeshes.size();
			// The species' own range, then "Size variation": x 2^(+-v), log-uniform, so halving and doubling are as likely.
			const float scale = glm::mix(species.desc.scale.x, species.desc.scale.y, treeHash01(treeHash(seed, 103u)))
				* std::exp2(m_sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0f - 1.0f));
			const glm::quat yaw = glm::angleAxis(treeHash01(treeHash(seed, 104u)) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
			place(species, species.variantMeshes[variant], Transform(glm::vec3(p.x, groundAt(p) - 0.05f, p.y), scale, yaw));
		};
		// Trees from the TREE species only ("Mixed" alternates them); BUSHES (`Kind Bush`) are scattered around them.
		oc::vector<const Species*> treeSpecies, bushSpecies;
		for (const Species& species : m_species)
			(species.desc.bush ? bushSpecies : treeSpecies).push_back(&species);

		// Per tree species the bushes of its CLIMATE (`Climate`, case-insensitive); a tree whose climate no bush shares
		// takes every bush.
		auto sameClimate = [](const oc::string& a, const oc::string& b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
					return false;
			return true;
		};
		oc::unordered_map<const Species*, oc::vector<const Species*>> bushesFor;
		for (const Species* tree : treeSpecies)
		{
			oc::vector<const Species*>& list = bushesFor[tree];
			for (const Species* bush : bushSpecies)
				if (sameClimate(bush->desc.climate, tree->desc.climate))
					list.push_back(bush);
			if (list.empty())
				list = bushSpecies;
		}

		auto placeTree = [&](const Species* tree, uint32 seed, glm::vec2 p)
		{
			if (tree)
				placeVariant(*tree, seed, p);
			// "Bushes per tree": the whole part always, the fraction by chance. Each around its tree - out of the
			// trunk's way (1.5 m), out to 0.75 x the spacing (area-uniform) - a random bush species of its climate.
			const oc::vector<const Species*>& bushes = tree ? bushesFor[tree] : bushSpecies;
			if (bushes.empty() || m_bushesPerTree <= 0.0f)
				return;
			const float whole = std::floor(m_bushesPerTree);
			const uint32 count = (uint32)whole + (treeHash01(treeHash(seed, 110u)) < m_bushesPerTree - whole ? 1u : 0u);
			for (uint32 b = 0; b < count; ++b)
			{
				const uint32 bushSeed = treeHash(seed, 200u + b);
				const float angle = treeHash01(treeHash(bushSeed, 111u)) * 6.28318531f;
				const float radius = glm::mix(1.5f, glm::max(0.75f * m_spacing, 1.5f), std::sqrt(treeHash01(treeHash(bushSeed, 112u))));
				const Species& bush = *bushes[treeHash(bushSeed, 113u) % (uint32)bushes.size()];
				placeVariant(bush, bushSeed, p + glm::vec2(std::cos(angle), std::sin(angle)) * radius);
			}
		};

		uint32 treeIdx = 0;
		for (int j = 0; j < grid; ++j)
		{
			for (int i = 0; i < grid; ++i, ++treeIdx)
			{
				const uint32 seed = treeHash((uint32)m_seed, treeIdx);
				const glm::vec2 jitter(treeHash01(treeHash(seed, 100u)) - 0.5f, treeHash01(treeHash(seed, 101u)) - 0.5f);
				const glm::vec2 p = center + right * ((float)i * m_spacing - half) + fwd * ((float)j * m_spacing - half)
					+ jitter * (m_spacing * m_positionJitter);
				placeTree(only ? only : !treeSpecies.empty() ? treeSpecies[(size_t)(i + j * grid) % treeSpecies.size()] : nullptr, seed, p);
			}
		}

		if (m_showLibrary)
		{
			// The piece library: one row per species behind the grove, trunks first, then the modules upright.
			const glm::quat faceCamera = glm::angleAxis(std::atan2(fwd.x, fwd.y), glm::vec3(0.0f, 1.0f, 0.0f));
			for (size_t s = 0; s < m_species.size(); ++s)
			{
				const Species& species = m_species[s];
				const glm::vec2 rowStart = center - fwd * (half + 25.0f + (float)s * 15.0f) - right * half;
				float x = 0.0f;
				auto placeInRow = [&](const PieceMeshes& meshes, float width)
				{
					const glm::vec2 p = rowStart + right * (x + width * 0.5f);
					place(species, meshes, Transform(glm::vec3(p.x, groundAt(p), p.y), 1.0f, faceCamera));
					x += width;
				};
				for (const PieceMeshes& meshes : species.trunkMeshes)
					placeInRow(meshes, 3.0f);
				const float moduleWidth = glm::max(species.library.modules.empty() ? 1.0f : species.library.modules[0].length * 0.8f, 1.0f);
				for (const PieceMeshes& meshes : species.moduleMeshes)
					placeInRow(meshes, moduleWidth);
			}
		}

		if (!gpu || gpuPieces.empty())
			return;

		// THE VEGETATION IN THE TERRAIN CHUNKS: the pieces sorted by the terrain chunk under their base, one set chunk
		// per terrain chunk. The terrain's render walk lists the chunks it draws (with their passes) and hands the list
		// to the renderer (the sink below): the culls see only the plants of the visible and the shadow / GI chunks.
		m_vegChunkSize = Globals::terrain.chunkSize();
		const float chunkSize = (float)m_vegChunkSize;
		auto chunkKey = [&](const Renderer::TreeInstancePiece& piece)
		{
			const int32 x = (int32)std::floor(piece.transform.pos.x / chunkSize);
			const int32 z = (int32)std::floor(piece.transform.pos.z / chunkSize);
			return (uint64)(uint32)x | ((uint64)(uint32)z << 32);
		};
		oc::vector<oc::pair<uint64, uint32>> order(gpuPieces.size());
		for (uint32 i = 0; i < (uint32)gpuPieces.size(); ++i)
			order[i] = { chunkKey(gpuPieces[i]), i };
		oc::sort(order.begin(), order.end());
		oc::vector<Renderer::TreeInstancePiece> sorted(gpuPieces.size());
		oc::vector<Renderer::TreeInstanceChunk> chunks;
		m_vegChunkOf.clear();
		for (uint32 i = 0; i < (uint32)order.size(); ++i)
		{
			sorted[i] = gpuPieces[order[i].second];
			if (i == 0 || order[i].first != order[i - 1].first)
			{
				m_vegChunkOf.emplace(order[i].first, (int32)chunks.size());
				chunks.push_back({ i, 0 });
			}
			++chunks.back().count;
		}
		m_treeSet = renderer.createTreeInstanceSet(gpuTypes, sorted, chunks);
		renderer.bindTreeInstanceSet(m_treeSet);
		hookTerrain((uint32)chunks.size());
	}

	// WORLD MODE's spawn: the dynamic set (every type, no pieces yet), the expansion context, the terrain hook.
	void TreeSystem::spawnWorld(Renderer& renderer, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
		m_worldGeneration = m_world.generation();
		m_worldFullLogged = false;
		if (m_species.empty())
			return;
		oc::vector<Renderer::TreeInstanceType> gpuTypes;
		oc::unordered_map<const PieceMeshes*, uint32> typeOf;
		buildGpuTypes(gpuTypes, typeOf);

		auto context = oc::make_shared<ExpandContext>();
		context->maps = maps;
		context->worldSeed = m_world.seed();
		context->chunkSize = (float)Globals::terrain.chunkSize();
		context->sizeVariation = m_sizeVariation;
		context->bushesPerTree = m_bushesPerTree;
		context->bushRadius = glm::max(0.75f * m_spacing, 1.5f);
		// Every loaded species (its baked variants), then per TREE species the bushes of its climate (as the preview:
		// case-insensitive; a tree whose climate no bush shares takes every bush).
		oc::vector<uint32> bushes;
		for (const Species& species : m_species)
		{
			ExpandSpecies& out = context->species.emplace_back();
			out.scale = species.desc.scale;
			for (const PieceMeshes& meshes : species.variantMeshes)
				out.variants.push_back({ typeOf[&meshes], meshes.farCentre, meshes.farRadius });
			if (species.desc.bush && !species.variantMeshes.empty())
				bushes.push_back((uint32)(context->species.size() - 1));
		}
		auto sameClimate = [](const oc::string& a, const oc::string& b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
					return false;
			return true;
		};
		for (size_t s = 0; s < m_species.size(); ++s)
		{
			if (m_species[s].desc.bush)
				continue;
			for (uint32 b : bushes)
				if (sameClimate(m_species[b].desc.climate, m_species[s].desc.climate))
					context->species[s].bushes.push_back(b);
			if (context->species[s].bushes.empty())
				context->species[s].bushes = bushes;
		}
		// The record types are TreeWorld's (its name-sorted .tree list): matched by name.
		for (uint32 type = 0; type < 256; ++type)
		{
			const oc::string_view name = m_world.typeName(type);
			if (name.empty())
				break;
			int32 index = -1;
			for (size_t s = 0; s < m_species.size(); ++s)
				if (!m_species[s].desc.bush && oc::string_view(m_species[s].desc.name) == name && !m_species[s].variantMeshes.empty())
					index = (int32)s;
			context->speciesOfRecordType.push_back(index);
		}
		m_expandContext = context;

		m_treeSet = renderer.createDynamicTreeInstanceSet(gpuTypes, (uint32)glm::max(m_worldCapacity, 1));

		// The far volume's view of the RECORDS (W4; RendererVK TreeRecordTypeGpu), per record type (TreeWorld's name-sorted
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
			int32 index = -1;
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
			out.sizeVariation = m_sizeVariation;
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
				out.bushesPerTree = context->bushesPerTree;
				out.bushRadius = context->bushRadius;
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
		if (clipped)
			Log::warning(oc::format("Trees: a species has more than {} variants or a tree more than {} bush species - the far volume's "
				"records then pick differently from the meshes", TREE_RECORD_MAX_VARIANTS, TREE_RECORD_MAX_BUSHES));
		renderer.setTreeRecordTypes(recordTypes, context->chunkSize, context->worldSeed, m_treeSet);
		m_recordTypesSet = true;
		renderer.bindTreeInstanceSet(m_treeSet);
		m_vegChunkSize = Globals::terrain.chunkSize();
		// The set's chunk indices are recycled: at most the near square (+1 of hysteresis) plus one frame of removals.
		const uint32 side = 2u * (uint32)m_nearRadius + 3u;
		hookTerrain(side * side * 2u);
	}

	// WORLD MODE, every frame: drop the chunks that left the near radius (+1 of hysteresis), start the expansion of the
	// ones inside it that TreeWorld holds records for, and add up to "Expand per frame" finished ones to the set.
	void TreeSystem::updateWorld(Renderer& renderer, const Camera& camera)
	{
		if (!m_expandContext || m_treeSet == UINT32_MAX)
			return;
		const float chunkSize = m_expandContext->chunkSize;
		const glm::ivec2 cam((int32)std::floor(camera.position.x / chunkSize), (int32)std::floor(camera.position.z / chunkSize));
		auto key = [](glm::ivec2 c) { return (uint64)(uint32)c.x | ((uint64)(uint32)c.y << 32); };
		auto chebyshev = [&](glm::ivec2 c) { return glm::max(glm::abs(c.x - cam.x), glm::abs(c.y - cam.y)); };

		for (auto it = m_near.begin(); it != m_near.end(); )
		{
			const glm::ivec2 coord((int32)(uint32)it->first, (int32)(uint32)(it->first >> 32));
			if (chebyshev(coord) <= m_nearRadius + 1)
			{
				++it;
				continue;
			}
			if (it->second.setChunk != UINT32_MAX)
			{
				renderer.removeTreeInstanceChunk(m_treeSet, it->second.setChunk);
				m_vegChunkOf.erase(it->first);
				Globals::terrain.restampVegetation(coord);
			}
			it = m_near.erase(it); // a pending one: its result finds no entry
		}

		// The finished expansions (this generation's, still wanted), nearest first is the jobs' order already.
		oc::vector<ExpandResult> results;
		{
			std::lock_guard<std::mutex> lk(m_expandMutex);
			const size_t take = glm::min(m_expandResults.size(), (size_t)glm::max(m_expandPerFrame, 1));
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
			const uint32 setChunk = renderer.addTreeInstanceChunk(m_treeSet, result.pieces);
			if (setChunk == UINT32_MAX)
			{
				if (!m_worldFullLogged)
					Log::warning(oc::format("Trees: the world set is full ({} pieces) - raise 'Trees/World/Set capacity (pieces)'", m_worldCapacity));
				m_worldFullLogged = true;
				continue;
			}
			it->second.setChunk = setChunk;
			m_vegChunkOf[it->first] = (int32)setChunk;
			Globals::terrain.restampVegetation(result.coord);
		}

		// New expansions, nearest first, a few in flight at a time.
		constexpr int32 MAX_IN_FLIGHT = 4;
		for (int r = 0; r <= m_nearRadius && m_expandInFlight.load(oc::memory_order_relaxed) < MAX_IN_FLIGHT; ++r)
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
						expandChunk(*context, coord, chunkRecords, result.pieces);
						std::lock_guard<std::mutex> lk(m_expandMutex);
						m_expandResults.push_back(oc::move(result));
						m_expandInFlight.fetch_sub(1, oc::memory_order_relaxed);
					}, { "treeExpandChunk", EProfileCategory::Procedural }, EJobPriority::Low, &m_expandCounter);
					if (m_expandInFlight.load(oc::memory_order_relaxed) >= MAX_IN_FLIGHT)
						break;
				}
	}

	void TreeSystem::stopExpansion()
	{
		Globals::jobSystem.wait(m_expandCounter);
		++m_expandGeneration;
		std::lock_guard<std::mutex> lk(m_expandMutex);
		m_expandResults.clear();
	}

	// One chunk's trees + bushes from its records: the preview's placeVariant rules (variant, scale, yaw from the seed),
	// the record's seed for the tree, the ground from one Full grid at 2 m (the terrain's LOD 0 spacing) over the chunk
	// plus the bushes' reach. MIRRORED on the GPU by the far volume (tree_volume_splat.cs's TREE_SPLAT_RECORDS,
	// tree_volume_records.cs) - keep the hashes and the order of the choices in step.
	void TreeSystem::expandChunk(const ExpandContext& context, glm::ivec2 coord, const oc::vector<TreeRecord>& records,
		oc::vector<Renderer::TreeInstancePiece>& out)
	{
		out.clear();
		if (records.empty() || !context.maps)
			return;
		const float cs = context.chunkSize;
		constexpr float STEP = 2.0f;
		const float margin = std::ceil((context.bushRadius + 2.0f) / STEP) * STEP;
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
		auto placeVariant = [&](const ExpandSpecies& species, uint32 seed, glm::vec2 p)
		{
			if (species.variants.empty())
				return;
			const ExpandVariant& variant = species.variants[treeHash(seed, 102u) % (uint32)species.variants.size()];
			const float scale = glm::mix(species.scale.x, species.scale.y, treeHash01(treeHash(seed, 103u)))
				* std::exp2(context.sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0f - 1.0f));
			const glm::quat yaw = glm::angleAxis(treeHash01(treeHash(seed, 104u)) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
			Renderer::TreeInstancePiece& piece = out.emplace_back();
			piece.transform = Transform(glm::vec3(p.x, groundAt(p) - 0.05f, p.y), scale, yaw);
			piece.centre = piece.transform.transformPoint(variant.farCentre);
			piece.radius = variant.farRadius * scale;
			piece.type = variant.type;
		};

		const glm::vec2 chunkOrigin = glm::vec2(coord) * cs;
		for (TreeRecord record : records)
		{
			const uint32 type = treeRecordType(record);
			const int32 s = type < context.speciesOfRecordType.size() ? context.speciesOfRecordType[type] : -1;
			if (s < 0)
				continue;
			const ExpandSpecies& tree = context.species[(size_t)s];
			const uint32 seed = treeRecordSeed(context.worldSeed, coord, record);
			const glm::vec2 p = chunkOrigin + treeRecordLocal(record, cs);
			placeVariant(tree, seed, p);
			// "Bushes per tree": the whole part always, the fraction by chance, out of the trunk's way (1.5 m) to
			// bushRadius (area-uniform), a random bush species of its climate.
			if (tree.bushes.empty() || context.bushesPerTree <= 0.0f)
				continue;
			const float whole = std::floor(context.bushesPerTree);
			const uint32 count = (uint32)whole + (treeHash01(treeHash(seed, 110u)) < context.bushesPerTree - whole ? 1u : 0u);
			for (uint32 b = 0; b < count; ++b)
			{
				const uint32 bushSeed = treeHash(seed, 200u + b);
				const float angle = treeHash01(treeHash(bushSeed, 111u)) * 6.28318531f;
				const float radius = glm::mix(1.5f, context.bushRadius, std::sqrt(treeHash01(treeHash(bushSeed, 112u))));
				const ExpandSpecies& bush = context.species[tree.bushes[treeHash(bushSeed, 113u) % (uint32)tree.bushes.size()]];
				placeVariant(bush, bushSeed, p + glm::vec2(std::cos(angle), std::sin(angle)) * radius);
			}
		}
	}
}
