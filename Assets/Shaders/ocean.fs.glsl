#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable // the BRDF colour side is half (punctual_lights.inc.glsl)
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
#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl" // the edge fade's film surface: the pool level the wetness fills to

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
layout (binding = 22) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
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
#ifdef GI_VOLUME
layout (binding = 21) uniform sampler3D u_giVolume[GI_VOLUME_MAX_IMAGES]; // the baked irradiance volume + sky SH
#define GI_VOLUME_TEXTURES_NAME u_giVolume
#endif
#define GI_PROBE_HALF // the fp16 read side (shadeHit)
#include "gi_probe.inc.glsl"

#include "rt_shadow.inc.glsl"

// Sun CSM (PCSS) fallback when RT sun shadows are off.
layout (binding = 8) uniform sampler2DArrayShadow u_shadowMap;
layout (binding = 9) uniform sampler2DArray u_shadowMapDepth;
#include "shadows.inc.glsl"
#include "punctual_lights.inc.glsl"
#include "reflection_fog.inc.glsl"

layout (location = 0) in vec3 in_pos;                       // displaced world position
layout (location = 4) in vec2 in_uv;                        // undisplaced world XZ
layout (location = 5) in flat uint in_meshIdxMaterialIdx;
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// DUAL-SOURCE composite (the Ocean variant): out = out_color + scene * out_factor, the alpha too. Opaque
// (factor 0, alpha 0 = TAA's ocean flag) except over the edge fade, where the ground and film show through.
layout (location = 0, index = 0) out vec4 out_color;
layout (location = 0, index = 1) out vec4 out_factor;

// --- Debug view ("Ocean/Debug mode" tweak). Paints the depth-keyed terms instead of shading, to find which
// one's boundary a visible line on the water follows.
//   1 = calm depth      black 0 -> white 4 m, green iso-lines every 0.5 m, RED = land (depth < 0)
//   2 = swash weight    the tongue weight (backflow): white = full, black = none
//   3 = surface weight  oceanSurfaceWeight: white = open water, darker = eased toward the swash amplitude
//   5 = shore foam band the surf band's nearShore target (u_oceanParams5.z)
//   6 = mirror ray      GREEN = scene hit (pink-white near, pure green far), BLUE = fired, missed (TLAS has
//                       geometry), WHITE = hit only past "Reflection range", GREY = nothing hits anywhere
//                       (the TLAS is EMPTY - every RT effect is dead, not just this ray),
//                       RED = skipped as invisible (mirror weight <= 2%: nadir, foam, blur), YELLOW = skipped by "Reflection max rough",
//                       MAGENTA = skipped by "Ray cutoff dist", BLACK = OCEAN_RT_REFLECTIONS off
//                       UNDERSIDE: the same colours for the WINDOW ray (the scene above the water);
//                       outside Snell's window CYAN = TIR, mirror traced, DARK CYAN = TIR, mirror skipped
// Modes 1-5 are position-keyed and show on both sides of the surface.
// Set by the "Ocean/Debug mode" tweak (a define on this variant; toggling reloads the pipeline).
#ifndef OCEAN_DEBUG_MODE
#define OCEAN_DEBUG_MODE 0
#endif

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
// fp16 copies for the top side's shading block (GGX D is punctual_lights.inc.glsl's D_GGX_H).
float16_t V_SmithGGX_H(float16_t NoV, float16_t NoL, float16_t a)
{
    const float16_t a2 = a * a;
    const float16_t ggxV = NoL * sqrt(NoV * NoV * (float16_t(1.0) - a2) + a2);
    const float16_t ggxL = NoV * sqrt(NoL * NoL * (float16_t(1.0) - a2) + a2);
    return float16_t(0.5) / max(ggxV + ggxL, float16_t(1e-4));
}
float16_t F_SchlickH(float16_t u, float16_t f0)
{
    const float16_t x = clamp(float16_t(1.0) - u, float16_t(0.0), float16_t(1.0));
    const float16_t x2 = x * x;
    return f0 + (float16_t(1.0) - f0) * (x2 * x2 * x);
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
    f16vec3 albedo = terrainSplat(worldPos, geoN, geoN, f).albedo;
    // The seabed is, by definition, fully wet: darken it exactly as the terrain shader darkens ground at
    // full wetness ("Wet darkening" - instanced_indirect_terrain.fs.glsl), so the sand seen through the
    // water and the wet sand the water just left are the same colour at the waterline.
    if (u_terrainWetParams2.x > 0.5)
        albedo *= float16_t(u_terrainWetParams2.y);
    return vec3(albedo);
}

