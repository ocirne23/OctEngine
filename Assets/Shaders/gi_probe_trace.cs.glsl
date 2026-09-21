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
layout (binding = 15) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
// Per-wave visit stamps (one uint per trace workgroup): u_frameIndex + 1 on a regular visit, for the irradiance
// volume's partial bake (gi_volume_bake.cs.glsl), which re-bakes only the voxels over this frame's waves.
layout (binding = 14, std430) writeonly buffer GiWaveStamps { uint gi_waveStamp[]; };
layout (binding = 10) uniform sampler2DArray u_skyMap; // the per-frame sky bake (gi_sky_map.cs.glsl; mapping in atmosphere.inc.glsl)
layout (binding = 11) uniform sampler2DArrayShadow u_shadowMap;
// GI probe clipmap volume (persistent SH, read+write for the multi-bounce lookup + temporal blend).
// NOT coherent: every invocation writes only its own probe and reads other probes' cells, where a
// stale (last-visit) value is accepted by design - the qualifier would bypass L1 on the ~30 vec4 loads
// per gather hit that the multi-bounce lookup makes.
layout (binding = 12, std430) buffer GiGridData { vec4 gi_gridData[]; };

// Trace parameters come from the UBO (u_giTrace0 / u_giTrace1 / u_frameIndex), not push constants, so the
// GI command buffer records once: numRays, temporalAlpha, maxRayDist, updateInterval (a probe workgroup
// traces every N frames; fresh probes always trace) and prevViewPos (last frame's scene focus, the
// previous clipmap window for freshness).
#define GI_MAX_RAY_DIST (u_giTrace0.z)
#define GI_DEAD_INTERVAL 8 // a backface-dead probe traces every N-th regular visit
#define GI_VISIT_ALPHA_MAX 0.15    // cap on the per-visit blend alpha the update interval can scale up to
#define GI_FRESH_RAY_MULT 4        // ray count multiplier for a fresh (just scrolled-in) probe's replace visit
#define GI_STEP_LIMIT_REL 2.0      // a visit brighter than (1 + this) x the stored luminance has its blend step limited (huge = off)
#define GI_STEP_LIMIT_MIN 0.25     // ... but never below this fraction of the asked step (bounds the switch-on lag)

// Alpha-masked-aware shadow rays: sun visibility at gather-ray hits, and the grid lights' shadows there
// (GI_LIGHT_RT_SHADOWS - lighting.inc.glsl needs rtShadowVisibility in scope, so this comes first).
#include "rt_shadow.inc.glsl"
#define GI_LIGHT_RT_SHADOWS

// Light grid (read) + shared diffuse lighting.
#define GRID_DATA_NAME  in_gridData
#define GRID_TABLE_NAME in_gridTable
#define TABLE_SIZE_NAME in_tableSize
#include "light_grid.inc.glsl"
#include "lighting.inc.glsl"

// GI probe clipmap (read + write).
#define GI_PROBE_WRITE
#define GI_GRID_DATA_NAME    gi_gridData
#ifdef GI_VOLUME
// The irradiance volume: the multi-bounce lookup at gather hits reads LAST frame's bake (this frame's bake
// runs after the trace) - the probe path read probes this dispatch may be writing, so the lag is the same.
layout (binding = 13) uniform sampler3D u_giVolume[GI_VOLUME_MAX_IMAGES];
#define GI_VOLUME_TEXTURES_NAME u_giVolume
#endif
#include "gi_probe.inc.glsl"

