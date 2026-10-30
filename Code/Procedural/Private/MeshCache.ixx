export module Procedural:MeshCache;

import Core;
import Core.glm;
import RendererVK;

import :TreeGenerator;
import :RockGenerator;

// THE GENERATED-MESH CACHE: the tree, rock and clutter generators' output, saved under Assets/Local (generated, never
// in git) and loaded instead of generating again. One file per generated unit (a tree species, a rock variant, a clutter
// type), keyed INSIDE by a hash of its inputs - the asset file's text plus the settings that feed the generator - so a
// changed input is a miss that regenerates and overwrites the file (no stale files pile up).
//
// The key does NOT cover the generator CODE: a change that moves a generator's output for the same inputs must bump
// its *_MESH_CACHE_VERSION below (or delete Assets/Local/<Trees|Rocks|Clutter>/Meshes).
//
// Any thread (the rocks and the clutter load / save from their generation jobs); the trees from main, under the
// caller's AllowMainThreadIO.
export namespace Procedural
{
	constexpr uint32 TREE_MESH_CACHE_VERSION = 20;// generateTreeLibrary, bakeTreeVariant, bakeTreeDensity
	constexpr uint32 ROCK_MESH_CACHE_VERSION = 3;    // generateRockVariant (and RockShape's layout: the file holds it raw)
	constexpr uint32 CLUTTER_MESH_CACHE_VERSION = 1; // generateClutterMeshes (its pebbles: generateRockVariant too)
	// The bark / leaf / billboard generators, their mip chains (buildBarkMips, buildLeafClusterMips, buildNormalMips) and
	// the BC encoders (TextureConvert)
	constexpr uint32 TEXTURE_CACHE_VERSION = 1;

	constexpr const char* TREE_MESH_DIR = "Local/Trees/Meshes";
	constexpr const char* ROCK_MESH_DIR = "Local/Rocks/Meshes";
	constexpr const char* CLUTTER_MESH_DIR = "Local/Clutter/Meshes";
	constexpr const char* TREE_TEXTURE_DIR = "Local/Trees/Textures";
	constexpr const char* ROCK_TEXTURE_DIR = "Local/Rocks/Textures";
	constexpr const char* CLUTTER_TEXTURE_DIR = "Local/Clutter/Textures";

	// FNV-1a 64 over an asset file's text, then any settings mixed in.
	uint64 meshCacheHash(oc::string_view text);
	uint64 meshCacheMix(uint64 hash, uint64 value);

	// A baked tree variant's far-volume grid (TreeSystem's PieceMeshes::density + its box).
	struct TreeDensityGrid
	{
		oc::vector<float> values;
		glm::vec3 min{ 0.0f };
		glm::vec3 max{ 0.0f };
	};

	// A whole species: its piece library, its baked variants and their density grids. False = a miss.
	bool loadTreeMeshes(const oc::string& path, uint64 hash, TreeLibrary& library, oc::vector<TreePiece>& variants,
		oc::vector<TreeDensityGrid>& density);
	void saveTreeMeshes(const oc::string& path, uint64 hash, const TreeLibrary& library, const oc::vector<TreePiece>& variants,
		const oc::vector<TreeDensityGrid>& density);

	bool loadRockVariant(const oc::string& path, uint64 hash, RockVariant& out);
	void saveRockVariant(const oc::string& path, uint64 hash, const RockVariant& variant);

	bool loadClutterMeshes(const oc::string& path, uint64 hash, oc::vector<Renderer::ClutterMesh>& out);
	void saveClutterMeshes(const oc::string& path, uint64 hash, const oc::vector<Renderer::ClutterMesh>& meshes);

	// A generated material's textures AS UPLOADED (.tex): the albedo and normal mip chains in their GPU encoding (BC blocks,
	// or RGBA8), level 0 first, plus the albedo's LINEAR mean (the far volume's colour). A load is one read and one zstd
	// decompress - no image decode, no mip build, no BC encode.
	struct CachedTexture
	{
		uint32 width = 0;
		uint32 height = 0;
		RenderMaterial::ETextureEncoding albedoEncoding = RenderMaterial::ETextureEncoding::Rgba8;
		RenderMaterial::ETextureEncoding normalEncoding = RenderMaterial::ETextureEncoding::Rgba8;
		oc::vector<oc::vector<uint8>> albedo;
		oc::vector<oc::vector<uint8>> normal; // empty: none
		glm::vec3 mean{ 0.0f };
	};
	bool loadCachedTexture(const oc::string& path, uint64 hash, CachedTexture& out);
	void saveCachedTexture(const oc::string& path, uint64 hash, const CachedTexture& texture);
}
