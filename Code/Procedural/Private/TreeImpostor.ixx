export module Procedural:TreeImpostor;

import Core;
import Core.glm;

import :TreeGenerator;

// Octahedral impostors of tree PIECES (branch modules), baked on the CPU by a small software rasterizer.
// An atlas of N x N frames; frame (i, j) is an orthographic view of the piece's bounding sphere from the
// octahedrally encoded direction of its centre. Per texel: albedo + coverage alpha, and the normal in the
// FRAME's tangent space (x = right, y = up, z = toward the viewer; alpha = depth). The frame mapping, the
// frame basis and the atlas layout are MIRRORED in Assets/Shaders/tree_impostor.vs.glsl - keep them in step.
export namespace Procedural
{
	// A level-0 RGBA8 image to sample while baking (the bark tiles; the leaf atlas is alpha-tested).
	struct TreeBakeImage
	{
		oc::span<const uint8> rgba;
		uint32 size = 0;
	};

	struct TreeImpostorBounds
	{
		glm::vec3 centre{ 0.0f };
		float radius = 1.0f;
	};

	// The bounding sphere the impostor frames cover (all LOD-0 bark and leaf vertices).
	TreeImpostorBounds impostorBounds(const TreePiece& piece);

	// Bakes the atlas (frames x frameSize square) into level-0 albedo and normal images.
	void bakeImpostor(const TreePiece& piece, const TreeImpostorBounds& bounds, const TreeBakeImage& bark, const TreeBakeImage& leaves,
		uint32 frames, uint32 frameSize, oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal);

	// A hash of what the bake depends on (geometry + settings), for cache file names.
	uint32 impostorHash(const TreePiece& piece, uint32 frames, uint32 frameSize);

	// --- Billboards: the branch module as two crossed cards along its axis (module +Y). ---
	// The vertical card (normal +X) and the horizontal card (normal +Z, i.e. world-up after the composite), their
	// views stacked as horizontal strips of a square image. `numViews` (Trees/Billboard views):
	//   4 - each card FACE has its own strip, top to bottom: side from +X, side from -X, top, bottom;
	//   2 - one strip per card (side from +X, top), its back face shows the front view through the card.
	// Real geometry, so unlike the impostor they cast alpha-tested shadows.
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
	// through the axis at mid height, seen from above - the top-down view (MATERIAL_FLAG_FOLIAGE_TOP_CARD tells
	// the lit FS, which finds the card by its up-facing normal).
	TreeBillboardView billboardHorizontalView(const TreeBillboardBox& box);
	// The strips stacked top to bottom (card by card, each card's faces in turn), each stripHeight rows - rounded
	// so every strip boundary stays on a texel boundary down to the last of numMips mips (spare rows at the bottom).
	struct TreeBillboardLayout
	{
		uint32 numStrips = 2;
		uint32 stripHeight = 1;
		uint32 numMips = 1;
	};
	TreeBillboardLayout billboardLayout(uint32 size, uint32 numViews, bool horizontal);
	// Bakes every card's views into level-0 albedo + normal images (size x size; normals in each card's tangent
	// space). VOLUME normals: bent by `normalBend` (0..1) toward "out of the clump's centre", sign kept (they may
	// face away from the card's front) - so the cards shade like a round clump from any sun side.
	void bakeBillboards(const TreePiece& piece, const TreeBakeImage& bark, const TreeBakeImage& leaves, uint32 size, float normalBend,
		uint32 numViews, bool horizontal, oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal);
	// The cards (both faces), UVs into the strips of a `numViews` / `horizontal` bake.
	void billboardMesh(const TreeBillboardBox& box, uint32 size, uint32 numViews, bool horizontal, TreeMesh& out);

	// Mirrors of tree_impostor.vs.glsl.
	glm::vec2 impostorOctEncode(glm::vec3 d);
	glm::vec3 impostorOctDecode(glm::vec2 p);
	void impostorFrameBasis(const glm::vec3& dir, glm::vec3& outRight, glm::vec3& outUp);
}