uint hashU(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

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

// Miss radiance: the per-frame sky bake (skyRadiance layer) instead of the analytic march per ray. The
// virtual sky probe (projectSkySH) samples the same bake, so the two agree by construction.
vec3 skyMiss(vec3 d) { return textureLod(u_skyMap, vec3(skyMapUV(d), SKY_MAP_LAYER_GI), 0.0).rgb; }

// View-independent sun visibility from a point: one shadow ray toward the sun via the TLAS. Returns 1
// (lit) or 0 (occluded). Used per gather-ray hit (RT sun mode) so off-screen hits are shadowed
// correctly, unlike the camera-frustum-fit shadow maps.
float sunVisibility(vec3 origin)
{
    return rtShadowVisibility(origin, normalize(u_sunDirection.xyz), 0.05, 1.0e4);
}

vec3 traceRadiance(vec3 origin, vec3 dir, int cascade, out float hitDist, out float backface)
{
    const float rayMax = GI_MAX_RAY_DIST * (cascade + 1);
    hitDist = rayMax; // misses (and out-of-bounds hits) count as open space at the gather range
    backface = 0.0;   // 1 when the committed hit faces away (ray started inside/behind the geometry)
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsOpaqueEXT, 0xFFu, origin, 0.05, dir, rayMax);
    while (rayQueryProceedEXT(rq)) {}

    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT)
        return skyMiss(dir);

    // Bound every post-hit buffer access. A bad meshIdx/triBase/vertex index would otherwise read wildly
    // out of bounds and MMU-fault; treat any out-of-range hit as a miss.
    const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
    if (uint(instanceIdx) >= in_instances.length())
        return skyMiss(dir);
    // Geometry comes from the RT meshIdx the TLAS writer packed into the instance's sbtOffset (a LOD
    // chain traces one shared BLAS, which may differ from the raster-selected level the instance
    // references); the material still comes from the instance.
    const uint meshIdx     = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rq, true);
    const uint materialIdx = in_instances[instanceIdx].meshIdxMaterialIdx >> 16;
    if (meshIdx >= in_meshInfos.length() || materialIdx >= in_materialInfos.length())
        return skyMiss(dir);
    const InMeshInfo mi    = in_meshInfos[meshIdx];

    const int  prim    = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
    const uint triBase = mi.firstIndex + uint(prim) * 3u;
    if (triBase + 2u >= in_indices.length())
        return skyMiss(dir);
    const uint v0 = uint(mi.vertexOffset) + in_indices[triBase + 0u];
    const uint v1 = uint(mi.vertexOffset) + in_indices[triBase + 1u];
    const uint v2 = uint(mi.vertexOffset) + in_indices[triBase + 2u];
    if ((max(max(v0, v1), v2) * 12u + 11u) >= in_vertices.length())
        return skyMiss(dir);

    const vec2 bc    = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    const float t    = rayQueryGetIntersectionTEXT(rq, true);
    const mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
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
    // the carried-forward irradiance for this frame. Probe path: the CHEAP lookup (no Chebyshev, no
    // cross-cascade fade): the result is albedo-scaled and blended at temporalAlpha, so its noise is free
    // and the shading-quality path's ~2x loads are not. Volume path: the shading lookup itself - 4 filtered
    // fetches per cascade, WITH the baked Chebyshev visibility, cheaper than either probe loop.
    float giCov;
#ifdef GI_VOLUME
    vec3 prevE = evalProbeVolumeCoverage(worldPos, worldN, giCov);
#else
    vec3 prevE = giEvalBounce(worldPos, worldN, giCov);
