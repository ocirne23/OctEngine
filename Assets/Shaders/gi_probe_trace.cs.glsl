#version 460

#extension GL_EXT_ray_query : require
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_EXT_nonuniform_qualifier : enable
//#extension GL_EXT_debug_printf : enable

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"

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
layout (binding = 5, std430) readonly buffer InVertices    { MeshVertex in_vertices[]; };
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
// ENCLOSURE: the backface fraction counts as "embedded" only as far as the backfaces dominate the HITS. From
// inside a closed solid (terrain, a wall, a trunk) a ray reaches only the inside of the shell - every hit is a
// backface. Among double-sided geometry (a tree canopy: double-sided leaf diamonds, cards traced opaque) about
// half the hits are backfaces; there the raw fraction relocated the probe on every visit and marked it dead.
#define GI_ENCLOSED_SHARE_MIN 0.6  // backface share of the hits at which a probe starts to count as embedded ...
#define GI_ENCLOSED_SHARE_MAX 0.85 // ... and fully does
// The visit clamp and change detection (main, after the trace): see the comment there.
#define GI_CLAMP_SIGMA 2.5         // a visit's DC luminance is clamped to the stored luminance +- this many sigma
#define GI_SIGMA_MIN_REL 0.1       // sigma floor, relative to the stored luminance (a converged, quiet probe still moves)
#define GI_SIGMA_INIT_REL 0.5      // the sigma a replaced probe starts from, relative to its luminance
#define GI_FAST_MEAN_ALPHA 0.3     // blend of the fast visit-luminance mean (change detection)
#define GI_CHANGE_SIGMA 2.0        // the fast mean this many sigma off the stored luminance ...
#define GI_CHANGE_CONFIRM_SIGMA 1.0 // ... and the previous fast mean this many, on the same side = a real change
#define GI_CHANGE_ALPHA 0.3        // the blend of a visit in a real change (unclamped)

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

// Miss radiance: the per-frame sky bake (skyRadiance layer) instead of the analytic march per ray. The
// virtual sky probe (projectSkySH) samples the same bake, so the two agree by construction.
vec3 skyMiss(vec3 d) { return textureLod(u_skyMap, vec3(skyMapUV(d), SKY_MAP_LAYER_GI), 0.0).rgb; }

// A gather MISS: THE SKY SH's radiance (the virtual sky probe, giSkySHRadiance), not the sky map. A probe in open
// space then gathers exactly the integral the out-of-field fallback evaluates (giIrradiance -> giEvalSkySHCloud),
// cloud dimming included: under a cloud the probes were darker than the area past the field. (From the sky map,
// a miss in a cone around the sun counted all of its radiance as SUN - the aureole, a small bright target that a
// few rays per visit hit or not - and the lookup dimmed it again under a cloud the averaged GI layer had already
// counted in; the fallback never dimmed it.) Its sun part is the sky SH's own (giSkySunIrradiance): the sunlit
// ground G below the horizon, its "Ground Horizon" share w G above - as the STEP, not its L1 form (which goes
// negative toward the zenith for w < 0.2): the probe keeps only the sun's DC, and the two have the same DC.
// Volume mode reads LAST frame's sky SH (the bake's copy); probe mode reads the slot this dispatch's last workgroup
// rewrites - a frame-to-frame change only, never a value from elsewhere.
// skyOpen = the miss's weight in the sky visibility (its cosine to up, 0 below the horizon).
// Everything from the UBO is derived HERE, per miss, not hoisted into main: a value held across the ray loop
// is a register through the whole ray query (measured: 96 -> 128 with the sun direction, the ground sun, up and
// the loop's sky sums held there).
vec3 traceMiss(vec3 d, out float sunLuma, out float skyOpen)
{
    const vec3  up    = normalize(u_skyUp);
    const float cosUp = dot(d, up);
    skyOpen = max(cosUp, 0.0);
    sunLuma = dot(skyGroundSun(up), GI_LUMA_W) * (cosUp < 0.0 ? 1.0 : u_groundParams.w);
    return giSkySHRadiance(d);
}

// View-independent sun visibility from a point: one shadow ray toward the sun via the TLAS. Returns 1
// (lit) or 0 (occluded). Used per gather-ray hit (RT sun mode) so off-screen hits are shadowed
// correctly, unlike the camera-frustum-fit shadow maps.
float sunVisibility(vec3 origin)
{
    return rtShadowVisibility(origin, normalize(u_sunDirection.xyz), 0.05, 1.0e4);
}

