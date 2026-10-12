#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable // the BRDF colour side is half (punctual_lights.inc.glsl)
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// River / lake water (EPipelineIndex::River; Procedural RiverSystem's ribbons and lake surfaces): the ocean's shading without
// its waves, the scene side shared through water_scene.inc.glsl. Normal = the river waves (river_wave.inc.glsl) + a flow-mapped
// noise ripple. Fresnel (F0 0.02) splits the RT refraction to the bed (Beer-Lambert both ways) and the RT mirror (sky fallback);
// plus the GGX sun glint and the scene lights. The RT toggles and ranges are the ocean's (OCEAN_RT_*, u_ocean_rt*).

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"
#define RIVER_WAVE_TEX(uvLayer) texture(u_oceanMaps, uvLayer)
#include "river_wave.inc.glsl"

#define WATER_ABSORPTION u_river_absorption
#include "water_scene.inc.glsl" // the bindings, the BRDF terms, the sky lookups, traceScene / shadeHit

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;          // x = across a ribbon (-1 .. 1), 2 = a lake; y = along the river (m)
layout (location = 2) in vec3 in_flow;        // xz = the flow (m/s), y = the whitewater amount (rapids, falls)
layout (location = 3) in flat float in_dense; // 1 = a dense near-cell ribbon
layout (location = 4) in float in_waveSize;   // the waves' scale with the river's size (river.vs.glsl)
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// DUAL-SOURCE composite: out = out_color + scene * out_factor, the alpha too; opaque except over the edge fade.
// Alpha 0 = TAA's animated-surface flag (the ripples move without motion vectors), as the ocean.
layout (location = 0, index = 0) out vec4 out_color;
layout (location = 0, index = 1) out vec4 out_factor;

// The NEAR data cascade's weight at xz (terrainDataAt's handover band): only it holds the carved ground. The far cascade is
// the generator's coarse stage, uncarved and smoothed, and stands above most lakes.
float nearMapWeight(vec2 xz)
{
    if (!terrainHeightMapPresent())
        return 0.0;
    const vec2 uv = (xz - u_terrain_mapCentre) * u_terrain_mapInvNearSize + 0.5;
    return u_terrain_mapInvFarSize > 0.0 ? 1.0 - smoothstep(0.42, 0.48, max(abs(uv.x - 0.5), abs(uv.y - 0.5))) : 1.0;
}

// The bed (water_scene.inc.glsl's bed hook): the terrain splat with the river bed's beach layer (river = 1) and the
// full-wetness darkening. The column stays the caller's (the river surface, not the terrain map's water level).
vec3 waterBedAlbedo(vec3 worldPos, vec3 geoN, float rayT, inout float waterLevel)
{
    TerrainFields f; // mild-climate fallbacks without a map, as the terrain VS
    f.altitude    = worldPos.y - u_terrain_seaLevel;
    f.temperature = 12.5;
    f.humidity    = 0.5;
    f.waterLevel  = u_terrain_seaLevel;
    f.river       = 1.0;
    if (terrainHeightMapPresent())
    {
        const vec4 td      = terrainDataAt(worldPos.xz);
        const vec4 climate = terrainClimateNearestAt(worldPos.xz);
        f.altitude    = td.w;
        f.waterLevel  = td.y;
        f.humidity    = climate.w;
        f.temperature = terrainTemperatureAt(climate, worldPos.y);
    }

    g_seabedLod = min(log2(max(rayT, 1.0)) + 1.0, 7.0);
    f16vec3 albedo = terrainSplat(worldPos, geoN, geoN, f).albedo;
    if (u_terrainWater_enabled > 0.5)
        albedo *= float16_t(u_terrainWater_darkening);
    return vec3(albedo);
}

// The body's in-scatter. Entrained air (aeration, from the turbulence) moves it toward the foam colour: a riffle goes
// pale green, a cascade milky.
vec3 riverInscatter(float aeration, vec3 ambientSky, vec3 sunTint, vec3 L)
{
    return mix(u_river_scatterColor, 0.35 * u_river_foamColor, aeration) * (ambientSky + sunTint * (max(L.y, 0.0) * INV_PI));
}