// underwater: the hit sits in the water column, so its calm level is fetched for shadeHit's absorption
// (a reflection ray's hit is above the surface: no column, no fetch).
bool traceScene(vec3 origin, vec3 dir, float tMax, bool underwater, out SceneHit hit)
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
    if (underwater && hit.waterLevel < -1e29) // non-terrain hit (or an unresolved one): one shore fetch for the column
        hit.waterLevel = oceanSampleShoreData(hit.pos.xz).y;
    return true;
}

// Sun + GI probe irradiance at a ray hit; grid lights behind OCEAN_HIT_LIGHTS ("Hit lighting" tweak).
// Analytic only - no shadow rays inside what is already a refraction/reflection ray.
vec3 shadeHit(SceneHit hit, vec3 rayDir, vec3 sunRadiance, vec3 L)
{
    // Half shading (the hit's colour terms, the GI read, the column absorption), widened once at the end.
    const f16vec3 hitN = f16vec3(hit.N);
    const f16vec3 albedo = f16vec3(hit.albedo);
    const f16vec3 sunOverPi = f16vec3(sunRadiance * (max(dot(hit.N, L), 0.0) * INV_PI));
    const f16vec3 indirect = giIndirectOverPiH(hit.pos, hitN) * float16_t(u_aoParams.y);
    // Ambient/GI reaching an underwater hit must Beer-Lambert down the column too - the probes don't
    // know about the water (it's not in the TLAS), so the seabed would read open-air-lit at any depth.
    const f16vec3 ambientAtten = f16vec3(exp(-u_oceanAbsorption.rgb * max(hit.waterLevel - hit.pos.y, 0.0)));
    vec3 radiance = vec3(albedo * (sunOverPi + (indirect + f16vec3(u_ambientColor)) * ambientAtten));

#ifdef OCEAN_HIT_LIGHTS
    const f16vec3 matColOverPi = albedo * float16_t(INV_PI); // half: the BRDF colour side (punctual_lights.inc.glsl)
    const f16vec3 V = f16vec3(-rayDir);
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
            // ONE loop over large + cell lights: each loop inlines every light type.
            const uint numLargeLights = min(getLargeLightCount(gridIdx), MAX_LARGE_LIGHTS_PER_GRID);
            const uint cellOffset     = calcCellOffset(gridIdx, gridMin, hit.pos);
            const uint numLights      = numLargeLights + min(getNumLightsForCell(cellOffset), MAX_LIGHTCELL_LIGHTS);
            for (uint i = 0; i < numLights; ++i)
            {
                const uint lightId = i < numLargeLights ? getLargeLightId(gridIdx, i) : getLightId(cellOffset, i - numLargeLights);
                radiance += doLight(in_lightInfos[lightId], hit.pos, V, hitN, f16vec3(0.04), matColOverPi, 0.0, float16_t(0.7));
            }
            break;
        }
        tableIdx = getNextTableIdx(tableIdx);
    }
#endif
    return radiance;
}

