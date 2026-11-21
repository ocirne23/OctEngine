#version 450

#extension GL_EXT_nonuniform_qualifier : enable

#include "shared.inc.glsl"

// Fragment stage for the sun shadow depth pass: depth-only, it discards the holes of alpha-masked (foliage/cutout)
// materials. The cull resolved the mask: in_alphaTexIdx == 0xFFFF = opaque (no test). No gl_FragDepth write: a
// per-leaf baked depth cost ~15 % of the shadow draw.
// TREE_VOXEL (ShadowMapGraphicsPipeline's voxel pipeline): the casters that WRITE their depth. A tree's VOXEL caster
// ("Trees/Voxel shadows") - a rectangle around its box in the cascade (shadow_depth.vs.glsl): the pixel's light ray against
// the box, then the march along the light through its grid from the entry, to where the coverage
// reaches "Voxel shadow coverage", and that depth written (or nothing). A FIXED threshold: a shadow map gets no TAA to
// resolve noise. And a tree's SHADOW BILLBOARD (in_voxelStepLod.x < 0): its mask's alpha test, then the depth of the
// tree's surface from the mask's green (shadow_depth.vs.glsl) - one tap for both.
#define SHADOW_ALPHA_CUTOFF 0.5

layout (binding = 7) uniform sampler2D u_textures[];

layout (location = 0) in vec2 in_uv;
layout (location = 1) in flat uint in_alphaTexIdx;

#ifdef TREE_VOXEL
layout (location = 2) in vec3 in_voxelGrid;         // the point of this pixel's light ray on the rectangle, in grid space
                                                    // a billboard: per axis the way along the sun out of the tree's box (m)
layout (location = 3) in flat vec3 in_voxelDirG;    // grid units per metre along the light's travel, no zero component
layout (location = 4) in flat vec2 in_voxelStepLod; // x = the cube root of the box's volume (world metres), y = log2(texel / x) + the LOD bias; x < 0: a BILLBOARD
layout (location = 5) in flat vec2 in_voxelDepth;   // x = depth per metre along the light, y = the coverage that casts
                                                    // a billboard: x = the depth of its whole range along the light, y = depth per metre
// Standard Z (shadow maps): the hit lies at or behind the rectangle (the box's depth nearest to the light), so the written
// depth is never smaller - early depth still rejects a tree behind another caster.
layout (depth_greater) out float gl_FragDepth;
#include "tree_voxel.inc.glsl"

// The written depth's bias along the light (m): the rasterizer's depth bias does not reach a written depth.
const float VOXEL_SHADOW_BIAS = 0.15;
// A billboard's lift stays this far (m) short of the tree's surface: the trunk and the outermost leaves do not shade
// themselves through it.
const float CARD_SHADOW_BIAS = 0.2;
#endif

void main()
{
#ifdef TREE_VOXEL
	// A billboard: the card stands its whole depth range toward the sun; the texel's LIFT off the card's plane = its
	// share of that range, at most out to the tree's box, less the bias. The depth goes back by the rest.
	if (in_voxelStepLod.x < 0.0)
	{
		const vec4 mask = texture(u_textures[nonuniformEXT(in_alphaTexIdx)], in_uv);
		if (mask.a < SHADOW_ALPHA_CUTOFF)
			discard;
		const float boxExit = min(min(in_voxelGrid.x, in_voxelGrid.y), in_voxelGrid.z);
		const float lift = max(min(mask.g * in_voxelDepth.x, boxExit * in_voxelDepth.y) - CARD_SHADOW_BIAS * in_voxelDepth.y, 0.0);
		gl_FragDepth = gl_FragCoord.z + in_voxelDepth.x - lift;
		return;
	}
	// The alpha index is the grid's opacity atlas: its header names the index texture.
	const VoxelGrid grid = voxelGrid(in_alphaTexIdx);
	// A voxel about a cascade texel (+ "Voxel LOD bias").
	const float lod = clamp(floor(in_voxelStepLod.y + log2(grid.res)), 0.0, grid.levels - 1.0);
	const VoxelLevel level = voxelLevel(grid, lod);
	const float n = level.n;
	const float stepWorld = in_voxelStepLod.x / n;
	// WHERE THE LIGHT RAY ENTERS THE BOX (m from the rectangle; 0: the rectangle lies inside it). A pixel of the rectangle
	// beside the box leaves here, before any fetch of the march.
	float t;
	{
		const vec3 t0 = -in_voxelGrid / in_voxelDirG;
		const vec3 t1 = (1.0 - in_voxelGrid) / in_voxelDirG;
		const vec3 tLow = min(t0, t1);
		const vec3 tHigh = max(t0, t1);
		t = max(max(max(tLow.x, tLow.y), tLow.z), 0.0);
		if (t >= min(min(tHigh.x, tHigh.y), tHigh.z))
			discard;
	}
	// The march in the level's voxel coordinates (tree_voxel.inc.glsl). An EMPTY brick is jumped to its far side.
	// THIS MARCH SETS THE SHADER'S REGISTER COUNT (40), so its ray is NOT held in voxel coordinates: the rectangle's point and
	// the direction are read from the interpolants at each step (no register between steps) and scaled there, and the box is
	// tested on the sample. Held as a start + a direction + an exit distance (the main pass's form) it took 48.
	t += 0.5 * stepWorld;
	float transmit = 1.0;
	const int maxSteps = int(n * 1.75) + 2;
	VoxelBrick marchBrick = voxelBrickNone();
	for (int i = 0; i < maxSteps; ++i)
	{
		const vec3 c = (in_voxelGrid + in_voxelDirG * t) * n;
		if (any(greaterThan(abs(c - 0.5 * n), vec3(0.501 * n)))) // the box, + the entry's own rounding
			break;
		float skip;
		const float a = voxelOpacityNearest(grid, level, c, in_voxelDirG * n, stepWorld, marchBrick, skip); // one fetch: the threshold is binary
		if (skip > 0.0)
		{
			t += skip;
			continue;
		}
		transmit *= 1.0 - voxelThin(a);
		if (1.0 - transmit >= in_voxelDepth.y)
		{
			gl_FragDepth = max(gl_FragCoord.z + (t + VOXEL_SHADOW_BIAS) * in_voxelDepth.x, gl_FragCoord.z);
			return;
		}
		t += stepWorld;
	}
	discard;
#else
	if (in_alphaTexIdx != 0xFFFFu)
	{
		if (texture(u_textures[nonuniformEXT(in_alphaTexIdx)], in_uv).a < SHADOW_ALPHA_CUTOFF)
			discard;
	}
#endif
}
