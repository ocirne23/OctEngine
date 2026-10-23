#version 450

// AERIAL PERSPECTIVE LUT (VolumetricFogPipeline, after the fog integrate): the sky's own atmosphere (atmosphere.inc.glsl)
// between the CENTRE view's camera and every point of its frustum, so distant terrain takes the blue of the sky behind
// it. vol_apply.fs.glsl lays it BEHIND the fog (the height fog sits near the camera; the air fills the distance).
//
// ONE WORKGROUP PER (x, y) COLUMN, one lane per slice: rgb = in-scatter (sun + sky-radiance light), a = the view ray's
// mean transmittance (the apply's blend is scalar). Slice k holds the value at its FAR edge, t = maxDist * ((k + 1) / Z)^2
// (quadratic: most slices where the haze changes fastest). Each step is independent (the view optical depth is closed
// form, atmosRayOD), so a slice is a PREFIX SUM: each lane marches its own substeps and a subgroup scan adds the slices
// in front (one thread per column is only 2304 threads: latency-bound). The workgroup is ONE subgroup: AERIAL_LUT_Z (32)
// = NVIDIA's warp, the minimum spec.
// The sun is cloud-shadowed, so overcast air does not light distant terrain blue. Matches sky.fs.glsl: observer at the
// camera altitude along u_skyUp, the same scatter boost and eclipse curve. No jitter: no temporal pass averages it.
// "Strength" (u_fog_aerialStrength) scales the air density along the VIEW ray only; the sun path is unscaled, as in the sky.

#extension GL_KHR_shader_subgroup_arithmetic : require

#include "shared.inc.glsl"

layout (binding = 1, rgba16f) uniform writeonly image3D u_outAerial;
#define CLOUD_SHADOW_BINDING 2
#include "cloud_shadow.inc.glsl"

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

    const vec3 up = u_skyUp;
    const vec3 L = u_sunDirection.xyz;
    const vec3 ro = up * (ATMOS_R_PLANET + max(u_viewPos.y, ATMOS_OBSERVE_HEIGHT));
    const AtmosRay ray = atmosRayBegin(ro, dir);

    const float muSun = dot(dir, L);
    const float pRSun = phaseRayleigh(muSun);
    const float pMSun = phaseHG(muSun, u_sky_mieG);
    const bool skyLight = dot(u_sky_radiance, u_sky_radiance) > 0.0;
    const float muSky = dot(dir, up);
    const float pRSky = phaseRayleigh(muSky);
    const float pMSky = phaseHG(muSky, u_sky_mieG);

    const float eclipse = u_sunVisible;
    const vec3 luminosity = vec3(0.2126, 0.7152, 0.0722);

    // This slice's substeps: per unit light, sum(attenuation * density * dt).
    vec3 sunR = vec3(0.0), sunM = vec3(0.0);
    vec3 skyR = vec3(0.0), skyM = vec3(0.0);
    const float sPrev = float(k) / float(AERIAL_LUT_Z), s = float(k + 1) / float(AERIAL_LUT_Z);
    const float tPrev = maxDist * sPrev * sPrev;
    const float tNext = maxDist * s * s;
    const float dt = (tNext - tPrev) / float(AERIAL_SUBSTEPS);
    for (int j = 0; j < AERIAL_SUBSTEPS; ++j)
    {
        const float t = tPrev + (float(j) + 0.5) * dt;
        const vec3 p = ro + dir * t;
        const float h = length(p) - ATMOS_R_PLANET;
        const vec2 dens = exp(-max(h, 0.0) / vec2(ATMOS_H_RAY, ATMOS_H_MIE)) * (dt * strength);
        const vec3 viewTau = atmosTau(atmosRayOD(ray, t)) * strength;
        const vec3 sunAtt = exp(-(viewTau + atmosTau(atmosLightOpticalDepth(p, L)))) * cloudSunTransmittanceBilinear(u_viewPos + dir * t);
        sunR += sunAtt * dens.x;
        sunM += sunAtt * dens.y;
        if (skyLight)
        {
            const vec3 skyAtt = exp(-(viewTau + atmosTau(atmosLightOpticalDepth(p, up))));
            skyR += skyAtt * dens.x;
            skyM += skyAtt * dens.y;
        }
    }

    // The slices in front, added: the scattered light from the camera to this slice's far edge.
    sunR = subgroupInclusiveAdd(sunR);
    sunM = subgroupInclusiveAdd(sunM);
    if (skyLight) // uniform: a UBO value
    {
        skyR = subgroupInclusiveAdd(skyR);
        skyM = subgroupInclusiveAdd(skyM);
    }

    vec3 color = (sunR * u_sky_betaRayleigh * pRSun + sunM * vec3(u_sky_betaMie) * pMSun) * (u_sky_scatterBoost) * u_sunColor.rgb;
    color = max(mix(vec3(dot(color, luminosity)), color, u_sky_saturation * (2.0 - eclipse)), vec3(0.0)) * eclipse; // sky.fs.glsl's curve
    if (skyLight)
        color += (skyR * u_sky_betaRayleigh * pRSky + skyM * vec3(u_sky_betaMie) * pMSky) * u_sky_scatterBoost * u_sky_radiance;
    const vec3 T = exp(-atmosTau(atmosRayOD(ray, tNext)) * strength);
    imageStore(u_outAerial, ivec3(cell, k), vec4(color, dot(T, vec3(1.0 / 3.0))));
}