// The water body along `dir` from `origin` (a surface point offset to the water side): the scene hit
// (TLAS; the baked terrain height field ONLY where the TLAS cannot answer), shaded, Beer-Lambert
// absorbed over the path and blended into the in-scatter. Bounded at ~99% extinction and "Refraction
// range" (the bottom fades over the range's last 25%: a hard cutoff draws a contour on the seabed).
// surfPos/shoreHW: the pixel's surface point and its shore fetch, the height-field fallback's first
// estimate (bottom measured from the SURFACE point, not the calm level - the waterline band would
// reject otherwise; never rejected when negative: terrain stood behind the water in the depth test, so
// a bottom exists - a map-vs-mesh disagreement otherwise draws a deep-blue line along the shore).
vec3 traceWaterBody(vec3 origin, vec3 dir, vec3 surfPos, vec2 shoreHW, vec3 sunTint, float sunVis, vec3 L, vec3 inscatter, bool rtInRange)
{
    const vec3 sigmaT = u_oceanAbsorption.rgb;
    const float minSigma = max(min(sigmaT.r, min(sigmaT.g, sigmaT.b)), 1e-3);
    const float range = u_oceanParams9.y;
    const float tMax = min(4.6 / minSigma, range);
    SceneHit hit;
    // A ray the TLAS can answer is FINAL: a miss means no bottom within tMax, which is the in-scatter.
    // The height field stands in only where the TLAS cannot answer - rays off ("Ray cutoff dist"), or the
    // ray's reach leaving the TLAS instance range around the scene focus ("RT/TLAS Range").
    const bool traced = rtInRange && distance(surfPos, u_sceneFocus.xyz) + tMax < u_giTrace1.w;
    bool haveHit = rtInRange && traceScene(origin, dir, tMax, true, hit);
    if (!haveHit && !traced && dir.y < -0.02)
    {
        float bottom = surfPos.y - shoreHW.x;
        const float t = max(bottom, 0.02) / -dir.y;
        bottom = surfPos.y - oceanSampleShoreData(surfPos.xz + dir.xz * t).x; // one refinement for sloped shelves
        hit.t = max(bottom, 0.02) / -dir.y;
        // Past tMax the bottom is extinct: the in-scatter, not a hit pulled in to tMax.
        if (hit.t < tMax)
        {
            hit.pos = surfPos + dir * hit.t;
            hit.N = vec3(0.0, 1.0, 0.0);
            hit.albedo = terrainSeabedAlbedo(hit.pos, hit.N, hit.t, hit.waterLevel);
            haveHit = true;
        }
    }
    if (!haveHit)
        return inscatter;
    const float sunPath = max(hit.waterLevel - hit.pos.y, 0.0) / max(L.y, 0.25); // column above the hit
    const vec3 hitRadiance = shadeHit(hit, dir, sunTint * sunVis * exp(-sigmaT * sunPath), L);
    const vec3 T = exp(-sigmaT * hit.t) * (1.0 - smoothstep(0.75 * range, range, hit.t));
    return hitRadiance * T + inscatter * (1.0 - T);
}

// THE EDGE FADE ("Terrain/Water/Ocean edge fade (m)"; 0 = off, returns 1): over its last centimetres of water
// column the ocean composites over the ground and the film drawn before it (the cull puts the ocean in the
// transparent sequence for this), so its mesh no longer ends in a hard line where it cuts the ground - the
// film carries the water on (instanced_indirect_terrain.fs.glsl covers the live ocean's thin edge).
// The column is to the FILM's water surface under this pixel, where the ocean hands over to it: the film's
// tessellated surface (terrain_tess.tes.glsl) without the splat relief - the terrain height, plus the pool
// level the wetness fills the relief band to (terrainPoolLevel), in the ground/beach relief depth with the
// tessellation's distance falloff (0 past it, and with tessellation off: the film then lies on the mesh). At
// the pixel's own position (in_pos.xz), not in_uv's undisplaced lattice point. Ease-out over the band,
// 1 - (1 - t)^2: always transparent at the bottom and mostly opaque above it; the fade width is how fast.
// From nothing main computed: no value of main's stays live for it (the no-map fallback is the open-ocean
// floor oceanSampleShoreData returns, re-derived from the UBO).
float oceanEdgeCover()
{
    if (u_terrainWetParams6.x <= 0.0)
        return 1.0;
    float filmY = u_oceanParams2.w - u_oceanParams1.z;
    if (terrainHeightMapPresent())
    {
        filmY = terrainHeightAt(in_pos.xz);
        float reliefDepth = 0.0;
        if (u_terrainTessParams0.x > 0.5 && u_terrainTexParams0.x >= 0.0 && u_terrainTexParams0.y >= 1.0)
        {
            const float tf = clamp((distance(in_pos, u_viewPos) - u_terrainTessParams1.x)
                / max(u_terrainTessParams1.y - u_terrainTessParams1.x, 1e-3), 0.0, 1.0);
            reliefDepth = u_terrainTessParams1.z * (1.0 - pow(tf, u_terrainTessParams0.w));
        }
        filmY += (terrainPoolLevel(terrainWetnessAt(in_pos.xz), 1.0) - 0.5) * reliefDepth;
    }
    const float s = 1.0 - clamp((in_pos.y - filmY) / u_terrainWetParams6.x, 0.0, 1.0);
    return 1.0 - s * s;
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    // out_factor (the dual-source dst multiplier) is written AT each return, never as a default up here: an
    // early store plus the edge fade's dynamic one at the end kept the output live through the whole shader
    // (80/16 -> 80/32). Every early return (the debug views, the underside) writes it opaque: vec4(0).
    const vec3 up = normalize(u_skyUp);
    const vec3 L  = normalize(u_sunDirection.xyz);
    const vec3 toCam = u_viewPos - in_pos;
    const float viewDist = length(toCam);
    const vec3 V = toCam / max(viewDist, 1e-4);
    const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * u_eclipseParams.x; // per-frame atmosTransmittanceToLight
    // "Ray cutoff dist": beyond it no scene rays at all - the body uses the analytic bottom (the path
    // misses already take), reflection the sky. 0 = unlimited.
    const bool rtInRange = u_oceanParams8.w <= 0.0 || viewDist < u_oceanParams8.w;

    // Wave normal, fold Jacobian (shoaled + raw), LEAN slope variance, vertical acceleration; shoreHW =
    // (terrain height, water level) here, reused by the surf band and SSS below.
    vec2 slope, slopeVar, shoreHW;
    float jacobian, jacobianRaw, accel;
    oceanSampleSurface(in_uv, slope, jacobian, jacobianRaw, slopeVar, accel, shoreHW);
    const float ns = u_oceanParams1.w;
    vec3 N = normalize(vec3(-slope.x * ns, 1.0, -slope.y * ns));
    // Screen derivatives up front: the underside path returns early, and derivatives are undefined in
    // non-uniform control flow.
    const vec3 faceN = cross(dFdx(in_pos), dFdy(in_pos));
    const float uvFootprint = length(fwidth(in_uv));

    // Foam, BEFORE the side split: both sides wear it (the underside sees it from below), and the
    // turbulence fetch takes screen derivatives, which the underside branch cannot.
    // Accumulated turbulence (churn energy of past breaking). Drives aged foam, milkiness and extra
    // roughness.
    const float turbulence = oceanSampleTurbulence(in_uv, uvFootprint);
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

        if (target > 0.001)
        {
            const float b = mix(0.75, 1.45, target) + u_oceanParams8.y; // "Shore foam bias"
            shoreFoam = target * (1.0 - smoothstep(b - 0.4, b + 0.4, jacobianRaw));
            // "Shore foam max": keep the bottom visible through the lace. A soft knee, not a min(): a hard
            // clamp flattened the whole waterline band into a plateau with an edge wherever the target
            // exceeded the cap; this eases toward the cap and never quite reaches it.
            const float foamMax = max(u_oceanParams7.y, 1e-3);
            shoreFoam = foamMax * (1.0 - exp(-shoreFoam / foamMax));
        }
    }
    const float foamW = clamp(max(foam, shoreFoam), 0.0, 1.0);

