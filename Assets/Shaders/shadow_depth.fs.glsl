#version 450

#extension GL_EXT_nonuniform_qualifier : enable

#include "shared.inc.glsl"

// Fragment stage for the sun shadow depth pass. Writes no color (depth-only); it discards fragments of
// alpha-masked (foliage/cutout) materials so their holes cast correct shadows.
// The cull pass already resolved the mask: out_alphaTexIdx == 0xFFFF means opaque (no test).
// No gl_FragDepth write (a per-leaf baked depth cost ~15 % of the shadow draw with the 350² grove).
#define SHADOW_ALPHA_CUTOFF 0.5

layout (binding = 7) uniform sampler2D u_textures[];

layout (location = 0) in vec2 in_uv;
layout (location = 1) in flat uint in_alphaTexIdx;

void main()
{
	if (in_alphaTexIdx != 0xFFFFu)
	{
		if (texture(u_textures[nonuniformEXT(in_alphaTexIdx)], in_uv).a < SHADOW_ALPHA_CUTOFF)
			discard;
	}
}
