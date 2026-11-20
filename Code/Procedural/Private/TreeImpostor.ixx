export module Procedural:TreeImpostor;

import Core;
import Core.glm;

import :TreeGenerator;

// What stands for a baked tree where its meshes do not, baked on the CPU: the shadow billboard's mask (a small software
// rasterizer), the far-tree volume's density grid and the voxel tier's grid.
export namespace Procedural
{
	// A level-0 RGBA8 image to sample while baking (the bark tiles; the leaf atlas is alpha-tested).
	struct TreeBakeImage
	{
		oc::span<const uint8> rgba;
		uint32 size = 0;
	};

	// A hash of what the bake depends on, for cache file names: `source` (what the piece was generated from - TreeSystem:
	// the species file's text, the piece's kind and index) + BAKE_VERSION + two settings words. NOT the geometry's floats:
	// those differ in their last bits between Debug and the optimized builds (/fp:fast), and a position on a rounding
	// edge renamed the file at every switch - each build deleted the other's cache and baked it again.
	uint32 treeBakeHash(uint32 source, uint32 a, uint32 b);

	// A tree's box (tree space, +Y up): its LOD-0 bark + leaves. Every side > 0.
	struct TreeBillboardBox
	{
		glm::vec3 min{ 0.0f };
		glm::vec3 max{ 1e-3f };
	};
	TreeBillboardBox billboardBox(const TreePiece& piece);

	// ---- The SHADOW BILLBOARD (RendererVK MATERIAL_FLAG_BILLBOARD) ----
	// A baked tree as THREE crossed cards with a MASK: its sun-shadow caster and rain shelter at every distance, and what
	// the rays see of it. Never in the main pass - it has no colour and no normals. Two vertical cards crossed on the trunk
	// axis (seen along X and along Z) and a horizontal one at `topCardHeight` of the box (seen from above: the shadow under
	// a high sun). Their views are stacked as three horizontal strips of one square image; a card's back face shows its
	// strip through the card (a silhouette is the same from both sides).
	// The strips are stripHeight rows each - rounded so every strip boundary stays on a texel boundary down to the last of
	// numMips mips (spare rows at the bottom).
	struct TreeBillboardLayout
	{
		uint32 stripHeight = 1;
		uint32 numMips = 1;
	};
	TreeBillboardLayout billboardLayout(uint32 size);
	// THE MASK, TWO bytes a texel, a level per mip (size x size, halving; numMips of the layout). The first: the texel's
	// COVER up to `barkValue` (0.5..1, RendererVK TREE_CARD_BARK; the alpha test at 0.5 reads a continuous cover), and from
	// there up - solid for certain - WHAT it is: `barkValue` = leaves only .. 255 = bark only (the leaf texture's alpha is
	// tested). Every level keeps level 0's solid share (an alpha test at 0.5 covers as much at any distance); a level's leaf
	// share is the cover-weighted mean of the texels under it.
	// The second: THE DEPTH - how far the tree's surface stands off the card's plane, 0..255 over the card's depth range
	// (billboardMesh), so the shadow pass casts from where the tree is and not from the flat card. A vertical card: half
	// the tree's thickness there (the sun lights either side). The horizontal card: the crown's top above the card at
	// `topCardHeight`. Cover-weighted in the levels, and spread into the empty texels around the crown.
	void bakeBillboardMask(const TreePiece& piece, const TreeBakeImage& leaves, uint32 size, float barkValue, float topCardHeight,
		oc::vector<oc::vector<uint8>>& outMips);
	// The cards (both faces), UVs into the strips. `topCardHeight`: the horizontal card's height in the box (0.5 = mid
	// height; `Billboard TopCardHeight`). Every vertex's TANGENT = the crown shape's radii (the cards' half extents): a
	// ray hit takes its normal from that shape, not from the flat card (RendererVK tree_card.inc.glsl). Its NORMAL = the
	// card's, its LENGTH the card's depth range in metres (the mask's depth at 255).
	void billboardMesh(const TreeBillboardBox& box, uint32 size, float topCardHeight, TreeMesh& out);

	// The FAR-TREE VOLUME's view of a piece (RendererVK TreeVolumePipeline): its EXTINCTION (1/m) over its
	// box, res^3 voxels (x fastest, then y, then z), from the LOD-0 LEAF triangles - a leaf card blocks half its area on
	// average (x `leafCoverage`, the texture's opaque fraction) - or, for a piece without leaves (a succulent's bark
	// blades), its bark: a quarter of its surface (a convex body's mean projected area). Each spread over its voxels per
	// unit volume.
	void bakeTreeDensity(const TreePiece& piece, uint32 res, float leafCoverage, oc::vector<float>& out, TreeBillboardBox& outBox);