// The water body along the refracted `dir`: the traced bed (TLAS; the baked height map only where the TLAS cannot answer),
// shaded, Beer-Lambert absorbed and blended into the in-scatter, then faded behind the aeration. The column counts from this
// pixel's surface `surfPos`. The in-scatter is built AFTER the trace: no vec3 of it live across the ray query (registers).
vec3 traceRiverBody(vec3 origin, vec3 dir, vec3 surfPos, vec3 sunTint, float sunVis, vec3 L, float aeration, vec3 ambientSky, bool rtInRange)
{
    const vec3 sigmaT = u_river_absorption;
    const float range = u_ocean_rtRefractionRange;
    const float tMax = u_river_bodyTMax; // 1 % transmittance, capped at the range
    SceneHit hit;
    const bool traced = rtInRange && distance(surfPos, u_sceneFocus.xyz) + tMax < u_rt_giTlasRange;

    // The ray starts 1 m BACK: in a shallow stream the TLAS bed (a coarser terrain LOD) can lie at or above the drawn
    // surface, and a ray from the surface misses it (a bright band along the banks). The path still counts from the surface.
    const float back = 1.0;
    bool haveHit = rtInRange && traceScene(origin - dir * back, dir, tMax + back, surfPos.y, hit);
    if (haveHit)
        hit.t = max(hit.t - back, 0.0);

    // Out of the TLAS range: the bed from the baked height map (near cascade only, nearMapWeight).
    if (!haveHit && !traced && dir.y < -0.02 && nearMapWeight(surfPos.xz) > 0.5)
    {
        const float bottom = surfPos.y - terrainHeightAt(surfPos.xz);
        hit.t = max(bottom, 0.02) / -dir.y;
        if (hit.t < tMax)
        {
            hit.pos = surfPos + dir * hit.t;
            hit.N = vec3(0.0, 1.0, 0.0);
            hit.waterLevel = surfPos.y;
            hit.albedo = waterBedAlbedo(hit.pos, hit.N, hit.t, hit.waterLevel);
            haveHit = true;
        }
    }
    if (!haveHit)
        return riverInscatter(aeration, ambientSky, sunTint, L);

    const float sunPath = max(hit.waterLevel - hit.pos.y, 0.0) / max(L.y, 0.25); // the column above the hit
    const vec3 hitRadiance = shadeHit(hit, dir, sunTint * sunVis * exp(-sigmaT * sunPath), L);
    // = mix(hit x T + inscatter x (1 - T), inscatter, aeration), T also faded out at the end of the range.
    const vec3 T = exp(-sigmaT * hit.t) * ((1.0 - smoothstep(0.75 * range, range, hit.t)) * (1.0 - aeration));
    return hitRadiance * T + riverInscatter(aeration, ambientSky, sunTint, L) * (1.0 - T);
}

