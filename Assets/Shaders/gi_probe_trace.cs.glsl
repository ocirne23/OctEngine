#version 460

#extension GL_EXT_ray_query : require
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_EXT_nonuniform_qualifier : enable
//#extension GL_EXT_debug_printf : enable

#include "shared.inc.glsl"

#define GI_ALBEDO_LOD 3.0 // sample a coarse mip for hit albedo (no ray-hit derivatives anyway)

struct LightInfo
{
    vec3 pos;
    float range;
    vec3 color;
    float width;
    vec3 direction;
    float rotation;
};
struct MaterialInfo
{
    uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
    uint metalRoughnessTexIdxAlphaMode;
};
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

layout (binding = 1, std430) readonly buffer InLightInfos { LightInfo in_lightInfos[]; };
layout (binding = 2, std430) readonly buffer InLightGrid  { uint in_gridData[]; };
layout (binding = 3, std430) readonly buffer InGridTable
{
    uint in_numGrids;
    uint in_gridDataCounter;
    uint in_tableSize;
    uint in_gridTable[];
};
layout (binding = 4) uniform accelerationStructureEXT u_tlas;
layout (binding = 5, std430) readonly buffer InVertices    { float in_vertices[]; }; // MeshVertex as 12 floats
layout (binding = 6, std430) readonly buffer InIndices     { uint in_indices[]; };
layout (binding = 7, std430) readonly buffer InMeshInfos   { InMeshInfo in_meshInfos[]; };
layout (binding = 8, std430) readonly buffer InInstances   { InMeshInstance in_instances[]; };
layout (binding = 9, std430) readonly buffer InMaterials   { MaterialInfo in_materialInfos[]; };
layout (binding = 13) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
layout (binding = 11) uniform sampler2DArrayShadow u_shadowMap;
// GI probe clipmap volume (persistent SH, read+write for the multi-bounce lookup + temporal blend).
layout (binding = 12, std430) coherent buffer GiGridData { float gi_gridData[]; };

layout (push_constant) uniform PushConstants
{
    uint  frameIndex;
    uint  numRays;
    float temporalAlpha;
    float maxRayDist;
    vec3  prevViewPos; // last frame's scene focus (drives the previous clipmap window for freshness)
    float _pad;
} pc;

// Light grid (read) + shared diffuse lighting.
#define GRID_DATA_NAME  in_gridData
#define GRID_TABLE_NAME in_gridTable
#define TABLE_SIZE_NAME in_tableSize
#include "light_grid.inc.glsl"
#include "lighting.inc.glsl"

// GI probe clipmap (read + write).
#define GI_PROBE_WRITE
#define GI_GRID_DATA_NAME    gi_gridData
#include "gi_probe.inc.glsl"

// Alpha-masked-aware shadow rays (sun visibility at gather-ray hits).
#include "rt_shadow.inc.glsl"

