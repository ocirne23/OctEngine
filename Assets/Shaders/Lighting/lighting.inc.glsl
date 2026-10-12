// Diffuse-only direct lighting for GI ray-trace hits: the forward pass' light grid plus the sun, so a bounce is lit
// like the primary surfaces. No specular (probes store diffuse irradiance only).
// Requires the includer to have declared / included, with these names:
//   - struct LightInfo + in_lightInfos[]
//   - the light grid buffers + #include "light_grid.inc.glsl"
//   - the UBO, PI (shared.inc.glsl)
//   - u_shadowMap (sampler2DArrayShadow)

#ifndef LIGHTING_INC_GLSL
#define LIGHTING_INC_GLSL

float giSquareFalloff(float dist, float lightRadius)
{
    float attenuation = 1.0 / (dist * dist + 1.0);
    float dr = dist / lightRadius;
    float dr2 = dr * dr;
    float falloff = clamp(1.0 - dr2 * dr2, 0.0, 1.0);
    falloff *= falloff;
    return attenuation * falloff;
}

// Hard single-tap cascaded sun shadow: shadows.inc.glsl's PCSS without its gl_FragCoord dither (unavailable in
// compute); good enough for diffuse GI.
float giSunShadow(vec3 worldPos, vec3 N)
{
    float dist = length(worldPos - u_sceneFocus.xyz); // same pick as shadows.inc.glsl's getSunCascade
    int cascade = NUM_SHADOW_CASCADES - 1;
    for (int i = 0; i < NUM_SHADOW_CASCADES; ++i)
    {
        if (dist < u_cascadeViewProj[i][0][3]) { cascade = i; break; } // per-cascade far stashed in m[0][3]
    }

    mat4 m = u_cascadeViewProj[cascade];
    m[0][3] = 0.0; m[1][3] = 0.0; m[2][3] = 0.0; m[3][3] = 1.0; // restore canonical bottom row
    vec4 lp = m * vec4(worldPos + N * 0.1, 1.0);
    vec3 proj = lp.xyz / lp.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || proj.z > 1.0)
        return 1.0;
    return texture(u_shadowMap, vec4(uv, float(cascade), proj.z - 0.0015));
}

// Incoming irradiance (radiance * NdotL) from one grid light. The type has the forward pass' doLight() encoding
// (punctual_lights.inc.glsl): width < 0 = spot, width > 0 && range < 0 = tube, width > 0 = rect area, width == 0 =
// point. Every light is a point at its centre for the falloff, but the spot cone and the rect's one-sided emission
// are kept, to match the primary surfaces.
vec3 giLightIrradiance(LightInfo light, vec3 pos, vec3 N)
{
    vec3 toLight = light.pos - pos;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-4);
    float NdotL = dot(N, L);
    if (NdotL <= 0.0)
        return vec3(0.0);

    float shape = 1.0; // spot cone / rect facing factor; the falloff is applied after the early-outs
    if (light.width < 0.0) // spot light: rotation = cone half-angle, |direction| = edge softness
    {
        float softness = length(light.direction);
        float cosAngle = dot(-L, light.direction / softness); // CPU: spot |direction| >= 0.001
        float cosOuter = cos(light.rotation);
        float cosInner = mix(cosOuter, 1.0, softness);
        shape = smoothstep(cosOuter, cosInner, cosAngle);
    }
    else if (light.width > 0.0 && light.range >= 0.0) // rect area light: one-sided, emits along its normal
    {
        // The quad's normal exactly as doAreaLight() builds it; only points in front of the quad receive light.
        float height = length(light.direction);
        vec3 up     = light.direction / height;
        vec3 ref    = abs(up.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 right0 = normalize(cross(up, ref));
        vec3 right  = right0 * cos(light.rotation) + cross(up, right0) * sin(light.rotation);
        vec3 quadNormal = cross(up, right);
        float facing = dot(quadNormal, -L); // -L points from the light toward the surface
        if (facing <= 0.0)
            return vec3(0.0);
        shape = facing;
    }

    return light.color * (giSquareFalloff(dist, abs(light.range)) * shape * NdotL);
}