	// ---- The VOXEL tier (RendererVK LitTreeVoxel, instanced_indirect_tree_voxel.fs.glsl) ----
	// A baked variant as a PREFILTERED voxel grid of res^3 voxels at level 0 over treeVoxelBox, with levels halving down to
	// TREE_VOXEL_MIN_RES - stored SPARSE, in BRICKS (keep tree_voxel.inc.glsl in step):
	// - A level of n voxels is cut into bricks of TREE_VOXEL_BRICK voxels a side, CENTRE TO CENTRE: brick k covers the
	//   voxel centres k B .. k B + B per axis, so it stores B + 1 = TREE_VOXEL_BRICK_TEXELS voxels a side and a filtered
	//   sample inside it never reads another brick (the shared face is stored by both). A brick with no opacity in any of
	//   its voxels is NOT stored: most of a tree's box is air.
	// - THE OPACITY ATLAS (one channel; BC4): every stored brick of EVERY level, slot s at column s % cols, row s / cols,
	//   as its 8 z slices side by side (64 x 8 texels: whole BC blocks). A voxel's OPACITY = the share of a ray it stops
	//   over one voxel crossing (the cube root of its volume) at that level. Slot 0 is the HEADER, no brick: its first two
	//   blocks hold the index texture's bindless id (TreeSystem writes it at upload).
	// - THE SURFACE ATLAS (RGBA, linear; BC3), half the opacity atlas each way: per stored brick
	//   TREE_VOXEL_SURFACE_TEXELS^3 samples over the SAME span (one every B / 3 voxels: the colour and the normal change
	//   slowly, the opacity carries the silhouette), as two tile sets side by side (2 x 16 x 4 texels): RGB = the mean
	//   surface colour (sRGB-ENCODED: the FS decodes it), A = the INTERIOR (0 = the crown's surface .. 1 = its core); then
	//   RGB = the mean leaf normal (outward from the crown) x 0.5 + 0.5 - its length is how much the leaves agree. Each
	//   sample is the opacity-weighted mean under a tent of its spacing.
	// - THE INDEX (RGBA8, exact): texel (0, 0) = the header (R = log2 res, G = the levels, B = log2 cols); below it level
	//   by level (level 0 first) its D^3 bricks, D = treeVoxelBricks(n): brick (x, y, z) at (x + z D, y) of the level's D
	//   rows - RGB = the brick's slot (24 bits), 0 = empty.
	// One level of each texture, no mip chain: a level's bricks take what it needs, nothing more.
	// Coarser levels keep the coverage: a coarse voxel's opacity is 1 - the mean transmission through its children along
	// the three axes, not the mean opacity (which made dense clumps too thin far away).
	constexpr uint32 TREE_VOXEL_BRICK = 7;
	constexpr uint32 TREE_VOXEL_BRICK_TEXELS = TREE_VOXEL_BRICK + 1;
	constexpr uint32 TREE_VOXEL_SURFACE_TEXELS = 4;
	constexpr uint32 TREE_VOXEL_MAX_COLS = 128;   // bricks per atlas row: at most 8192 texels wide
	constexpr uint32 TREE_VOXEL_MIN_RES = 4;      // the last level
	constexpr uint32 TREE_VOXEL_BAKE_VERSION = 6; // bump when the voxel bake changes (its .tex re-bakes)
	struct TreeVoxelBake
	{
		uint32 width = 0;  // the opacity atlas; the surface atlas is (width / 2) x (height / 2)
		uint32 height = 0;
		oc::vector<uint8> opacity; // R8
		oc::vector<uint8> surface; // RGBA8
		uint32 indexWidth = 0;
		uint32 indexHeight = 0;
		oc::vector<uint8> index;   // RGBA8
		uint32 bricks = 0;         // stored (the header's slot not counted)
		uint32 bricksTotal = 0;    // of every level
	};
	uint32 treeVoxelLevels(uint32 res); // res down to TREE_VOXEL_MIN_RES
	uint32 treeVoxelBricks(uint32 n);   // bricks per axis of a level of n voxels
	// The grid's box: the tree's box with a voxel of margin on every side (the trilinear filter never cuts the crown).
	TreeBillboardBox treeVoxelBox(const TreePiece& piece, uint32 res);
	// From the LOD-0 leaves (their texture's alpha and colour) and bark (its texture's colour), both sampled per point. The
	// bark's COLOUR is weighted by how much the voxel's own leaves leave of it (exp(-their optical depth)); its cover
	// counts in full. `albedoScale` x the linear colour (`Billboard AlbedoScale`).
	void bakeTreeVoxels(const TreePiece& piece, const TreeBakeImage& bark, const TreeBakeImage& leaves, uint32 res, float albedoScale,
		TreeVoxelBake& out);
	// The box the FS marches from (its front faces), in `slices` + 1 RINGS up its height: per vertex the position, normal =
	// its GRID coordinate (0..1 per axis), tangent = the box size (the VS rebuilds the box from both), texCoords.z =
	// TREE_VOXEL_BOX_WIND_CODE. THE SLICES are for the wind's trunk bend, a curve: the tier that crossfades with the meshes
	// takes TREE_VOXEL_BOX_SLICES (the box then bends as the mesh does), the far voxel trees 1 (8 corners: up to a million
	// boxes, none of them beside a mesh).
	void treeVoxelBoxMesh(const TreeBillboardBox& box, uint32 slices, TreeMesh& out);
	constexpr uint32 TREE_VOXEL_BOX_SLICES = 4;
	constexpr float TREE_VOXEL_BOX_WIND_CODE = 2.5f; // rides the tangent's w: >= 2 = the trunk bend only (tree_wind.inc.glsl treeWindPayload)
}
