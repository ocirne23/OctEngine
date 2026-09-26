#version 460

// The clouds in the GI SKY MAP: one invocation per sky-map texel (the same lat-long mapping, atmosphere.inc.glsl
// skyMapDir), marched with the screen march's own function (cloud_raymarch.inc.glsl) and written as
// (in-scatter, transmittance). gi_sky_map.cs.glsl composites it over its GI and mirror layers, so the ocean /
// wet-film reflections, the GI miss rays and the sky SH all see the clouds.
// THE OBSERVER stands on the ground (ATMOS_OBSERVE_HEIGHT, like the sky map's own atmosphere) straight under
// the camera: every reader of the sky map sits under the clouds - the ocean surface, the ground the GI
// lights - even while the camera flies above them. Below the horizon: no cloud (the sky map's ground).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"
#define CLOUD_NOISE_BINDING 4
#include "clouds.inc.glsl"
#define CLOUD_SHADOW_BINDING 3
#include "cloud_shadow.inc.glsl"

layout (binding = 1, rgba16f) uniform writeonly image2D u_outSkyClouds;
layout (binding = 2) uniform sampler2DArray u_skyMap; // LAST frame's bake: its clear layer is the ambient

#include "cloud_raymarch.inc.glsl"

const int SKY_CLOUD_STEPS = 64;

void main()
{
    const ivec2 size = imageSize(u_outSkyClouds);
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, size)))
        return;
    const vec3 dir = skyMapDir((vec2(xy) + 0.5) / vec2(size));
    if (dir.y <= 0.0 || u_cloudShape0.w < 0.5)
    {
        imageStore(u_outSkyClouds, xy, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    const float camAlt = u_viewPos.y; // centre view (g_viewIndex 0)
    const vec3 origin = vec3(0.0, ATMOS_OBSERVE_HEIGHT - camAlt, 0.0);
    vec2 seg0, seg1;
    cloudShellIntervals(ATMOS_OBSERVE_HEIGHT, cloudRayB(origin, dir, camAlt), u_cloudMarch0.y, seg0, seg1);
    // One texel spans PI / height radians: the mip level from that footprint.
    const CloudMarchResult r = cloudRaymarch(origin, dir, seg0, seg1, SKY_CLOUD_STEPS, 0.5, PI / float(size.y), false);
    imageStore(u_outSkyClouds, xy, vec4(r.inScatter, r.transmittance));
}