#if OCEAN_DEBUG_MODE != 0 && OCEAN_DEBUG_MODE != 6
    // The depth-keyed modes are a function of the position alone: before the side split, so the underside
    // shows them too.
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
        out_factor = vec4(0.0);
        return;
    }
#endif

    // Underside (camera on the water side of this triangle): through Snell's window the scene above the
    // water or the sky, outside it (TIR) the mirrored water body, surface foam over both. The side comes from the rasterized triangle's plane, not
    // gl_FrontFacing: the clipmap carries both windings under back-face culling, so the surviving copy
    // is always front-facing.
    if (dot(faceN, V) * dot(faceN, N) < 0.0)
    {
        // Keep the detail normal inside the camera's hemisphere at grazing (a flip would open the window
        // at TIR angles).
        const float nv = dot(N, V);
        if (nv > -1e-3)
            N = normalize(N - V * (nv + 1e-3));
        // The underwater fog carries the water column only within its 100-300 m fade (vol_scatter); the
        // path beyond it is absorbed here, on the same curve. Resolved first: past 0.1% nothing this pixel
        // traces can show (not 2%, the rays' own cut: the window's sun glitter is ~30x the sky).
        const vec3 pathAbsorb = exp(-u_oceanAbsorption.rgb * (viewDist * smoothstep(100.0, 300.0, viewDist)));
        if (max(pathAbsorb.r, max(pathAbsorb.g, pathAbsorb.b)) < 0.001)
        {
            out_color = vec4(0.0, 0.0, 0.0, 1.0);
            out_factor = vec4(0.0);
            return;
        }
        const vec3 ambientSkyU = skyAmbientUp(up);
        const vec3 inscatterU = u_oceanScatter.rgb * u_oceanScatter.w * (ambientSkyU + sunTint * max(L.y, 0.0) / PI);
        // The window's transmission: Fresnel from inside the water - Schlick on the transmitted (air-side)
        // cosine, which reaches 0 at the critical angle - scaled by "Underside transmission". It splits
        // the pixel between the window (trans) and the mirror (1 - trans).
        const vec3 refrUp = refract(-V, -N, 1.33);
        const bool inWindow = dot(refrUp, refrUp) > 1e-6;
        const vec3 tDir = inWindow ? normalize(refrUp) : vec3(0.0, 1.0, 0.0);
        const float trans = inWindow ? (1.0 - F_Schlick(clamp(dot(tDir, N), 0.0, 1.0), 0.02)) * u_oceanParams10.z : 0.0;
        // Surface foam covers both from below (laid over the result at the end), so it scales both rays'
        // weights; under 2% a ray is skipped.
        const float clearW = 1.0 - foamW;
        const bool traceMirror = (1.0 - trans) * clearW > 0.02;
#if OCEAN_DEBUG_MODE == 6
        {
            // The underside's scene ray is the WINDOW ray; outside the window (TIR) there is only the mirror.
            vec3 dbg = vec3(0.0);
            if (!inWindow)
                dbg = traceMirror ? vec3(0.0, 1.0, 1.0) : vec3(0.0, 0.3, 0.3);
#ifdef OCEAN_RT_REFLECTIONS
            else if (trans * clearW <= 0.02)
                dbg = vec3(1.0, 0.0, 0.0);
            else if (!rtInRange)
                dbg = vec3(1.0, 0.0, 1.0);
            else
            {
                SceneHit dbgHit;
                if (traceScene(in_pos + N * 0.05, tDir, u_oceanParams9.z, false, dbgHit))
                    dbg = vec3(0.0, 1.0, 0.0) + vec3(0.8, 0.0, 0.8) * exp(-dbgHit.t * 0.05);
                else if (rtShadowVisibility(in_pos + N * 0.05, tDir, 0.05, 100000.0) < 0.5)
                    dbg = vec3(1.0);
                else if (rtShadowVisibility(u_viewPos, vec3(0.0, -1.0, 0.0), 0.05, 100000.0) < 0.5)
                    dbg = vec3(0.0, 0.0, 1.0);
                else
                    dbg = vec3(0.3);
            }
#endif
            out_color = vec4(dbg, 1.0);
            out_factor = vec4(0.0);
            return;
        }
#endif
        float sunVisU = 0.0; // for the mirrored seabed and the foam's backlight
        if (L.y > 0.0 && (traceMirror || foamW > 0.003))
            sunVisU = u_rtSunShadow > 0.5 ? rtShadowVisibility(in_pos + N * 0.1, L, 0.05, 10000.0) : sampleSunShadowHard(in_pos, N);
        // The mirror: the seabed and submerged shore reflected in the underside - the same traced water
        // body the top side refracts into, along the mirrored ray.
        vec3 color = inscatterU;
        if (traceMirror)
            color = traceWaterBody(in_pos - N * 0.05, reflect(-V, N), in_pos, shoreHW, sunTint, sunVisU, L, inscatterU, rtInRange);
        if (inWindow)
        {
            // The window: the scene above the water along the refracted ray, else the sky with the sun's
            // glitter. The top side's mirror ray in every respect but the fog: the viewer is under water,
            // so the mirror rule has no direct view to match and the LITERAL path is fogged.
            vec3 above;
            bool aboveHit = false;
#ifdef OCEAN_RT_REFLECTIONS
            if (trans * clearW > 0.02 && rtInRange)
            {
                SceneHit hit;
                aboveHit = traceScene(in_pos + N * 0.05, tDir, u_oceanParams9.z, false, hit); // "Reflection range"
                if (aboveHit)
                    above = applyRayFog(shadeHit(hit, tDir, sunTint, L), in_pos, tDir, hit.t, sunTint, L, ambientSkyU);
            }
#endif
            if (!aboveHit)
            {
                above = reflectedSkyRadiance(tDir);
                const float sunDot = max(dot(tDir, L), 0.0);
                above += sunTint * (pow(sunDot, 600.0) * 30.0 + pow(sunDot, 24.0) * 0.6);
                above = applyRayFogSky(above, in_pos, tDir, sunTint, L, ambientSkyU);
            }
            color = mix(color, above, trans);
        }
        // Foam from below: a backlit diffuse sheet - the top side's whitewater light, of which about half
        // comes through.
        if (foamW > 0.003)
        {
            const vec3 foamBelow = u_oceanFoam.rgb * (0.5 * (sunTint * (max(L.y, 0.0) * sunVisU) / PI + ambientSkyU + u_ambientColor));
            color = mix(color, foamBelow, foamW);
        }
        color *= pathAbsorb;
        out_color = vec4(color, 0.0); // alpha 0 = TAA's ocean flag (see the end of main)
        out_factor = vec4(0.0);
        return;
    }

    // The edge fade (oceanEdgeCover); fully faded: nothing to shade.
    const float cover = oceanEdgeCover();
    if (cover <= 0.0)
    {
        out_color = vec4(0.0);
        out_factor = vec4(1.0);
        return;
    }

    if (dot(N, V) < 0.0) // grazing: keep the shading hemisphere consistent
        N = -N;

    // Microfacet roughness = base + spec AA + LEAN slope variance (both scaled by "Glint filtering")
    // + the sub-grid capillary band + turbulence micro-roughness. The variance terms stretch the sun
    // glitter toward the horizon.
    //
    // "Micro roughness" (u_oceanParams9.x): the slope variance of everything BELOW the finest cascade's
    // Nyquist. LEAN returns only what the mip chain removed - exactly zero at mip 0 - so without it the
    // near field falls onto the 0.02 alpha clamp: a mirror, plastic water up close. Not scaled by "Glint
    // filtering" (that trades away FILTERED variance; this band was never in the spectrum). Enters as
    // alpha^2 = 2 sigma^2, like the LEAN term.
    const float perceptualRough = clamp(u_oceanAbsorption.w, 0.02, 1.0);
    const float slopeVariance = 0.5 * (slopeVar.x + slopeVar.y) * (ns * ns);
    const float microVariance = u_oceanParams9.x * (ns * ns);
    const float alphaSq = perceptualRough * perceptualRough * perceptualRough * perceptualRough
        + (normalVariance(N) + 2.0 * slopeVariance) * u_oceanParams6.y + 2.0 * microVariance
        + turbulence * u_oceanParams5.y * 0.35;

    // From here the top side shades in HALF math: the vectors, the dots, the roughness, the ray weights
    // and the colours (the scene colour is RGBA16F and the sun intensity is single digits, so the radiance
    // fits). Positions, ray origins and the traced ray directions stay 32-bit (widened from the half
    // N / V), and each traced result converts to half ONCE. The scene lights accumulate in 32-bit (a
    // light's radiance has no bound).
    const float16_t alphaF = float16_t(clamp(sqrt(alphaSq), 0.02, 1.0));
    // "Reflection max rough"'s gate value (see the mirror below), resolved here so alphaSq dies before the body trace.
    const float alphaGate = sqrt(max(alphaSq - 2.0 * microVariance, 0.0));
    const f16vec3 Nh = f16vec3(N);
    const f16vec3 Vh = f16vec3(V);
    const f16vec3 Lh = f16vec3(L);
    const f16vec3 Hh = normalize(Lh + Vh);
    const float16_t NoV = clamp(dot(Nh, Vh), float16_t(1e-3), float16_t(1.0));
    const float16_t NoL = max(dot(Nh, Lh), float16_t(0.0));
    const float16_t NoH = max(dot(Nh, Hh), float16_t(0.0));
    const float16_t LoH = max(dot(Lh, Hh), float16_t(0.0));
    const float16_t foamH = float16_t(foamW);
    const f16vec3 sunTintH = f16vec3(sunTint);

    // Sun visibility: one RT shadow ray (or PCSS fallback). Back-lit crests still need it while crest
    // SSS is on - the subsurface glow must stay shadow-gated.
    const bool sunUp = L.y > 0.0 && (NoL > float16_t(0.0) || u_oceanParams6.z > 0.0);
    const float16_t sunVis = float16_t(!sunUp ? 0.0
        : (u_rtSunShadow > 0.5 ? rtShadowVisibility(in_pos + vec3(Nh) * 0.1, L, 0.05, 10000.0)
                               : sampleSunShadowHard(in_pos, vec3(Nh)))); // one tap: the moving water hides a penumbra
    const f16vec3 ambientSky = f16vec3(skyAmbientUp(up));
    const f16vec3 whitewater = f16vec3(u_oceanFoam.rgb) * (sunTintH * (NoL * sunVis * float16_t(INV_PI)) + ambientSky + f16vec3(u_ambientColor));

    const f16vec3 inscatter = f16vec3(u_oceanScatter.rgb * u_oceanScatter.w) * (ambientSky + sunTintH * float16_t(max(L.y, 0.0) * INV_PI));

    const float16_t F = F_SchlickH(NoV, float16_t(0.02));

    // Each scene ray's weight in the final pixel, resolved before it is traced: foam covers both,
    // turbidity replaces the body, Fresnel splits the rest, the roughness blur hands part of the mirror
    // to the average sky. Under 2% the ray is skipped.
    const float16_t milk = float16_t(clamp(turbulence * u_oceanParams5.y, 0.0, 1.0)); // entrained bubbles ("Turbidity")
    const float16_t reflBlur = clamp(alphaF * float16_t(2.0) - float16_t(0.05), float16_t(0.0), float16_t(0.6));
    const float16_t clearW = float16_t(1.0) - foamH;
    const float16_t bodyWeight = (float16_t(1.0) - F) * (float16_t(1.0) - milk) * clearW;
    const float16_t mirrorWeight = F * (float16_t(1.0) - reflBlur) * clearW;

    // The pixel is LINEAR in the two traced radiances: body * bodyWeight + mirror * mirrorWeight + C.
    //   color = mix(mix(body, whitewater * 0.55, milk) + sss, mix(mirror, ambientSky, reflBlur), F) + glint
    //   final = mix(color, whitewater, foam)
    // Everything that is not traced - the glint, the crest SSS, the turbidity, the blur's sky share and the
    // foam - folds into C BEFORE the traces, so only C, the weights and the light inputs stay live across
    // them (the ray-query loops are the register peak).
    f16vec3 C = whitewater * (float16_t(0.55) * milk);
    // Crest SSS: sun through back-lit crests glows the scatter color, scaled by height above the calm
    // line. In the transmitted body so Fresnel fades it at grazing like all subsurface light.
    const float sssStrength = u_oceanParams6.z;
    if (sssStrength > 0.0)
    {
        const float16_t waveH = float16_t(max(in_pos.y - shoreHW.y, 0.0));
        const float16_t towardSun = pow(clamp(dot(Vh, -Lh), float16_t(0.0), float16_t(1.0)), float16_t(u_oceanParams6.w));
        const float16_t backSlope = float16_t(0.5) - float16_t(0.5) * dot(Lh, Nh);
        C += f16vec3(u_oceanScatter.rgb) * sunTintH * (float16_t(sssStrength) * waveH * towardSun * backSlope * backSlope * backSlope * sunVis);
    }
    // Sun glint: Cook-Torrance specular, shadow-gated. D * (Vis * NoL) is clamped to the half range
    // (<= ~2e4 at the 0.02 alpha floor), and so is the tinted glint.
    const float16_t D  = D_GGX_H(alphaF, NoH, cross(Nh, Hh));
    const float16_t Vv = V_SmithGGX_H(NoV, NoL, alphaF);
    const float16_t glint = min(D * (Vv * NoL), float16_t(MEDIUMP_FLT_MAX)) * (sunVis * F_SchlickH(LoH, float16_t(0.02)));
    // Crest foam + shoreline surf: the final mix over everything (no longer gated at 0.3% foam - the
    // branch saved nothing once the terms fold here).
    C = clearW * ((float16_t(1.0) - F) * C + (F * reflBlur) * ambientSky + min(sunTintH * glint, f16vec3(MEDIUMP_FLT_MAX)))
      + foamH * whitewater;

    // Refracted body: the traced water column (Beer-Lambert both ways).
    f16vec3 body = inscatter;
    if (bodyWeight > float16_t(0.02))
    {
        const vec3 refrDir = refract(-vec3(Vh), vec3(Nh), 1.0 / 1.33);
        if (dot(refrDir, refrDir) > 1e-6)
            body = f16vec3(traceWaterBody(in_pos + vec3(Nh) * 0.05, refrDir, in_pos, shoreHW, sunTint, float(sunVis), L, vec3(inscatter), rtInRange));
    }
    f16vec3 color = C + body * bodyWeight;

    // Reflection: ray-traced mirror, sky fallback, roughness-blurred toward the average sky.
    vec3 R = reflect(-vec3(Vh), vec3(Nh));
    R.y = max(R.y, 0.02); // keep grazing reflections just above the horizon
    R = normalize(R);
    vec3 reflColor;
    bool mirrorHit = false;
    // "Reflection max rough": a wide lobe can't be one mirror sample. The gate reads the roughness WITHOUT
    // "Micro roughness" - a constant floor on every pixel, which would switch the mirror off everywhere.
