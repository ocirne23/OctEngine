#version 460

// The clouds in the GI SKY MAP: one invocation per sky-map texel (the same lat-long mapping, atmosphere.inc.glsl
// skyMapDir), marched with the screen march's own function (cloud_raymarch.inc.glsl) and written as
// (in-scatter, transmittance). gi_sky_map.cs.glsl composites it over its GI and mirror layers, so the ocean /
// wet-film reflections, the GI miss rays and the sky SH all see the clouds.
// THE OBSERVER stands on the ground (ATMOS_OBSERVE_HEIGHT, like the sky map's own atmosphere) straight under
// the camera: every reader of the sky map sits under the clouds - the ocean surface, the ground the GI
// lights - even while the camera flies above them. Below the horizon: no cloud (the sky map's ground).
// TWO LAYERS (z = the dispatch's z): 0 = from the ground under the camera, for the MIRROR sky (reflections show the
// clouds really above). 1 = the GI sky: each march from a different observer in a disc around the camera ("GI sky
// observer radius", u_clouds_giSkyObserverRadius), its history ("GI sky history", u_cloudsLive_giSkyHistory) the AVERAGE over them.
// From one observer, the cloud overhead and the cloud in front of the sun ARE that observer's cloud shadow, and
// every GI-layer reader (sky SH -> fog / ocean / fallback ambient, far trees, GI misses) lit the whole world with
// it: fog under a cloud brightened when the camera moved into the sun. Layer 1 is LOW-RES: a 64x32 grid in the
// image's corner (atmosphere.inc.glsl SKY_MAP_GI_*, read with skyMapGISample).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"
#define CLOUD_NOISE_BINDING 4
#include "clouds.inc.glsl"
#define CLOUD_SHADOW_BINDING 3
#include "cloud_shadow.inc.glsl"

// Read-write: the texel's own last value is the history (below). Cleared to "no cloud" at creation.
layout (binding = 1, rgba16f) uniform image2DArray u_outSkyClouds;
layout (binding = 2) uniform sampler2DArray u_skyMap; // LAST frame's bake: its clear layer is the ambient

#include "cloud_raymarch.inc.glsl"

const int SKY_CLOUD_STEPS = 64;
// TEMPORAL: the march is far too coarse for the cloud detail near the horizon (the steps grow to kilometres
// there, and the ocean reflects mostly those directions), and its samples sit relative to the camera. So a moving
// camera slid them through the noise field and the whole reflected sky changed colour every frame. Now each frame
// marches with a new jitter and blends into the texel's history: the average of many jittered marches, which
// changes smoothly with the camera. The history weight is frame-time based (u_cloudsLive_skyHistory = exp(-3 dt / T),
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

// The GI layer's observer for this march: a point of the disc, low-discrepancy over the texel's marches (R2, per-texel
// offset), so the history averages the disc evenly instead of by white-noise luck. sqrt(u): uniform over the AREA.
vec2 skyCloudObserver(ivec2 p)
{
    uint h = uint(p.x) * 2654435761u ^ uint(p.y) * 2246822519u;
    h = (h ^ (h >> 15u)) * 0x846ca68bu;
    h ^= h >> 16u;
    const float n = float(u_frameIndex >> 2u); // the texel's march count (one march per SKY_UPDATE_FRAMES frames)
    const float u = fract(float(h & 0xFFFFu) * (1.0 / 65536.0) + n * 0.7548776662);
    const float v = fract(float(h >> 16u) * (1.0 / 65536.0) + n * 0.5698402910);
    const float a = v * 2.0 * PI;
    return vec2(cos(a), sin(a)) * (sqrt(u) * u_clouds_giSkyObserverRadius);
}

void main()
{
    const int layer = int(gl_GlobalInvocationID.z);
    // The GI layer has its own low-res grid in the image's corner (atmosphere.inc.glsl SKY_MAP_GI_WIDTH): its
    // readers reduce it to L1, so 1/16 of the texels march; the rest of the dispatch's layer-1 threads exit here.
    const ivec2 size = layer == 1 ? ivec2(SKY_MAP_GI_WIDTH, SKY_MAP_GI_HEIGHT) : imageSize(u_outSkyClouds).xy;
    const ivec2 xy = skyCloudTexel();
    // The full layer marches the upper hemisphere only (its lower half stays "no cloud" from the creation clear);
    // the GI corner also rewrites its lower half below as "no cloud" (cheap), so a shader reload cannot leave the
    // old full-res layer's texels there.
    if (xy.x >= size.x || xy.y >= (layer == 1 ? size.y : size.y / 2))
        return;
    const ivec3 texel = ivec3(xy, layer);
    const vec3 dir = skyMapDir((vec2(xy) + 0.5) / vec2(size));
    if (dir.y <= 0.0 || u_cloudsLive_enabled < 0.5)
    {
        imageStore(u_outSkyClouds, texel, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    const float camAlt = u_viewPos.y; // centre view (g_viewIndex 0)
    const vec2 offset = layer == 1 ? skyCloudObserver(xy) : vec2(0.0);
    const vec3 origin = vec3(offset.x, ATMOS_OBSERVE_HEIGHT - camAlt, offset.y);
    vec2 seg0, seg1;
    cloudShellIntervals(cloudAltitude(origin, camAlt), cloudRayB(origin, dir, camAlt), u_clouds_maxDistance, seg0, seg1);
    // One texel spans PI / height radians: the mip level from that footprint.
    const CloudMarchResult r = cloudRaymarch(origin, dir, seg0, seg1, SKY_CLOUD_STEPS, skyCloudJitter(xy + layer * 7919), PI / float(size.y), false);
    const vec4 history = imageLoad(u_outSkyClouds, texel);
    imageStore(u_outSkyClouds, texel, mix(vec4(r.inScatter, r.transmittance), history, layer == 1 ? u_cloudsLive_giSkyHistory : u_cloudsLive_skyHistory));
}