uint hashU(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float hashToFloat(uint x) { return float(hashU(x) & 0x00FFFFFFu) / float(0x01000000u); }

// jitter = the two per-probe random offsets (loop-invariant; computed once by the caller).
vec3 sampleSphere(uint i, uint n, vec2 jitter)
{
    float u1 = fract(float(i) * 0.61803398875 + jitter.x);
    float u2 = (float(i) + jitter.y) / float(n);
    float z = 1.0 - 2.0 * u1;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * PI * u2;
    return vec3(r * cos(phi), r * sin(phi), z);
}

vec3 vNormal(uint vi) { uint b = vi * 12u; return vec3(in_vertices[b + 3u], in_vertices[b + 4u], in_vertices[b + 5u]); }
vec2 vUV(uint vi)     { uint b = vi * 12u; return vec2(in_vertices[b + 10u], in_vertices[b + 11u]); }

// View-independent sun visibility from a point: one shadow ray toward the sun via the TLAS. Returns 1
// (lit) or 0 (occluded). Used per gather-ray hit (RT sun mode) so off-screen hits are shadowed
// correctly, unlike the camera-frustum-fit shadow maps.
float sunVisibility(vec3 origin)
{
    return rtShadowVisibility(origin, normalize(u_sunDirection.xyz), 0.05, 1.0e4);
}

vec3 traceRadiance(vec3 origin, vec3 dir, int cascade, out float hitDist, out float backface)
{
    const float rayMax = pc.maxRayDist * (cascade + 1);
    hitDist = rayMax; // misses (and out-of-bounds hits) count as open space at the gather range
    backface = 0.0;   // 1 when the committed hit faces away (ray started inside/behind the geometry)
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsOpaqueEXT, 0xFFu, origin, 0.05, dir, rayMax);
    while (rayQueryProceedEXT(rq)) {}

    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT)
        return skyRadiance(dir);

    const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
    const int prim        = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
    const vec2 bc         = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    const float t         = rayQueryGetIntersectionTEXT(rq, true);
    const mat4x3 o2w      = rayQueryGetIntersectionObjectToWorldEXT(rq, true);

    // Bound every post-hit buffer access. A bad meshIdx/triBase/vertex index would otherwise read wildly
    // out of bounds and MMU-fault; treat any out-of-range hit as a miss.
    if (uint(instanceIdx) >= in_instances.length())
        return skyRadiance(dir);
    // Geometry comes from the RT meshIdx the TLAS writer packed into the instance's sbtOffset (a LOD
    // chain traces one shared BLAS, which may differ from the raster-selected level the instance
    // references); the material still comes from the instance.
    const uint meshIdx     = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rq, true);
    const uint materialIdx = in_instances[instanceIdx].meshIdxMaterialIdx >> 16;
    if (meshIdx >= in_meshInfos.length() || materialIdx >= in_materialInfos.length())
        return skyRadiance(dir);
    const InMeshInfo mi    = in_meshInfos[meshIdx];

    const uint triBase = mi.firstIndex + uint(prim) * 3u;
    if (triBase + 2u >= in_indices.length())
        return skyRadiance(dir);
    const uint v0 = uint(mi.vertexOffset) + in_indices[triBase + 0u];
    const uint v1 = uint(mi.vertexOffset) + in_indices[triBase + 1u];
    const uint v2 = uint(mi.vertexOffset) + in_indices[triBase + 2u];
    if ((max(max(v0, v1), v2) * 12u + 11u) >= in_vertices.length())
        return skyRadiance(dir);

    const vec3 b = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    const vec3 objN = normalize(b.x * vNormal(v0) + b.y * vNormal(v1) + b.z * vNormal(v2));
    vec3 worldN = normalize(mat3(o2w) * objN);
    const vec2 uv = b.x * vUV(v0) + b.y * vUV(v1) + b.z * vUV(v2);
    const vec3 worldPos = origin + dir * t;
    hitDist = t; // committed surface hit -> actual distance to geometry along this ray
    if (dot(worldN, dir) > 0.0)
    {
        worldN = -worldN;
        backface = 1.0;
    }

    const uint diffuseTexIdx = in_materialInfos[materialIdx].diffuseNormalTexIdx & 0x0000FFFFu;
    const vec3 albedo = textureLod(u_textures[nonuniformEXT(diffuseTexIdx)], uv, GI_ALBEDO_LOD).rgb;

    g_sunShadowOverride = sunVisibility(worldPos + worldN * 0.02);
    vec3 radiance = giGatherDirect(worldPos, worldN, albedo);
    // Previous-frame indirect at the hit -> multi-bounce (infinite, temporally). The cur SH already holds
    // the carried-forward irradiance for this frame.
    float giCov;
    vec3 prevE = evalProbeSHCoverage(worldPos, worldN, giCov);
    if (prevE.x >= 0.0) // fade the multi-bounce with coverage so traced hits near the field's edge don't step
        radiance += albedo * (prevE / PI) * giCov;
    return radiance;
}

layout(local_size_x = 64) in;

// Virtual sky probe (GI_SKY_SH_BASE): the last workgroup of the dispatch cooperatively projects
// skyRadiance over the sphere into the extra SH-L1 slot the out-of-field fallback evaluates. Fixed
// (unjittered) direction set: the integrand is analytic and smooth, so 64 deterministic samples give a
// stable projection with no temporal blend.
shared vec3 s_skySH[4 * 64];