// THE RIPPLES: the terrain noise texture's gradient fBms (R, G: a slope pair) at two scales, dragged downstream with the
// same two-phase flow map as the waves (river_wave.inc.glsl). Implicit-LOD taps: uniform control flow only.
vec2 rippleSlope(vec2 xz, vec2 flow, float t)
{
    const float period = 1.6; // s
    const float phase = t * (1.0 / period);
    const float p0 = fract(phase);
    const float p1 = fract(phase + 0.5);
    const float w0 = 1.0 - abs(2.0 * p0 - 1.0);
    const vec2 drift = flow * (period * u_river_invRippleSize);
    const mat2 octaveStep = mat2(0.8, -0.6, 0.6, 0.8) * 2.7; // next octave: rotated, x 2.7, shifted

    vec2  slope = vec2(0.0);
    float amp   = 1.0;
    vec2  base  = xz * u_river_invRippleSize;
    for (int octave = 0; octave < 2; octave++)
    {
        const vec2 a = texture(TERRAIN_NOISE_SAMPLER, base - drift * p0).rg * 2.0 - 1.0;
        const vec2 b = texture(TERRAIN_NOISE_SAMPLER, base - drift * p1 + vec2(0.37, 0.61)).rg * 2.0 - 1.0;
        slope += (a * w0 + b * (1.0 - w0)) * amp;
        amp *= 0.5;
        base = octaveStep * base + vec2(0.13, 0.71);
    }
    return slope;
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const vec3 up = u_skyUp;
    const vec3 L  = u_sunDirection.xyz; // normalized on the CPU: read from the UBO at each use, never held in registers
    const vec3 toCam = u_viewPos - in_pos;
    const float viewDist = length(toCam);
    const vec3 V = toCam / max(viewDist, 1e-4);
    const bool lake = in_uv.x > 1.5;

    // A LIGHT ribbon or flat lake quad goes where the dense near cells are all built: else a camera in a deep channel sees
    // the sunk light surface as a second layer. Safe before the derivatives: the covered disc is far larger than a quad.
    if (in_dense < 0.5 && distance(in_pos, u_views_viewPos[VIEW_CENTER].xyz) < u_river_nearCovered)
        discard;

    // The normal (uniform control flow: the taps take screen derivatives): the WAVES' slope, so the shading follows the dense
    // geometry, plus the noise ripples as finer detail. A lake drifts slowly in one direction and ripples weaker.
    const vec2 flow = lake ? vec2(0.05, 0.03) * u_river_flowSpeed : in_flow.xz * u_river_flowSpeed;
    const float whitewaterIn = lake ? 0.0 : in_flow.y;
    const float strength = u_river_rippleStrength * (lake ? u_river_lakeRipple : 1.0 + in_flow.y);
    const vec2 slope = riverWaveSlope(in_pos.xz, flow, whitewaterIn) * ((lake ? u_river_lakeRipple : 1.0) * in_waveSize)
                     + rippleSlope(in_pos.xz, flow, u_timeSeconds) * strength;
    vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
    const float variance = normalVariance(N);

    // TURBULENCE, a smooth gradient from calm water: whitewater x "Foam strength" x the river's size (a trickle has too
    // little water to churn). Rougher water, a milkier body (aeration) and higher waves (river_wave.inc.glsl).
    const float turb = lake ? 0.0 : clamp(in_flow.y * u_river_foamStrength * in_waveSize, 0.0, 1.0);

    // The edge fade. A lake only inside the near data cascade (nearMapWeight); past it opaque, the terrain mesh cuts the shore.
    float cover = 1.0;
    if (lake)
    {
        const float nearW = nearMapWeight(in_pos.xz);
        if (nearW > 0.0)
            cover = mix(1.0, clamp((in_pos.y - terrainHeightAt(in_pos.xz)) / u_river_lakeEdgeFade, 0.0, 1.0), nearW);
    }
    else
        cover = 1.0 - smoothstep(1.0 - u_river_edgeSoftness, 1.0, abs(in_uv.x));
    if (cover <= 0.0)
    {
        out_color = vec4(0.0);
        out_factor = vec4(1.0);
        return;
    }

    if (dot(N, V) < 0.0) // seen from below or at grazing: keep the shading hemisphere consistent
        N = -N;
    const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * (u_sunVisible * cloudSunTransmittance(in_pos));
    const bool rtInRange = u_ocean_rtRayCutoff <= 0.0 || viewDist < u_ocean_rtRayCutoff;

    const float perceptualRough = u_river_roughness;
    const float alphaSq = perceptualRough * perceptualRough * perceptualRough * perceptualRough + variance + turb * 0.12;
    const float alpha = clamp(sqrt(alphaSq), 0.02, 1.0);
    const float NoV = clamp(dot(N, V), 1e-3, 1.0);

    const float sunVis = L.y <= 0.0 ? 0.0
        : (u_rt_sunShadow > 0.5 ? rtShadowVisibility(in_pos + N * 0.1, L, 0.05, 10000.0) : sampleSunShadowHard(in_pos, N));
    const vec3 ambientSky = skyAmbientUp(up);
    const float skyVis = giSkyVisibility(in_pos, up);
    const float aeration = 0.6 * turb; // entrained air (riverInscatter, traceRiverBody)
    const float F = F_Schlick(NoV, 0.02);

    // The refracted body FIRST (registers): no part of `color` is live across its ray query - the glint and the sky share
    // are added after it.
    const float bodyWeight = 1.0 - F;
    vec3 body;
    const vec3 refrDir = refract(-V, N, 1.0 / 1.33); // air into water (N faces V): never totally reflects
    if (bodyWeight > 0.02)
        body = traceRiverBody(in_pos + N * 0.05, refrDir, in_pos, sunTint, sunVis, L, aeration, ambientSky, rtInRange);
    else
        body = riverInscatter(aeration, ambientSky, sunTint, L);

    // The sun glint + the blurred reflection's sky share (the ocean's split).
    const float reflBlur = clamp(alpha * 2.0 - 0.05, 0.0, 0.6);
    const vec3 H = normalize(L + V);
    const float NoL = max(dot(N, L), 0.0);
    const float NoH = max(dot(N, H), 0.0);
    const float LoH = dot(L, H); // = |L + V| / 2 >= 0
    const float glint = D_GGX(NoH, alpha) * V_SmithGGX(NoV, NoL, alpha) * NoL * (sunVis * F_Schlick(LoH, 0.02)); // <= ~2e4 at alpha >= 0.02

    vec3 color = body * bodyWeight + F * reflBlur * skyVis * ambientSky + sunTint * glint;

    // The mirror: ray-traced (the ocean's toggle and ranges), sky fallback.
    const float mirrorWeight = F * (1.0 - reflBlur);
    vec3 R = reflect(-V, N);
    R.y = max(R.y, 0.02);
    R = normalize(R);
    vec3 reflColor;
    bool mirrorHit = false;
#ifdef OCEAN_RT_REFLECTIONS
    if (mirrorWeight > 0.02 && alpha < u_ocean_rtReflectionMaxRough && rtInRange)
    {
        SceneHit hit;
        mirrorHit = traceScene(in_pos + N * 0.05, R, u_ocean_rtReflectionRange, -1e30, hit);
        if (mirrorHit)
            reflColor = applyReflectionFog(shadeHit(hit, R, sunTint, L), in_pos, R, hit.t, sunTint, L, ambientSky);
    }
#endif
    if (!mirrorHit)
        reflColor = applyReflectionFogSky(reflectedSkyRadiance(R), in_pos, R, sunTint, L, ambientSky) * skyVis;
    color += reflColor * mirrorWeight;

    // Scene lights (the light grid, RT-shadowed): dielectric specular + the body's in-scatter as "diffuse".
    {
        const f16vec3 matColOverPi = f16vec3(u_river_scatterColor * INV_PI);
        const float16_t lightRough = float16_t(alpha); // already clamped to [0.02, 1]
        const f16vec3 Nh = f16vec3(N);
        const f16vec3 Vh = f16vec3(V);
        const ivec3 gridPos = getGridPos(in_pos);
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
                const uint cellOffset     = calcCellOffset(gridIdx, gridMin, in_pos);
                const uint numLights      = numLargeLights + getNumLightsForCell(cellOffset);
                for (uint i = 0; i < numLights; ++i)
                {
                    const uint lightId = i < numLargeLights ? getLargeLightId(gridIdx, i) : getLightId(cellOffset, i - numLargeLights);
                    color += doLightShadowed(in_lightInfos[lightId], in_pos, Vh, Nh, f16vec3(0.02), matColOverPi, 0.0, lightRough);
                }
                break;
            }
            tableIdx = getNextTableIdx(tableIdx);
        }
    }

    out_color = vec4(color * cover, 0.0);
    out_factor = vec4(vec3(1.0 - cover), cover > 0.5 ? 0.0 : 1.0);
}
