#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]
#extension GL_EXT_nonuniform_qualifier : enable    // the splat height maps (u_textures)

// THE CLUTTER CULL (ClutterPipeline; Docs/GroundClutterPlan.md 3): ONE WORKGROUP PER PATCH of the grid around the camera
// (ClutterFrameGpu), ONE THREAD PER RANK. Per clutter type, rank k stands at the k-th point of an R2 sequence over the
// patch (offset per patch and type) and is KEPT where k + 0.5 < the type's density there x the patch area - every
// prefix of the ranks is evenly spread, so a thinning density removes objects evenly, and an object near the threshold
// shrinks into the ground over the grow band instead of popping. Types are independent lists: no type ever swaps for
// another as the camera moves.
//
// A type's density = its Density x its climate fit x the product of its terms, each mix(a, b, measure):
//   the TERRAIN (the patch corners, as the grass cull reads them - terrain_splat.inc.glsl terrainLayers): grass cover,
//   bedrock (crag), beach; the climate's humidity (wet); snow removes everything;
//   the FOREST FLOOR MAP (ClutterSystem: the trees' and rocks' records): canopy, trunk and rock proximity; nothing
//   inside a trunk or a rock;
//   the slope under the object, its height above the water, its cluster noise and its fairy rings.
// A kept object goes into its BUCKET (its mesh and LOD; the flowers' LOD) with a bucket-local index; clutter_prefix
// then lays the buckets out and clutter_scatter sorts the records into them.

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#include "mesh_vertex.inc.glsl"
layout (binding = 11) uniform sampler2D u_textures[]; // the splat height maps (clutterRelief)
#define TERRAIN_SPLAT_RELIEF      // the height composite (terrainReliefAt), as the tessellation evaluation shader
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
layout (binding = 9, std430) buffer Counts { uint io_counts[4]; };         // [0] the kept records
layout (binding = 10, std430) buffer BucketCounts { uint io_bucketCounts[]; };

// THE TERRAIN'S MEASURES over the patch: a CLUTTER_GRID^2 grid of samples (threads 0..24; every patch size / 4 m),
// bilinear per candidate. Only the 4 corners (a 4 m lattice) aliased the bedrock coverage's small-scale noise into
// grid-aligned diamonds - the stones (Crag x4) gathered in blobs on a regular pattern.
#define CLUTTER_GRID 5u
#define CLUTTER_GRID_SAMPLES (CLUTTER_GRID * CLUTTER_GRID)
shared float s_height[CLUTTER_GRID_SAMPLES];
shared float s_water[CLUTTER_GRID_SAMPLES];
shared float s_temperature[CLUTTER_GRID_SAMPLES];
shared float s_humidity[CLUTTER_GRID_SAMPLES];
shared vec4 s_cover[CLUTTER_GRID_SAMPLES]; // x grass, y crag (bedrock), z beach, w snow

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

// One grid sample's terrain measures, fed as the grass cull feeds the splat. Without a texture set or a map: no cover, a
// mild climate.
void clutterCorner(uint i, vec2 xz, GrassGround g)
{
    vec3 facetN;
    const float h = grassGroundHeight(g, xz, facetN);
    s_height[i] = h;
    s_water[i] = -1e9;
    s_temperature[i] = 12.5;
    s_humidity[i] = 0.5;
    s_cover[i] = vec4(0.0);
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
    const TerrainLayers L = terrainLayers(vec3(xz.x, h, xz.y), grassGroundSmoothNormal(g, xz), TerrainFields(td.w, temperature, climate.w, td.y));
    s_cover[i] = vec4(grassTerrainCover(L), float(L.rockW), float(L.beachW), float(L.snowW));
}

