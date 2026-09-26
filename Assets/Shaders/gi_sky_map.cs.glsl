#version 450

#include "shared.inc.glsl"

// THE SKY MAP: the per-frame lat-long bake of the analytic sky (mapping + layer ids in
// atmosphere.inc.glsl: skyMapUV / skyMapDir, SKY_MAP_LAYER_*). One 8x8-workgroup dispatch replaces an
// atmosphere march per consumer sample:
//   layer 0 = skyRadiance       (GI miss rays, the forward pass's skyRadiance(up) ambient)
//   layer 1 = mirrorSkyRadiance (ocean / terrain wet-film reflection rays: 12-step, saturation curve)
//   layer 2 = skyRadiance, CLEAR (the volumetric clouds' own ambient)
// Layers 0 and 1 carry the volumetric clouds: cloud_sky.cs.glsl marched them into u_skyClouds (the same
// texel grid) earlier in the frame, composited here as inScatter + sky * transmittance.
// The GI virtual sky probe (gi_probe_trace.cs.glsl projectSkySH) samples layer 0 too, so the
// out-of-field fallback and the miss rays agree by construction.

layout (binding = 1, rgba16f) uniform writeonly image2DArray u_skyMap;
layout (binding = 2) uniform sampler2D u_skyClouds;

layout(local_size_x = 8, local_size_y = 8) in;

void main()
{
    const ivec2 size = imageSize(u_skyMap).xy;
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, size)))
        return;
    const uint layer = gl_GlobalInvocationID.z;
    const vec3 dir = skyMapDir((vec2(xy) + 0.5) / vec2(size));
    vec3 radiance = layer == 1u ? mirrorSkyRadiance(dir) : skyRadiance(dir);
#ifdef CLOUDS
    if (layer != 2u && u_cloudShape0.w > 0.5)
    {
        const vec4 cloud = texelFetch(u_skyClouds, xy, 0);
        radiance = cloud.rgb + radiance * cloud.a;
    }
#endif
    imageStore(u_skyMap, ivec3(xy, int(layer)), vec4(radiance, 1.0));
}
