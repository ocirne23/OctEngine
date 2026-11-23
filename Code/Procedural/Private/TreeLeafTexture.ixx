export module Procedural:TreeLeafTexture;

import Core;
import Core.glm;

import :TreeSpecies;

// The procedural leaf-cluster texture (placeholder until authored textures exist): a 2x2 atlas of cluster variants, each a
// curved twig with leaves on both sides, stem at v = 0 and growing toward v = 1 (the card geometry's convention).
// Alpha-tested at TREE_LEAF_ALPHA_CUTOFF.
// THE ATLAS HOLDS NO SPECIES COLOUR: it is a species' leaf SHAPE (its `Leaves` block), and every species that names it
// (`Leaves Texture <species>`) draws the same texture with its own colours (TreeLeafPalette) - the mesh leaves' fragment
// shader forms the colour (RendererVK instanced_indirect_tree.fs.glsl), the CPU bakes take a coloured copy
// (colourLeafClusterImage). THE CHANNELS (the albedo uploads as sRGB, so its RGB reach the shader decoded):
//   albedo R = the leaf's TONE (0..1, a leaf's own random: how far it stands toward the species' variation colour; 0 on
//              twigs and needles), stored through the sRGB curve - the sampler hands back the tone itself
//   albedo G = the SHADE x 0.5 (the midrib, the rim, the fold, the veins, the leaf's own brightness): it multiplied the
//              sRGB colour before, so decoded it multiplies the linear one (the palette holds 1 / decode(0.5))
//   albedo B = the TWIG share (1 on the twigs, the pinnate stems partly), stored through the sRGB curve
//   albedo A = the cover
//   normal RG = the tangent-space normal's X / Y (the shader rebuilds Z), B = the ACCENT share (1 on a leaf in the
//              species' accent colour), A = the leaf's RIM (0 at the midrib .. 1 at the edge)
// The albedo's mip chain is built here, not by a GPU blit: each level's alpha is rescaled so the fraction of texels
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

	// A species' COLOURS for the atlas, linear, x the shade's scale. A texel's colour:
	// mix(mix(leaf + tone x TONE, accent, ACCENT), twig, TWIG) x SHADE. Mirrored in instanced_indirect_tree.fs.glsl.
	struct TreeLeafPalette
	{
		glm::vec3 leaf{ 0.0f };   // `Color Leaf`
		glm::vec3 tone{ 0.0f };   // + this at tone 1: the leaf 0.35 x `Color LeafVariation` toward its autumn tint
		glm::vec3 accent{ 0.0f }; // `Color Accent`
		glm::vec3 twig{ 0.0f };   // `Color Twig` (absent: Bark)
	};
	TreeLeafPalette treeLeafPalette(const TreeSpeciesDesc& species);

	// Level 0 of the atlas of `shape`'s `Leaves` block and of its normal map, RGBA8 each (the channels above). Of the
	// colours only `Color LeafVariation` (the leaves' brightness spread) and `Color AccentShare` (which leaves are accent
	// leaves) are read: they are part of the shape.
	void generateLeafClusterImage(const TreeSpeciesDesc& shape, uint32 size, oc::vector<uint8>& outRgba, oc::vector<uint8>& outNormal);
	// The atlas in a species' colours, RGBA8 sRGB (the bakes' input: the voxel grids, the shadow masks).
	void colourLeafClusterImage(oc::span<const uint8> albedo, oc::span<const uint8> normal, const TreeLeafPalette& palette, oc::vector<uint8>& outRgba);
	// The atlas's STATISTICS: the cover-weighted mean share of each palette colour (x the shade) - leaf, tone, accent, twig.
	// leafClusterMean = the atlas's linear mean colour in a palette (the far volume's leaf colour), with no image.
	glm::vec4 leafClusterSums(oc::span<const uint8> albedo, oc::span<const uint8> normal);
	glm::vec3 leafClusterMean(const glm::vec4& sums, const TreeLeafPalette& palette);

	// The full chain from a level 0 (generated or loaded from disk), coverage-preserving.
	void buildLeafClusterMips(oc::span<const uint8> level0, uint32 size, TreeLeafTexture& out);
	// The atlas's NORMAL chain: X / Y from the mean of the texels' unit normals, the accent share and the rim averaged.
	void buildLeafNormalMips(oc::span<const uint8> normal0, uint32 size, oc::vector<oc::vector<uint8>>& outMips);
}
