module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Tweaks;
import Core.Log;

import RendererVK;
import File;

import :TreeSystem;
import :TreeSpecies;
import :TreeGenerator;
import :TreeLeafTexture;
import :TreeBarkTexture;
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

	// Species textures are generated ONCE into Assets/Trees/Textures (<species>_bark.png, _bark_normal.png,
	// _leaves.png) and read back on later loads - also the place to drop authored replacements.
	constexpr const char* TREE_TEXTURE_DIR = "Trees/Textures";
	constexpr uint32 TREE_TEXTURE_SIZE = 1024;

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
		m_nodes.clear();
		for (const Species& species : m_species)
		{
			if (species.ownsLeafMaterial)
				Globals::rendererVK.destroyTextureMaterial(species.leafMaterial);
			if (species.ownsBarkMaterial)
				Globals::rendererVK.destroyTextureMaterial(species.barkMaterial);
		}
		m_species.clear();
	}

	void TreeSystem::initialize()
	{
		auto respawn = [this]() { m_respawn = true; };
		Tweak::boolean("Trees", "Enabled", &m_enabled);
		Tweak::boolean("Trees", "Reload species", &m_reload);
		Tweak::boolean("Trees", "Respawn preview", &m_respawn);
		Tweak::boolean("Trees", "Regenerate textures", &m_regenerateTextures, [this]() { if (m_regenerateTextures) m_reload = true; });
		Tweak::boolean("Trees", "Show piece library", &m_showLibrary, respawn);
		Tweak::intVar("Trees", "Grove size", &m_gridSize, 1, 32, 1.0f, respawn);
		Tweak::floatVar("Trees", "Spacing (m)", &m_spacing, 2.0f, 50.0f, 0.1f, respawn);
		Tweak::intVar("Trees", "Seed", &m_seed, 0, 1000000, 1.0f, respawn);
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
			m_nodes.clear();
			spawnPreview(renderer, camera, maps.get());
			m_spawned = true;
		}
		for (const RenderNode& node : m_nodes)
			renderer.renderNode(node);
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

			size_t barkTris = 0, leafTris = 0;
			for (const TreePiece& piece : species.library.trunks)
			{
				species.trunkMeshes.push_back({ uploadTreeMesh(renderer, piece.bark), uploadTreeMesh(renderer, piece.leaves) });
				barkTris += numTriangles(piece.bark);
				leafTris += numTriangles(piece.leaves);
			}
			for (const TreePiece& piece : species.library.modules)
			{
				species.moduleMeshes.push_back({ uploadTreeMesh(renderer, piece.bark), uploadTreeMesh(renderer, piece.leaves) });
				barkTris += numTriangles(piece.bark);
				leafTris += numTriangles(piece.leaves);
			}
			const oc::string& name = species.desc.name.empty() ? entry.name : species.desc.name;
			{
				const oc::string albedoPath = oc::format("{}/{}_bark.png", TREE_TEXTURE_DIR, name);
				const oc::string normalPath = oc::format("{}/{}_bark_normal.png", TREE_TEXTURE_DIR, name);
				uint32 size = 0, normalSize = 0;
				oc::vector<uint8> albedo, normal;
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
				uint32 size = 0;
				oc::vector<uint8> image;
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
				species.leafMaterial = renderer.createTextureMaterial(texture.size, texture.size, mips, TREE_LEAF_ALPHA_CUTOFF, "TreeLeafCluster");
				species.ownsLeafMaterial = true;
				species.leafPipeline = RendererVKLayout::EPipelineIndex::LitMasked;
			}
			else
				species.leafMaterial = renderer.getOrCreateSolidColorMaterial(species.desc.leafColor);
			Log::info(oc::format("Trees: '{}' - {} trunks, {} modules, library {} bark + {} leaf triangles",
				species.desc.name, species.library.trunks.size(), species.library.modules.size(), barkTris, leafTris));
		}
	}

	void TreeSystem::spawnPiece(Renderer& renderer, const Species& species, const PieceMeshes& meshes, const Transform& transform)
	{
		if (meshes.bark.isValid())
			m_nodes.push_back(renderer.spawnMeshNode(meshes.bark, species.barkMaterial, RendererVKLayout::EPipelineIndex::LitOpaque, transform));
		if (meshes.leaves.isValid())
			m_nodes.push_back(renderer.spawnMeshNode(meshes.leaves, species.leafMaterial, species.leafPipeline, transform));
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

		oc::vector<TreePiecePlacement> placements;
		uint32 treeIdx = 0;
		for (int j = 0; j < grid; ++j)
		{
			for (int i = 0; i < grid; ++i, ++treeIdx)
			{
				const Species& species = m_species[(size_t)(i + j * grid) % m_species.size()];
				const uint32 seed = treeHash((uint32)m_seed, treeIdx);
				const glm::vec2 jitter(treeHash01(treeHash(seed, 100u)) - 0.5f, treeHash01(treeHash(seed, 101u)) - 0.5f);
				const glm::vec2 p = center + right * ((float)i * m_spacing - half) + fwd * ((float)j * m_spacing - half)
					+ jitter * (m_spacing * 0.3f);
				float treeScale = 1.0f;
				compositeTree(species.desc, species.library, seed, placements, treeScale);
				const Transform tree(glm::vec3(p.x, groundAt(p) - 0.05f, p.y), treeScale, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				for (const TreePiecePlacement& placement : placements)
				{
					const PieceMeshes& meshes = placement.trunk ? species.trunkMeshes[placement.pieceIdx] : species.moduleMeshes[placement.pieceIdx];
					spawnPiece(renderer, species, meshes, composeTransform(tree, placement.local));
				}
			}
		}

		if (!m_showLibrary)
			return;

		// The piece library: one row per species behind the grove, trunks first, then the modules upright.
		const glm::quat faceCamera = glm::angleAxis(std::atan2(fwd.x, fwd.y), glm::vec3(0.0f, 1.0f, 0.0f));
		for (size_t s = 0; s < m_species.size(); ++s)
		{
			const Species& species = m_species[s];
			const glm::vec2 rowStart = center - fwd * (half + 25.0f + (float)s * 15.0f) - right * half;
			float x = 0.0f;
			auto place = [&](const PieceMeshes& meshes, float width)
			{
				const glm::vec2 p = rowStart + right * (x + width * 0.5f);
				spawnPiece(renderer, species, meshes, Transform(glm::vec3(p.x, groundAt(p), p.y), 1.0f, faceCamera));
				x += width;
			};
			for (const PieceMeshes& meshes : species.trunkMeshes)
				place(meshes, 3.0f);
			const float moduleWidth = glm::max(species.library.modules.empty() ? 1.0f : species.library.modules[0].length * 0.8f, 1.0f);
			for (const PieceMeshes& meshes : species.moduleMeshes)
				place(meshes, moduleWidth);
		}
	}
}
