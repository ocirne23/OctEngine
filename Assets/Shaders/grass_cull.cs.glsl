#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE GRASS PATCH CULL (GrassPipeline): one thread per patch of the grid around the camera (the CPU's ground table,
// GrassFrameGpu). A patch on a resident terrain chunk, inside the range and the frustum, with grass at any corner,
// gets ONE indexed draw of the first K blades of its LOD's blade mesh (grass.inc.glsl) and its record - read by the
// grass vertex shader as instance-rate attributes (firstInstance = the slot).
// Density per corner: THE TERRAIN TEXTURES' OWN LOGIC (terrain_splat.inc.glsl terrainLayers, fed exactly as the
// terrain VS feeds it): grass only where the GROUND layer shows - not under rock, beach or snow - times the grass
// amount of the ground textures the climate picks there (TerrainSplatMaterial::grass: the grassland texture 1, sand
// 0). Corners are shared by 4 patches, so the bilinear density the vertex shader reads is continuous across borders.

#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#include "mesh_vertex.inc.glsl"
#define TERRAIN_SPLAT_HEIGHT_ONLY // the coverages only: no texture taps
#include "terrain_splat.inc.glsl"

layout (local_size_x = GRASS_CULL_GROUP) in;

layout (binding = 2, std430) readonly buffer InGrassFrame // RendererVKLayout::GrassFrameGpu
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

#include "grass.inc.glsl"

struct GrassPatch // RendererVKLayout::GrassPatchGpu
{
    vec2 origin;
    vec2 chunkOrigin;
    uint firstVertex;
    uint resLod;
    uint density;
    uint cellTemperature; // packHalf2x16(the chunk's cell size (m), the patch's mean temperature (C))
};
layout (binding = 4, std430) writeonly buffer OutPatches { GrassPatch out_patches[]; };
struct DrawIndexedIndirect // VkDrawIndexedIndirectCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout (binding = 5, std430) writeonly buffer OutCommands { DrawIndexedIndirect out_commands[]; };
// [0] = the main draws, [1] = the near grass cascade's caster draws (the draw counts), [2] = the patch records (every
// list's firstInstance).
layout (binding = 6, std430) buffer OutCounts { uint out_counts[3]; };
layout (binding = 7, std430) writeonly buffer OutNearShadowCommands { DrawIndexedIndirect out_nearShadowCommands[]; };