// THE TESSELLATED RELIEF under an object: the drawn ground is the mesh DISPLACED along its smooth normal by
// (height - 0.5) x depth (terrain_tess.tes.glsl - the same layers, fade, slope gate, depth and height mip), so an
// object on the mesh floats over the relief's hollows and sinks into its bumps. The offset to add to a mesh point:
// over a footprint (m, 0 = the point alone) the height is between the mean and the lowest of 5 taps - a branch over a
// bump buries a little rather than float at its ends.
vec3 clutterRelief(vec3 meshPos, vec3 smoothN, float footprint)
{
    if (u_terrainTess_enabled < 0.5 || u_terrain_splatBase < 0.0 || u_terrain_numGround < 1.0 || !terrainHeightMapPresent())
        return vec3(0.0);
    const float dist = distance(meshPos, u_views_viewPos[VIEW_CENTER].xyz);
    if (dist >= u_terrainTess_fadeEnd)
        return vec3(0.0);
    const vec4 td = terrainDataAt(meshPos.xz);
    const vec4 climate = terrainClimateAt(meshPos.xz);
    const TerrainLayers L = terrainLayers(meshPos, smoothN, TerrainFields(td.w, terrainTemperatureAt(climate, meshPos.y), climate.w, td.y));
    const float t = clamp((dist - u_terrainTess_fadeStart) / max(u_terrainTess_fadeEnd - u_terrainTess_fadeStart, 1e-3), 0.0, 1.0);
    const float strength = (1.0 - pow(t, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, smoothN.y);
    const float depth = mix(mix(u_terrainTess_depthGround, u_terrainTess_depthRock, float(L.rockW)), u_terrainTess_depthGround, float(L.snowW)) * strength;
    if (depth <= 1e-4)
        return vec3(0.0);
    // The TES's height mip: the target subdivided edge at this distance.
    const mat4 centreMvp = u_views_mvp[VIEW_CENTER];
    const float projY = length(vec3(centreMvp[0][1], centreMvp[1][1], centreMvp[2][1]));
    const float spacing = max(dist, u_terrainTess_freezeDistance) * 2.0 * u_terrainTess_targetEdgePx / max(projY * u_screenSize.y * u_viewportRect.w, 1.0);
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

vec4 clutterQuatMul(vec4 a, vec4 b)
{
    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}

// The turn from straight up to n (unit).
vec4 clutterQuatFromUp(vec3 n)
{
    const float d = n.y; // dot(up, n)
    if (d > 0.9999)
        return vec4(0.0, 0.0, 0.0, 1.0);
    return normalize(vec4(cross(vec3(0.0, 1.0, 0.0), n), 1.0 + d));
}

// The fairy rings of a type: rings of `radius` +- noise, one per `cell` with `chance`, density 1 on the ring falling
// off over `width`. 0 off every ring.
float clutterRing(vec2 xz, vec4 ring, uint typeSeed)
{
    const float cell = max(ring.z, 1.0);
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
            const float off = (distance(xz, centre) - radius) / max(ring.y, 0.05);
            best = max(best, exp(-off * off));
        }
    return best;
}

void main()
{
    const uint patchIdx = gl_WorkGroupID.x;
    const uint k = gl_LocalInvocationIndex;
    // Everything up to the grid's barrier is uniform over the workgroup (the patch alone decides it).
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

    // The patch's box (its grid + the tallest clutter) against the view, and the near grass cascade's casters (in
    // view or not: an object just off screen still casts).
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
    const float area = P * P;
    const float rank = float(k) + 0.5;
    for (uint t = 0u; t < cf_numTypes; ++t)
    {
        const ClutterType type = in_types[t];
        const float densityScale = u_clutter_densityScale;
        // The ranks this type can keep anywhere (every term at its largest): the others never test.
        if (rank >= type.bound.x * densityScale * area)
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

        // The terrain's measures (bilinear in the patch's sample grid) and the forest floor.
        const vec2 gp = uv * float(CLUTTER_GRID - 1u);
        const uvec2 c0 = min(uvec2(gp), uvec2(CLUTTER_GRID - 2u));
        const vec2 f = gp - vec2(c0);
        const uint i00 = c0.y * CLUTTER_GRID + c0.x, i10 = i00 + 1u, i01 = i00 + CLUTTER_GRID, i11 = i01 + 1u;
        const vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
        const vec4 cover = s_cover[i00] * w.x + s_cover[i10] * w.y + s_cover[i01] * w.z + s_cover[i11] * w.w;
        const float temperature = dot(vec4(s_temperature[i00], s_temperature[i10], s_temperature[i01], s_temperature[i11]), w);
        const float humidity = dot(vec4(s_humidity[i00], s_humidity[i10], s_humidity[i01], s_humidity[i11]), w);
        const float water = dot(vec4(s_water[i00], s_water[i10], s_water[i01], s_water[i11]), w);
        const vec4 floorMap = clutterFloorAt(xz);
        // THE GRASS AS DRAWN (the `Grass` term's measure): the blades' own density - the terrain's cover x their clumps
        // and bare spots (grassClump) x the canopy thinning, as grass_cull / grass.vs keep them. The cover alone put
        // flowers in the bare spots between the clumps.
        const float grass = cover.x * grassClump(xz) * (1.0 - u_grass_canopyThinning * floorMap.x);

        // The climate fit: 1 inside the ideal box, a Gaussian of the distance outside it (as a tree species').
        const vec2 climate = vec2(clamp((temperature + 25.0) / 75.0, 0.0, 1.0), humidity);
        const vec2 outside = max(type.climate.xz - climate, vec2(0.0)) + max(climate - type.climate.yw, vec2(0.0));
        const vec2 scaled = outside * type.placement.y;
        float density = type.placement.x * exp(-0.5 * dot(scaled, scaled));
        density *= mix(type.terms0.x, type.terms0.y, grass) * mix(type.terms0.z, type.terms0.w, cover.y)
                 * mix(type.terms1.x, type.terms1.y, cover.z) * mix(type.terms1.z, type.terms1.w, floorMap.x)
                 * mix(type.terms2.x, type.terms2.y, floorMap.y) * mix(type.terms2.z, type.terms2.w, floorMap.z)
                 * mix(type.terms3.x, type.terms3.y, humidity);
        density *= (1.0 - cover.w) * (1.0 - smoothstep(0.3, 0.7, floorMap.w));
        if (type.placement.z > 0.0)
        {
            const float coverage = type.placement.w;
            const float n = grassValueNoise(xz * type.placement.z + vec2(float(t) * 37.1, float(t) * 11.7));
            density *= smoothstep(1.0 - coverage - 0.15, 1.0 - coverage + 0.15, n);
        }
        if (type.ring.x > 0.0)
            density *= clutterRing(xz, type.ring, typeSeed);
        const float keep = density * densityScale * area * rangeFade;
        if (rank >= keep)
            continue;

        // The ground under it: the drawn mesh (the slope gate and the lean), above the water.
        GrassGround g;
        if (!clutterGroundAt(xz, g))
            continue;
        vec3 groundN;
        const float groundY = grassGroundHeight(g, xz, groundN);
        const float slope = sqrt(max(1.0 - groundN.y * groundN.y, 0.0)) / max(groundN.y, 1e-3);
        if (slope > type.terms3.z || groundY - water < type.terms3.w)
            continue;

        const float grow = clamp((keep - rank) / max(u_clutter_growBand * keep, 1e-3), 0.0, 1.0);
        const uint h = grassHash(listHash ^ (k * 0x85EBCA6Bu));
        const uint kind = type.info.z;
        const uint variants = max(type.info.y, 1u);
        const uint variant = kind == CLUTTER_KIND_FLOWER ? 0u : h % variants;
        // A FLOWER shrinks with the grass's cover as the blades do ("Grass/Cover/Size by cover", grassCoverSize): where
        // the cover fades (the beach band at the shore, a dry edge) the few blades left are tiny and the ground reads
        // bare - full-size flowers stood out there.
        const float coverSize = kind == CLUTTER_KIND_FLOWER ? grassCoverSize(cover.x) : 1.0;
        const float scale = mix(type.shape.x, type.shape.y, grassUnit(grassHash(h + 1u))) * grow * coverSize;
        if (scale <= 0.0)
            continue;

        // Its turn: a yaw, then up -> the ground's normal by its align (a flower stands upright: its VS reads the yaw only).
        const float yaw = grassUnit(grassHash(h + 2u)) * 6.28318530718;
        const vec4 qYaw = vec4(0.0, sin(0.5 * yaw), 0.0, cos(0.5 * yaw));
        const float align = kind == CLUTTER_KIND_FLOWER ? 0.0 : type.albedo1.w;
        const vec4 q = clutterQuatMul(clutterQuatFromUp(normalize(mix(vec3(0.0, 1.0, 0.0), groundN, align))), qYaw);

        // Bounds, the sink and the LOD.
        const uint mesh = type.info.x + variant;
        float height, radius;
        if (kind == CLUTTER_KIND_FLOWER)
        {
            height = type.flower.x * scale;
            radius = 0.6 * height + type.flower.y * scale;
        }
        else
        {
            height = in_meshes[mesh].bounds.y * scale;
            radius = in_meshes[mesh].bounds.x * scale;
        }
        const vec3 up = clutterQuatRotate(q, vec3(0.0, 1.0, 0.0));
        // On the DRAWN ground: the mesh point + the tessellated relief there (over the object's footprint).
        const vec3 groundPos = vec3(xz.x, groundY, xz.y);
        const vec3 relief = clutterRelief(groundPos, grassGroundSmoothNormal(g, xz), kind == CLUTTER_KIND_FLOWER ? 0.0 : 0.35 * radius);
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
            const float size = radius / max(dist, 1e-3);
            lod = size > u_clutter_lod1Size ? 0u : size > u_clutter_lod2Size ? 1u : 2u;
            while (lod > 0u && in_meshes[mesh].lods[lod].y == 0u)
                --lod;
        }

        const uint slot = atomicAdd(io_counts[0], 1u);
        if (slot >= CLUTTER_MAX_INSTANCES)
            continue;
        const uint bucket = clutterBucket(kind, mesh, lod);
        const uint local = atomicAdd(io_bucketCounts[bucket], 1u);
        // The colours vary per object (+-12 %), as the grass blades do.
        const float variation = 1.0 + 0.24 * (grassUnit(grassHash(h + 3u)) - 0.5);
        const vec3 albedo0 = clamp(type.albedo0.rgb * variation, vec3(0.0), vec3(1.0));
        ClutterInstance inst;
        inst.posScale = vec4(base, scale);
        inst.data = uvec4(packHalf2x16(q.xy), packHalf2x16(q.zw), t | (variant << 8) | (lod << 16) | (kind << 24), h);
        inst.look = uvec4(packUnorm4x8(vec4(sqrt(albedo0), type.albedo0.w)),
            packUnorm4x8(vec4(sqrt(clamp(type.albedo1.rgb, vec3(0.0), vec3(1.0))), clamp(type.bound.y, 0.0, 1.0))),
            packHalf2x16(kind == CLUTTER_KIND_FLOWER ? type.flower.xy : vec2(height, 0.0)), // a rigid object: its height (the contact band)
            type.info.w | (uint(clamp(type.flower.z, 0.0, 1.0) * 255.0 + 0.5) << 16)
                | (uint(clamp(type.flower.w / 3.14159265 + 0.5, 0.0, 1.0) * 255.0 + 0.5) << 24));
        out_flat[slot] = inst;
        out_keys[slot] = (bucket << CLUTTER_LOCAL_BITS) | local;
    }
}
