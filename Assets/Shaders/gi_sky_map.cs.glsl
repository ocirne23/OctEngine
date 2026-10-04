#version 450

#include "shared.inc.glsl"

// THE SKY MAP: the per-frame lat-long bake of the analytic sky (mapping + layer ids in
// atmosphere.inc.glsl: skyMapUV / skyMapDir, SKY_MAP_LAYER_*). One 8x8-workgroup dispatch replaces an
// atmosphere march per consumer sample:
//   layer 0 = skyRadiance       (GI miss rays, the forward pass's skyRadiance(up) ambient)
//   layer 1 = mirrorSkyRadiance (ocean / terrain wet-film reflection rays: 12-step, saturation curve)
//   layer 2 = texel (0, 0) only: the volumetric clouds' own ambient (skyRadiance without clouds, 5 directions)
// Layers 0 and 1 carry the volumetric clouds: cloud_sky.cs.glsl marched them into u_skyClouds (the same
// texel grid) earlier in the frame, composited here as inScatter + sky * transmittance - layer 0 from the
// observer-averaged clouds, layer 1 from the clouds above the camera.
// The GI virtual sky probe (gi_probe_trace.cs.glsl projectSkySH) samples layer 0 too, so the
// out-of-field fallback and the miss rays agree by construction.

layout (binding = 1, rgba16f) uniform writeonly image2DArray u_skyMap;
layout (binding = 2) uniform sampler2DArray u_skyClouds; // layer 0 = from under the camera, 1 = averaged observers

layout(local_size_x = 8, local_size_y = 8) in;

// The GI layer's radiance in any direction, as main composites it (the observer-averaged clouds, bilinear).
vec3 giLayerRadiance(vec3 dir)
{
    vec3 radiance = skyRadiance(dir);
#ifdef CLOUDS
    if (u_cloudShape0.w > 0.5)
    {
        const vec4 cloud = textureLod(u_skyClouds, vec3(skyMapUV(dir), 1.0), 0.0);
        radiance = cloud.rgb + radiance * cloud.a;
    }
#endif
    return radiance;
}

void main()
{
    const ivec2 size = imageSize(u_skyMap).xy;
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, size)))
        return;
    const uint layer = gl_GlobalInvocationID.z;
    // THE CLEAR LAYER HOLDS PER-FRAME CONSTANTS (atmosphere.inc.glsl SKY_MAP_*_TEXEL): each was 5 fixed-direction
    // fetches per ray in its reader, and the clouds' ambient a whole skyRadiance layer for 5 values. Not dispatched
    // as a layer (z covers 0 and 1).
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
    }
    const vec3 dir = skyMapDir((vec2(xy) + 0.5) / vec2(size));
    vec3 radiance = layer == 1u ? mirrorSkyRadiance(dir) : skyRadiance(dir);
#ifdef CLOUDS
    if (u_cloudShape0.w > 0.5)
    {
        // The GI layer takes the clouds AVERAGED over observers around the camera (cloud_sky.cs.glsl layer 1): it
        // lights the whole world, so it must not carry the camera's own cloud shadow. The mirror layer keeps the
        // clouds seen from under the camera (layer 0).
        const vec4 cloud = texelFetch(u_skyClouds, ivec3(xy, layer == 0u ? 1 : 0), 0);
        radiance = cloud.rgb + radiance * cloud.a;
    }
#endif
    imageStore(u_skyMap, ivec3(xy, int(layer)), vec4(radiance, 1.0));
}