bool grassGroundAt(vec2 xz, out GrassGround g)
{
    const ivec2 cell = ivec2(floor(xz / gf_chunkSize)) - gf_tableMin;
    g = GrassGround(vec2(0.0), 0u, 0u, 1.0);
    if (any(lessThan(cell, ivec2(0))) || any(greaterThanEqual(cell, ivec2(gf_tableDim))))
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

// How much grass grows at a ground point [0, 1]: where the terrain draws its ground textures, by their grass amount.
// Fed like the terrain VS feeds the splat (instanced_indirect_terrain.vs.glsl): the baked fields at the point, the
// temperature at its height, the smooth mesh normal. No texture set registered yet (the startup bake): no grass.
// `temperature`: C at the point's height (the blades' cold tint), 12.5 without a map (the terrain VS's fallback).
float grassDensityAt(vec2 xz, float h, vec3 smoothN, out float temperature)
{
    temperature = 12.5;
    if (u_terrainTexParams0.x < 0.0 || u_terrainTexParams0.y < 1.0 || !terrainHeightMapPresent())
        return 0.0;
    const vec4 td = terrainDataAt(xz);
    const vec4 climate = terrainClimateAt(xz);
    temperature = terrainTemperatureAt(climate, h);
    const TerrainFields fields = TerrainFields(td.w, temperature, climate.w, td.y);
    return grassTerrainCover(terrainLayers(vec3(xz.x, h, xz.y), smoothN, fields)); // grass.inc.glsl
}

void main()
{
    const uint idx = gl_GlobalInvocationID.x;
    if (idx >= gf_gridDim * gf_gridDim)
        return;
    const float P = gf_patchSize;
    const vec2 origin = gf_gridOrigin + vec2(float(idx % gf_gridDim), float(idx / gf_gridDim)) * P;
    const vec2 centre = origin + 0.5 * P;

    // Range, horizontally first (no fetches): the nearest point of the patch's square.
    const vec2 nearestXZ = clamp(u_viewPos.xz, origin, origin + P);
    const float range = u_grassParams0.z;
    if (distance(nearestXZ, u_viewPos.xz) > range)
        return;

    GrassGround g;
    if (!grassGroundAt(centre, g))
        return;

    // The 4 corners: height (the bounds) and density.
    float hMin = 1e30, hMax = -1e30;
    vec4 density;
    float temperatureSum = 0.0;
    for (uint i = 0u; i < 4u; ++i)
    {
        const vec2 corner = origin + vec2(float(i & 1u), float(i >> 1u)) * P;
        vec3 facetN;
        const float h = grassGroundHeight(g, corner, facetN);
        hMin = min(hMin, h);
        hMax = max(hMax, h);
        float temperature;
        density[i] = grassDensityAt(corner, h, grassGroundSmoothNormal(g, corner), temperature);
        temperatureSum += temperature;
    }
    const float maxDensity = max(max(density.x, density.y), max(density.z, density.w));
    if (maxDensity <= 0.0)
        return;

    // Bounds: the ground between the corners can rise above them (a finer mesh than the patch), the blades stand
    // on it and lean with the wind by up to their height.
    const float bladeH = u_grassParams1.x + u_grassParams1.w;
    const float slack = 0.5 * P + bladeH;
    const vec3 boxMin = vec3(origin.x - bladeH, hMin - u_grassParams1.w - 1.0, origin.y - bladeH);
    const vec3 boxMax = vec3(origin.x + P + bladeH, hMax + slack, origin.y + P + bladeH);
    const vec3 sphereCentre = 0.5 * (boxMin + boxMax);
    const float sphereRadius = 0.5 * length(boxMax - boxMin);
    bool visible = true;
    for (int i = 0; i < 6; ++i)
        if (dot(vec4(sphereCentre, 1.0), u_frustumPlanes[i]) + sphereRadius < 0.0)
            visible = false;

    // The nearest point of the patch's box, with the box's slack below the corners: the ground between them can dip,
    // and no blade may be nearer than this (the LOD is picked from it, and the blades geomorph by their own distance).
    const vec3 nearest = clamp(u_viewPos, vec3(origin.x, boxMin.y, origin.y), vec3(origin.x + P, hMax + bladeH, origin.y + P));
    const float dist = distance(nearest, u_viewPos);
    // THE NEAR GRASS CASCADE's casters, in view or not (a blade just off screen still casts): within half size x 1.5 +
    // 2 m of its box's centre (u_grassParams14.yz - ahead of the camera; as grass.vs.glsl's near caster).
    const vec2 nearCentre = u_grassParams14.yz;
    const bool nearCaster = u_grassParams13.y > 0.0
        && distance(clamp(nearCentre, origin, origin + P), nearCentre) <= u_grassParams13.y * 1.5 + 2.0;
    if (!visible && !nearCaster)
        return;
    const float keep = grassKeep(dist);
    const uint N = uint(u_grassParams0.x);
    const uint blades = min(N, uint(ceil(maxDensity * keep * float(N))));
    if (blades == 0u)
        return;
    const uint lod = dist < u_grassParams3.x ? 0u : dist < u_grassParams3.y ? 1u : dist < u_grassParams10.x ? 2u : 3u;

    const uint slot = atomicAdd(out_counts[2], 1u);
    if (slot >= GRASS_MAX_PATCHES)
        return;
    out_patches[slot] = GrassPatch(origin, g.chunkOrigin, g.firstVertex, g.res | (lod << 16), packUnorm4x8(density),
        packHalf2x16(vec2(g.step, 0.25 * temperatureSum)));
    // The same draw in both lists (the same LOD: the caster is the blade the main pass shades).
    const DrawIndexedIndirect draw = DrawIndexedIndirect(blades * grassIndicesPerBlade(lod), 1u, grassLodFirstIndex(lod, N), 0, slot);
    if (visible)
        out_commands[atomicAdd(out_counts[0], 1u)] = draw; // <= slot < GRASS_MAX_PATCHES
    if (nearCaster)
        out_nearShadowCommands[atomicAdd(out_counts[1], 1u)] = draw;
}
