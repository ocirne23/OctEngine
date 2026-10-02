export module Procedural:TreeLeafTexture;

import Core;
import Core.glm;

import :TreeSpecies;

// The procedural leaf-cluster texture of a species (placeholder until authored textures exist): a 2x2
// atlas of cluster variants, each a curved twig with leaves on both sides, stem at v = 0 and growing toward
// v = 1 (the card geometry's convention). RGBA8, sRGB colour, alpha-tested at TREE_LEAF_ALPHA_CUTOFF.
// The mip chain is built here, not by a GPU blit: each level's alpha is rescaled so the fraction of texels
// passing the cutoff matches level 0, otherwise the crown thins out with distance.
export namespace Procedural
{
	constexpr float TREE_LEAF_ALPHA_CUTOFF = 0.5f;
	constexpr uint32 TREE_LEAF_ATLAS_CELLS = 2; // per axis

	struct TreeLeafTexture
	{
		uint32 size = 0;                       // level 0 width = height
		oc::vector<oc::vector<uint8>> mips;    // RGBA8 per level, level 0 first
	};

	// Level 0 of the atlas as RGBA8 - what TreeSystem writes to / reads from Assets/Local/Trees/Textures.
	void generateLeafClusterImage(const TreeSpeciesDesc& species, uint32 size, oc::vector<uint8>& outRgba);
	// The full chain from a level 0 (generated or loaded from disk), coverage-preserving.
	void buildLeafClusterMips(oc::span<const uint8> level0, uint32 size, TreeLeafTexture& out);
}
