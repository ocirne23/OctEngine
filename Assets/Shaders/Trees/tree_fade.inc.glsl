// The tree DISTANCE FADE (Layout.ixx makeDistanceFadeFlags; Procedural TreeSystem::applyFadeBands): a dithered fade over
// a camera-distance band, for the tree mesh -> card mesh -> billboard crossfades. Used by LitTree and LitFoliage.
#ifndef TREE_FADE_INC_GLSL
#define TREE_FADE_INC_GLSL

// True where this pixel is faded away. A fade-out surface keeps the pixels with dither < 1 - f, a fade-in one the
// others, so an out / in pair over the same band shares no pixel. Interleaved gradient noise, shifted per frame so TAA
// blends it.
// `treeOrigin` = the INSTANCE origin (the tree's base; every representation of one tree has the same one): the WHOLE
// tree fades by one distance and dissolves evenly. By the pixel's own distance the band's sphere cut a tree in two - the
// mesh on its near half, the voxel / card / billboard tier on its far half - and the cut showed the crown's dark inside.
// `treeDistance` = the camera's distance to the tree's origin: one value for the whole tree, so a vertex shader that
// has the origin hands the DISTANCE over (flat), not the origin for every pixel to measure again.
bool treeDistanceFaded(uint flags, float treeDistance)
{
	if ((flags & MATERIAL_FLAG_DISTANCE_FADE) == 0u)
		return false;
	const float fadeStart = float(flags & 0xFFFu);
	const float fadeWidth = float((flags >> 12) & 0x3FFu); // >= 1: makeDistanceFadeFlags (Layout.ixx)
	const float f = clamp((treeDistance - fadeStart) / fadeWidth, 0.0, 1.0);
	const vec2 pixel = gl_FragCoord.xy + 5.588238 * float(u_frameIndex & 7u);
	const float dither = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
	return ((flags & MATERIAL_FLAG_FADE_IN) != 0u) ? dither < 1.0 - f : dither >= 1.0 - f;
}

// From the tree's origin (the foliage cards: their fragment shader needs the origin itself too).
bool treeDistanceFaded(uint flags, vec3 treeOrigin)
{
	return treeDistanceFaded(flags, distance(u_viewPos, treeOrigin));
}

#endif
