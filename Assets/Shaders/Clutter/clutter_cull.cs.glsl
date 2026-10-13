#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]
#extension GL_EXT_nonuniform_qualifier : enable    // the splat height maps (u_textures)

// THE CLUTTER CULL (ClutterPipeline; Docs/GroundClutterPlan.md 3): ONE WORKGROUP PER PATCH around the camera, ONE THREAD PER
// RANK. Per type, rank k stands at the k-th R2 point over the patch (offset per patch and type) and is KEPT where
// k + 0.5 < density x patch area: every prefix is evenly spread, so a thinning density removes objects evenly, and one near
// the threshold shrinks into the ground over the grow band (no pops). Types are independent lists.
// Density = Density x climate fit x per term mix(a, b, measure): the terrain (grass, crag, beach, wet, river, flow, water),
// the forest floor (canopy, trunk, rock), slope, altitude, cluster noise, fairy rings. Snow or an occupied floor: none.
// A kept object goes to its BUCKET (mesh and LOD; a flower: its LOD) with a bucket-local index; clutter_prefix lays the
// buckets out, clutter_scatter sorts the records into them.

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#include "mesh_vertex.inc.glsl"
layout (binding = 11) uniform sampler2D u_textures[]; // the splat height maps (clutterRelief)
#define TERRAIN_SPLAT_RELIEF      // terrainReliefAt, as the TES
#define TERRAIN_SPLAT_HEIGHT_ONLY // the coverages + the height: no material taps
#include "terrain_splat.inc.glsl"

layout (local_size_x = CLUTTER_CANDIDATES) in;

layout (binding = 2, std430) readonly buffer InGrassFrame // RendererVKLayout::GrassFrameGpu: the ground table
{
    vec2 gf_gridOrigin;
    uint gf_gridDim;
    float gf_patchSize;
    ivec2 gf_tableMin;
    uint gf_tableDim;
    float gf_chunkSize;
    uvec2 gf_chunks[GRASS_TABLE_DIM * GRASS_TABLE_DIM];
};
layout (binding = 3, std430) readonly buffer InVertices { MeshVertex in_vertices[]; };

#include "grass.inc.glsl" // hashes, value noise, the ground mesh, grassTerrainCover
#define CLUTTER_FRAME_BINDING 4
#include "clutter.inc.glsl"

layout (binding = 5, std430) readonly buffer InTypes { ClutterType in_types[]; };
layout (binding = 6, std430) readonly buffer InMeshes { ClutterMesh in_meshes[]; };
layout (binding = 7, std430) writeonly buffer OutFlat { ClutterInstance out_flat[]; };
layout (binding = 8, std430) writeonly buffer OutKeys { uint out_keys[]; }; // bucket << CLUTTER_LOCAL_BITS | local index
layout (binding = 9, std430) buffer Counts { uint io_counts[4]; };          // [0] the kept records
layout (binding = 10, std430) buffer BucketCounts { uint io_bucketCounts[]; };

// THE TERRAIN'S MEASURES over the patch: a CLUTTER_GRID^2 grid (threads 0..24), bilinear per candidate. 4 corners alone
// aliased the bedrock's small-scale noise into grid-aligned diamonds.
#define CLUTTER_GRID 5u
#define CLUTTER_GRID_SAMPLES (CLUTTER_GRID * CLUTTER_GRID)
shared float s_height[CLUTTER_GRID_SAMPLES];
shared float s_water[CLUTTER_GRID_SAMPLES];
shared float s_temperature[CLUTTER_GRID_SAMPLES];
shared float s_humidity[CLUTTER_GRID_SAMPLES];
shared vec4 s_cover[CLUTTER_GRID_SAMPLES]; // x grass, y crag (bedrock), z beach, w snow
shared vec3 s_river[CLUTTER_GRID_SAMPLES]; // x influence, y flow (slow 0 .. fast 1), z under the water

