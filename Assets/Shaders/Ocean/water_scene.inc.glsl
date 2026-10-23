// The scene side of the water surfaces, shared by ocean.fs.glsl and river.fs.glsl (one pipeline layout): the bindings, the
// BRDF terms, the sky lookups, and the RAY-TRACED scene hit (traceScene) with its analytic shading (shadeHit).
// The includer defines WATER_ABSORPTION (vec3, 1/m) before the include, and the bed hook waterBedAlbedo (declared below).
#ifndef WATER_SCENE_INC_GLSL
#define WATER_SCENE_INC_GLSL

#ifndef WATER_ABSORPTION
#error "water_scene.inc.glsl: define WATER_ABSORPTION before the include"
#endif

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

layout (binding = 20) uniform sampler2DArray u_skyMap; // GI's per-frame sky bake (atmosphere.inc.glsl: skyMapUV / SKY_MAP_LAYER_*)
layout (binding = 24) uniform sampler2D u_textures[];  // highest binding in the set: variable descriptor count
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

float D_GGX(float NoH, float a)
{
    const float a2 = a * a;
    const float d  = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * d * d);
}

float V_SmithGGX(float NoV, float NoL, float a) // height-correlated Smith, includes 1/(4 NoV NoL)
{
    const float a2 = a * a;
    const float ggxV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    const float ggxL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (ggxV + ggxL); // callers: NoV >= 1e-3, a >= 0.02
}

float F_Schlick(float u, float f0) // callers pass u in [0, 1]
{
    const float x = 1.0 - u;
    const float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

// Geometric specular AA. Weight / cap far below the usual 0.5 / 0.18: a full-strength term re-widens the lobe by exactly what
// "Glint sharpness" reveals (cancelling the knob), and water wants a little sub-pixel shimmer.
float normalVariance(vec3 N)
{
    const vec3 dNdx = dFdx(N);
    const vec3 dNdy = dFdy(N);
    return min(0.25 * (dot(dNdx, dNdx) + dot(dNdy, dNdy)), 0.03);
}

// Sky for the mirror ray: the per-frame bake of mirrorSkyRadiance (atmosphere.inc.glsl). No sun disc: the GGX glint is its
// reflection.
vec3 reflectedSkyRadiance(vec3 dir)
{
    return textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR), 0.0).rgb;
}

// The sky light on the water: the HEMISPHERE average E(n) / pi of the GI sky SH. Not the sky map's zenith texel (with clouds,
// the one cloud over the camera), which is only the fallback when GI is off (the SH is stale then).
// The strength is read LIVE even while "Ray tracing" is locked: baked, the SH values lived across the traces (registers).
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
    float waterLevel; // the water surface above the hit (the column shadeHit and the body attenuate through)
};

// The bed at a ray hit IS the terrain: the terrain's own splat at the hit, with the baked fields. Albedo only (the water
// column blurs any detail normal away), at a ray-cone LOD (a ray hit has no derivatives).
float g_seabedLod = 0.0;
#define TERRAIN_SPLAT_TEX(tex, uv) textureLod(tex, uv, g_seabedLod)
#define TERRAIN_MACRO_TEX(tex, uv) textureLod(tex, uv, 0.0) // metres-wide texels: the top mip is fine under the water
#define TERRAIN_SPLAT_ALBEDO_ONLY
#include "terrain_splat.inc.glsl"

// The includer's bed at a terrain hit. It may set waterLevel from its own terrain fetch (one fetch serves both).
vec3 waterBedAlbedo(vec3 worldPos, vec3 geoN, float rayT, inout float waterLevel);

// columnLevel: the water surface over an underwater ray's hits; < -1e29 for none (a ray above the water, or one whose column
// waterBedAlbedo or the caller fills in).
bool traceScene(vec3 origin, vec3 dir, float tMax, float columnLevel, out SceneHit hit)
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
    hit.waterLevel = columnLevel;

    // Interpolated normal / uv + material albedo, bounds-checked like rt_shadow.inc.glsl. Bit 23 of the custom index = a
    // baked TREE (gi_tlas_instances.cs.glsl): the index holds its material, it has no stream entry.
    const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
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
                        hit.albedo = waterBedAlbedo(hit.pos, hit.N, hit.t, hit.waterLevel);
                    else
                    {
                        const uint diffuseTexIdx = in_materialInfos[materialIdx].diffuseNormalTexIdx & 0x0000FFFFu;
                        const float lod = min(log2(max(hit.t, 1.0)) + 1.0, 7.0); // ray-cone-ish LOD by distance
                        hit.albedo = textureLod(u_textures[nonuniformEXT(diffuseTexIdx)], uv, lod).rgb;
                    }
                }
            }
        }
    }
    return true;
}

// Sun + GI probe irradiance at a ray hit; grid lights behind OCEAN_HIT_LIGHTS ("Hit lighting" tweak). Analytic only:
// no shadow rays inside what is already a refraction / reflection ray. Half shading, widened once at the end.
vec3 shadeHit(SceneHit hit, vec3 rayDir, vec3 sunRadiance, vec3 L)
{
    const f16vec3 hitN = f16vec3(hit.N);
    const f16vec3 albedo = f16vec3(hit.albedo);
    const f16vec3 sunOverPi = f16vec3(sunRadiance * (max(dot(hit.N, L), 0.0) * INV_PI));
    const f16vec3 indirect = giIndirectOverPiH(hit.pos, hitN) * float16_t(u_rt_giStrength);
    // The ambient reaching an underwater hit Beer-Lamberts down the column too: the probes do not know about the
    // water (it is not in the TLAS), so the bed would read open-air lit at any depth.
    const f16vec3 ambientAtten = f16vec3(exp(-WATER_ABSORPTION * max(hit.waterLevel - hit.pos.y, 0.0)));
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
            const uint numLargeLights = getLargeLightCount(gridIdx);
            const uint cellOffset     = calcCellOffset(gridIdx, gridMin, hit.pos);
            const uint numLights      = numLargeLights + getNumLightsForCell(cellOffset);
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

#endif
