#version 450

// AERIAL PERSPECTIVE LUT (VolumetricFogPipeline, after the fog integrate): the sky's own atmosphere (atmosphere.inc.glsl)
// between the CENTRE view's camera and every point of its frustum, so distant terrain takes the blue of the sky behind
// it. vol_apply.fs.glsl lays it BEHIND the fog (the height fog sits near the camera; the air fills the distance).
//
// ONE WORKGROUP PER (x, y) COLUMN, one lane per slice: rgb = in-scatter (sun + sky-radiance light + the
// multiple-scattering light, atmosMultiScatter - under the clouds its sun part takes their mean shadow), a = the view ray's
// mean transmittance (the apply's blend is scalar). Slice k holds the value at its FAR edge, t = maxDist * ((k + 1) / Z)^2
// (quadratic: most slices where the haze changes fastest). Each step is independent (the view optical depth is closed
// form, atmosRayOD), so a slice is a PREFIX SUM: each lane marches its own substeps and a subgroup scan adds the slices
// in front (one thread per column is only 2304 threads: latency-bound). The workgroup is ONE subgroup: AERIAL_LUT_Z (32)
// = NVIDIA's warp, the minimum spec.
// The sun is cloud-shadowed, so overcast air does not light distant terrain blue, and terrain-shadowed. Matches sky.fs.glsl: observer at the
// camera altitude along u_skyUp, the same scatter boost and eclipse curve. No jitter: no temporal pass averages it.
// "Strength" (u_fog_aerialStrength) scales the air density along the VIEW ray only; the sun path is unscaled, as in the sky.

#extension GL_KHR_shader_subgroup_arithmetic : require

#include "shared.inc.glsl"

layout (binding = 1, rgba16f) uniform writeonly image3D u_outAerial;
#define CLOUD_SHADOW_BINDING 2
#include "cloud_shadow.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 3
#include "terrain_height.inc.glsl"

layout (local_size_x = AERIAL_LUT_Z, local_size_y = 1, local_size_z = 1) in;

const int AERIAL_SUBSTEPS = 2;