// sunLuma = the luminance of the returned radiance's SUN part (the direct sun + its share of the multi-bounce),
// traced WITHOUT the cloud shadow - see GI_SUN_V4 in gi_probe.inc.glsl. skyOpen = a miss's sky-visibility
// weight (traceMiss), 0 on a hit.
vec3 traceRadiance(vec3 origin, vec3 dir, int cascade, out float hitDist, out float backface, out float sunLuma, out float skyOpen)
{
    const float rayMax = GI_MAX_RAY_DIST * (cascade + 1);
    hitDist = rayMax; // misses (and out-of-bounds hits) count as open space at the gather range
    backface = 0.0;   // 1 when the committed hit faces away (ray started inside/behind the geometry)
    sunLuma = 0.0;
    skyOpen = 0.0;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsOpaqueEXT, 0xFFu, origin, 0.05, dir, rayMax);
    while (rayQueryProceedEXT(rq)) {}

    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT)
        return traceMiss(dir, sunLuma, skyOpen);

    // Bound every post-hit buffer access. A bad meshIdx/triBase/vertex index would otherwise read wildly
    // out of bounds and MMU-fault; treat any out-of-range hit as a miss.
    const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
    // Bit 23 = a baked TREE (gi_tlas_instances.cs.glsl): the custom index holds its material, it has no stream entry.
    const bool tree = (instanceIdx & 0x800000) != 0;
    if (!tree && uint(instanceIdx) >= in_instances.length())
        return traceMiss(dir, sunLuma, skyOpen);
    // Geometry comes from the RT meshIdx the TLAS writer packed into the instance's sbtOffset (a LOD
    // chain traces one shared BLAS, which may differ from the raster-selected level the instance
    // references); the material still comes from the instance.
    const uint meshIdx     = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rq, true);
    const uint materialIdx = tree ? uint(instanceIdx) & 0xFFFFu : in_instances[instanceIdx].meshIdxMaterialIdx >> 16;
    if (meshIdx >= in_meshInfos.length() || materialIdx >= in_materialInfos.length())
        return traceMiss(dir, sunLuma, skyOpen);
    const InMeshInfo mi    = in_meshInfos[meshIdx];

    const int  prim    = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
    const uint triBase = mi.firstIndex + uint(prim) * 3u;
    if (triBase + 2u >= in_indices.length())
        return traceMiss(dir, sunLuma, skyOpen);
    const uint v0 = uint(mi.vertexOffset) + in_indices[triBase + 0u];
    const uint v1 = uint(mi.vertexOffset) + in_indices[triBase + 1u];
    const uint v2 = uint(mi.vertexOffset) + in_indices[triBase + 2u];
    if (max(max(v0, v1), v2) >= in_vertices.length())
        return traceMiss(dir, sunLuma, skyOpen);

    const vec2 bc    = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    const float t    = rayQueryGetIntersectionTEXT(rq, true);
    const mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
    const vec3 b = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    // Two wide loads per vertex: the normal and the uv's v share normalV, the u rides positionU.w.
    const vec4 nv0 = in_vertices[v0].normalV, nv1 = in_vertices[v1].normalV, nv2 = in_vertices[v2].normalV;
    const vec3 objN = normalize(b.x * nv0.xyz + b.y * nv1.xyz + b.z * nv2.xyz);
    vec3 worldN = normalize(mat3(o2w) * objN);
    const vec2 uv = b.x * vec2(in_vertices[v0].positionU.w, nv0.w) + b.y * vec2(in_vertices[v1].positionU.w, nv1.w)
                  + b.z * vec2(in_vertices[v2].positionU.w, nv2.w);
    const vec3 worldPos = origin + dir * t;
    hitDist = t; // committed surface hit -> actual distance to geometry along this ray
    if (dot(worldN, dir) > 0.0)
    {
        worldN = -worldN;
        backface = 1.0;
    }

    const uint diffuseTexIdx = in_materialInfos[materialIdx].diffuseNormalTexIdx & 0x0000FFFFu;
    const vec3 albedo = textureLod(u_textures[nonuniformEXT(diffuseTexIdx)], uv, GI_ALBEDO_LOD).rgb;

    // NO cloud shadow here: the probe keeps its sun part apart and the LOOKUP dims it by the cloud shadow at the
    // shaded point (giIrradiance). Cloud-shadowed hits made every probe lag a moving cloud shadow by its update
    // interval - flashing GI under shadows that fall away from the camera, where the intervals are long.
    g_sunShadowOverride = sunVisibility(worldPos + worldN * 0.02);
    vec3 radiance = giGatherDirect(worldPos, worldN, albedo);
    // Previous-frame indirect at the hit -> multi-bounce (infinite, temporally). The cur SH already holds
    // the carried-forward irradiance for this frame. Probe path: the CHEAP lookup (no Chebyshev, no
    // cross-cascade fade): the result is albedo-scaled and blended at temporalAlpha, so its noise is free
    // and the shading-quality path's ~2x loads are not. Volume path: the shading lookup itself - 4 filtered
    // fetches per cascade, WITH the baked Chebyshev visibility, cheaper than either probe loop.
    float giCov, prevSun;