bool clutterGroundAt(vec2 xz, out GrassGround g)
{
    const ivec2 cell = ivec2(floor(xz / gf_chunkSize)) - gf_tableMin;
    g = GrassGround(vec2(0.0), 0u, 0u, 1.0);
    if (gf_tableDim == 0u || any(lessThan(cell, ivec2(0))) || any(greaterThanEqual(cell, ivec2(gf_tableDim))))
        return false;
    const uvec2 entry = gf_chunks[uint(cell.y) * gf_tableDim + uint(cell.x)];
    if (entry.y == 0u)
        return false;
    g.chunkOrigin = vec2(cell + gf_tableMin) * gf_chunkSize;
    g.firstVertex = entry.x;
    g.res = entry.y;
    g.step = gf_chunkSize / float(entry.y);
    return true;
}

// One grid sample's terrain measures (as the grass cull feeds the splat). No texture set or height map: no cover, a mild
// climate.
void clutterCorner(uint i, vec2 xz, GrassGround g)
{
    vec3 facetN;
    const float h = grassGroundHeight(g, xz, facetN);
    s_height[i] = h;
    s_water[i] = -1e9;
    s_temperature[i] = 12.5;
    s_humidity[i] = 0.5;
    s_cover[i] = vec4(0.0);
    const vec3 river = grassGroundRiverData(g, xz);
    s_river[i] = vec3(river.x, smoothstep(RIVER_FLOW_SLOW, RIVER_FLOW_FAST, river.y), river.z);
    if (!terrainHeightMapPresent())
        return;

    const vec4 td = terrainDataAt(xz);
    const vec4 climate = terrainClimateAt(xz);
    const float temperature = terrainTemperatureAt(climate, h);
    s_water[i] = td.y;
    s_temperature[i] = temperature;
    s_humidity[i] = climate.w;
    if (u_terrain_splatBase < 0.0 || u_terrain_numGround < 1.0)
        return;

    const TerrainLayers L = terrainLayers(vec3(xz.x, h, xz.y), grassGroundSmoothNormal(g, xz),
        TerrainFields(td.w, temperature, climate.w, td.y, river.x));
    s_cover[i] = vec4(grassTerrainCover(L), float(L.rockW), float(L.beachW), float(L.snowW));
}

// The relief's view terms: uniform only, so main takes them once, outside the type loop.
struct ClutterReliefView
{
    bool  enabled;
    vec3  viewPos;
    float invFadeWidth;
    float spacingPerMetre; // the TES's target edge length (m) per metre of distance
};

ClutterReliefView clutterReliefView()
{
    ClutterReliefView v;
    v.enabled = !(u_terrainTess_enabled < 0.5 || u_terrain_splatBase < 0.0 || u_terrain_numGround < 1.0 || !terrainHeightMapPresent());
    v.viewPos = u_views_viewPos[VIEW_CENTER].xyz;
    v.invFadeWidth = u_terrainTess_invFadeWidth;
    v.spacingPerMetre = u_terrainTess_spacingPerMetre;
    return v;
}