void main()
{
    const ivec2 cell = ivec2(gl_WorkGroupID.xy);
    const int k = int(gl_LocalInvocationID.x); // this lane's slice

    const float strength = u_fog_aerialStrength;
    const float maxDist = u_fog_aerialMaxDistance;

    // The view ray (ubo.inc.glsl's u_rayFromNdc has why).
    const vec2 uv = (vec2(cell) + 0.5) / vec2(AERIAL_LUT_X, AERIAL_LUT_Y);
    const vec3 dir = normalize(vec3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0) * u_rayFromNdc);

    const vec3 L = u_sunDirection.xyz;
    const float sPrev = float(k) / float(AERIAL_LUT_Z), s = float(k + 1) / float(AERIAL_LUT_Z);
    const float tPrev = maxDist * sPrev * sPrev;
    const float tNext = maxDist * s * s;
    const float dt = (tNext - tPrev) / float(AERIAL_SUBSTEPS);

    // THE TERRAIN'S SHADOW on the sun's part ("Terrain shadow"): ONE cone march of the height cascades per slice, from its
    // middle (50 m x 2^9: a 25.6 km reach). Without it the air in front of a mountain that hides the sun takes the haze's
    // whole glow. A sun under the horizon marches LEVEL: the planet's own shadow is the atmosphere's (atmosLightOpticalDepth).
    // REGISTERS: it comes FIRST, before the ray's atmosphere constants exist, and its result goes into the sun's two
    // phases: no value of its own across the steps.
    float terrainVis = 1.0;
    if (u_fog_aerialTerrainShadow > 0.5 && terrainHeightMapPresent())
        terrainVis = terrainSunVisibility(u_viewPos + dir * (0.5 * (tPrev + tNext)), vec3(L.x, max(L.y, 0.002), L.z), 50.0, 10, 0.02, 4.0);

    const vec3 up = u_skyUp;
    const vec3 ro = up * (ATMOS_R_PLANET + max(u_viewPos.y, ATMOS_OBSERVE_HEIGHT));
    const AtmosRay ray = atmosRayBegin(ro, dir);

    const float muSun = dot(dir, L);
    const float pRSun = phaseRayleigh(muSun) * terrainVis;
    const float pMSun = atmosHazePhase(muSun, ATMOS_HAZE_SUN) * terrainVis; // both lobes: terrain toward the sun takes the haze's glow
    const bool skyLight = dot(u_sky_radiance, u_sky_radiance) > 0.0;
    const float muSky = dot(dir, up);
    const float pRSky = phaseRayleigh(muSky);
    const float pMSky = atmosHazePhase(muSky, ATMOS_HAZE_DIFFUSE);

    const float eclipse = u_sunVisible;
    const vec3 luminosity = vec3(0.2126, 0.7152, 0.0722);

    // This slice's substeps. TWO sums (registers, and one subgroup scan each): the sun's part per unit sun colour (the
    // saturation curve and its clamp come after the scan), and the rest as a radiance - the sky-radiance light and the
    // multiple-scattering light (atmosMultiScatter: both lights in it).
    vec3 sun = vec3(0.0);
    vec3 rest = vec3(0.0);
    for (int j = 0; j < AERIAL_SUBSTEPS; ++j)
    {
        const float t = tPrev + (float(j) + 0.5) * dt;
        const vec3 p = ro + dir * t;
        const float h = max(length(p) - ATMOS_R_PLANET, 0.0);
        const vec2 dens = atmosDensity(h) * (dt * strength);
        const vec3 viewTau = atmosTau(atmosRayOD(ray, t)) * strength;
        sun += exp(-(viewTau + atmosTau(atmosLightOpticalDepth(p, L)))) * cloudSunTransmittanceBilinear(u_viewPos + dir * t)
             * (u_sky_betaRayleigh * (dens.x * pRSun) + vec3(u_sky_betaMie * dens.y * pMSun));
        rest += exp(-viewTau) * atmosScattering(dens) * atmosMultiScatter(h);
        // THE OZONE (a layer far above this LUT's range): on the lights' way in only. The sky-radiance light comes
        // straight DOWN: through the whole layer, then the column above the sample (atmosZenithOpticalDepth - no Chapman
        // evaluation; with one the shader took 63 registers, without 52). The sun's ozone is after the scan.
        // REGISTERS: this order is measured. The sun's optical depth first: 54; the cloud shadow first: 53; the optical
        // depths added per species before atmosTau: 53 - 55.
        if (skyLight)
            rest += exp(-(viewTau + atmosTau(atmosZenithOpticalDepth(h))))
                  * (u_sky_betaRayleigh * (dens.x * pRSky) + vec3(u_sky_betaMie * dens.y * pMSky)) * (u_sky_scatterBoost * u_sky_radiance * atmosOzoneZenith());
    }

    // The slices in front, added: the scattered light from the camera to this slice's far edge.
    sun = subgroupInclusiveAdd(sun);
    rest = subgroupInclusiveAdd(rest);

    // The sun's ozone, ONE length for the whole column (the camera's): over the LUT's range the sun ray's way through a
    // layer 15 km up or more changes by under a percent.
    vec3 color = sun * (u_sky_scatterBoost * u_sunColor.rgb) * exp(-atmosOzoneTau(atmosOzonePath(ro, L)));
    color = max(mix(vec3(dot(color, luminosity)), color, u_sky_saturation * (2.0 - eclipse)), vec3(0.0)) * eclipse; // sky.fs.glsl's curve
    color += rest; // past the saturation, as the sky
    const vec3 T = exp(-atmosTau(atmosRayOD(ray, tNext)) * strength);
    imageStore(u_outAerial, ivec3(cell, k), vec4(color, dot(T, vec3(1.0 / 3.0))));
}