#ifdef OCEAN_RT_REFLECTIONS // "Ocean/RT/Reflections"
    if (mirrorWeight > float16_t(0.02) && alphaGate < u_oceanParams9.w && rtInRange)
    {
        SceneHit hit;
        mirrorHit = traceScene(in_pos + vec3(Nh) * 0.05, R, u_oceanParams9.z, false, hit); // "Reflection range"
        if (mirrorHit)
            reflColor = applyReflectionFog(shadeHit(hit, R, sunTint, L), in_pos, R, hit.t, sunTint, L, vec3(ambientSky));
    }
#endif
    // Sky fallback, fogged too (the baked mirror sky carries none). Only on a miss: a hit replaces it whole.
    if (!mirrorHit)
        reflColor = applyReflectionFogSky(reflectedSkyRadiance(R), in_pos, R, sunTint, L, vec3(ambientSky));
#if OCEAN_DEBUG_MODE == 6
    {
        vec3 dbg = vec3(0.0);
#ifdef OCEAN_RT_REFLECTIONS
        SceneHit dbgHit;
        if (mirrorWeight <= float16_t(0.02))
            dbg = vec3(1.0, 0.0, 0.0);
        else if (alphaGate >= u_oceanParams9.w)
            dbg = vec3(1.0, 1.0, 0.0);
        else if (!rtInRange)
            dbg = vec3(1.0, 0.0, 1.0);
        else if (traceScene(in_pos + vec3(Nh) * 0.05, R, u_oceanParams9.z, false, dbgHit))
            dbg = vec3(0.0, 1.0, 0.0) + vec3(0.8, 0.0, 0.8) * exp(-dbgHit.t * 0.05);
        else if (rtShadowVisibility(in_pos + vec3(Nh) * 0.05, R, 0.05, 100000.0) < 0.5)
            dbg = vec3(1.0);           // WHITE: a hit exists, but past "Reflection range"
        else if (rtShadowVisibility(u_viewPos, vec3(0.0, -1.0, 0.0), 0.05, 100000.0) < 0.5)
            dbg = vec3(0.0, 0.0, 1.0); // BLUE: this ray missed, but the TLAS holds geometry (straight below the camera)
        else
            dbg = vec3(0.3);           // GREY: no ray hits anything - the TLAS is empty or not the bound one
#endif
        out_color = vec4(dbg, 1.0);
        out_factor = vec4(0.0);
        return;
    }
