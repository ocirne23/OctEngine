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
layout (location = 3) in flat vec3 in_voxelDirG;    // grid units per metre along the light's travel
layout (location = 4) in flat vec4 in_voxelSizeLod; // xyz = the box in world metres, w = log2(texel / the box volume's cube root)
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
	const uint tex = in_alphaTexIdx;
	const float res = voxelGridRes(tex);
	const float tilesX = res / TREE_VOXEL_TILES_Y;
	const float maxLod = float(textureQueryLevels(u_textures[tex]) - 1);
	// A voxel about a cascade texel (+ "Voxel LOD bias"); the shadow sampler has no mip bias.
	const float lod = clamp(floor(in_voxelSizeLod.w + log2(res)), 0.0, maxLod);
	const float n = res * exp2(-lod);
	const float stepWorld = pow(in_voxelSizeLod.x * in_voxelSizeLod.y * in_voxelSizeLod.z, 1.0 / 3.0) / res * exp2(lod);
	// Empty space is skipped two levels coarser (voxelSkip), tested only while the last sample was empty.
	const vec3 dirG = mix(vec3(1e-6), in_voxelDirG, greaterThan(abs(in_voxelDirG), vec3(1e-6)));
	const float skipLod = min(lod + 2.0, maxLod);
	float t = 0.5 * stepWorld;
	float transmit = 1.0;
	bool empty = true;
	const int maxSteps = int(n * 1.75) + 2;
	for (int i = 0; i < maxSteps; ++i)
	{
		const vec3 g = in_voxelGrid + dirG * t;
		if (any(lessThan(g, vec3(-1e-3))) || any(greaterThan(g, vec3(1.0 + 1e-3))))
			break;
		if (empty && skipLod > lod)
		{
			const float skip = voxelSkip(tex, g, dirG, res, tilesX, skipLod, 0.0);
			if (skip > 0.0)
			{
				t += skip;
				continue;
			}
		}
		const float a = voxelOpacityNearest(tex, g, n, tilesX, lod); // one fetch: the threshold is binary
		empty = a <= VOXEL_SKIP_EMPTY;
		transmit *= 1.0 - a;
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
