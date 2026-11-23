#version 460

// THE SNOW SITE MAP's INVALIDATION (SnowSitePipeline::recordInvalidate; Docs/Context/RendererVK/Snow.md): one rectangle of
// lattice texels per dispatch, at most one window's size (each slot once). ORs pc_bits (SNOW_SITE_STALE: the caps there go
// at once; SNOW_SITE_RETRACE: the next tick that sees a texel traces it) into the class image; the trace writes the class
// fresh, which clears both.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r8ui) uniform restrict uimage2D u_siteClass;

#include "push.generated.glsl"

void main()
{
	const ivec2 rel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(rel, pc_size)))
		return;
	const ivec2 slot = (pc_min + rel) & ivec2(SNOW_SITE_RES - 1);
	imageStore(u_siteClass, slot, uvec4(imageLoad(u_siteClass, slot).r | pc_bits));
}
