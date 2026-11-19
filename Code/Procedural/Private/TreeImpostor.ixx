export module Procedural:TreeImpostor;

import Core;
import Core.glm;

import :TreeGenerator;

// The far representations of tree PIECES baked on the CPU by a small software rasterizer: the billboard cards and the
// far-tree volume's density grid.
export namespace Procedural
{
	// A level-0 RGBA8 image to sample while baking (the bark tiles; the leaf atlas is alpha-tested).
	struct TreeBakeImage
	{
		oc::span<const uint8> rgba;
		uint32 size = 0;
	};

	// A hash of what the bake depends on, for cache file names: `source` (what the piece was generated from - TreeSystem:
	// the species file's text, the piece's set and index) + BAKE_VERSION + two settings words. NOT the geometry's floats:
	// those differ in their last bits between Debug and the optimized builds (/fp:fast), and a position on a rounding
	// edge renamed the file at every switch - each build deleted the other's cache and baked it again.
	uint32 treeBakeHash(uint32 source, uint32 a, uint32 b);

	// --- Billboards: the branch module as two crossed cards along its axis (module +Y). ---
	// The vertical card (normal +X) and the horizontal card (normal +Z, i.e. world-up after the composite), their
	// views stacked as horizontal strips of a square image. `numViews` (Trees/Billboard views):
	//   4 - each card FACE has its own strip, top to bottom: side from +X, side from -X, top, bottom;
	//   2 - one strip per card (side from +X, top), its back face shows the front view through the card.
	// Real geometry, so they cast alpha-tested shadows.
	struct TreeBillboardBox
	{
		glm::vec3 min{ 0.0f };
		glm::vec3 max{ 1e-3f };
	};
	// One card: its centre and basis (u along `right`, v down along -`up`, `normal` its front) and its half size.
	struct TreeBillboardView
	{
		glm::vec3 centre;
		glm::vec3 right;
		glm::vec3 up;
		glm::vec3 normal;
		glm::vec2 halfExtent;
	};

	TreeBillboardBox billboardBox(const TreePiece& piece); // LOD-0 bark + leaves
	void billboardViews(const TreeBillboardBox& box, TreeBillboardView& outSide, TreeBillboardView& outTop);
	// `horizontal` (whole trees, whose +Y is up - both cards above stand vertical): a THIRD card, horizontal
	// through the axis at `height` of the box (0.5 = mid height; `Billboard TopCardHeight`), seen from above - the
	// top-down view (MATERIAL_FLAG_BILLBOARD_TOP_CARD tells the lit FS, which finds the card by its up-facing normal).
	TreeBillboardView billboardHorizontalView(const TreeBillboardBox& box, float height);
	// The strips stacked top to bottom (card by card, each card's faces in turn), each stripHeight rows - rounded
	// so every strip boundary stays on a texel boundary down to the last of numMips mips (spare rows at the bottom).
	struct TreeBillboardLayout
	{
		uint32 numStrips = 2;
		uint32 stripHeight = 1;
		uint32 numMips = 1;
	};
	TreeBillboardLayout billboardLayout(uint32 size, uint32 numViews, bool horizontal);
	// Bakes every card's views into level-0 albedo + normal images (size x size). The NORMAL image: RGB = the normal in
	// the card's tangent space, sign kept (it may face away from the card's front, toward a sun behind the tree), A = the
	// INTERIOR (0 = on the crown's surface .. 1 = about the crown's core depth; linear - the lit FS remaps it). Both come
	// from the piece's own CROWN FIELD (its leaves splatted into a grid and blurred: a soft hull of whatever shape -
	// round, cone, flat pad): the normals bent by `normalBend` (0..1) toward the hull's outward gradient at the texel's
	// leaf, the interior the crown density along that gradient out of the hull.
	void bakeBillboards(const TreePiece& piece, const TreeBakeImage& bark, const TreeBakeImage& leaves, uint32 size, float normalBend,
		uint32 numViews, bool horizontal, oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal);
	// The cards (both faces), UVs into the strips of a `numViews` / `horizontal` bake. `axisInZ` (the merged branch
	// cards): texCoords.z = TREE_CARD_AXIS_CODE + the texture v of the piece's axis (its +Y line through the origin) on
	// that card - RenderMeshData puts it into the tangent's w magnitude, and the lit FS rebuilds the crown frame from
	// it where the instance origin is not the card's piece. `topCardHeight`: the horizontal card's height (billboardHorizontalView).
	void billboardMesh(const TreeBillboardBox& box, uint32 size, uint32 numViews, bool horizontal, TreeMesh& out, bool axisInZ = false,
		float topCardHeight = 0.5f);
	constexpr float TREE_CARD_AXIS_CODE = 3.0f; // keep in step with instanced_indirect_foliage.fs.glsl's foliageCrownFrame
	// The merged branch cards' WIND on top of the axis code: + this x (weight | phase << 4) - the vertex's 4-bit weight
	// along its module (0 at the root, 15 at the tip) and its placement's 3-bit phase (tree_wind.inc.glsl treeWindPayload).
	constexpr float TREE_CARD_WIND_STEP = 4.0f;
	// Every other card: this + its strip height in texture v (< 1): the FS's crown ellipsoid needs the card's width.
	constexpr float TREE_CARD_STRIP_CODE = 2.0f;

	// The FAR-TREE VOLUME's view of a piece (RendererVK TreeVolumePipeline): its EXTINCTION (1/m) over its billboard
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
	// The grid's box: the billboard box with a voxel of margin on every side (the trilinear filter never cuts the crown).
	TreeBillboardBox treeVoxelBox(const TreePiece& piece, uint32 res);
	// From the LOD-0 leaves (their texture's alpha and colour) and bark (its texture's colour), both sampled per point. The
	// bark's COLOUR is weighted by how much the voxel's own leaves leave of it (exp(-their optical depth)); its cover
	// counts in full. `albedoScale` x the linear colour (`Billboard AlbedoScale`, as the whole-tree billboards).
	void bakeTreeVoxels(const TreePiece& piece, const TreeBakeImage& bark, const TreeBakeImage& leaves, uint32 res, float albedoScale,
		TreeVoxelBake& out);
	// The box the FS marches from (its front faces): per corner the position, normal = its GRID coordinate (0 / 1 per
	// axis), tangent = the box size (the VS rebuilds the box from both), texCoords.z = TREE_CARD_STRIP_CODE + 0.5 (the
	// trunk bend of tree_wind.inc.glsl, as the billboards).
	void treeVoxelBoxMesh(const TreeBillboardBox& box, TreeMesh& out);
}
