// Diffuse-only direct lighting used to shade GI ray-trace hits. It reuses the exact same world-space
// light grid as the main forward pass (light_grid.inc.glsl accessors) plus the sun, so an indirect
// bounce is lit consistently with primary surfaces. Reflections/specular are intentionally dropped
// (probes only store diffuse irradiance).
//
// Requires the includer to have declared/included, with these names:
//   - struct LightInfo + in_lightInfos[]              (the light list)
//   - the light grid buffers + #include "light_grid.inc.glsl"
//   - UBO
//   - u_shadowMap (sampler2DArrayShadow)
//   - PI (shared.inc.glsl)

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

// Hard single-tap cascaded sun shadow. A trimmed version of shadows.inc.glsl's PCSS that avoids its
// gl_FragCoord-based dither (unavailable in compute); cheap and leak-free enough for diffuse GI.
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

// Incoming irradiance (radiance * NdotL, colored) from one grid light. The light type is selected with
// the same width/range encoding as the forward pass' doLight() (instanced_indirect.fs.glsl): width < 0 =>
// spot, width > 0 && range < 0 => tube, width > 0 => rect area, width == 0 => point. Lights are treated as
// a point at their center for the falloff/distance (low-frequency diffuse only), but the cone (spot) and
// one-sided forward emission (rect area) are honored so they match the primary surfaces.
vec3 giLightIrradiance(LightInfo light, vec3 pos, vec3 N)
{
    vec3 toLight = light.pos - pos;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-4);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return vec3(0.0);

    vec3 rad = light.color * giSquareFalloff(dist, abs(light.range));
    if (light.width < 0.0) // spot light: rotation = cone half-angle, |direction| = edge softness
    {
        float softness = length(light.direction);
        float cosAngle = dot(-L, light.direction / max(softness, 1e-4));
        float cosOuter = cos(light.rotation);
        float cosInner = mix(cosOuter, 1.0, softness);
        rad *= smoothstep(cosOuter, cosInner, cosAngle);
    }
    else if (light.width > 0.0 && light.range >= 0.0) // rect area light: one-sided, emits along its normal
    {
        // Reconstruct the quad's facing normal exactly as doAreaLight() does: up = normalize(direction),
        // right is rotated about up by light.rotation, normal = cross(up, right). Only fragments in front
        // of the quad receive light, faded by how directly the quad faces them.
        float height = length(light.direction);
        vec3 up     = light.direction / height;
        vec3 ref    = abs(up.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 right0 = normalize(cross(up, ref));
        vec3 right  = right0 * cos(light.rotation) + cross(up, right0) * sin(light.rotation);
        vec3 quadNormal = cross(up, right);
        float facing = dot(quadNormal, -L); // -L points from the light toward the surface
        if (facing <= 0.0)
            return vec3(0.0);
        rad *= facing;
    }
    return rad * NdotL;
}

// Optional sun-shadow override: when >= 0, giGatherDirect uses this value instead of the cascade shadow
// map (giSunShadow). The GI probe trace sets it to a view-independent, ray-traced per-probe visibility so
// off-screen probes are shadowed correctly (the camera shadow maps only cover the view frustum). The
// fragment shader never sets it, so it keeps using giSunShadow.
float g_sunShadowOverride = -1.0;

// Full diffuse direct lighting at a world position: sun (shadowed) + all grid lights, times albedo/PI.
vec3 giGatherDirect(vec3 pos, vec3 N, vec3 albedo)
{
    vec3 E = vec3(0.0);

    vec3 sunL = normalize(u_sunDirection.xyz);
    float sunNdotL = max(dot(N, sunL), 0.0);
    if (sunNdotL > 0.0)
    {
        float sunShadow = (g_sunShadowOverride >= 0.0) ? g_sunShadowOverride : giSunShadow(pos, N);
        // Same atmospheric attenuation as the forward pass' doSunLight - without it, probe-lit bounce
        // light stays at full space-sun strength at low sun angles while direct light (and the
        // out-of-field skyGroundRadiance fallback) dims with transmittance. u_sunTransmittance = the
        // Chapman evaluation, done ONCE per frame on the CPU (this ran per gather hit before).
        E += u_sunTransmittance * u_sunColor.rgb * sunShadow * sunNdotL * u_eclipseParams.x;
    }

    // The sky radiance light is intentionally absent here: it is injected directly into the probe SH
    // (gi_probe_trace.cs.glsl), and its bounces arrive through the multi-bounce prevE feedback - adding
    // it at hit points too would double-count.

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
            for (uint i = 0; i < min(numLargeLights, MAX_LARGE_LIGHTS_PER_GRID); ++i)
                E += giLightIrradiance(in_lightInfos[getLargeLightId(gridIdx, i)], pos, N);

            const uint cellOffset = calcCellOffset(gridIdx, gridMin, pos);
            const uint numLights  = getNumLightsForCell(cellOffset);
            for (uint i = 0; i < min(numLights, MAX_LIGHTCELL_LIGHTS); ++i)
                E += giLightIrradiance(in_lightInfos[getLightId(cellOffset, i)], pos, N);
            break;
        }
        tableIdx = getNextTableIdx(tableIdx);
    }

    return albedo * (E / PI);
}

#endif