void projectSkySH(uint lane)
{
    const vec3 dir = sampleSphere(lane, 64u, vec2(0.0));
    vec3 rad = skyRadiance(dir);
    const vec3 up = normalize(u_skyUp);
    // Sky/Ground Horizon (u_groundParams.w): on rolling terrain part of the above-horizon hemisphere is
    // other sunlit ground, not sky — real probes see that as geometry hits; blend the analytic ground in.
    if (dot(dir, up) > 0.0)
        rad = mix(rad, skyRadiance(-up), u_groundParams.w);
    const float wsh = 4.0 * PI / 64.0;
    const vec4 Y = shBasisL1(dir) * wsh;
    s_skySH[lane]        = rad * Y.x;
    s_skySH[lane + 64u]  = rad * Y.y;
    s_skySH[lane + 128u] = rad * Y.z;
    s_skySH[lane + 192u] = rad * Y.w;
    barrier();
    for (uint s = 32u; s > 0u; s >>= 1u)
    {
        if (lane < s)
        {
            s_skySH[lane]        += s_skySH[lane + s];
            s_skySH[lane + 64u]  += s_skySH[lane + s + 64u];
            s_skySH[lane + 128u] += s_skySH[lane + s + 128u];
            s_skySH[lane + 192u] += s_skySH[lane + s + 192u];
        }
        barrier();
    }
    if (lane == 0u)
    {
        vec3 c0 = s_skySH[0], c1 = s_skySH[64], c2 = s_skySH[128], c3 = s_skySH[192];
        // Direct sky-radiance light (moonlight / space light) delta projection, matching the per-probe
        // injection in main() — unoccluded here (the virtual probe floats in open sky).
        if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
        {
            const vec4 Ysky = shBasisL1(up);
            c0 += u_skyRadianceColor * Ysky.x;
            c1 += u_skyRadianceColor * Ysky.y;
            c2 += u_skyRadianceColor * Ysky.z;
            c3 += u_skyRadianceColor * Ysky.w;
        }
        giStoreCell(GI_SKY_SH_BASE, c0, c1, c2, c3);
    }
}

