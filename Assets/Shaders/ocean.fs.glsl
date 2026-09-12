#version 460

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// FFT ocean surface shading (EPipelineIndex::Ocean). The vertex shader passed the undisplaced world XZ
// in in_uv; the gradient maps rebuild a per-pixel wave normal + fold Jacobian, then: Fresnel split
// (Schlick, F0 = 0.02) between the RAY-TRACED refracted body (Snell n = 1.33, Beer-Lambert both ways,
// closed-form single-scatter; the water itself has TLAS mask 0 so rays pass through) and the RAY-TRACED
// mirror reflection (sky fallback). GGX/Smith sun glint widened by spec AA + LEAN-filtered slope
// variance (Bruneton 2010 - the elongated glitter path at distance), Jacobian whitecap foam with
// temporal turbulence (ocean_foam.cs.glsl), one RT sun shadow ray. Ray budgets: "Ocean/RT" tweaks.

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "ocean_wave.inc.glsl"

struct MaterialInfo
{
    uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
    uint metalRoughnessTexIdxAlphaMode;
};
layout (binding = 2, std430) readonly buffer InMaterialInfos { MaterialInfo in_materialInfos[]; };

struct LightInfo
{
    vec3 pos;
    float range;
    vec3 color;
    float width;     // 0 = point light, > 0 = rectangular area light, < 0 = spot
    vec3 direction;  // area light: up-axis, magnitude = height
    float rotation;  // area light: rotation of the quad around direction
};
layout (binding = 4, std430) readonly buffer InLightInfos { LightInfo in_lightInfos[]; };
layout (binding = 5, std430) readonly buffer InLightGrid { uint in_gridData[]; };
layout (binding = 6, std430) readonly buffer InGridTable
{
    uint in_numGrids;
    uint in_gridDataCounter;
    uint in_tableSize;
    uint in_gridTable[];
};
#define GRID_DATA_NAME  in_gridData
#define GRID_TABLE_NAME in_gridTable
#define TABLE_SIZE_NAME in_tableSize
#include "light_grid.inc.glsl"

layout (binding = 20) uniform sampler2DArray u_skyMap;   // GI's per-frame sky bake (atmosphere.inc.glsl: skyMapUV / SKY_MAP_LAYER_*)
layout (binding = 21) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
layout (binding = 11) uniform accelerationStructureEXT u_tlas;

// Scene geometry for ray hits (custom index = index into in_instances; RT meshIdx rides the sbtOffset).
struct InMeshInfo
{
    vec3 center;
    float radius;
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint _padding;
};
struct InMeshInstance
{
    uint renderNodeIdx;
    uint instanceOffsetIdx;
    uint meshIdxMaterialIdx;
    uint pipelineIdxAlphaMode;
};
layout (binding = 14, std430) readonly buffer InRTVertices  { float in_vertices[]; }; // MeshVertex as 12 floats
layout (binding = 15, std430) readonly buffer InRTIndices   { uint in_indices[]; };
layout (binding = 16, std430) readonly buffer InRTMeshInfos { InMeshInfo in_meshInfos[]; };
layout (binding = 17, std430) readonly buffer InRTInstances { InMeshInstance in_instances[]; };

layout (binding = 10, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };
#define GI_GRID_DATA_NAME gi_gridData
#include "gi_probe.inc.glsl"

#include "rt_shadow.inc.glsl"

// Sun CSM (PCSS) fallback when RT sun shadows are off.
layout (binding = 8) uniform sampler2DArrayShadow u_shadowMap;
layout (binding = 9) uniform sampler2DArray u_shadowMapDepth;
#include "shadows.inc.glsl"
#include "punctual_lights.inc.glsl"

layout (location = 0) in vec3 in_pos;                       // displaced world position
layout (location = 1) in mat3 in_tbn;                       // placeholder (normal rebuilt here)
layout (location = 4) in vec2 in_uv;                        // undisplaced world XZ
layout (location = 5) in flat uint in_meshIdxMaterialIdx;
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (location = 0) out vec4 out_color;

