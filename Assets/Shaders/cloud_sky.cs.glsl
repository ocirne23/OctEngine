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

// Read-write: the texel's own last value is the history (below). Cleared to "no cloud" at creation.
layout (binding = 1, rgba16f) uniform image2D u_outSkyClouds;
layout (binding = 2) uniform sampler2DArray u_skyMap; // LAST frame's bake: its clear layer is the ambient

#include "cloud_raymarch.inc.glsl"

const int SKY_CLOUD_STEPS = 64;
// TEMPORAL: the march is far too coarse for the cloud detail near the horizon (the steps grow to kilometres
// there, and the ocean reflects mostly those directions), and its samples sit relative to the camera. So a moving
// camera slid them through the noise field and the whole reflected sky changed colour every frame. Now each frame
// marches with a new jitter and blends into the texel's history: the average of many jittered marches, which
// changes smoothly with the camera. The history weight is frame-time based (u_cloudShape4.z = exp(-3 dt / T),
// "Sky/Clouds/Quality/Sky map history (s)" = T): 95 % of a change after T seconds at any frame rate.
// PROGRESSIVE: with a seconds-long history a texel need not march every frame. Each frame marches ONE texel of
// every 2x2 block (the phase rotates with the frame, the shadow map's order), and the CPU's weight counts the 4
// frames since that texel's last march. And only the UPPER hemisphere is dispatched (rows [0, H/2): v =
// acos(y) / pi): the lower half stays "no cloud" from the clear at creation.

ivec2 skyCloudTexel()
{
    const uint p = u_frameIndex & 3u;
    return ivec2(gl_GlobalInvocationID.xy) * 2 + ivec2(int((p & 1u) ^ ((p >> 1u) & 1u)), int(p & 1u));
}

float skyCloudJitter(ivec2 p)
{
    uint h = uint(p.x) * 1597334673u ^ uint(p.y) * 3812015801u;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h ^= h >> 15u;
    return fract(float(h) * (1.0 / 4294967296.0) + float(u_frameIndex & 1023u) * 0.61803398875);
}

void main()
{
    const ivec2 size = imageSize(u_outSkyClouds);
    const ivec2 xy = skyCloudTexel();
    if (xy.x >= size.x || xy.y >= size.y / 2)
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
    const CloudMarchResult r = cloudRaymarch(origin, dir, seg0, seg1, SKY_CLOUD_STEPS, skyCloudJitter(xy), PI / float(size.y), false);
    const vec4 history = imageLoad(u_outSkyClouds, xy);
    imageStore(u_outSkyClouds, xy, mix(vec4(r.inScatter, r.transmittance), history, u_cloudShape4.z));
}
