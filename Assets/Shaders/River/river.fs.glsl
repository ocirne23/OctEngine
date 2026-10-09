#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable // the BRDF colour side is half (punctual_lights.inc.glsl)
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// River / lake water (EPipelineIndex::River; Procedural RiverSystem's ribbons and lake surfaces). The ocean's shading
// (ocean.fs.glsl, copied, not shared - the ocean stays as it is): the normal from the ocean's FFT field scaled to the
// river and dragged downstream (river_wave.inc.glsl: the same field the dense near geometry is displaced by) plus a
// flow-mapped noise ripple, the Fresnel split (Schlick, F0 = 0.02) between the RAY-TRACED refraction to
// the bed (Beer-Lambert both ways, "Terrain/Rivers/Surface/Absorption") and the RAY-TRACED mirror (sky fallback), the
// GGX sun glint, TURBULENCE as a smooth gradient (rougher, milkier, higher water; no foam layer on top), the
// scene lights. The RT toggles and ranges are the ocean's ("Ocean/RT":
// OCEAN_RT_REFLECTIONS / OCEAN_HIT_LIGHTS, u_ocean_rt*). Composited DUAL-SOURCE like the ocean: a ribbon fades out over
// its last "Edge softness" of half-width, a lake over its last "Lake edge fade" of water column (the baked height map).
// Writes alpha 0: TAA's animated-surface flag, as the ocean.

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"
#define RIVER_WAVE_TEX(uvLayer) texture(u_oceanMaps, uvLayer)
#include "river_wave.inc.glsl"

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
layout (binding = 23) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
#define CLOUD_SHADOW_BINDING 22
#include "cloud_shadow.inc.glsl"
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
layout (binding = 14, std430) readonly buffer InRTVertices  { MeshVertex in_vertices[]; };
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

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;   // x = across a ribbon (-1 .. 1), 2 = a lake; y = along the river (m)
layout (location = 2) in vec3 in_flow; // xz = the flow (m/s), y = the whitewater amount (rapids, falls)
layout (location = 3) in flat float in_dense; // 1 = a dense near-cell ribbon
layout (location = 4) in float in_waveSize;    // the waves' scale with the river's size (river.vs.glsl)
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// DUAL-SOURCE composite: out = out_color + scene * out_factor, the alpha too. Opaque (factor 0, alpha 0 = TAA's
// animated-surface flag) except over the edge fade, where the bank shows through.
layout (location = 0, index = 0) out vec4 out_color;
layout (location = 0, index = 1) out vec4 out_factor;

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
// Geometric specular AA (the ocean's, at its deliberately low weight: water wants a little shimmer).
float normalVariance(vec3 N)
{
    vec3 dNdx = dFdx(N);
    vec3 dNdy = dFdy(N);
    return min(0.25 * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), 0.03);
}

// Sky for the mirror ray: the per-frame bake (atmosphere.inc.glsl). No sun disc: the GGX glint is its reflection.
vec3 reflectedSkyRadiance(vec3 dir)
{
    return textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR), 0.0).rgb;
}
// The sky light on the water: the hemisphere average of the GI sky SH (the zenith texel with GI off) - as the ocean.
vec3 skyAmbientUp(vec3 up)
{
    if (UBO_LIVE_rt_giStrength > 0.0)
        return max(giEvalSkySH(up) * INV_PI, vec3(0.0));
    return texelFetch(u_skyMap, SKY_MAP_GI_ZENITH_TEXEL, 0).rgb;
}

struct SceneHit
{
    float t;
    vec3 pos;
    vec3 N;
    vec3 albedo;
    float waterLevel; // the water surface above the hit (the column shadeHit / the body attenuate through)
};