void main()
{
    const uint numProbes = uint(GI_NUM_CASCADES) * uint(GI_CASCADE_PROBES);
    if (gl_WorkGroupID.x == numProbes / 64u) // the extra workgroup past the probes (probe count is a multiple of 64)
    {
        projectSkySH(gl_LocalInvocationID.x);
        return;
    }
    const uint id = gl_GlobalInvocationID.x;
    if (id >= numProbes)
        return;

    const int  cascade = int(id / uint(GI_CASCADE_PROBES));
    const uint local   = id - uint(cascade) * uint(GI_CASCADE_PROBES);
    const uint DX      = uint(GI_PROBE_DIM_X), DY = uint(GI_PROBE_DIM_Y);
    const ivec3 oc     = ivec3(int(local % DX), int((local / DX) % DY), int(local / (DX * DY)));

    const int   spacing    = giCascadeSpacing(cascade);
    const ivec3 lc         = giCascadeOrigin(cascade, u_sceneFocus.xyz) + oc;
    const vec3  probeCenter = vec3(lc) * float(spacing);

    // A probe is "fresh" when its lattice coord was outside the previous frame's clipmap window for this
    // cascade (it just scrolled in), so we replace rather than blend to converge immediately.
    const ivec3 prevOrigin = giCascadeOrigin(cascade, pc.prevViewPos);
    const bool  fresh = any(lessThan(lc, prevOrigin)) || any(greaterThanEqual(lc, prevOrigin + GI_PROBE_DIMS));

    // Relocation: trace from the offset position steered in previous frames (fresh slots hold a scrolled-out
    // probe's offset -> start back on the lattice).
    const uint cellBase = giProbeBase(cascade, lc);
    const vec3 probeOffset = fresh ? vec3(0.0) : giProbeOffset(cellBase);
    const vec3 probePos    = probeCenter + probeOffset;

    const uint N = max(pc.numRays, 1u);
    const float wsh = 4.0 * PI / float(N);
    const uint seed = hashU(id ^ (pc.frameIndex * 0x9e3779b9u));
    const vec2 jitter = vec2(hashToFloat(seed), hashToFloat(seed ^ 0x9e3779b9u));

    vec3 c0 = vec3(0.0), c1 = vec3(0.0), c2 = vec3(0.0), c3 = vec3(0.0);
    vec4  dsh = vec4(0.0), d2sh = vec4(0.0); // SH-L1 depth moments for Chebyshev visibility at lookup
    const float depthCap = GI_DEPTH_CAP_SPACING * float(spacing);
    float backfaceSum = 0.0;
    float closestBack = 1e30, closestFront = 1e30;
    vec3  closestBackDir = vec3(0.0), closestFrontDir = vec3(0.0);
    for (uint i = 0u; i < N; ++i)
    {
        const vec3 dir = sampleSphere(i, N, jitter);
        float hitDist, backface;
        const vec3 radiance = traceRadiance(probePos, dir, cascade, hitDist, backface);
        backfaceSum += backface;
        if (backface > 0.5)
        {
            if (hitDist < closestBack) { closestBack = hitDist; closestBackDir = dir; }
        }
        else if (hitDist < closestFront) { closestFront = hitDist; closestFrontDir = dir; }
        const vec4 Y = shBasisL1(dir);
        c0 += radiance * (Y.x * wsh);
        c1 += radiance * (Y.y * wsh);
        c2 += radiance * (Y.z * wsh);
        c3 += radiance * (Y.w * wsh);
        const float dc = min(hitDist, depthCap);
        dsh  += Y * (dc * wsh);
        d2sh += Y * (dc * dc * wsh);
    }

    // Sky radiance (moonlight / space light): a directional delta light can't be hit by gather rays, so
    // its direct irradiance is projected straight into the probe SH (one SH-L1 delta projection; eval's
    // cosine convolution then yields ~E * max(dot(n, up), 0)). Visibility is a single ray from the probe
    // center toward up (toggleable) — probes float in open space, so this is a soft, low-noise gate, and
    // the temporal blend smooths it further. Bounces arrive for free through the prevE multi-bounce.
    if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
    {
        const vec3 skyL = normalize(u_skyUp);
        const float skyVis = u_rtSkyRadiance > 0.5 ? rtShadowVisibility(probePos, skyL, 0.05, 1.0e4) : 1.0;
        const vec3 cd = u_skyRadianceColor * skyVis;
        const vec4 Ysky = shBasisL1(skyL);
        c0 += cd * Ysky.x;
        c1 += cd * Ysky.y;
        c2 += cd * Ysky.z;
        c3 += cd * Ysky.w;
    }

    const float backFrac = backfaceSum / float(N);

    // Relocation update (RTXGI-style, simplified). Embedded probes punch through the closest backface
    // along the ray that found it; probes grazing a wall back off along the ray that found the closest
    // frontface; comfortable probes drift home so the offset doesn't fossilize around moved geometry.
    // Clamped to a fraction of the spacing so the probe stays representative of its trilinear cell.
    // closestFront/closestBack are minima over few jittered rays — very noisy estimators — so every
    // steering step except the discrete punch-through is damped: raw per-frame corrections make the probe
    // position (and with it the whole Chebyshev visibility field) wobble frame to frame.
    const float minFront = 0.15 * float(spacing);
    vec3 newOffset = probeOffset;
    if (backFrac > 0.25 && closestBack < 1e29)
        newOffset += closestBackDir * (closestBack + minFront * 0.5); // escape is all-or-nothing: full step
    else if (closestFront < minFront)
        newOffset -= closestFrontDir * (0.5 * (minFront - closestFront));
    else if (closestFront > minFront * 1.5)
        newOffset *= 0.99;
    const float maxLen = 0.45 * float(spacing);
    const float offLen = length(newOffset);
    if (offLen > maxLen)
        newOffset *= maxLen / offLen;

    float alpha = fresh ? 1.0 : pc.temporalAlpha;
    if (!fresh)
    {
        // The stored moments were traced from the old position: after a relocation step, blend faster in
        // proportion to how far the probe moved so the depth/irradiance history re-syncs in a few frames
        // instead of ~1/temporalAlpha frames of parallax-wrong Chebyshev data.
        const float moved = length(newOffset - probeOffset);
        alpha = max(alpha, 0.5 * clamp(moved / (0.25 * float(spacing)), 0.0, 1.0));

        // A probe whose stored backface fraction says "embedded" but whose rays now mostly hit frontfaces
        // has just escaped: flush the near-black inside-the-wall history quickly — but softly (a hard
        // alpha-1 replace would stamp a single noisy N-ray snapshot that then persists for ~1/alpha
        // frames). This refires for a few frames while the stored fraction descends, averaging the reset.
        if (giProbeBackfaceFrac(cellBase) > GI_BACKFACE_DEAD_MAX && backFrac < GI_BACKFACE_DEAD_MIN)
            alpha = max(alpha, 0.35);
    }

    giBlendCell(cellBase, c0, c1, c2, c3, alpha);
    giBlendProbeStats(cellBase, dsh, d2sh, backFrac, alpha); // depth moments + embedded-probe stats for lookup
    giStoreProbeOffset(cellBase, newOffset);
}
