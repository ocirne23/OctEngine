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

namespace
{
	using namespace Procedural;

	RenderMesh uploadTreeMesh(Renderer& renderer, const TreeMesh& mesh)
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
		return renderer.createMesh(data);
	}

	size_t numTriangles(const TreeMesh& mesh) { return mesh.indices.size() / 3; }

	// Uploads every level and chains the valid prefix (a level can be empty, e.g. no leaves left at a stride).
	void uploadLodChain(Renderer& renderer, const TreeMesh (&levels)[TREE_PIECE_LODS], const float (&errors)[TREE_PIECE_LODS],
		RenderMesh (&outMeshes)[TREE_PIECE_LODS], uint32& outChain)
	{
		const RenderMesh* chain[TREE_PIECE_LODS] = {};
		uint32 numValid = 0;
		for (uint32 k = 0; k < TREE_PIECE_LODS; ++k)
		{
			outMeshes[k] = uploadTreeMesh(renderer, levels[k]);
			if (numValid == k && outMeshes[k].isValid())
				chain[numValid++] = &outMeshes[k];
		}
		outChain = numValid >= 2
			? renderer.createMeshLodChain(oc::span<const RenderMesh* const>(chain, numValid), oc::span<const float>(errors, numValid))
			: UINT32_MAX;
	}

	// Species textures are generated ONCE into Assets/Local/Trees/Textures (<species>_bark.png, _bark_normal.png,
	// _leaves.png, the impostor / billboard bakes) and read back on later loads. Generated output: not in git.
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
					if (meshes.barkChain != UINT32_MAX)
						Globals::rendererVK.freeMeshLodChain(meshes.barkChain);
					if (meshes.leafChain != UINT32_MAX)
						Globals::rendererVK.freeMeshLodChain(meshes.leafChain);
					if (meshes.impostorMaterial != UINT16_MAX)
						Globals::rendererVK.destroyTextureMaterial(meshes.impostorMaterial);
					if (meshes.billboardMaterial != UINT16_MAX)
						Globals::rendererVK.destroyTextureMaterial(meshes.billboardMaterial);
				}
			if (species.barkFadeMaterial != UINT16_MAX)
				Globals::rendererVK.releaseMaterial(species.barkFadeMaterial); // derived: textures stay with the source
			if (species.leafFadeMaterial != UINT16_MAX)
				Globals::rendererVK.releaseMaterial(species.leafFadeMaterial);
			if (species.ownsLeafMaterial)
				Globals::rendererVK.destroyTextureMaterial(species.leafMaterial);
			if (species.ownsBarkMaterial)
				Globals::rendererVK.destroyTextureMaterial(species.barkMaterial);
		}
		m_species.clear();
	}

	void TreeSystem::destroyTreeSet()
	{
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
			const float start = glm::max(species.desc.billboardDistance * m_impostorDistanceScale - width * 0.5f, 0.0f);
			const uint32 fadeOut = RendererVKLayout::makeDistanceFadeFlags(start, width, false);
			const uint32 fadeIn = RendererVKLayout::makeDistanceFadeFlags(start, width, true);
			for (uint16 material : { species.barkFadeMaterial, species.leafFadeMaterial })
				renderer.setMaterialFlags(material, (renderer.getMaterialFlags(material) & ~FADE_BITS) | fadeOut);
			// Force far draws the billboards at every distance: no fade-in then, or they would vanish nearby.
			const uint32 billboardFade = m_forceImpostors ? 0u : fadeIn;
			for (const oc::vector<PieceMeshes>* pieces : { &species.moduleMeshes, &species.trunkMeshes, &species.variantMeshes })
				for (const PieceMeshes& meshes : *pieces)
					if (meshes.billboardMaterial != UINT16_MAX)
						renderer.setMaterialFlags(meshes.billboardMaterial, (renderer.getMaterialFlags(meshes.billboardMaterial) & ~FADE_BITS) | billboardFade);
		}
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
		Tweak::intVar("Trees", "Seed", &m_seed, 0, 1000000, 1.0f, respawn);
		Tweak::enumVar("Trees", "Grove type", &m_groveType, GROVE_TYPES, respawn);
		static constexpr oc::string_view FAR_MODES[] = { "Billboards", "Octahedral impostors", "None" };
		Tweak::enumVar("Trees", "Far mode", &m_farMode, FAR_MODES, [this]() { m_reload = true; });
		static constexpr oc::string_view BILLBOARD_VIEWS[] = { "2 (side + top)", "4 (+ other side + bottom)" };
		Tweak::enumVar("Trees", "Billboard views", &m_billboardViews, BILLBOARD_VIEWS, [this]() { m_reload = true; });
		Tweak::floatVar("Trees", "Far distance scale", &m_impostorDistanceScale, 0.0f, 10.0f, 0.01f, [this]() { m_fadeBandsDirty = true; });
		Tweak::boolean("Trees", "Force far", &m_forceImpostors, [this]() { m_fadeBandsDirty = true; });
		Tweak::boolean("Trees", "GPU expansion", &m_gpuExpansion, respawn);
	}

	void TreeSystem::update(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps)
	{
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
		if (!m_spawned || m_respawn)
		{
			m_respawn = false;
			destroyTreeSet();
			m_pieces.clear();
			spawnPreview(renderer, camera, maps.get());
			m_spawned = true;
		}
		if (m_fadeBandsDirty)
		{
			m_fadeBandsDirty = false;
			applyFadeBands(renderer);
		}

		// The GPU path: one call, the expansion decides per piece (the same rules as the CPU loop below).
		if (m_treeSet != UINT32_MAX)
			renderer.renderTreeInstanceSet(m_treeSet, camera.position, m_impostorDistanceScale, m_forceImpostors);
		// The far-tree volume lays its cells out by the camera's height above the ground under it.
		if (maps)
			renderer.setFarTreeCameraGround(maps->sampleHeight(camera.position.x, camera.position.z));

		// The CPU path (GPU expansion off, or the impostor mode): per module, the far representation beyond its
		// distance (5% hysteresis), else the mesh LOD chain. Billboards crossfade instead (see PlacedPiece).
		constexpr uint32 SHADOW_AND_GI = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI;
		for (PlacedPiece& piece : m_pieces)
		{
			if (piece.farIsBillboard)
			{
				// The same band the materials carry (applyFadeBands). A pixel's distance varies by up to the
				// module's radius from the centre's, so both sides draw while any of it can be in the band.
				// The billboard stands in for the piece in every pass but MAIN (shadow, GI, RT): the mesh draws in
				// MAIN only, the cards always draw, in MAIN too only while they show (as tree_cull.inc.glsl).
				const float switchDistance = piece.farDistance * m_impostorDistanceScale;
				const float bandStart = glm::max(switchDistance - piece.fadeWidth * 0.5f, 0.0f);
				const float bandEnd = bandStart + piece.fadeWidth;
				const float distance = glm::distance(camera.position, piece.centre);
				if (m_forceImpostors)
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
			if (piece.far.isValid())
			{
				const float switchDistance = piece.farDistance * m_impostorDistanceScale;
				const float distance = glm::distance(camera.position, piece.centre);
				piece.farActive = m_forceImpostors
					|| (switchDistance > 0.0f && distance > switchDistance * (piece.farActive ? 0.95f : 1.05f));
			}
			if (piece.farActive)
			{
				renderer.renderNode(piece.far, RendererVKLayout::PASS_MAIN);
				renderer.renderNode(piece.bark, SHADOW_AND_GI);
				renderer.renderNode(piece.leaves, SHADOW_AND_GI);
			}
			else
			{
				renderer.renderNode(piece.bark);
				renderer.renderNode(piece.leaves);
			}
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
			auto uploadPieces = [&](const oc::vector<TreePiece>& pieces, oc::vector<PieceMeshes>& out)
			{
				out.resize(pieces.size());
				for (size_t i = 0; i < pieces.size(); ++i)
				{
					uploadLodChain(renderer, pieces[i].bark, pieces[i].lodError, out[i].bark, out[i].barkChain);
					uploadLodChain(renderer, pieces[i].leaves, pieces[i].lodError, out[i].leaves, out[i].leafChain);
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
			// Level-0 images, kept for the impostor bake.
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
				species.barkMaterial = renderer.createTextureMaterial(bark.size, bark.size, albedoMips, 0.0f, "TreeBark", &normalMips);
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
				species.leafMaterial = renderer.createTextureMaterial(texture.size, texture.size, mips, TREE_LEAF_ALPHA_CUTOFF, "TreeLeafCluster",
					nullptr, RendererVKLayout::MATERIAL_FLAG_LEAF);
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
			}
			else if (m_farMode == 1 && species.desc.impostorDistance > 0.0f)
				buildImpostors(renderer, species, name, barkAlbedo, barkSize, leafImage, leafSize);
			Log::info(oc::format("Trees: '{}' - {} trunks, {} modules, library triangles bark/leaf per LOD: {}/{} {}/{} {}/{} {}/{}",
				species.desc.name, species.library.trunks.size(), species.library.modules.size(),
				barkTris[0], leafTris[0], barkTris[1], leafTris[1], barkTris[2], leafTris[2], barkTris[3], leafTris[3]));
		}
		applyFadeBands(renderer);
	}

	void TreeSystem::buildImpostors(Renderer& renderer, Species& species, const oc::string& name,
		oc::span<const uint8> barkAlbedo, uint32 barkSize, oc::span<const uint8> leafImage, uint32 leafSize)
	{
		const uint32 frames = (uint32)species.desc.impostorFrames;
		const uint32 frameSize = (uint32)species.desc.impostorFrameSize;
		const uint32 atlas = frames * frameSize;
		// Mips down to 4 px per frame: below that a level blends neighbouring frames.
		uint32 numMips = 1;
		for (uint32 s = frameSize; s > 4; s /= 2)
			++numMips;

		oc::vector<FileSystem::DirEntry> existing;
		FileSystem::listDirectory(TREE_TEXTURE_DIR, existing, true);

		// Modules, trunks and the baked whole trees alike (the octahedral views cover any piece).
		const oc::vector<TreePiece>* pieceSets[3] = { &species.library.modules, &species.library.trunks, &species.variants };
		oc::vector<PieceMeshes>* meshSets[3] = { &species.moduleMeshes, &species.trunkMeshes, &species.variantMeshes };
		for (uint32 set = 0; set < 3; ++set)
		for (size_t i = 0; i < pieceSets[set]->size(); ++i)
		{
			const TreePiece& piece = (*pieceSets[set])[i];
			PieceMeshes& meshes = (*meshSets[set])[i];
			const TreeImpostorBounds bounds = impostorBounds(piece);

			// The file name carries a hash of the geometry and bake settings: a changed piece re-bakes on its
			// own, and the stale files of this slot are removed.
			const oc::string prefix = oc::format("{}_{}impostor{}_", name, PIECE_KINDS[set], i);
			const oc::string stem = oc::format("{}{:08x}", prefix, impostorHash(piece, frames, frameSize));
			const oc::string albedoPath = oc::format("{}/{}.png", TREE_TEXTURE_DIR, stem);
			const oc::string normalPath = oc::format("{}/{}_normal.png", TREE_TEXTURE_DIR, stem);

			oc::vector<uint8> albedo, normal;
			uint32 albedoSize = 0, normalSize = 0;
			const bool loaded = !m_regenerateTextures && loadSquareImage(albedoPath, albedoSize, albedo)
				&& loadSquareImage(normalPath, normalSize, normal) && albedoSize == atlas && normalSize == atlas;
			if (!loaded)
			{
				for (const FileSystem::DirEntry& entry : existing)
					if (entry.name.rfind(prefix.c_str(), 0) == 0)
						FileSystem::remove(entry.path, true);
				bakeImpostor(piece, bounds, TreeBakeImage{ barkAlbedo, barkSize }, TreeBakeImage{ leafImage, leafSize },
					frames, frameSize, albedo, normal);
				saveImage(albedoPath, atlas, albedo);
				saveImage(normalPath, atlas, normal);
			}

			TreeLeafTexture albedoChain;
			buildLeafClusterMips(albedo, atlas, albedoChain); // coverage-preserving, like the leaf cards
			oc::vector<oc::vector<uint8>> normalChain;
			buildNormalMips(normal, atlas, normalChain);
			oc::vector<oc::span<uint8>> albedoMips, normalMips;
			for (uint32 k = 0; k < numMips; ++k)
			{
				albedoMips.push_back(oc::span<uint8>(albedoChain.mips[k].data(), albedoChain.mips[k].size()));
				normalMips.push_back(oc::span<uint8>(normalChain[k].data(), normalChain[k].size()));
			}
			meshes.impostorMaterial = renderer.createTextureMaterial(atlas, atlas, albedoMips, TREE_LEAF_ALPHA_CUTOFF, "TreeImpostor", &normalMips);

			// The quad: no geometry, only per-piece constants (see tree_impostor.vs.glsl). Its bounds are the
			// piece's sphere, which is what the cull tests.
			RenderMeshData quad;
			constexpr float CORNERS[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
			quad.vertices.resize(4);
			for (uint32 k = 0; k < 4; ++k)
			{
				quad.vertices[k].positionU = glm::vec4(bounds.centre, 0.0f);
				quad.vertices[k].normalV = glm::vec4(CORNERS[k][0], CORNERS[k][1], bounds.radius, 0.0f);
				quad.vertices[k].tangent = glm::vec4((float)frames, 0.0f, 0.0f, 1.0f);
			}
			quad.indices = { 0, 1, 2, 0, 2, 3 };
			quad.bounds = Sphere(bounds.centre, bounds.radius);
			meshes.impostor = renderer.createMesh(quad);
			meshes.farCentre = bounds.centre;
		}
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
		for (uint32 set = 0; set < 3; ++set)
		for (size_t i = 0; i < pieceSets[set]->size(); ++i)
		{
			const TreePiece& piece = (*pieceSets[set])[i];
			PieceMeshes& meshes = (*meshSets[set])[i];
			const TreeBillboardBox box = billboardBox(piece);
			const bool horizontal = set == 2;
			// Mips down to ~4 px per strip, while the strip boundaries stay on texel boundaries (billboardLayout).
			const uint32 numMips = billboardLayout(size, numViews, horizontal).numMips;

			// Same cache scheme as the impostors: geometry + settings in the name, stale slot files removed.
			// The view count is in the name, so both Billboard views settings keep their own cache.
			const oc::string prefix = oc::format("{}_{}billboard{}v{}_", name, PIECE_KINDS[set], i, numViews);
			// Before the view count was named.
			const oc::string legacyPrefix = oc::format("{}_{}billboard{}_", name, PIECE_KINDS[set], i);
			const float bend = species.desc.billboardNormalBend;
			const uint32 settingsHash = treeHash(0xB1B0A2D5u ^ numViews, (uint32)std::round(bend * 1000.0f));
			const oc::string stem = oc::format("{}{:08x}", prefix, impostorHash(piece, 2u, size) ^ settingsHash);
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

			// Coverage-preserving albedo mips (alpha scaled up per level to keep the level-0 coverage).
			TreeLeafTexture albedoChain;
			buildLeafClusterMips(albedo, size, albedoChain);
			oc::vector<oc::vector<uint8>> normalChain;
			buildNormalMips(normal, size, normalChain);
			oc::vector<oc::span<uint8>> albedoMips, normalMips;
			for (uint32 k = 0; k < numMips; ++k)
			{
				albedoMips.push_back(oc::span<uint8>(albedoChain.mips[k].data(), albedoChain.mips[k].size()));
				normalMips.push_back(oc::span<uint8>(normalChain[k].data(), normalChain[k].size()));
			}
			// FOLIAGE: the sun shadow is not rejected by the flat card normal (see instanced_indirect.fs.glsl).
			meshes.billboardMaterial = renderer.createTextureMaterial(size, size, albedoMips, TREE_LEAF_ALPHA_CUTOFF, "TreeBillboard",
				&normalMips, RendererVKLayout::MATERIAL_FLAG_BILLBOARD | RendererVKLayout::MATERIAL_FLAG_LEAF
				| (horizontal ? RendererVKLayout::MATERIAL_FLAG_BILLBOARD_TOP_CARD : 0u));

			TreeMesh cards;
			billboardMesh(box, size, numViews, horizontal, cards);
			meshes.billboard = uploadTreeMesh(renderer, cards);
			meshes.farCentre = (box.min + box.max) * 0.5f;
			meshes.farRadius = glm::length(box.max - box.min) * 0.5f;
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
			piece.farIsBillboard = true;
			piece.fadeWidth = species.desc.billboardFadeWidth;
			piece.radius = meshes.farRadius * transform.scale;
			// The crossfade copies: the same LOD-chain meshes on LitMasked (the dither discards) with the fade-out
			// materials. Drawn only inside the band, so the bark keeps LitOpaque's early depth elsewhere.
			if (meshes.bark[0].isValid())
				piece.barkFade = renderer.spawnMeshNode(meshes.bark[0], species.barkFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked, transform);
			if (meshes.leaves[0].isValid())
				piece.leavesFade = renderer.spawnMeshNode(meshes.leaves[0], species.leafFadeMaterial, RendererVKLayout::EPipelineIndex::LitMasked, transform);
		}
		else if (meshes.impostor.isValid())
		{
			piece.far = renderer.spawnMeshNode(meshes.impostor, meshes.impostorMaterial, RendererVKLayout::EPipelineIndex::TreeImpostor, transform);
			piece.farDistance = species.desc.impostorDistance;
		}
		piece.centre = transform.transformPoint(meshes.farCentre);
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
		// piece TYPE per library piece, then per frame one call instead of a renderNode per piece node. The
		// octahedral impostor mode keeps the CPU path (its main-pass-only quad + shadow-only meshes).
		const bool gpu = m_gpuExpansion && m_farMode != 1;
		oc::vector<Renderer::TreeInstanceType> gpuTypes;
		oc::unordered_map<const PieceMeshes*, uint32> typeOf;
		oc::vector<Renderer::TreeInstancePiece> gpuPieces;
		if (gpu)
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
						}
						typeOf.emplace(&meshes, (uint32)gpuTypes.size());
						gpuTypes.push_back(type);
					}
		}
		auto place = [&](const Species& species, const PieceMeshes& meshes, const Transform& transform)
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

		uint32 treeIdx = 0;
		for (int j = 0; j < grid; ++j)
		{
			for (int i = 0; i < grid; ++i, ++treeIdx)
			{
				const Species& species = only ? *only : m_species[(size_t)(i + j * grid) % m_species.size()];
				const uint32 seed = treeHash((uint32)m_seed, treeIdx);
				const glm::vec2 jitter(treeHash01(treeHash(seed, 100u)) - 0.5f, treeHash01(treeHash(seed, 101u)) - 0.5f);
				const glm::vec2 p = center + right * ((float)i * m_spacing - half) + fwd * ((float)j * m_spacing - half)
					+ jitter * (m_spacing * m_positionJitter);
				// A BAKED variant per tree (no runtime composite): variant, scale and yaw from the seed.
				if (species.variantMeshes.empty())
					continue;
				const uint32 variant = treeHash(seed, 102u) % (uint32)species.variantMeshes.size();
				// The species' own range, then "Size variation": x 2^(+-v), log-uniform, so halving and doubling are as likely.
				const float treeScale = glm::mix(species.desc.scale.x, species.desc.scale.y, treeHash01(treeHash(seed, 103u)))
					* std::exp2(m_sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0f - 1.0f));
				const glm::quat yaw = glm::angleAxis(treeHash01(treeHash(seed, 104u)) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
				place(species, species.variantMeshes[variant], Transform(glm::vec3(p.x, groundAt(p) - 0.05f, p.y), treeScale, yaw));
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

		if (gpu && !gpuPieces.empty())
			m_treeSet = renderer.createTreeInstanceSet(gpuTypes, gpuPieces);
	}
}