// --- Shore debug view: set a mode, F5. Paints the depth-keyed terms instead of shading, to find which
// one's boundary a visible line on the water follows.
//   1 = calm depth      black 0 -> white 4 m, green iso-lines every 0.5 m, RED = land (depth < 0)
//   2 = swash weight    the tongue weight (backflow): white = full, black = none
//   3 = surface weight  oceanSurfaceWeight: white = open water, darker = eased toward the swash amplitude
//   5 = shore foam band the surf band's nearShore target (u_oceanParams5.z)
#define OCEAN_DEBUG_MODE 0

float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float d  = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * d * d);
}
float V_SmithGGX(float NoV, float NoL, float a) // height-correlated Smith, includes 1/(4 NoV NoL)
{
    float a2 = a * a;
    float ggxV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float ggxL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(ggxV + ggxL, 1e-5);
}
float F_Schlick(float u, float f0)
{
    float x = clamp(1.0 - u, 0.0, 1.0);
    float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}
// Geometric specular AA. Weight/cap deliberately far below the usual 0.5/0.18: a full-strength term
// re-widens the lobe by exactly what "Glint sharpness" reveals (cancelling the knob), and water wants
// a little sub-pixel shimmer - that IS the sparkle.
float normalVariance(vec3 N)
{
    vec3 dNdx = dFdx(N);
    vec3 dNdy = dFdy(N);
    return min(0.25 * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), 0.03);
}

// Sky for the mirror ray: the per-frame bake of mirrorSkyRadiance (atmosphere.inc.glsl - 12-step march
// + eclipse saturation, matched to sky.fs.glsl) - one fetch instead of a march per pixel. No sun disc:
// the GGX glint is its reflection.
vec3 reflectedSkyRadiance(vec3 dir)
{
    return textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR), 0.0).rgb;
}
// skyRadiance(up): the same constant for every pixel - one fetch of the GI layer.
vec3 skyAmbientUp(vec3 up)
{
    return textureLod(u_skyMap, vec3(skyMapUV(up), SKY_MAP_LAYER_GI), 0.0).rgb;
}

struct SceneHit
{
    float t;
    vec3 pos;
    vec3 N;
    vec3 albedo;
    float waterLevel; // calm water level above the hit (the column shadeHit / the body attenuate through):
                      // fetched ONCE per hit - the seabed splat already reads the terrain data there
};

// The seabed at a ray hit IS the terrain: the terrain shader's own splat (terrain_splat.inc.glsl -
// ground / beach / rock / snow by climate, slope and relief) evaluated at the hit, with the baked fields
// fetched the way the terrain VS does per vertex. Albedo only (the water column blurs any detail normal
// away), at a ray-cone LOD instead of screen derivatives (a ray hit has none).
float g_seabedLod = 0.0;
#define TERRAIN_SPLAT_TEX(tex, uv) textureLod(tex, uv, g_seabedLod)
#define TERRAIN_SPLAT_ALBEDO_ONLY
#include "terrain_splat.inc.glsl"

// waterLevel returns the calm level the fields carry (the hit's column: one terrain fetch serves both).
vec3 terrainSeabedAlbedo(vec3 worldPos, vec3 geoN, float rayT, out float waterLevel)
{
    TerrainFields f; // mild-climate fallbacks without a map, as the terrain VS
    f.altitude = worldPos.y - u_terrainParams.z;
    f.temperature = 12.5;
    f.humidity = 0.5;
    f.waterLevel = u_terrainParams.z;
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(worldPos.xz);
        f.altitude = td.w;
        f.waterLevel = td.y;
        // NEAREST climate texel (the terrain VS bilinears): seen through water at a ray-cone LOD the
        // data map's texel grid never shows, and it is one decode instead of four.
        const vec4 climate = terrainClimateNearestAt(worldPos.xz);
        f.humidity = climate.w;
        f.temperature = terrainTemperatureAt(climate, worldPos.y);
    }
    waterLevel = f.waterLevel;
    g_seabedLod = clamp(log2(max(rayT, 1.0)) + 1.0, 0.0, 7.0);
    vec3 albedo = terrainSplat(worldPos, geoN, f).albedo;
    // The seabed is, by definition, fully wet: darken it exactly as the terrain shader darkens ground at
    // full wetness (damp x standing film - instanced_indirect_terrain.fs.glsl), so the sand seen through
    // the water and the wet sand the water just left are the same colour at the waterline.
    if (u_terrainWetParams2.x > 0.5)
        albedo *= u_terrainWetParams5.y * u_terrainWetParams2.y;
    return albedo;
}

