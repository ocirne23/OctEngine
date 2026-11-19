#version 450

#extension GL_EXT_nonuniform_qualifier : enable

#include "shared.inc.glsl"

// Fragment stage for the sun shadow depth pass: depth-only, it discards the holes of alpha-masked (foliage/cutout)
// materials. The cull resolved the mask: in_alphaTexIdx == 0xFFFF = opaque (no test). No gl_FragDepth write: a
// per-leaf baked depth cost ~15 % of the shadow draw.
// TREE_VOXEL (ShadowMapGraphicsPipeline's voxel pipeline, "Trees/Voxel shadows"): a tree's VOXEL caster instead - the
// march along the light through its grid from the face the sun sees, to where the coverage reaches "Voxel shadow
// coverage", and that depth written (or nothing). A FIXED threshold: a shadow map gets no TAA to resolve noise.
#define SHADOW_ALPHA_CUTOFF 0.5

layout (binding = 7) uniform sampler2D u_textures[];

layout (location = 0) in vec2 in_uv;
layout (location = 1) in flat uint in_alphaTexIdx;

#ifdef TREE_VOXEL
layout (location = 2) in vec3 in_voxelGrid;
layout (location = 3) in flat vec3 in_voxelDirG;    // grid units per metre along the light's travel, no zero component
layout (location = 4) in flat vec2 in_voxelStepLod; // x = the cube root of the box's volume (world metres), y = log2(texel / x) + the LOD bias
layout (location = 5) in flat vec2 in_voxelDepth;   // x = depth per metre along the light, y = the coverage that casts
// Standard Z (shadow maps): the hit lies at or behind the face, so the written depth is never smaller - early depth still
// rejects a face behind another caster.
layout (depth_greater) out float gl_FragDepth;
#include "tree_voxel.inc.glsl"

// The written depth's bias along the light (m): the rasterizer's depth bias does not reach a written depth.
const float VOXEL_SHADOW_BIAS = 0.15;
#endif

void main()
{
#ifdef TREE_VOXEL
	// The alpha index is the grid's opacity atlas: its header names the index texture.
	const VoxelGrid grid = voxelGrid(in_alphaTexIdx);
	// A voxel about a cascade texel (+ "Voxel LOD bias").
	const float lod = clamp(floor(in_voxelStepLod.y + log2(grid.res)), 0.0, grid.levels - 1.0);
	const VoxelLevel level = voxelLevel(grid, lod);
	const float n = level.n;
	const float stepWorld = in_voxelStepLod.x / n;
	// The march in the level's voxel coordinates (tree_voxel.inc.glsl). An EMPTY brick is jumped to its far side.
	// THIS MARCH SETS THE SHADER'S REGISTER COUNT (40), so its ray is NOT held in voxel coordinates: the face point and the
	// direction are read from the interpolants at each step (no register between steps) and scaled there, and the box is
	// tested on the sample. Held as a start + a direction + an exit distance (the main pass's form) it took 48.
	float t = 0.5 * stepWorld;
	float transmit = 1.0;
	const int maxSteps = int(n * 1.75) + 2;
	VoxelBrick marchBrick = voxelBrickNone();
	for (int i = 0; i < maxSteps; ++i)
	{
		const vec3 c = (in_voxelGrid + in_voxelDirG * t) * n;
		if (any(greaterThan(abs(c - 0.5 * n), vec3(0.501 * n)))) // the box, + the face's own rounding
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
