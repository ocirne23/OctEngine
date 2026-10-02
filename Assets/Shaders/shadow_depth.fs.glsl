#version 450

#extension GL_EXT_nonuniform_qualifier : enable

#include "shared.inc.glsl"

// Fragment stage for the sun shadow depth pass. Writes no color (depth-only); it discards fragments of
// alpha-masked (foliage/cutout) materials so their holes cast correct shadows.
// The cull pass already resolved the mask: out_alphaTexIdx == 0xFFFF means opaque (no test).
#define SHADOW_ALPHA_CUTOFF 0.5

// FOLIAGE casters (the tree billboards) write each leaf at its BAKED DEPTH off the card (the normal map's alpha,
// signed along the card's front normal, in units of the card's u length) - the same point the lit FS looks its
// shadow up from (instanced_indirect.fs.glsl sunShadowFirstFoliage), so the crossed cards shadow each other leaf by
// leaf instead of as two flat planes. The VS pulled them toward the light by their bounding radius, so the depth
// only ever grows: depth_greater keeps the early / hierarchical reject for every caster. Every other caster writes
// gl_FragCoord.z - the depth it would get without the write, the rasterizer's depth bias included.
layout (depth_greater) out float gl_FragDepth;

layout (binding = 7) uniform sampler2D u_textures[];

layout (location = 0) in vec2 in_uv;
layout (location = 1) in flat uint in_alphaTexIdx;
layout (location = 2) in vec3 in_worldPos;
layout (location = 3) in flat uint in_foliageNormalTexIdx;
layout (location = 4) in flat vec4 in_foliageDepth; // xyz = d(depth)/d(world), w = the VS's pull toward the light

void main()
{
	float depth = gl_FragCoord.z;
	if (in_foliageNormalTexIdx != 0xFFFFu)
	{
		// The card frame from the screen derivatives (before the discard), as in the lit FS.
		const vec2 uvDx = dFdx(in_uv), uvDy = dFdy(in_uv);
		const float uvDet = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
		float push = in_foliageDepth.w;
		if (abs(uvDet) > 1e-20)
		{
			const vec3 pdx = dFdx(in_worldPos), pdy = dFdy(in_worldPos);
			const vec3 cardDu = (pdx * uvDy.y - pdy * uvDx.y) / uvDet;
			const vec3 cardDv = (pdy * uvDx.x - pdx * uvDy.x) / uvDet;
			const vec3 frontN = cross(cardDv, cardDu);
			const float frontLen = length(frontN);
			if (frontLen > 0.0)
			{
				const float depth01 = texture(u_textures[nonuniformEXT(in_foliageNormalTexIdx)], in_uv).a;
				const float offset = (depth01 * 2.0 - 1.0) * length(cardDu) / frontLen; // world, along frontN / frontLen
				push += offset * dot(frontN, in_foliageDepth.xyz);
			}
		}
		depth += max(push, 0.0);
	}
	if (in_alphaTexIdx != 0xFFFFu)
	{
		if (texture(u_textures[nonuniformEXT(in_alphaTexIdx)], in_uv).a < SHADOW_ALPHA_CUTOFF)
			discard;
	}
	gl_FragDepth = depth;
}
