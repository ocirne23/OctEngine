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

	// A hash of what the bake depends on (geometry + two settings words), for cache file names.
	uint32 treeBakeHash(const TreePiece& piece, uint32 a, uint32 b);

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
	constexpr float TREE_CARD_AXIS_CODE = 3.0f; // keep in step with instanced_indirect.fs.glsl's foliageCrownFrame
	// Every other card: this + its strip height in texture v (< 1): the FS's crown ellipsoid needs the card's width.
	constexpr float TREE_CARD_STRIP_CODE = 2.0f;

	// The FAR-TREE VOLUME's view of a piece (RendererVK TreeVolumePipeline): its EXTINCTION (1/m) over its billboard
	// box, res^3 voxels (x fastest, then y, then z), from the LOD-0 triangles - a leaf card blocks half its area on
	// average (x `leafCoverage`, the texture's opaque fraction), the bark a quarter of its surface (a convex body's
	// mean projected area), each spread over its voxels per unit volume.
	void bakeTreeDensity(const TreePiece& piece, uint32 res, float leafCoverage, oc::vector<float>& out, TreeBillboardBox& outBox);
}