bool traceScene(vec3 origin, vec3 dir, float tMax, out SceneHit hit)
{
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsNoneEXT, 0xFFu, origin, 0.05, dir, tMax);
    while (rayQueryProceedEXT(rq))
    {
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT
            && rtsCandidateBlocks(rq))
            rayQueryConfirmIntersectionEXT(rq);
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT)
        return false;

    hit.t = rayQueryGetIntersectionTEXT(rq, true);
    hit.pos = origin + dir * hit.t;
    hit.N = -dir;
    hit.albedo = vec3(0.3);
    hit.waterLevel = -1e30; // unset: a terrain hit fills it from the seabed splat's own terrain fetch, the rest fetch below

    // Interpolated normal/uv + material albedo, bounds-checked like rt_shadow.inc.glsl.
    const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
    const uint meshIdx = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rq, true);
    if (uint(instanceIdx) < in_instances.length() && meshIdx < in_meshInfos.length())
    {
        const InMeshInfo mi = in_meshInfos[meshIdx];
        const uint triBase = mi.firstIndex + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)) * 3u;
        if (triBase + 2u < in_indices.length())
        {
            const uint v0 = uint(mi.vertexOffset) + in_indices[triBase + 0u];
            const uint v1 = uint(mi.vertexOffset) + in_indices[triBase + 1u];
            const uint v2 = uint(mi.vertexOffset) + in_indices[triBase + 2u];
            if ((max(max(v0, v1), v2) * 12u + 11u) < in_vertices.length())
            {
                const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
                const vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
                #define RT_V(vi, o) vec3(in_vertices[(vi) * 12u + (o)], in_vertices[(vi) * 12u + (o) + 1u], in_vertices[(vi) * 12u + (o) + 2u])
                const vec3 objN = RT_V(v0, 3u) * w.x + RT_V(v1, 3u) * w.y + RT_V(v2, 3u) * w.z;
                const vec2 uv = w.x * rtsVertexUV(v0) + w.y * rtsVertexUV(v1) + w.z * rtsVertexUV(v2);
                hit.N = normalize(mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, true)) * objN);
                if (dot(hit.N, dir) > 0.0)
                    hit.N = -hit.N;

                const uint materialIdx = in_instances[instanceIdx].meshIdxMaterialIdx >> 16;
                if (materialIdx < in_materialInfos.length())
                {
                    if ((in_materialInfos[materialIdx].flags & MATERIAL_FLAG_TERRAIN) != 0u)
                        hit.albedo = terrainSeabedAlbedo(hit.pos, hit.N, hit.t, hit.waterLevel);
                    else
                    {
                        const uint diffuseTexIdx = in_materialInfos[materialIdx].diffuseNormalTexIdx & 0x0000FFFFu;
                        const float lod = clamp(log2(max(hit.t, 1.0)) + 1.0, 0.0, 7.0); // ray-cone-ish LOD by distance
                        hit.albedo = textureLod(u_textures[nonuniformEXT(diffuseTexIdx)], uv, lod).rgb;
                    }
                }
            }
        }
    }
    if (hit.waterLevel < -1e29) // non-terrain hit (or an unresolved one): one shore fetch for the column
        hit.waterLevel = oceanSampleShoreData(hit.pos.xz).y;
    return true;
}