// The bed at a ray hit IS the terrain: its own splat at the hit (as the ocean's seabed), with the river bed's beach
// layer (river = 1) and the full-wetness darkening. Albedo only, at a ray-cone LOD.
float g_seabedLod = 0.0;
#define TERRAIN_SPLAT_TEX(tex, uv) textureLod(tex, uv, g_seabedLod)
#define TERRAIN_MACRO_TEX(tex, uv) textureLod(tex, uv, 0.0)
#define TERRAIN_SPLAT_ALBEDO_ONLY
#include "terrain_splat.inc.glsl"

// The NEAR data cascade's weight at xz (terrainDataAt's handover band): only it holds the carved ground. The far cascade
// is the generator's coarse stage, uncarved and smoothed - it stands above most lakes, so a lake read against it vanished.
float nearMapWeight(vec2 xz)
{
    if (!terrainHeightMapPresent())
        return 0.0;
    const vec2 uv = (xz - u_terrain_mapCentre) * u_terrain_mapInvNearSize + 0.5;
    return u_terrain_mapInvFarSize > 0.0 ? 1.0 - smoothstep(0.42, 0.48, max(abs(uv.x - 0.5), abs(uv.y - 0.5))) : 1.0;
}

vec3 riverBedAlbedo(vec3 worldPos, vec3 geoN, float rayT)
{
    TerrainFields f; // mild-climate fallbacks without a map, as the terrain VS
    f.altitude = worldPos.y - u_terrain_seaLevel;
    f.temperature = 12.5;
    f.humidity = 0.5;
    f.waterLevel = u_terrain_seaLevel;
    f.river = 1.0; // under a river: its bed
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(worldPos.xz);
        f.altitude = td.w;
        f.waterLevel = td.y;
        const vec4 climate = terrainClimateNearestAt(worldPos.xz);
        f.humidity = climate.w;
        f.temperature = terrainTemperatureAt(climate, worldPos.y);
    }
    g_seabedLod = clamp(log2(max(rayT, 1.0)) + 1.0, 0.0, 7.0);
    f16vec3 albedo = terrainSplat(worldPos, geoN, geoN, f).albedo;
    if (u_terrainWater_enabled > 0.5)
        albedo *= float16_t(u_terrainWater_darkening);
    return vec3(albedo);
}

// underwaterLevel: the water surface over an underwater ray's hits (the column); < -1e29 for a ray above the water.
bool traceScene(vec3 origin, vec3 dir, float tMax, float underwaterLevel, out SceneHit hit)
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
    hit.waterLevel = underwaterLevel;

    const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
    // Bit 23 = a baked TREE (gi_tlas_instances.cs.glsl): the custom index holds its material, it has no stream entry.
    const bool tree = (instanceIdx & 0x800000) != 0;
    const uint meshIdx = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rq, true);
    if ((tree || uint(instanceIdx) < in_instances.length()) && meshIdx < in_meshInfos.length())
    {
        const InMeshInfo mi = in_meshInfos[meshIdx];
        const uint triBase = mi.firstIndex + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)) * 3u;
        if (triBase + 2u < in_indices.length())
        {
            const uint v0 = uint(mi.vertexOffset) + in_indices[triBase + 0u];
            const uint v1 = uint(mi.vertexOffset) + in_indices[triBase + 1u];
            const uint v2 = uint(mi.vertexOffset) + in_indices[triBase + 2u];
            if (max(max(v0, v1), v2) < in_vertices.length())
            {
                const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
                const vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
                const vec3 objN = in_vertices[v0].normalV.xyz * w.x + in_vertices[v1].normalV.xyz * w.y + in_vertices[v2].normalV.xyz * w.z;
                const vec2 uv = w.x * rtsVertexUV(v0) + w.y * rtsVertexUV(v1) + w.z * rtsVertexUV(v2);
                hit.N = normalize(mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, true)) * objN);
                if (dot(hit.N, dir) > 0.0)
                    hit.N = -hit.N;

                const uint materialIdx = tree ? uint(instanceIdx) & 0xFFFFu : in_instances[instanceIdx].meshIdxMaterialIdx >> 16;
                if (materialIdx < in_materialInfos.length())
                {
                    if ((in_materialInfos[materialIdx].flags & MATERIAL_FLAG_TERRAIN) != 0u)
                        hit.albedo = riverBedAlbedo(hit.pos, hit.N, hit.t);
                    else
                    {
                        const uint diffuseTexIdx = in_materialInfos[materialIdx].diffuseNormalTexIdx & 0x0000FFFFu;
                        const float lod = clamp(log2(max(hit.t, 1.0)) + 1.0, 0.0, 7.0);
                        hit.albedo = textureLod(u_textures[nonuniformEXT(diffuseTexIdx)], uv, lod).rgb;
                    }
                }
            }
        }
    }
    return true;
}