// One grid light's irradiance with its ray-traced shadow. GI_LIGHT_RT_SHADOWS: the includer has rt_shadow.inc.glsl in
// scope BEFORE this file (the GI probe trace); without it the light is unshadowed (vol_scatter). One ray to the light's
// CENTRE: the bounce is low-frequency and temporally blended. Unshadowed, a lamp lights every hit in its range THROUGH
// walls - a leak the probe visibility test cannot see. Traces only past GI_LIGHT_SHADOW_MIN: a cell lists every light
// whose RANGE touches it, and most of those give next to nothing at the hit.
#ifndef GI_LIGHT_SHADOW_MIN
#define GI_LIGHT_SHADOW_MIN 0.002
#endif

vec3 giLightIrradianceShadowed(LightInfo light, vec3 pos, vec3 N)
{
    vec3 e = giLightIrradiance(light, pos, N);
#ifdef GI_LIGHT_RT_SHADOWS
    if (u_rt_lightShadows > 0.5 && max(max(e.r, e.g), e.b) > GI_LIGHT_SHADOW_MIN)
    {
        const vec3  toLight = light.pos - pos;
        const float dist    = length(toLight);
        if (dist > 0.05)
            e *= rtShadowVisibility(pos + N * 0.02, toLight / dist, 0.01, dist - 0.02);
    }
#endif
    return e;
}

// >= 0: giGatherDirect uses this sun visibility instead of the cascades. The GI probe trace sets a ray-traced
// per-probe value, so off-screen probes are shadowed too (the cascades only cover the view frustum).
float g_sunShadowOverride = -1.0;

// Output: the SUN's part of the last giGatherDirect's result (albedo / PI included). The GI trace keeps it apart as the
// probe's sun fraction (GI_SUN_V4 in gi_probe.inc.glsl); dead code for everyone else.
vec3 g_gatherSunRadiance = vec3(0.0);

// Full diffuse direct lighting at a world position: sun (shadowed) + all grid lights, times albedo/PI.
vec3 giGatherDirect(vec3 pos, vec3 N, vec3 albedo)
{
    vec3 E = vec3(0.0);
    g_gatherSunRadiance = vec3(0.0);

    vec3 sunL = u_sunDirection.xyz;
    float sunNdotL = dot(N, sunL);
    if (sunNdotL > 0.0)
    {
        float sunShadow = (g_sunShadowOverride >= 0.0) ? g_sunShadowOverride : giSunShadow(pos, N);
        // The forward pass' atmospheric attenuation (doSunLight), so the bounce dims with the direct light at low sun.
        E += u_sunTransmittance * u_sunColor.rgb * sunShadow * sunNdotL * u_sunVisible;
        g_gatherSunRadiance = albedo * (E / PI);
    }

    // No sky light here: gi_probe_trace.cs.glsl injects it into the probe SH and its bounces arrive through the prevE
    // feedback - adding it at the hits would count it twice.

    const ivec3 gridPos = getGridPos(pos);
    uint tableIdx = getTableIdx(gridPos);
    while (true)
    {
        const uint gridIdx = getGridIdx(tableIdx);
        if (gridIdx == EMPTY_ENTRY)
            break;
        const ivec3 gridMin = getGridMin(gridIdx);
        if (gridMin == gridPos)
        {
            const uint numLargeLights = getLargeLightCount(gridIdx);
            // The counts are written capped; the mins stay as the loops' compile-time bound: without them gi_probe_trace
            // (which inlines this loop) needs 125 instead of 96 registers.
            for (uint i = 0; i < min(numLargeLights, MAX_LARGE_LIGHTS_PER_GRID); ++i)
                E += giLightIrradianceShadowed(in_lightInfos[getLargeLightId(gridIdx, i)], pos, N);

            const uint cellOffset = calcCellOffset(gridIdx, gridMin, pos);
            const uint numLights  = getNumLightsForCell(cellOffset);
            for (uint i = 0; i < min(numLights, MAX_LIGHTCELL_LIGHTS); ++i)
                E += giLightIrradianceShadowed(in_lightInfos[getLightId(cellOffset, i)], pos, N);
            break;
        }
        tableIdx = getNextTableIdx(tableIdx);
    }

    return albedo * (E / PI);
}

#endif