// Sun + GI probe irradiance at a ray hit; grid lights behind OCEAN_HIT_LIGHTS ("Hit lighting" tweak).
// Analytic only - no shadow rays inside what is already a refraction/reflection ray.
vec3 shadeHit(SceneHit hit, vec3 rayDir, vec3 sunRadiance, vec3 L)
{
    const vec3 sun = sunRadiance * max(dot(hit.N, L), 0.0);
    float giCoverage;
    const vec3 probeE = evalProbeSHCoverage(hit.pos, hit.N, giCoverage);
    vec3 indirect = probeE.x >= 0.0 ? probeE / PI : vec3(0.0);
    if (giCoverage < 1.0)
        indirect = mix(giEvalSkySH(hit.N) / PI, indirect, giCoverage);
    indirect *= u_aoParams.y;
    // Ambient/GI reaching an underwater hit must Beer-Lambert down the column too - the probes don't
    // know about the water (it's not in the TLAS), so the seabed would read open-air-lit at any depth.
    const vec3 ambientAtten = exp(-u_oceanAbsorption.rgb * max(hit.waterLevel - hit.pos.y, 0.0));
    vec3 radiance = hit.albedo * (sun / PI + (indirect + u_ambientColor) * ambientAtten);

#ifdef OCEAN_HIT_LIGHTS
    const vec3 matColOverPi = hit.albedo / PI;
    const vec3 V = -rayDir;
    const ivec3 gridPos = getGridPos(hit.pos);
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
                radiance += doLight(in_lightInfos[getLargeLightId(gridIdx, i)], hit.pos, V, hit.N, vec3(0.04), matColOverPi, 0.0, 0.7, 0.49);
            const uint cellOffset = calcCellOffset(gridIdx, gridMin, hit.pos);
            const uint numLights = getNumLightsForCell(cellOffset);
            for (uint i = 0; i < min(numLights, MAX_LIGHTCELL_LIGHTS); ++i)
                radiance += doLight(in_lightInfos[getLightId(cellOffset, i)], hit.pos, V, hit.N, vec3(0.04), matColOverPi, 0.0, 0.7, 0.49);
            break;
        }
        tableIdx = getNextTableIdx(tableIdx);
    }
#endif
    return radiance;
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const vec3 up = normalize(u_skyUp);
    const vec3 L  = normalize(u_sunDirection.xyz);
    const vec3 V  = normalize(u_viewPos - in_pos);

    // Wave normal, fold Jacobian, LEAN slope variance, vertical acceleration; shoreHW = (terrain
    // height, water level) here, reused by the surf band and SSS below.
    vec2 slope, slopeVar, shoreHW;
    float jacobian, accel;
    oceanSampleSurface(in_uv, slope, jacobian, slopeVar, accel, shoreHW);

    // Accumulated turbulence (churn energy of past breaking; sampled unconditionally - derivatives
    // need uniform control flow). Drives aged foam, milkiness and extra roughness.
    const float turbulence = oceanSampleTurbulence(in_uv, length(fwidth(in_uv)));
    const float foam = oceanInstantFoam(jacobian, accel, turbulence * u_oceanParams5.x);

    // Shoreline surf band: coverage target from the breaking bore front + the waterline, realized
    // through the RAW (un-shoaled) fold Jacobian so it reads as filaments along the swell, not a
    // painted gradient.
    float shoreFoam = 0.0;
    const float shoreFoamDepth = u_oceanParams5.z;
    if (shoreFoamDepth > 0.0)
    {
        // Same floored depth the waves use: a distant too-shallow reading otherwise drives the
        // waterline lace term (column < 0.35 * Shore foam depth) across whole bays as a white wash.
        const float shoreDepth = oceanEffectiveDepth(in_uv, shoreHW.y - shoreHW.x);
        const float waveH = in_pos.y - shoreHW.y;               // surface height above the LOCAL calm water level
        const float column = max(shoreDepth, 0.0) + waveH;      // instantaneous water column at this pixel
        const float nearShore = 1.0 - smoothstep(shoreFoamDepth, 4.0 * shoreFoamDepth, shoreDepth);
        const float bore = max(waveH, 0.0) / max(column, 0.05); // crest fraction of the column (bore front)
        float target = nearShore * smoothstep(0.4, 0.8, bore);
        target = max(target, 1.0 - smoothstep(0.0, 0.35 * shoreFoamDepth, column));

        // fwidth stays OUTSIDE the target gate: derivatives in non-uniform control flow are undefined.
        const float fp = length(fwidth(in_pos.xz));
        if (target > 0.001) // open water: skip the taps
        {
            float sxx = 0.0, szz = 0.0, sxz = 0.0;
            const float chop = u_oceanParams0.w;
            for (int c = 0; c < OCEAN_CASCADES; ++c)
            {
                const float Lc = u_oceanParams2[c];
                const float lod = max(log2(max(fp, 1e-3) * float(OCEAN_FFT_SIZE) / Lc), 0.0);
                const vec2 uvc = in_uv / Lc;
                const vec4 g = textureLod(u_oceanMaps, vec3(uvc, float(OCEAN_CASCADES + c)), lod); // (dh/dx, dh/dz, dDx/dx, dDz/dz)
                sxx += g.z;
                szz += g.w;
                sxz += textureLod(u_oceanMaps, vec3(uvc, float(c)), lod).w; // displacement layer w = dDx/dz
            }
            const float Jraw = (1.0 + chop * sxx) * (1.0 + chop * szz) - chop * sxz * chop * sxz;
            const float b = mix(0.75, 1.45, target) + u_oceanParams8.y; // "Shore foam bias"
            shoreFoam = target * (1.0 - smoothstep(b - 0.4, b + 0.4, Jraw));
            // "Shore foam max": keep the bottom visible through the lace. A soft knee, not a min(): a hard
            // clamp flattened the whole waterline band into a plateau with an edge wherever the target
            // exceeded the cap; this eases toward the cap and never quite reaches it.
            const float foamMax = max(u_oceanParams7.y, 1e-3);
            shoreFoam = foamMax * (1.0 - exp(-shoreFoam / foamMax));
        }
    }
    const float ns = u_oceanParams1.w;
    vec3 N = normalize(vec3(-slope.x * ns, 1.0, -slope.y * ns));