// Sun + GI probe irradiance at a ray hit; grid lights behind OCEAN_HIT_LIGHTS. Analytic only.
vec3 shadeHit(SceneHit hit, vec3 rayDir, vec3 sunRadiance, vec3 L)
{
    const f16vec3 hitN = f16vec3(hit.N);
    const f16vec3 albedo = f16vec3(hit.albedo);
    const f16vec3 sunOverPi = f16vec3(sunRadiance * (max(dot(hit.N, L), 0.0) * INV_PI));
    const f16vec3 indirect = giIndirectOverPiH(hit.pos, hitN) * float16_t(u_rt_giStrength);
    // The ambient reaching an underwater hit Beer-Lamberts down the column too (the water is not in the TLAS).
    const f16vec3 ambientAtten = f16vec3(exp(-u_river_absorption * max(hit.waterLevel - hit.pos.y, 0.0)));
    vec3 radiance = vec3(albedo * (sunOverPi + (indirect + f16vec3(u_ambientColor)) * ambientAtten));

#ifdef OCEAN_HIT_LIGHTS
    const f16vec3 matColOverPi = albedo * float16_t(INV_PI);
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

// The water body along `dir` (refracted) from `origin`: the traced bed (TLAS; the baked height map only where the
// TLAS cannot answer), shaded, Beer-Lambert absorbed over the path and blended into the in-scatter. As the ocean's
// traceWaterBody, the column measured from this pixel's water surface `surfaceY`.
vec3 traceRiverBody(vec3 origin, vec3 dir, vec3 surfPos, vec3 sunTint, float sunVis, vec3 L, vec3 inscatter, bool rtInRange)
{
    const vec3 sigmaT = u_river_absorption;
    const float minSigma = max(min(sigmaT.r, min(sigmaT.g, sigmaT.b)), 1e-3);
    const float range = u_ocean_rtRefractionRange;
    const float tMax = min(4.6 / minSigma, range);
    SceneHit hit;
    const bool traced = rtInRange && distance(surfPos, u_sceneFocus.xyz) + tMax < u_rt_giTlasRange;
    // The ray STARTS BACK along itself: in a shallow stream the TLAS bed (a coarser terrain LOD, the soft carve) can lie
    // AT or just ABOVE the drawn surface - a ray from the surface began under it, missed, and the pixel took the flat
    // in-scatter (a bright band along the banks). The water path counts from the surface (t - back, at least 0).
    const float back = 1.0;
    bool haveHit = rtInRange && traceScene(origin - dir * back, dir, tMax + back, surfPos.y, hit);
    if (haveHit)
        hit.t = max(hit.t - back, 0.0);
    if (!haveHit && !traced && dir.y < -0.02 && nearMapWeight(surfPos.xz) > 0.5)
    {
        const float bottom = surfPos.y - terrainHeightAt(surfPos.xz);
        hit.t = max(bottom, 0.02) / -dir.y;
        if (hit.t < tMax)
        {
            hit.pos = surfPos + dir * hit.t;
            hit.N = vec3(0.0, 1.0, 0.0);
            hit.albedo = riverBedAlbedo(hit.pos, hit.N, hit.t);
            hit.waterLevel = surfPos.y;
            haveHit = true;
        }
    }
    if (!haveHit)
        return inscatter;
    const float sunPath = max(hit.waterLevel - hit.pos.y, 0.0) / max(L.y, 0.25); // the column above the hit
    const vec3 hitRadiance = shadeHit(hit, dir, sunTint * sunVis * exp(-sigmaT * sunPath), L);
    const vec3 T = exp(-sigmaT * hit.t) * (1.0 - smoothstep(0.75 * range, range, hit.t));
    return hitRadiance * T + inscatter * (1.0 - T);
}

// THE RIPPLES: the terrain noise texture's two gradient fBms (R, G: a slope pair) dragged downstream with the flow -
// the classic two-phase flow map (each phase restarts every `period` seconds; the two are half a period apart and
// cross-faded, so no restart shows) - at two scales. Screen derivatives: uniform control flow only (top of main).
vec2 rippleSlope(vec2 xz, vec2 flow, float t)
{
    const float period = 1.6; // s
    const float p0 = fract(t / period);
    const float p1 = fract(t / period + 0.5);
    const float w0 = 1.0 - abs(2.0 * p0 - 1.0);
    const vec2 drift = flow * (period * u_river_invRippleSize);
    vec2 slope = vec2(0.0);
    float amp = 1.0;
    vec2 base = xz * u_river_invRippleSize;
    for (int octave = 0; octave < 2; octave++)
    {
        const vec2 a = texture(TERRAIN_NOISE_SAMPLER, base - drift * p0).rg * 2.0 - 1.0;
        const vec2 b = texture(TERRAIN_NOISE_SAMPLER, base - drift * p1 + vec2(0.37, 0.61)).rg * 2.0 - 1.0;
        slope += (a * w0 + b * (1.0 - w0)) * amp;
        amp *= 0.5;
        base = mat2(0.8, -0.6, 0.6, 0.8) * base * 2.7 + vec2(0.13, 0.71);
    }
    return slope;
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const vec3 up = normalize(u_skyUp);
    const vec3 L  = normalize(u_sunDirection.xyz);
    const vec3 toCam = u_viewPos - in_pos;
    const float viewDist = length(toCam);
    const vec3 V = toCam / max(viewDist, 1e-4);
    const bool lake = in_uv.x > 1.5;
    // A LIGHT ribbon or flat lake quad where the dense near cells are all built ("near covered", RiverSystem per frame): the dense waves are
    // the only surface there. Only sunk under them, the flat ribbon showed as a second water layer from below - a
    // camera inside a deep channel sees both. (Discarding before the derivatives below: a whole ribbon pixel goes, so
    // its quad's neighbours go with it - the coverage is a disc far larger than a pixel.)
    if (in_dense < 0.5 && distance(in_pos, u_views_viewPos[VIEW_CENTER].xyz) < u_river_nearCovered)
        discard; // a light ribbon or a flat lake quad

    // The normal (uniform control flow: the taps take screen derivatives): the WAVES' slope - the same field the dense
    // near geometry is displaced by (river_wave.inc.glsl), so the shading follows the waves drawn - plus the noise
    // ripples as finer detail on top. A lake's still water drifts slowly in one direction (the wind it has no data for)
    // and ripples weaker.
    const vec2 flow = lake ? vec2(0.05, 0.03) * u_river_flowSpeed : in_flow.xz * u_river_flowSpeed;
    const float whitewaterIn = lake ? 0.0 : in_flow.y;
    const float strength = u_river_rippleStrength * (lake ? u_river_lakeRipple : 1.0 + in_flow.y);
    const vec2 slope = riverWaveSlope(in_pos.xz, flow, whitewaterIn) * ((lake ? u_river_lakeRipple : 1.0) * in_waveSize)
        + rippleSlope(in_pos.xz, flow, u_timeSeconds) * strength;
    vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));
    const float variance = normalVariance(N);
    // TURBULENCE, a smooth gradient from calm water - not a separate kind of water: the vertex whitewater amount (a
    // smooth function of how steep the water runs, blurred along the river) x "Foam strength". As it rises the water gets
    // rougher, its body milkier (entrained air: below) and its waves higher (the VS / river_wave.inc.glsl). No foam layer
    // on top (removed, the user 2026-10-09).
    // x the river's SIZE (in_waveSize: its depth / "Full size depth"): a trickle has too little water to churn.
    const float turb = lake ? 0.0 : clamp(in_flow.y * u_river_foamStrength * in_waveSize, 0.0, 1.0);

    // The edge fade: a ribbon over its last "Edge softness" of half-width, a lake over its last "Lake edge fade" of
    // water column (the baked height map; 1 without one).
    float cover = 1.0;
    if (lake)
    {
        // Past the near cascade the fade goes (nearMapWeight): opaque, the terrain mesh cuts the shore by depth.
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
    const vec3 H = normalize(L + V);
    const float NoV = clamp(dot(N, V), 1e-3, 1.0);
    const float NoL = max(dot(N, L), 0.0);
    const float NoH = max(dot(N, H), 0.0);
    const float LoH = max(dot(L, H), 0.0);

    const float sunVis = L.y <= 0.0 ? 0.0
        : (u_rt_sunShadow > 0.5 ? rtShadowVisibility(in_pos + N * 0.1, L, 0.05, 10000.0) : sampleSunShadowHard(in_pos, N));
    const vec3 ambientSky = skyAmbientUp(up);
    const float skyVis = giSkyVisibility(in_pos, up);
    // Entrained air whitens the body with the turbulence: the in-scatter takes the foam colour's share, and (below) the
    // traced bed fades behind it - a riffle goes pale green, a cascade milky.
    const float aeration = 0.6 * turb;
    const vec3 inscatter = mix(u_river_scatterColor, 0.35 * u_river_foamColor, aeration) * (ambientSky + sunTint * (max(L.y, 0.0) * INV_PI));
    const float F = F_Schlick(NoV, 0.02);
    const float reflBlur = clamp(alpha * 2.0 - 0.05, 0.0, 0.6);

    // Glint + the blur's sky share, then the two traced radiances (the ocean's split).
    const float glint = min(D_GGX(NoH, alpha) * V_SmithGGX(NoV, NoL, alpha) * NoL, MEDIUMP_FLT_MAX) * (sunVis * F_Schlick(LoH, 0.02));
    vec3 color = F * reflBlur * skyVis * ambientSky + sunTint * glint;

    // Refracted body: the traced bed through the water column.
    const float bodyWeight = 1.0 - F;
    vec3 body = inscatter;
    if (bodyWeight > 0.02)
    {
        const vec3 refrDir = refract(-V, N, 1.0 / 1.33);
        if (dot(refrDir, refrDir) > 1e-6)
            body = traceRiverBody(in_pos + N * 0.05, refrDir, in_pos, sunTint, sunVis, L, inscatter, rtInRange);
    }
    color += mix(body, inscatter, aeration) * bodyWeight;

    // Reflection: the ray-traced mirror (the ocean's toggle and ranges), sky fallback.
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
        const float16_t lightRough = float16_t(clamp(alpha, 0.02, 1.0));
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
                const uint numLargeLights = min(getLargeLightCount(gridIdx), MAX_LARGE_LIGHTS_PER_GRID);
                const uint cellOffset     = calcCellOffset(gridIdx, gridMin, in_pos);
                const uint numLights      = numLargeLights + min(getNumLightsForCell(cellOffset), MAX_LIGHTCELL_LIGHTS);
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

    // Alpha 0 = TAA's animated-surface flag (the ripples move without motion vectors), as the ocean writes it.
    out_color = vec4(color * cover, 0.0);
    out_factor = vec4(vec3(1.0 - cover), cover > 0.5 ? 0.0 : 1.0);
}