#ifdef GI_VOLUME
    vec3 prevE = evalProbeCoverage(worldPos, worldN, giCov, prevSun);
#else
    vec3 prevE = giEvalBounce(worldPos, worldN, giCov, prevSun);
#endif
    // Faded with coverage so traced hits near the field's edge don't step; the vec3(-1) "no data" result always
    // comes with coverage 0.
    const vec3 bounce = albedo * prevE * (giCov * INV_PI);
    sunLuma = dot(g_gatherSunRadiance + bounce * prevSun, GI_LUMA_W);
    return radiance + bounce;
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
    float frontSum = 0.0; // front-face hits within the depth cap (the enclosure test; depthCap is live in the loop anyway)
    float sunSum = 0.0; // sun luminance over the rays: the sun part of luma(c0) after the Y.x * wsh below
    // The cosine-weighted open share of the upper hemisphere: the misses' cosines to up over the whole
    // hemisphere's, which is N / 4 in expectation for sphere-uniform rays - ONE accumulator through the loop.
    float skyOpenSum = 0.0;
    float closestBack = 1e30;
    vec3  escapeOffset = vec3(0.0);
    for (uint i = 0u; i < N; ++i)
    {
        const vec3 dir = sampleSphere(i, N, jitter);
        float hitDist, backface, sunLuma, skyOpen;
        const vec3 radiance = traceRadiance(probePos, dir, cascade, hitDist, backface, sunLuma, skyOpen);
        backfaceSum += backface;
        frontSum += hitDist < depthCap && backface < 0.5 ? 1.0 : 0.0;
        sunSum += sunLuma;
        skyOpenSum += skyOpen;
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
        c0 += radiance * Y.x;
        c1 += radiance * Y.y;
        c2 += radiance * Y.z;
        c3 += radiance * Y.w;
        const float dc = min(hitDist, depthCap);
        dsh  += Y * dc;
        d2sh += Y * (dc * dc);
    }
    // The Monte Carlo weight once, not per ray.
    c0 *= wsh; c1 *= wsh; c2 *= wsh; c3 *= wsh;
    dsh *= wsh; d2sh *= wsh;

    // Sky radiance (moonlight / space light): a directional delta light can't be hit by gather rays, so
    // its direct irradiance is projected straight into the probe SH (one SH-L1 delta projection; eval's
    // cosine convolution then yields ~E * max(dot(n, up), 0)). Visibility is a single ray from the probe
    // center toward up (toggleable) - probes float in open space, so this is a soft, low-noise gate, and
    // the temporal blend smooths it further. Bounces arrive for free through the prevE multi-bounce.
    if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
    {
        const vec3 up = normalize(u_skyUp);
        const float upVis = u_rtSkyRadiance > 0.5 ? rtShadowVisibility(probePos, up, 0.05, 1.0e4) : 1.0;
        const vec3 cd = u_skyRadianceColor * upVis;
        const vec4 Ysky = shBasisL1(up);
        c0 += cd * Ysky.x;
        c1 += cd * Ysky.y;
        c2 += cd * Ysky.z;
        c3 += cd * Ysky.w;
    }

    // The embedded fraction: the backface fraction, weighted by the enclosure (see GI_ENCLOSED_SHARE_MIN). It
    // drives the escape, the stored "dead" fraction and the just-escaped flush alike.
    const float backShare = backfaceSum / max(backfaceSum + frontSum, 1.0);
    const float backFrac  = backfaceSum / float(N) * smoothstep(GI_ENCLOSED_SHARE_MIN, GI_ENCLOSED_SHARE_MAX, backShare);

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
    // from zero is ~1/alpha visits of a too-dark field, and the visit clamp below would hold it near zero.
    // (A truly black probe replaces black with black.)
    const vec3  lumaW   = GI_LUMA_W;
    const float oldLuma = dot(gi_gridData[cellBase].xyz, lumaW); // [0].xyz = the stored SH DC term
    const bool  replace = fresh || oldLuma <= 0.0;
    const float frameAlpha = u_giTrace0.y;
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

    // VISIT CLAMP + CHANGE DETECTION (the irradiance blend only; the depth stats take the plain alpha). One
    // N-ray visit of a high-variance integrand - a small sunlit patch, a lamp: a ray hits it or not - lands far
    // from the converged value, and each such visit kicks the blend; the kicks are what reads as flicker. The
    // probe keeps the history of its visits' DC luminance (GI_SUN_V4.zw): a SLOW second moment (plain alpha)
    // gives the visit variance around the stored luminance, sigma, and a FAST mean follows the recent visits.
    // * A regular visit outside stored +- GI_CLAMP_SIGMA sigma is SCALED onto that bound (every SH coefficient
    //   and the sun part alike: direction and sun fraction kept) - both ways, a dark outlier too.
    // * A REAL change (a light switched, a door opened) moves the fast mean, which noise does not: the fast mean
    //   GI_CHANGE_SIGMA off the stored luminance, with the previous one already GI_CHANGE_CONFIRM_SIGMA off on
    //   the same side (two visits: one spike alone cannot trigger it), blends that visit unclamped at
    //   GI_CHANGE_ALPHA. The moments are never clamped, so sigma widens while a change passes.
    // Replaced / boosted visits (relocation, just escaped) skip both; a replace restarts the moments at the
    // visit, with GI_SIGMA_INIT_REL of it as sigma.
    const vec4  prevStats = fresh ? vec4(0.0) : gi_gridData[cellBase + GI_SUN_V4];
    const float visitLuma = dot(c0, lumaW);
    float fastMean = mix(prevStats.z, visitLuma, GI_FAST_MEAN_ALPHA);
    float secondMoment = mix(prevStats.w, visitLuma * visitLuma, alpha);
    float shAlpha = alpha;
    float visitScale = 1.0;
    if (replace)
    {
        fastMean = visitLuma;
        secondMoment = visitLuma * visitLuma * (1.0 + GI_SIGMA_INIT_REL * GI_SIGMA_INIT_REL);
    }
    else if (alpha == visitAlpha)
    {
        const float sigma   = max(sqrt(max(prevStats.w - oldLuma * oldLuma, 0.0)), GI_SIGMA_MIN_REL * oldLuma);
        const float devPrev = prevStats.z - oldLuma;
        const float devNew  = fastMean - oldLuma;
        if (abs(devNew) > GI_CHANGE_SIGMA * sigma && abs(devPrev) > GI_CHANGE_CONFIRM_SIGMA * sigma && devPrev * devNew > 0.0)
            shAlpha = max(alpha, GI_CHANGE_ALPHA);
        else if (visitLuma > 0.0)
            visitScale = clamp(visitLuma, oldLuma - GI_CLAMP_SIGMA * sigma, oldLuma + GI_CLAMP_SIGMA * sigma) / visitLuma;
    }

    giBlendCell(cellBase, c0 * visitScale, c1 * visitScale, c2 * visitScale, c3 * visitScale, shAlpha);
    const float skyVis = min(skyOpenSum * (4.0 / float(N)), 1.0);
    giStoreSunStats(cellBase, vec4(mix(prevStats.x, sunSum * (0.282095 * wsh) * visitScale, shAlpha),
                                   mix(prevStats.y, skyVis, alpha), fastMean, secondMoment));
    // An escape visit pins the stored fraction to exactly DEAD_MAX: still dead at lookup and still
    // triggering the just-escaped flush above (>=), but not skipped by the dead-probe interval (>), so the
    // probe traces from its new position on the very next visit instead of GI_DEAD_INTERVAL visits later.
    const float storedFrac = escaped ? GI_BACKFACE_DEAD_MAX : prevMisc.x;
    const float visitFrac  = escaped ? GI_BACKFACE_DEAD_MAX : backFrac;
    giBlendProbeStats(cellBase, dsh, d2sh, storedFrac, visitFrac, newOffset, alpha); // depth moments + embedded-probe stats + offset
}
