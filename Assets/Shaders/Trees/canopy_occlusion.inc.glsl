// THE CANOPY MAP as SKY OCCLUSION (Renderer::setCanopyMap; Procedural ClutterSystem bakes it from the tree records, with
// the forest floor map). A top-down map of all the trees near the camera: R = the tree cover above the ground (blurred
// over a few metres: the share of the sky the crowns around a point hide), G = the canopy's top over the ground /
// CANOPY_MAP_MAX_HEIGHT (the cover-weighted mean of the crowns there). A lit surface under it loses "Trees/Canopy sky
// occlusion" of its AMBIENT light by the cover ABOVE it: all of the cover on the ground, less up the crown, none at the
// canopy's top - the forest floor, the trunks, the lower leaves, a unit among the trees.
// The GI probes carry the same thing at THEIR spacing (2 m at the scene focus, twice that per cascade): this is the part
// they cannot resolve - one crown from the next, tens of metres out. It is NOT the sun's shadow (the cascades).
// The includer declares u_textures[] and has the frame UBO.
#ifndef CANOPY_OCCLUSION_INC_GLSL
#define CANOPY_OCCLUSION_INC_GLSL

// The canopy over xz: x = the ambient's loss on the GROUND there (the cover x the tweak; 0 = none: no map, outside it, the
// tweak 0, open sky), y = the canopy's top over the ground (m, at least 1). Two steps, so a caller works out its own
// height only where x > 0.
vec2 canopyCover(vec2 xz)
{
	if (u_foliage_canopyInvSize <= 0.0 || u_foliage_canopyOcclusion <= 0.0)
		return vec2(0.0, 1.0);
	const vec2 uv = (xz - u_foliage_canopyOrigin) * u_foliage_canopyInvSize;
	const vec2 inside = min(uv, 1.0 - uv);
	const float edge = min(inside.x, inside.y); // the map's sampler repeats: nothing is read past its square
	if (edge <= 0.0)
		return vec2(0.0, 1.0);
	const vec2 tap = textureLod(u_textures[u_foliage_canopyTex], uv, 0.0).rg;
	// Faded out over the square's outer 5 %: the map's edge never shows as a line.
	return vec2(u_foliage_canopyOcclusion * tap.r * min(edge * 20.0, 1.0), max(tap.g * CANOPY_MAP_MAX_HEIGHT, 1.0));
}

// The tree COVER over xz (0..1, the map's R, blurred over a few metres), without the sky occlusion's tweak: the snow's
// shelter under the crowns (snow.inc.glsl snowSiteCatch). 0 with no map and outside it (faded over the outer 5 %).
float canopyTreeCover(vec2 xz)
{
	if (u_foliage_canopyInvSize <= 0.0)
		return 0.0;
	const vec2 uv = (xz - u_foliage_canopyOrigin) * u_foliage_canopyInvSize;
	const vec2 inside = min(uv, 1.0 - uv);
	const float edge = min(inside.x, inside.y);
	if (edge <= 0.0)
		return 0.0;
	return textureLod(u_textures[u_foliage_canopyTex], uv, 0.0).r * min(edge * 20.0, 1.0);
}

// The ambient's factor `height` metres above the ground under that canopy: the share of it that stands ABOVE the point.
float canopySkyVisibility(vec2 cover, float height)
{
	return 1.0 - cover.x * clamp(1.0 - height / cover.y, 0.0, 1.0);
}

#endif