#endif
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
    // From THE SKY MAP (baked + barriered before this dispatch), not the analytic skyRadiance: the
    // virtual probe then matches the miss rays by construction - both see the same filtered bake.
    const vec3 dir = sampleSphere(lane, 64u, vec2(0.0));
    vec3 rad = skyMiss(dir);
    const vec3 up = normalize(u_skyUp);
    // Sky/Ground Horizon (u_groundParams.w): on rolling terrain part of the above-horizon hemisphere is
    // other sunlit ground, not sky - real probes see that as geometry hits; blend the ground in.
    if (dot(dir, up) > 0.0)
        rad = mix(rad, skyMiss(-up), u_groundParams.w);
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
        // injection in main() - unoccluded here (the virtual probe floats in open sky).
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

    // A WAVE (64 lanes) IS A COMPACT 4x4x4 PROBE BLOCK, not 64 consecutive x-row probes: every dim is a
    // power of two >= 4, so the volume tiles exactly. Every per-wave early-out below (update interval,
    // priority, dead probes) only saves time when the WHOLE wave exits, and a block is what is spatially
    // coherent - same distance, same side of the frustum, same side of the ground. The id enumerates
    // toroidal SLOT space, whose 4x4x4 blocks are WORLD-aligned (see giWaveMin for why that matters); lc
    // is the one lattice coord of the current window that lives in the slot.
    const int   cascade  = int(id / uint(GI_CASCADE_PROBES));
    const uint  local    = id - uint(cascade) * uint(GI_CASCADE_PROBES);
    const uvec2 blocksXY = uvec2(GI_PROBE_DIM_X, GI_PROBE_DIM_Y) / 4u;
    const uint  block    = local >> 6, inner = local & 63u;
    const ivec3 slot     = 4 * ivec3(int(block % blocksXY.x), int((block / blocksXY.x) % blocksXY.y), int(block / (blocksXY.x * blocksXY.y)))
                         + ivec3(int(inner & 3u), int((inner >> 2) & 3u), int(inner >> 4));
    const ivec3 origin   = giCascadeOrigin(cascade, u_sceneFocus.xyz);
    const ivec3 lc       = origin + ((slot - origin) & GI_DIM_MASK);
    const ivec3 waveMin  = giWaveMin(lc, origin);
    const int   spacing  = giCascadeSpacing(cascade);

    // A probe is "fresh" when its lattice coord was outside the previous frame's clipmap window for this
    // cascade (it just scrolled in), so we replace rather than blend to converge immediately.
    const bool fresh = giProbeFresh(cascade, lc);

    // Update interval: the wave traces every updateInterval frames - ONE product of every rate factor
    // (giWaveUpdateInterval: "GI/Update Interval Mult" x the distance / out-of-view priority, which a close
    // wave's factor < 1 cancels, down to every frame), with the blend
    // alpha scaled to match (below), so convergence in WALL time holds while the ray count divides by the
    // interval. Interleaved per WORKGROUP (whole waves exit, no half-empty waves); fresh probes always
    // trace - a skipped fresh slot would show the scrolled-out probe's data for a frame.
    const uint updateInterval = giWaveUpdateInterval(cascade, waveMin, spacing);
    const bool visits = giWaveVisits(gl_WorkGroupID.x, updateInterval);
    // Publish the regular visit for the irradiance volume's partial bake: THIS decision, once per wave (the
    // interval is identical on every lane). Fresh probes are the bake's own test (giProbeFresh); the dead-probe
    // skip below may still drop the visit, which only costs the bake an unneeded re-bake.
    if (visits && gl_LocalInvocationIndex == 0u)
        gi_waveStamp[gl_WorkGroupID.x] = u_frameIndex + 1u;
    if (!fresh && !visits)
        return;

    // Relocation: trace from the offset position steered in previous frames (fresh slots hold a scrolled-out
    // probe's offset -> start back on the lattice).
    // The misc vec4 (x = stored backface fraction, yzw = offset) is read ONCE here and rewritten once at
    // the end (giBlendProbeStats); a fresh slot's contents belong to a scrolled-out probe -> zeros.
    const uint cellBase    = giProbeBase(cascade, lc);
    const vec4 prevMisc    = fresh ? vec4(0.0) : gi_gridData[cellBase + GI_MISC_V4];
    const vec3 probeOffset = prevMisc.yzw;
    const vec3 probePos    = vec3(lc) * float(spacing) + probeOffset;

    // Dead-probe skipping: a probe whose stored backface fraction says "embedded" (under the terrain, inside
    // a wall) produces data the lookup rejects, so it traces only every GI_DEAD_INTERVAL-th visit - enough
    // to keep the escape and the wake-up (geometry moved away) working. The exit is per lane, but embedded
    // probes are spatially coherent (whole 4x4x4 blocks below ground), so most waves exit as a unit. An escape
    // visit stores the fraction as exactly DEAD_MAX (see the end), so the escaped probe is NOT skipped on
    // its next visit and refills its history at once.
    if (!fresh && prevMisc.x > GI_BACKFACE_DEAD_MAX
        && ((gl_WorkGroupID.x + u_frameIndex) % (updateInterval * uint(GI_DEAD_INTERVAL))) != 0u)
        return;

    // A fresh probe's visit REPLACES the slot (alpha 1), so its ray count is the whole history: with the
    // priority multiplier the far tiers then refine that snapshot only every few dozen frames. It traces
    // GI_FRESH_RAY_MULT times the rays - half the noise at 4x - and costs little, because only the one
    // probe layer that scrolled in is fresh.
    const uint N = max(uint(u_giTrace0.x), 1u) * (fresh ? uint(GI_FRESH_RAY_MULT) : 1u);
    const float wsh = 4.0 * PI / float(N);
    // The per-visit shift of the ray lattice is an R2 (plastic-number Kronecker) SEQUENCE over the wave's
    // visit number, not a white hash: the temporal blend is an average over the last ~1/alpha visits, and
    // an average of stratified shifts converges ~1/n where random shifts converge 1/sqrt(n) - the same
    // rays, a much steadier blend. The per-probe hash only decorrelates neighbours. Integer fixed point
    // (the multipliers are 0.7548776662 and 0.5698402910 x 2^32; the wrap IS the fract): a float product
    // loses its fraction after a few hundred thousand frames. The visit number is exact on a regular visit
    // ((workgroup + frame) is a multiple of the interval there) and steps by 1 while the interval holds.
    const uint visit  = (gl_WorkGroupID.x + u_frameIndex) / updateInterval;
    const uint seed   = hashU(id);
    const vec2 jitter = vec2(float((seed + visit * 3242174889u) >> 8),
                             float((hashU(seed) + visit * 2447445414u) >> 8)) / float(0x01000000u);

    vec3 c0 = vec3(0.0), c1 = vec3(0.0), c2 = vec3(0.0), c3 = vec3(0.0);
    vec4  dsh = vec4(0.0), d2sh = vec4(0.0); // SH-L1 depth moments for Chebyshev visibility at lookup
    const float depthCap = GI_DEPTH_CAP_SPACING * float(spacing);
    // Relocation (embedded probes only): the closest backface hit whose escape target lands INSIDE the
    // offset clamp. A probe deeper in solid geometry than the clamp can never get out - stepping + clamping
    // it every visit along that visit's random closest ray made it jump around the clamp sphere forever -
    // so such a probe holds still (it is backface-dead at lookup anyway).
    const float escapeMargin = 0.125  * float(spacing); // how far past the backface the probe lands
    const float maxLen       = 0.45 * float(spacing);
    float backfaceSum = 0.0;
    float closestBack = 1e30;
    vec3  escapeOffset = vec3(0.0);
    for (uint i = 0u; i < N; ++i)
    {
        const vec3 dir = sampleSphere(i, N, jitter);
        float hitDist, backface;
        const vec3 radiance = traceRadiance(probePos, dir, cascade, hitDist, backface);
        backfaceSum += backface;
        if (backface > 0.5 && hitDist < closestBack)
        {
            const vec3 target = probeOffset + dir * (hitDist + escapeMargin);
            if (dot(target, target) <= maxLen * maxLen)
            {
                closestBack = hitDist;
                escapeOffset = target;
            }
        }
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
    // center toward up (toggleable) - probes float in open space, so this is a soft, low-noise gate, and
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

    // Relocation update: an embedded probe punches through the closest backface along the ray that found it
    // - one discrete step, no continuous steering. Front-face back-off and a drift home were tried and
    // removed: every continuous rule reads a minimum over a few jittered rays, and the probes never came to
    // rest. The escape target is already inside the clamp (filtered above), so no clamp is needed here; an
    // offset is otherwise held as is (a fresh slot starts at zero).
    vec3 newOffset = probeOffset;
    const bool escaped = backFrac > 0.25 && closestBack < 1e29;
    if (escaped)
        newOffset = escapeOffset;

    // u_giTrace0.y is THIS FRAME's blend (the CPU already rescaled "GI/Temporal Alpha" by the wall delta, so
    // convergence is frame-rate independent). A visit every updateInterval frames compounds it over the
    // interval - 1 - (1 - a)^k, which saturates where the linear a * k overshoots - so a slow wave converges
    // at the same WALL-time rate, up to GI_VISIT_ALPHA_MAX: past it one N-ray visit would dominate the
    // history and the probe would flicker at its visit rate, so the slow tiers converge slower instead.
    // (A per-frame alpha already above the cap is taken as asked.)
    // A CLEARED slot (all-zero irradiance: grid reset, start-up) is replaced like a fresh one - blending up
    // from zero is ~1/alpha visits of a too-dark field, and the step limiter below would read it as an
    // infinite relative change. (A truly black probe replaces black with black.)
    const vec3  lumaW   = vec3(0.2126, 0.7152, 0.0722);
    const float oldLuma = dot(gi_gridData[cellBase].xyz, lumaW); // [0].xyz = the stored SH DC term
    const bool  replace = fresh || oldLuma <= 0.0;
    const float frameAlpha = u_giTrace0.y;
    //const float visitAlpha = max(frameAlpha, frameAlpha * float(updateInterval)); //max(frameAlpha, min(1.0 - pow(1.0 - frameAlpha, float(updateInterval)), GI_VISIT_ALPHA_MAX));
    const float visitAlpha = max(frameAlpha, min(1.0 - pow(1.0 - frameAlpha, float(updateInterval)), GI_VISIT_ALPHA_MAX));
    float alpha = replace ? 1.0 : visitAlpha;
    if (!replace)
    {
        // The stored moments were traced from the old position: after a relocation step, blend faster in
        // proportion to how far the probe moved so the depth/irradiance history re-syncs in a few frames
        // instead of ~1/temporalAlpha frames of parallax-wrong Chebyshev data.
        const float moved = length(newOffset - probeOffset);
        alpha = max(alpha, 0.5 * clamp(moved / (0.25 * float(spacing)), 0.0, 1.0));

        // A probe whose stored backface fraction says "embedded" but whose rays now mostly hit frontfaces
        // has just escaped: flush the near-black inside-the-wall history quickly - but softly (a hard
        // alpha-1 replace would stamp a single noisy N-ray snapshot that then persists for ~1/alpha
        // frames). This refires for a few frames while the stored fraction descends, averaging the reset.
        if (prevMisc.x >= GI_BACKFACE_DEAD_MAX && backFrac < GI_BACKFACE_DEAD_MIN)
            alpha = max(alpha, 0.35);
    }

    // Step limiter (the irradiance blend only; the depth stats take the plain alpha). One N-ray visit of a
    // high-variance integrand - a small sunlit patch, a lamp: a ray hits it or not - can land several times
    // above the converged value, and each such visit kicks the blend; the kicks are what reads as flicker.
    // Only BRIGHTENING can be an outlier (a visit cannot go below zero), so only a visit more than
    // (1 + GI_STEP_LIMIT_REL) x the stored DC luminance is limited: its step shrinks to what a visit AT
    // that bound would have made, but never below GI_STEP_LIMIT_MIN of the asked step - so a real
    // switch-on is slowed by a bounded factor, and only until the history has climbed to within the bound.
    // Costs: a small downward bias in probes whose visits are often limited, and that switch-on lag.
    // Boosted visits (relocation, just escaped) are deliberate fast replaces and skip it.
    float shAlpha = alpha;
    if (!replace && alpha == visitAlpha)
    {
        const float rel = (dot(c0, lumaW) - oldLuma) / oldLuma;
        if (rel > GI_STEP_LIMIT_REL)
            shAlpha *= max(GI_STEP_LIMIT_REL / rel, GI_STEP_LIMIT_MIN);
    }

    giBlendCell(cellBase, c0, c1, c2, c3, shAlpha);
    // An escape visit pins the stored fraction to exactly DEAD_MAX: still dead at lookup and still
    // triggering the just-escaped flush above (>=), but not skipped by the dead-probe interval (>), so the
    // probe traces from its new position on the very next visit instead of GI_DEAD_INTERVAL visits later.
    const float storedFrac = escaped ? GI_BACKFACE_DEAD_MAX : prevMisc.x;
    const float visitFrac  = escaped ? GI_BACKFACE_DEAD_MAX : backFrac;
    giBlendProbeStats(cellBase, dsh, d2sh, storedFrac, visitFrac, newOffset, alpha); // depth moments + embedded-probe stats + offset
}
