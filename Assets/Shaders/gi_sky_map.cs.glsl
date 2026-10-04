#version 450

#include "shared.inc.glsl"

// THE SKY MAP: the per-frame lat-long bake of the analytic sky (mapping + layer ids in
// atmosphere.inc.glsl: skyMapUV / skyMapDir, SKY_MAP_LAYER_*). One 8x8-workgroup dispatch replaces an
// atmosphere march per consumer sample:
//   layer 0 = skyRadiance       LOW-RES, the SKY_MAP_GI_WIDTH x SKY_MAP_GI_HEIGHT corner only (read with
//                               skyMapGISample): the sky SH's projection (gi_probe_trace.cs.glsl projectSkySH)
//   layer 1 = mirrorSkyRadiance (ocean / terrain wet-film reflection rays: 12-step, saturation curve), full res,
//             its clear sky from the layer-3 cache (re-marched 1/4 per frame)
//   layer 2 = per-frame constants, one texel each (atmosphere.inc.glsl SKY_MAP_*_TEXEL)
//   layer 3 = the clear mirror sky cache (SKY_MAP_LAYER_MIRROR_CACHE)
// Layers 0 and 1 carry the volumetric clouds: cloud_sky.cs.glsl marched them into u_skyClouds (the same
// texel grids) earlier in the frame, composited here as inScatter + sky * transmittance - layer 0 from the
// observer-averaged clouds, layer 1 from the clouds above the camera.

layout (binding = 1, rgba16f) uniform image2DArray u_skyMap; // read-write: the clear mirror sky cache (layer 3)
layout (binding = 2) uniform sampler2DArray u_skyClouds; // layer 0 = from under the camera, 1 = averaged observers (low-res corner)

layout(local_size_x = 8, local_size_y = 8) in;

// The GI layer's radiance in any direction, as main composites it (the observer-averaged clouds, bilinear).
vec3 giLayerRadiance(vec3 dir)
{
    vec3 radiance = skyRadiance(dir);
#ifdef CLOUDS
    if (u_cloudShape0.w > 0.5)
    {
        const vec4 cloud = skyMapGISample(u_skyClouds, dir, 1);
        radiance = cloud.rgb + radiance * cloud.a;
    }
#endif
    return radiance;
}

void main()
{
    const uint layer = gl_GlobalInvocationID.z;
    // The GI layer has its own low-res grid in the corner; the rest of the dispatch's layer-0 threads exit.
    const ivec2 size = layer == 0u ? ivec2(SKY_MAP_GI_WIDTH, SKY_MAP_GI_HEIGHT) : imageSize(u_skyMap).xy;
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, size)))
        return;
    // THE CLEAR LAYER HOLDS PER-FRAME CONSTANTS (atmosphere.inc.glsl SKY_MAP_*_TEXEL): each was a few fixed-direction
    // fetches in its reader (per ray for the clouds and the far trees), and the clouds' ambient a whole skyRadiance
    // layer for 5 values. Not dispatched as a layer (z covers 0 and 1). The GI values come from the clouds image,
    // not from layer 0, which this same dispatch is writing.
    if (all(equal(gl_GlobalInvocationID, uvec3(0u))))
    {
        const vec3 amb = skyRadiance(vec3(0.0, 1.0, 0.0)) + skyRadiance(vec3(0.866, 0.5, 0.0)) + skyRadiance(vec3(-0.866, 0.5, 0.0))
                       + skyRadiance(vec3(0.0, 0.5, 0.866)) + skyRadiance(vec3(0.0, 0.5, -0.866));
        imageStore(u_skyMap, SKY_MAP_CLOUD_AMBIENT_TEXEL, vec4(amb * 0.2, 1.0));
        const float ringS = 0.70710678;
        vec3 treeSky = 0.4 * giLayerRadiance(vec3(0.0, 1.0, 0.0));
        treeSky += 0.15 * (giLayerRadiance(vec3(ringS, ringS, 0.0)) + giLayerRadiance(vec3(-ringS, ringS, 0.0))
                         + giLayerRadiance(vec3(0.0, ringS, ringS)) + giLayerRadiance(vec3(0.0, ringS, -ringS)));
        imageStore(u_skyMap, SKY_MAP_TREE_SKY_TEXEL, vec4(treeSky, 1.0));
        imageStore(u_skyMap, SKY_MAP_GI_ZENITH_TEXEL, vec4(giLayerRadiance(normalize(u_skyUp)), 1.0));
    }
    const vec3 dir = skyMapDir((vec2(xy) + 0.5) / vec2(size));
    vec3 radiance;
    if (layer == 1u)
    {
        // PROGRESSIVE: the clear mirror sky (12-step) is re-marched for one texel of every 2x2 block per frame, the
        // phase rotating like the sky clouds'; the other three composite their cached value (written by this same
        // thread in an earlier frame - no hazard inside the dispatch). A moving sun lags by up to 3 frames.
        const ivec3 cacheTexel = ivec3(xy, SKY_MAP_LAYER_MIRROR_CACHE);
        const uint p = u_frameIndex & 3u;
        const bool due = (xy.x & 1) == int((p & 1u) ^ ((p >> 1u) & 1u)) && (xy.y & 1) == int(p & 1u);
        const vec4 cached = due ? vec4(0.0) : imageLoad(u_skyMap, cacheTexel);
        if (cached.a > 0.5)
            radiance = cached.rgb;
        else
        {
            radiance = mirrorSkyRadiance(dir);
            imageStore(u_skyMap, cacheTexel, vec4(radiance, 1.0));
        }
    }
    else
        radiance = skyRadiance(dir);
#ifdef CLOUDS
    if (u_cloudShape0.w > 0.5)
    {
        // The GI layer takes the clouds AVERAGED over observers around the camera (cloud_sky.cs.glsl layer 1, on the
        // same low-res grid): it lights the whole world, so it must not carry the camera's own cloud shadow. The
        // mirror layer keeps the clouds seen from under the camera (layer 0).
        const vec4 cloud = texelFetch(u_skyClouds, ivec3(xy, layer == 0u ? 1 : 0), 0);
        radiance = cloud.rgb + radiance * cloud.a;
    }
#endif
    imageStore(u_skyMap, ivec3(xy, int(layer)), vec4(radiance, 1.0));
}