#endif
    // The mirror's share (its blurred sky share is in C).
    color += f16vec3(reflColor) * mirrorWeight;

    // Scene lights (shared light-grid walk, RT-shadowed): dielectric specular + in-scatter "diffuse";
    // foam patches respond as lambertian whitewater instead.
    vec3 outColor = vec3(color);
    {
        const f16vec3 matColOverPi = mix(f16vec3(u_oceanScatter.rgb * u_oceanScatter.w), f16vec3(u_oceanFoam.rgb), foamH) * float16_t(INV_PI);
        const float16_t lightRough = clamp(mix(alphaF, float16_t(0.85), foamH), float16_t(0.02), float16_t(1.0));
        const f16vec3 waterSpec = f16vec3(0.02);

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
                // ONE loop over the grid's large lights, then the cell's lights (as the lit core): each loop
                // inlines doLightShadowed whole, light types plus shadow ray queries.
                const uint numLargeLights = min(getLargeLightCount(gridIdx), MAX_LARGE_LIGHTS_PER_GRID);
                const uint cellOffset     = calcCellOffset(gridIdx, gridMin, in_pos);
                const uint numLights      = numLargeLights + min(getNumLightsForCell(cellOffset), MAX_LIGHTCELL_LIGHTS);
                for (uint i = 0; i < numLights; ++i)
                {
                    const uint lightId    = i < numLargeLights ? getLargeLightId(gridIdx, i) : getLightId(cellOffset, i - numLargeLights);
                    const LightInfo light = in_lightInfos[lightId];
                    outColor += doLightShadowed(light, in_pos, Vh, Nh, waterSpec, matColOverPi, 0.0, lightRough);
                }
                break;
            }
            tableIdx = getNextTableIdx(tableIdx);
        }
    }

    // Scene colour ALPHA = TAA's animated-surface flag: 0 marks this pixel as ocean (the waves move
    // without motion vectors, so taa.cs.glsl caps the history weight here). Every other opaque surface
    // writes its material alpha (> 0), and the stages layered over the scene write RGB only. The blend
    // composites the alpha too (out.a = 0 + dst.a * out_factor.a): the flag where the ocean is the larger
    // part of the pixel, the ground's own alpha where the fade shows mostly the ground.
    out_color = vec4(outColor * cover, 0.0);
    out_factor = vec4(vec3(1.0 - cover), cover > 0.5 ? 0.0 : 1.0);
}