#if OCEAN_DEBUG_MODE != 0
    {
        const float depthDbg = oceanEffectiveDepth(in_uv, shoreHW.y - shoreHW.x);
        const float swDbg = oceanSwashWeight(depthDbg, shoreHW.y);
        vec3 dbg = vec3(0.0);
#if OCEAN_DEBUG_MODE == 1
        dbg = depthDbg < 0.0 ? vec3(1.0, 0.0, 0.0) : vec3(clamp(depthDbg / 4.0, 0.0, 1.0));
        if (depthDbg >= 0.0 && fract(depthDbg * 2.0) < 0.05)
            dbg.g = 1.0;
#elif OCEAN_DEBUG_MODE == 2
        dbg = vec3(swDbg / max(u_oceanParams7.z, 1e-3));
#elif OCEAN_DEBUG_MODE == 3
        dbg = vec3(oceanSurfaceWeight(depthDbg, shoreHW.y));
#elif OCEAN_DEBUG_MODE == 5
        dbg = vec3(u_oceanParams5.z > 0.0 ? 1.0 - smoothstep(u_oceanParams5.z, 4.0 * u_oceanParams5.z, depthDbg) : 0.0);
#endif
        out_color = vec4(dbg, 1.0);
        return;
    }
#endif

    // Underside (camera below the surface looking at its back face): refracted sky in Snell's window
    // with the sun blazing through it, TIR to the water body outside it. The underwater fog handles the
    // water column, so no view-path absorption here.
    if (dot(N, V) < 0.0 && u_viewPos.y < in_pos.y)
    {
        const vec3 sunTintU = u_sunColor.rgb * atmosTransmittanceToLight(0.0, L, up) * u_eclipseParams.x;
        const vec3 inscatterU = u_oceanScatter.rgb * u_oceanScatter.w * (skyAmbientUp(up) + sunTintU * max(L.y, 0.0) / PI);
        const vec3 refrUp = refract(-V, -N, 1.33);
        vec3 color = inscatterU; // TIR (refract() = 0): mirror of the water body
        if (dot(refrUp, refrUp) > 1e-6)
        {
            const vec3 tDir = normalize(refrUp);
            const float T = 1.0 - F_Schlick(clamp(dot(-V, N), 0.0, 1.0), 0.02);
            vec3 sky = reflectedSkyRadiance(tDir);
            const float sunDot = max(dot(tDir, L), 0.0);
            sky += sunTintU * (pow(sunDot, 600.0) * 30.0 + pow(sunDot, 24.0) * 0.6);
            color = sky * T + inscatterU * (1.0 - T);
        }
        out_color = vec4(color, 1.0);
        return;
    }

    if (dot(N, V) < 0.0) // grazing: keep the shading hemisphere consistent
        N = -N;

    const float NoV = clamp(dot(N, V), 1e-3, 1.0);
    const float NoL = max(dot(N, L), 0.0);
    const vec3  H   = normalize(L + V);
    const float NoH = max(dot(N, H), 0.0);
    const float LoH = max(dot(L, H), 0.0);

    // Microfacet roughness = base + spec AA + LEAN slope variance (both scaled by "Glint filtering")
    // + turbulence micro-roughness. The variance terms stretch the sun glitter toward the horizon.
    const float perceptualRough = clamp(u_oceanAbsorption.w, 0.02, 1.0);
    const float slopeVariance = 0.5 * (slopeVar.x + slopeVar.y) * (ns * ns);
    const float alphaSq = perceptualRough * perceptualRough * perceptualRough * perceptualRough
        + (normalVariance(N) + 2.0 * slopeVariance) * u_oceanParams6.y + turbulence * u_oceanParams5.y * 0.35;
    const float alphaF = clamp(sqrt(alphaSq), 0.02, 1.0);

    // Sun visibility: one RT shadow ray (or PCSS fallback). Back-lit crests still need it while crest
    // SSS is on - the subsurface glow must stay shadow-gated.
    const vec3 sunTint = u_sunColor.rgb * atmosTransmittanceToLight(0.0, L, up) * u_eclipseParams.x;
    const bool sunUp = L.y > 0.0 && (NoL > 0.0 || u_oceanParams6.z > 0.0);
    const float sunVis = !sunUp ? 0.0
        : (u_rtSunShadow > 0.5 ? rtShadowVisibility(in_pos + N * 0.1, L, 0.05, 10000.0)
                               : sampleSunShadow(in_pos, N));
    const vec3 ambientSky = skyAmbientUp(up);
    const vec3 whitewater = u_oceanFoam.rgb * (sunTint * (NoL * sunVis) / PI + ambientSky + u_ambientColor);

    const vec3 sigmaT = u_oceanAbsorption.rgb;
    const vec3 inscatter = u_oceanScatter.rgb * u_oceanScatter.w * (ambientSky + sunTint * max(L.y, 0.0) / PI);

    const float F = F_Schlick(NoV, 0.02);

    // "Ray cutoff dist": beyond it no scene rays at all - refraction uses the analytic bottom (the
    // path misses already take), reflection the sky. 0 = unlimited.
    const bool rtInRange = u_oceanParams8.w <= 0.0 || distance(u_viewPos, in_pos) < u_oceanParams8.w;

    // Refracted body: ray-traced, Beer-Lambert absorbed both ways, bounded at ~99% extinction and the
    // "Refraction range" tweak.
    vec3 body = inscatter;
    if (F < 0.98) // at grazing the transmitted term is invisible: skip the ray
    {
        const vec3 refrDir = refract(-V, N, 1.0 / 1.33);
        if (dot(refrDir, refrDir) > 1e-6)
        {
            const float minSigma = max(min(sigmaT.r, min(sigmaT.g, sigmaT.b)), 1e-3);
            // "Refraction range" is the max refracted-ray length that still shows the bottom - traced
            // OR height-field - so it caps underwater visibility in clear water where extinction alone
            // would not. The bottom fades out over the last 25% of the range (a hard cutoff draws a
            // contour on the seabed); beyond it the body is pure in-scatter.
            const float range = u_oceanParams9.y;
            const float tMax = min(4.6 / minSigma, range);
            SceneHit hit;
            bool haveHit = rtInRange && traceScene(in_pos + N * 0.05, refrDir, tMax, hit);
            if (!haveHit && refrDir.y < -0.02)
            {
                // Miss (seabed beyond the TLAS range / rays off): pseudo-hit against the baked terrain
                // height field instead. Bottom distance measured from the SURFACE point, not the calm
                // level - the waterline band would reject otherwise (RT tMin guarantees a miss exactly
                // there). Never reject a negative estimate: terrain stood behind the water in the depth
                // test, so a bottom exists - clamp to a centimeters-deep hit (a map-vs-mesh
                // disagreement otherwise draws a deep-blue line along the shore).
                float bottom = in_pos.y - oceanSampleShoreData(in_pos.xz).x;
                const float t = max(bottom, 0.02) / -refrDir.y;
                bottom = in_pos.y - oceanSampleShoreData(in_pos.xz + refrDir.xz * t).x; // one refinement for sloped shelves
                hit.t = max(bottom, 0.02) / -refrDir.y;
                if (hit.t < range) // the same bound as the traced ray: past it there is no bottom to draw
                {
                    hit.t = min(hit.t, 4.6 / minSigma);
                    hit.pos = in_pos + refrDir * hit.t;
                    hit.N = vec3(0.0, 1.0, 0.0);
                    hit.albedo = terrainSeabedAlbedo(hit.pos, hit.N, hit.t, hit.waterLevel);
                    haveHit = true;
                }
            }
            if (haveHit)
            {
                const float sunPath = max(hit.waterLevel - hit.pos.y, 0.0) / max(L.y, 0.25); // column above the hit
                const vec3 hitRadiance = shadeHit(hit, refrDir, sunTint * sunVis * exp(-sigmaT * sunPath), L);
                const vec3 T = exp(-sigmaT * hit.t) * (1.0 - smoothstep(0.75 * range, range, hit.t));
                body = hitRadiance * T + inscatter * (1.0 - T);
            }
        }
    }

    // Reflection: ray-traced mirror, sky fallback, roughness-blurred toward the average sky.
    vec3 R = reflect(-V, N);
    R.y = max(R.y, 0.02); // keep grazing reflections just above the horizon
    R = normalize(R);
    const float reflBlur = clamp(alphaF * 2.0 - 0.05, 0.0, 0.6);
    vec3 reflColor = reflectedSkyRadiance(R);
    if (alphaF < u_oceanParams9.w && rtInRange) // "Reflection max rough": a wide lobe can't be one mirror sample
    {
        SceneHit hit;
        if (traceScene(in_pos + N * 0.05, R, u_oceanParams9.z, hit)) // "Reflection range"
            reflColor = shadeHit(hit, R, sunTint, L);
    }
    const vec3 reflection = mix(reflColor, ambientSky, reflBlur);

    // Entrained bubbles: turbulent water turns milky ("Turbidity").
    body = mix(body, whitewater * 0.55, clamp(turbulence * u_oceanParams5.y, 0.0, 1.0));

    // Crest SSS: sun through back-lit crests glows the scatter color, scaled by height above the calm
    // line. In the transmitted body so Fresnel fades it at grazing like all subsurface light.
    const float sssStrength = u_oceanParams6.z;
    if (sssStrength > 0.0)
    {
        const float waveH = max(in_pos.y - shoreHW.y, 0.0);
        const float towardSun = pow(clamp(dot(V, -L), 0.0, 1.0), u_oceanParams6.w);
        const float backSlope = 0.5 - 0.5 * dot(L, N);
        body += u_oceanScatter.rgb * sunTint * (sssStrength * waveH * towardSun * backSlope * backSlope * backSlope * sunVis);
    }

    vec3 color = mix(body, reflection, F);

    // Sun glint: Cook-Torrance specular, shadow-gated.
    const float D  = D_GGX(NoH, alphaF);
    const float Vv = V_SmithGGX(NoV, NoL, alphaF);
    color += sunTint * (D * Vv * NoL * sunVis) * F_Schlick(LoH, 0.02);

    // Crest foam + shoreline surf.
    const float foamW = clamp(max(foam, shoreFoam), 0.0, 1.0);
    if (foamW > 0.003)
        color = mix(color, whitewater, foamW);

    // Scene lights (shared light-grid walk, RT-shadowed): dielectric specular + in-scatter "diffuse";
    // foam patches respond as lambertian whitewater instead.
    {
        const vec3 matColOverPi = mix(u_oceanScatter.rgb * u_oceanScatter.w, u_oceanFoam.rgb, foamW) / PI;
        const float lightRough = clamp(mix(alphaF, 0.85, foamW), 0.02, 1.0);
        const float lightRoughSq = lightRough * lightRough;
        const vec3 waterSpec = vec3(0.02);

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
                for (uint i = 0; i < min(numLargeLights, MAX_LARGE_LIGHTS_PER_GRID); ++i)
                {
                    const LightInfo light = in_lightInfos[getLargeLightId(gridIdx, i)];
                    color += doLightShadowed(light, in_pos, V, N, waterSpec, matColOverPi, 0.0, lightRough, lightRoughSq);
                }
                const uint cellOffset = calcCellOffset(gridIdx, gridMin, in_pos);
                const uint numLights = getNumLightsForCell(cellOffset);
                for (uint i = 0; i < min(numLights, MAX_LIGHTCELL_LIGHTS); ++i)
                {
                    const LightInfo light = in_lightInfos[getLightId(cellOffset, i)];
                    color += doLightShadowed(light, in_pos, V, N, waterSpec, matColOverPi, 0.0, lightRough, lightRoughSq);
                }
                break;
            }
            tableIdx = getNextTableIdx(tableIdx);
        }
    }

    out_color = vec4(color, 1.0);
}