// THE TESSELLATED RELIEF: the offset that puts a mesh point on the DRAWN ground (displaced along the smooth normal by
// (height - 0.5) x depth - keep in step with terrain_tess.tes.glsl: layers, fade, slope gate, depth, height mip).
// Over a footprint (m, 0 = the point alone) the height lies between the mean and the lowest of 5 taps: a branch over a
// bump buries a little rather than float at its ends.
vec3 clutterRelief(ClutterReliefView view, vec3 meshPos, vec3 smoothN, float river, float footprint)
{
    if (!view.enabled)
        return vec3(0.0);
    const float dist = distance(meshPos, view.viewPos);
    if (dist >= u_terrainTess_fadeEnd)
        return vec3(0.0);

    const vec4 td = terrainDataAt(meshPos.xz);
    const vec4 climate = terrainClimateAt(meshPos.xz);
    const TerrainLayers L = terrainLayers(meshPos, smoothN, TerrainFields(td.w, terrainTemperatureAt(climate, meshPos.y), climate.w, td.y, river));
    const float t = clamp((dist - u_terrainTess_fadeStart) * view.invFadeWidth, 0.0, 1.0);
    const float strength = (1.0 - pow(t, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, smoothN.y);
    const float depth = mix(mix(u_terrainTess_depthGround, u_terrainTess_depthRock, float(L.rockW)), u_terrainTess_depthGround, float(L.snowW)) * strength;
    if (depth <= 1e-4)
        return vec3(0.0);

    // The TES's height mip: the target edge length at this distance.
    const float spacing = max(dist, u_terrainTess_freezeDistance) * view.spacingPerMetre;
    const vec2 dx = vec2(spacing, 0.0), dy = vec2(0.0, spacing);
    float height = float(terrainReliefAt(L, meshPos.xz, dx, dy));
    if (footprint > 0.0)
    {
        float lowest = height, sum = height;
        const vec2 offsets[4] = vec2[4](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
        for (int i = 0; i < 4; ++i)
        {
            const float h = float(terrainReliefAt(L, meshPos.xz + offsets[i] * footprint, dx, dy));
            lowest = min(lowest, h);
            sum += h;
        }
        height = mix(0.2 * sum, lowest, 0.5);
    }
    return smoothN * ((height - 0.5) * depth);
}

// The turn from straight up to n (unit).
vec4 clutterQuatFromUp(vec3 n)
{
    const float d = n.y; // dot(up, n)
    if (d > 0.9999)
        return vec4(0.0, 0.0, 0.0, 1.0);
    return normalize(vec4(cross(vec3(0.0, 1.0, 0.0), n), 1.0 + d));
}

// The fairy rings of a type: radius +-30 %, one per `cell` with `chance`; 1 on a ring, falling off over `width`.
float clutterRing(vec2 xz, vec4 ring, uint typeSeed)
{
    const float cell = ring.z; // ClutterType: >= 2 x radius + 1
    const ivec2 c0 = ivec2(floor(xz / cell));
    float best = 0.0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const ivec2 c = c0 + ivec2(dx, dz);
            const uint h = grassHash(grassHash2(c) ^ typeSeed);
            if (grassUnit(h) >= ring.w)
                continue;
            const vec2 centre = (vec2(c) + 0.25 + 0.5 * vec2(grassUnit(grassHash(h + 1u)), grassUnit(grassHash(h + 2u)))) * cell;
            const float radius = ring.x * (0.7 + 0.6 * grassUnit(grassHash(h + 3u)));
            const float off = (distance(xz, centre) - radius) / ring.y; // ClutterType: width >= 0.05
            best = max(best, exp(-off * off));
        }
    return best;
}

void main()
{
    const uint patchIdx = gl_WorkGroupID.x;
    const uint k = gl_LocalInvocationIndex;
    // Every early return up to the barrier is uniform over the workgroup (the patch alone decides it).
    if (patchIdx >= cf_gridDim * cf_gridDim || cf_numTypes == 0u)
        return;

    const float P = cf_patchSize;
    const vec2 origin = cf_gridOrigin + vec2(float(patchIdx % cf_gridDim), float(patchIdx / cf_gridDim)) * P;
    const vec2 nearestXZ = clamp(u_viewPos.xz, origin, origin + P);
    if (distance(nearestXZ, u_viewPos.xz) > cf_range)
        return;
    GrassGround patchGround;
    if (!clutterGroundAt(origin + 0.5 * P, patchGround))
        return;

    const float gridStep = P / float(CLUTTER_GRID - 1u);
    if (k < CLUTTER_GRID_SAMPLES)
        clutterCorner(k, origin + vec2(float(k % CLUTTER_GRID), float(k / CLUTTER_GRID)) * gridStep, patchGround);
    barrier();

    // The patch's box (its grid + the tallest clutter) against the view, or within the near grass cascade's caster reach
    // (an object just off screen still casts).
    float hMin = s_height[0], hMax = s_height[0];
    for (uint i = 1u; i < CLUTTER_GRID_SAMPLES; ++i)
    {
        hMin = min(hMin, s_height[i]);
        hMax = max(hMax, s_height[i]);
    }
    const float slack = 0.5 * P + 2.0;
    const vec3 boxMin = vec3(origin.x - 1.0, hMin - 1.0, origin.y - 1.0);
    const vec3 boxMax = vec3(origin.x + P + 1.0, hMax + slack, origin.y + P + 1.0);
    const vec3 sphereCentre = 0.5 * (boxMin + boxMax);
    const float sphereRadius = 0.5 * length(boxMax - boxMin);
    bool patchVisible = true;
    for (int i = 0; i < 6; ++i)
        if (dot(vec4(sphereCentre, 1.0), u_frustumPlanes[i]) + sphereRadius < 0.0)
            patchVisible = false;

    const vec2 nearCentre = u_grass_nearCentre;
    const float nearReach = u_grass_nearRange * 1.5 + 2.0;
    const bool patchNear = u_grass_nearRange > 0.0 && distance(clamp(nearCentre, origin, origin + P), nearCentre) <= nearReach;
    if (!patchVisible && !patchNear)
        return;

    const uint patchHash = grassHash2(ivec2(floor(origin / P + 0.5)));
    const float countScale = u_clutter_densityScale * P * P; // density (per m^2) -> objects in the patch
    const float rank = float(k) + 0.5;
    const ClutterReliefView reliefView = clutterReliefView();
    for (uint t = 0u; t < cf_numTypes; ++t)
    {
        const ClutterType type = in_types[t];
        // The ranks this type can keep anywhere (every term at its largest): the others never test.
        if (rank >= type.bound.x * countScale)
            continue;

        const uint typeSeed = (t + 1u) * 0x9E3779B9u;
        const uint listHash = grassHash(patchHash ^ typeSeed);
        const vec2 r2Offset = vec2(grassUnit(listHash), grassUnit(grassHash(listHash + 1u)));
        const vec2 uv = fract(r2Offset + float(k) * vec2(0.7548776662, 0.5698402910));
        const vec2 xz = origin + uv * P;

        const float range = min(type.shape.z * u_clutter_rangeScale, cf_range);
        const float horizontal = distance(xz, u_viewPos.xz);
        if (horizontal > range)
            continue;
        const float rangeFade = 1.0 - smoothstep(range * (1.0 - u_clutter_rangeFade), range, horizontal);

        // The terrain's measures (bilinear in the patch's grid) and the forest floor.
        const vec2 gp = uv * float(CLUTTER_GRID - 1u);
        const uvec2 c0 = min(uvec2(gp), uvec2(CLUTTER_GRID - 2u));
        const vec2 f = gp - vec2(c0);
        const uint i00 = c0.y * CLUTTER_GRID + c0.x, i10 = i00 + 1u, i01 = i00 + CLUTTER_GRID, i11 = i01 + 1u;
        const vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
        const vec4 cover = s_cover[i00] * w.x + s_cover[i10] * w.y + s_cover[i01] * w.z + s_cover[i11] * w.w;
        const float temperature = dot(vec4(s_temperature[i00], s_temperature[i10], s_temperature[i01], s_temperature[i11]), w);
        const float humidity = dot(vec4(s_humidity[i00], s_humidity[i10], s_humidity[i01], s_humidity[i11]), w);
        const float water = dot(vec4(s_water[i00], s_water[i10], s_water[i01], s_water[i11]), w);
        const vec3 river = s_river[i00] * w.x + s_river[i10] * w.y + s_river[i01] * w.z + s_river[i11] * w.w;
        const vec4 floorMap = clutterFloorAt(xz);
        // THE GRASS AS DRAWN (the `Grass` term's measure): cover x the blades' clumps x the canopy thinning, as grass_cull
        // keeps them - so no flower stands in a bare spot between the clumps.
        const float grass = cover.x * grassClump(xz) * (1.0 - u_grass_canopyThinning * floorMap.x);

        // The climate fit: 1 inside the ideal box, a Gaussian of the distance outside it (as a tree species').
        // Temperature -25 .. 50 C -> 0 .. 1.
        const vec2 climate = vec2(clamp(temperature * (1.0 / 75.0) + (25.0 / 75.0), 0.0, 1.0), humidity);
        const vec2 outside = max(type.climate.xz - climate, vec2(0.0)) + max(climate - type.climate.yw, vec2(0.0));
        const vec2 scaled = outside * type.placement.y;
        float density = type.placement.x * exp(-0.5 * dot(scaled, scaled));
        density *= mix(type.terms0.x, type.terms0.y, grass) * mix(type.terms0.z, type.terms0.w, cover.y)
                 * mix(type.terms1.x, type.terms1.y, cover.z) * mix(type.terms1.z, type.terms1.w, floorMap.x)
                 * mix(type.terms2.x, type.terms2.y, floorMap.y) * mix(type.terms2.z, type.terms2.w, floorMap.z)
                 * mix(type.terms3.x, type.terms3.y, humidity)
                 * mix(type.terms4.x, type.terms4.y, pow(river.x, type.terms5.x)) * mix(type.terms4.z, type.terms4.w, river.y)
                 * mix(type.bound.z, type.bound.w, river.z);
        density *= (1.0 - cover.w) * (1.0 - smoothstep(0.3, 0.7, floorMap.w));
        float keep = density * countScale * rangeFade;
        // The cluster and ring factors are <= 1: a rank past the count so far never passes.
        if (rank >= keep)
            continue;

        if (type.placement.z > 0.0)
        {
            const float coverage = type.placement.w;
            const float n = grassValueNoise(xz * type.placement.z + float(t) * vec2(37.1, 11.7));
            keep *= smoothstep(1.0 - coverage - 0.15, 1.0 - coverage + 0.15, n);
        }
        if (type.ring.x > 0.0)
            keep *= clutterRing(xz, type.ring, typeSeed);
        if (rank >= keep)
            continue;

        // The ground under it: the drawn mesh (the slope gate and the lean), above the water.
        GrassGround g;
        if (!clutterGroundAt(xz, g))
            continue;
        vec3 groundN;
        const float groundY = grassGroundHeight(g, xz, groundN);
        // The slope (rise / run = sin / cos) against its max, multiplied out (no divide).
        const float sinSlope = length(groundN.xz);
        if (sinSlope > type.terms3.z * groundN.y || groundY - water < type.terms3.w)
            continue;

        const float grow = clamp((keep - rank) / (u_clutter_growBand * keep), 0.0, 1.0);
        const uint h = grassHash(listHash ^ (k * 0x85EBCA6Bu));
        const uint kind = type.info.z;
        // A rigid type has >= 1 variant (ClutterSystem skips one with no mesh); a flower's 0 is never used.
        const uint variant = kind == CLUTTER_KIND_FLOWER ? 0u : h % type.info.y;
        // A FLOWER shrinks with the grass cover as the blades do (grassCoverSize). Not REEDS or TUFTS (they grow on the
        // bare bank), FERNS (the shaded forest floor, where the grass is thin) or the WEEDS (rosettes, leafy stalks: on
        // bare and trampled ground too).
        const uint head = type.info.w & 0xFFu;
        const bool reed = kind == CLUTTER_KIND_FLOWER && (head == FLOWER_HEAD_REED || head == FLOWER_HEAD_TUFT
            || head == FLOWER_HEAD_FERN || head == FLOWER_HEAD_ROSETTE || head == FLOWER_HEAD_LEAFY);
        const float coverSize = kind == CLUTTER_KIND_FLOWER && !reed ? grassCoverSize(cover.x) : 1.0;
        const float scale = mix(type.shape.x, type.shape.y, grassUnit(grassHash(h + 1u))) * grow * coverSize;
        if (scale <= 0.0)
            continue;

        // A yaw, then up -> the ground's normal by its align (a flower stands upright: its VS reads the yaw only).
        const float halfYaw = grassUnit(grassHash(h + 2u)) * PI;
        const vec4 qYaw = vec4(0.0, sin(halfYaw), 0.0, cos(halfYaw));
        const float align = kind == CLUTTER_KIND_FLOWER ? 0.0 : type.albedo1.w;
        const vec4 q = quat_multiply(clutterQuatFromUp(normalize(mix(vec3(0.0, 1.0, 0.0), groundN, align))), qYaw);

        // The bounds.
        const uint mesh = type.info.x + variant;
        float height, radius;
        if (kind == CLUTTER_KIND_FLOWER)
        {
            height = type.flower.x * scale;
            // A fern's fronds reach out about their length (the Stem); a stalk leans out ~0.6 of it.
            radius = (head == FLOWER_HEAD_FERN ? 1.0 : 0.6) * height + type.flower.y * scale;
        }
        else
        {
            height = in_meshes[mesh].bounds.y * scale;
            radius = in_meshes[mesh].bounds.x * scale;
        }

        // On the DRAWN ground: the mesh point + the tessellated relief (over the object's footprint).
        const vec3 groundPos = vec3(xz.x, groundY, xz.y);
        const vec3 relief = clutterRelief(reliefView, groundPos, grassGroundSmoothNormal(g, xz), grassGroundRiver(g, xz), kind == CLUTTER_KIND_FLOWER ? 0.0 : 0.35 * radius);
        const vec3 up = quat_transform(vec3(0.0, 1.0, 0.0), q);

        // The sink, the view test and the LOD.
        const vec3 base = groundPos + relief - up * (type.shape.w * height);
        const vec3 centre = base + up * (0.5 * height);
        bool visible = patchVisible;
        if (visible)
            for (int i = 0; i < 6; ++i)
                if (dot(vec4(centre, 1.0), u_frustumPlanes[i]) + radius < 0.0)
                    visible = false;
        const bool nearCaster = patchNear && distance(xz, nearCentre) <= nearReach;
        if (!visible && !nearCaster)
            continue;

        const float dist = distance(centre, u_viewPos);
        uint lod;
        if (kind == CLUTTER_KIND_FLOWER)
            lod = dist < u_clutter_flowerLod1Distance ? 0u : dist < u_clutter_flowerLod2Distance ? 1u : 2u;
        else
        {
            // The projected size radius / dist against the LOD sizes, multiplied out (no divide).
            const float size = radius / max(dist, 1e-3);
            // An empty LOD slot holds the finer level's range (setClutterAssets).
            lod = size > u_clutter_lod1Size ? 0u : size > u_clutter_lod2Size ? 1u : 2u;
        }

        const uint slot = atomicAdd(io_counts[0], 1u);
        if (slot >= CLUTTER_MAX_INSTANCES)
            continue;
        const uint bucket = clutterBucket(kind, head, type.terms5.y > 0.0, mesh, lod);
        const uint local = atomicAdd(io_bucketCounts[bucket], 1u);

        // The colours vary per object (+-12 %), as the grass blades do.
        const float variation = 1.0 + 0.24 * (grassUnit(grassHash(h + 3u)) - 0.5);
        // The colours are >= 0 (ClutterSystem); packUnorm4x8 clamps the top. petalWidth / open are in range (ClutterType).
        const vec3 albedo0 = type.albedo0.rgb * variation;
        const vec3 albedo1 = type.albedo1.rgb;
        const uint petalWidth = uint(type.flower.z * 255.0 + 0.5);
        const uint open = uint((type.flower.w * INV_PI + 0.5) * 255.0 + 0.5); // -pi/2 .. pi/2
        ClutterInstance inst;
        inst.posScale = vec4(base, scale);
        // A flower has no orientation (its VS turns it by the hash): x = its texture + 1 (0 = none; a fern's frond).
        inst.data = uvec4(kind == CLUTTER_KIND_FLOWER ? uint(type.terms5.y) : packHalf2x16(q.xy), packHalf2x16(q.zw),
                          t | (variant << 8) | (lod << 16) | (kind << 24), h);
        inst.look = uvec4(packUnorm4x8(vec4(sqrt(albedo0), type.albedo0.w)),
                          packUnorm4x8(vec4(sqrt(albedo1), type.bound.y)),
                          packHalf2x16(kind == CLUTTER_KIND_FLOWER ? type.flower.xy : vec2(height, 0.0)),
                          type.info.w | (petalWidth << 16) | (open << 24));
        out_flat[slot] = inst;
        out_keys[slot] = (bucket << CLUTTER_LOCAL_BITS) | local;
    }
}
