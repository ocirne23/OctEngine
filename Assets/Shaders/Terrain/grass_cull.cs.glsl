#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE GRASS CELL CULL (GrassPipeline): one thread per CELL SLOT of the rings around the camera.
// THE RINGS: gf_numLevels nested blocks of GRASS_RING_CELLS^2 cells, level k's cells 2^k base cells wide (the blades
// belong to the BASE cells of a world grid - grass_blade.inc.glsl - so a cell of any level draws the same blades). A
// block's edges lie on the next level's grid, so that level's cells are all inside it (not drawn: the finer level has
// them) or all outside. A slot = level x cells + the cell's place in the block's near-first order (gf_cellOrder): the
// draw lists are in the slots' order, NEAR TO FAR, with no sort - EVERY slot writes its draw, an empty one where the
// cell is culled.
// A cell on a resident terrain chunk, inside the range and the frustum, with grass at any corner, gets ONE indexed draw
// of the first K blades of its ring's LOD (ONE LOD per ring) and its record (the VS's instance-rate attributes,
// firstInstance = the slot). K never passes the ring's worst case (gf_levelMesh.z: the mesh section's size).
// Density per corner: THE TERRAIN TEXTURES' OWN LOGIC (terrainLayers, fed as the terrain VS feeds it): grass only where
// the GROUND layer shows, times the grass amount of the ground textures the climate picks there. Corners are shared by
// the cells of a level, so the bilinear density the VS reads is continuous across their borders.

#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#include "mesh_vertex.inc.glsl"
layout (binding = 9) uniform sampler2D u_terrainNoise; // the terrain noise texture (the crag wander): no bindless array here
#define TERRAIN_NOISE_SAMPLER u_terrainNoise
#define GRASS_NOISE_SAMPLER u_terrainNoise // grass.inc.glsl's value noise (its A)
// The snow site map (snow.inc.glsl): bound directly, and read in the window it held before this frame's trace.
layout (binding = 10) uniform sampler2D u_snowSiteTop;
layout (binding = 11) uniform sampler2D u_snowSitePass;
#define SNOW_SITE_TOP_SAMPLER u_snowSiteTop
#define SNOW_SITE_PASS_SAMPLER u_snowSitePass
#define SNOW_SITE_ORIGIN u_snow_siteReadOrigin
#define TERRAIN_SPLAT_HEIGHT_ONLY // the coverages only: no material texture taps
#include "terrain_splat.inc.glsl"

layout (local_size_x = GRASS_CULL_GROUP) in;

layout (binding = 2, std430) readonly buffer InGrassFrame // RendererVKLayout::GrassFrameGpu
{
    uint gf_numLevels;  // ring levels in use (0 = no grass)
    float gf_patchSize; // m: the base cell
    uint gf_pad0, gf_pad1;
    ivec2 gf_tableMin;
    uint gf_tableDim;
    float gf_chunkSize;
    uvec2 gf_chunks[GRASS_TABLE_DIM * GRASS_TABLE_DIM];
    vec4 gf_levelBlock[GRASS_RING_LEVELS];   // xy = the block's min corner, z = its cell size (m)
    uvec4 gf_levelMesh[GRASS_RING_LEVELS];   // x = the ring's LOD, y = that LOD's first index, z = the most blades a cell draws,
                                             // w = 1: the next ring is one LOD coarser (the blades morph to it)
    uvec4 gf_levelCaster[GRASS_RING_LEVELS]; // the near casters (one LOD coarser): x = their LOD's first index, y = the most blades
    uint gf_cellOrder[GRASS_RING_CELLS * GRASS_RING_CELLS]; // a block's cells, nearest its centre first: x | z << 8
};
layout (binding = 3, std430) readonly buffer InTerrainVertices { TerrainVertex in_terrainVertices[]; };

#include "grass.inc.glsl"

struct GrassPatch // RendererVKLayout::GrassPatchGpu
{
    vec2 origin;
    vec2 chunkOrigin;
    uint firstVertex;
    uint resLod;
    uint density;
    uint cellTemperature; // packHalf2x16(the chunk's cell size (m), the patch's mean temperature (C))
    uint snow;            // the corners' ground white (TerrainLayers::snowW), unorm8 x 4 as density: the blades' snow
    uint pad;
};
layout (binding = 4, std430) writeonly buffer OutPatches { GrassPatch out_patches[]; };

// The draw lists, as words: a VkDrawIndexedIndirectCommand per cell slot (5 words, firstInstance = the slot: its record).
// The main draws, and the near grass cascade's caster draws. (The draw counts are the slots: GrassPipeline::record.)
layout (binding = 5, std430) writeonly buffer OutCommands { uint out_commands[]; };
layout (binding = 7, std430) writeonly buffer OutNearShadowCommands { uint out_nearShadowCommands[]; };
// THE FOREST FLOOR MAP (the clutter's: ClutterPipeline's frame; floorDim 0 = none): "Canopy thinning" under the crowns.
#define CLUTTER_FRAME_BINDING 8
#include "clutter.inc.glsl"

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
// Fed like the terrain VS feeds the splat. No texture set registered yet (the startup bake): no grass.
// `temperature`: C at the point's height (the blades' cold tint), 12.5 without a map (the terrain VS's fallback).
// `snow`: how white the ground is there (TerrainLayers::snowW: the pack, the permanent snow, shelter, slope): the blades
// take that white x "Grass snow" (grass.fs.glsl). From the layers this computes anyway: no cost of its own.
float grassDensityAt(vec2 xz, float h, vec3 smoothN, float river, out float temperature, out float snow)
{
    temperature = 12.5;
    snow = 0.0;
    if (u_terrain_splatBase < 0.0 || u_terrain_numGround < 1.0 || !terrainHeightMapPresent())
        return 0.0;

    const vec4 td = terrainDataAt(xz);
    const vec4 climate = terrainClimateAt(xz);
    temperature = terrainTemperatureAt(climate, h);
    const TerrainFields fields = TerrainFields(td.w, temperature, climate.w, td.y, river);
    const TerrainLayers L = terrainLayers(vec3(xz.x, h, xz.y), smoothN, fields);
    snow = float(L.snowW);
    // Under a closed canopy a forest floor holds little grass: x (1 - "Canopy thinning" x the crown cover).
    return grassTerrainCover(L) // grass.inc.glsl
        * (1.0 - u_grass_canopyThinning * clutterFloorAt(xz).x);
}

// The cell of `slot`: its record, and its two draws (indexCount, instanceCount, firstIndex, vertexOffset) - left empty
// where it draws nothing.
void grassCell(uint slot, inout uvec4 draw, inout uvec4 casterDraw)
{
    const uint cellsPerLevel = uint(GRASS_RING_CELLS * GRASS_RING_CELLS);
    const uint level = slot / cellsPerLevel;
    if (level >= gf_numLevels)
        return;
    const uint cell = gf_cellOrder[slot % cellsPerLevel];
    const vec4 block = gf_levelBlock[level];
    const float P = block.z; // the cell's size
    const vec2 origin = block.xy + vec2(float(cell & 0xFFu), float(cell >> 8)) * P;
    const vec2 centre = origin + 0.5 * P;
    // Inside the finer level's block (its edges are on this level's grid): that level draws it.
    if (level > 0u)
    {
        const vec4 inner = gf_levelBlock[level - 1u];
        const vec2 rel = centre - inner.xy;
        if (all(greaterThan(rel, vec2(0.0))) && all(lessThan(rel, vec2(float(GRASS_RING_CELLS) * inner.z))))
            return;
    }

    // Range, horizontally first (no fetches): the nearest point of the cell's square.
    const vec2 nearestXZ = clamp(u_viewPos.xz, origin, origin + P);
    const float range = u_grass_range;
    if (distance(nearestXZ, u_viewPos.xz) > range)
        return;

    GrassGround g;
    if (!grassGroundAt(centre, g))
        return;

    // The 4 corners: height (the bounds) and density.
    float hMin = 1e30, hMax = -1e30;
    vec4 density, snow;
    float temperatureSum = 0.0;
    for (uint i = 0u; i < 4u; ++i)
    {
        const vec2 corner = origin + vec2(float(i & 1u), float(i >> 1u)) * P;
        vec3 facetN;
        const float h = grassGroundHeight(g, corner, facetN);
        hMin = min(hMin, h);
        hMax = max(hMax, h);
        float temperature;
        density[i] = grassDensityAt(corner, h, grassGroundSmoothNormal(g, corner), grassGroundRiver(g, corner), temperature, snow[i]);
        temperatureSum += temperature;
    }
    const float maxDensity = max(max(density.x, density.y), max(density.z, density.w));
    if (maxDensity <= 0.0)
        return;

    // Bounds: the ground between the corners can rise above them (a finer mesh than the cell: half the cell's size of
    // slack), the blades stand on it and lean with the wind by up to their height - the tallest grass type's (a seed
    // stalk's) - and a tuft's blades stand up to its radius off its root (which is in the cell).
    const float bladeH = u_grass_bladeHeight * u_grass_maxHeightScale + u_grass_rootSink;
    const float reach = bladeH + u_grass_tuftRadius;
    const float slack = 0.5 * P + bladeH;
    const vec3 boxMin = vec3(origin.x - reach, hMin - u_grass_rootSink - 1.0, origin.y - reach);
    const vec3 boxMax = vec3(origin.x + P + reach, hMax + slack, origin.y + P + reach);
    const vec3 sphereCentre = 0.5 * (boxMin + boxMax);
    const float sphereRadius = 0.5 * length(boxMax - boxMin);
    bool visible = true;
    for (int i = 0; i < 6; ++i)
        if (dot(vec4(sphereCentre, 1.0), u_frustumPlanes[i]) + sphereRadius < 0.0)
            visible = false;

    // The nearest point of the cell's box, with the box's slack below the corners: the ground between them can dip,
    // and no blade may be nearer than this (the kept blades' bound; each blade thins by its own distance).
    const vec3 nearest = clamp(u_viewPos, vec3(origin.x, boxMin.y, origin.y), vec3(origin.x + P, hMax + bladeH, origin.y + P));
    const float dist = distance(nearest, u_viewPos);

    // THE NEAR GRASS CASCADE's casters, in view or not (a blade just off screen still casts): within half size x 1.5 +
    // 2 m of its box's centre (as grass.vs.glsl's near caster).
    const vec2 nearCentre = u_grass_nearCentre;
    const bool nearCaster = u_grass_nearRange > 0.0
        && distance(clamp(nearCentre, origin, origin + P), nearCentre) <= u_grass_nearRange * 1.5 + 2.0;
    if (!visible && !nearCaster)
        return;

    // THE BLADES: the first `ranks` of EACH base cell in it (a blade's index = its rank x the cell's base cells + its
    // base cell: grass_blade.inc.glsl grassBaseCell - so a prefix is the low ranks of them all), at most the ring's
    // worst case (the mesh section).
    const float keep = grassKeep(dist);
    const uint N = uint(u_grass_bladesPerPatch);
    const uint ranks = min(N, uint(ceil(maxDensity * keep * float(N))));
    if (ranks == 0u)
        return;
    const uvec4 mesh = gf_levelMesh[level];
    const uint lod = mesh.x;
    const uint blades = ranks << (2u * level);

    out_patches[slot] = GrassPatch(origin, g.chunkOrigin, g.firstVertex, g.res | (lod << 16) | (level << 18) | (mesh.w << 21),
        packUnorm4x8(density), packHalf2x16(vec2(g.step, 0.25 * temperatureSum)), packUnorm4x8(snow), 0u);

    // The same blades in both lists (the caster is the blade the main pass shades), the casters ONE LOD COARSER
    // (grassCasterLod; the record holds the main LOD).
    if (visible)
        draw = uvec4(min(blades, mesh.z) * grassIndicesPerBlade(lod), 1u, mesh.y, 0u);
    if (nearCaster)
    {
        const uvec4 caster = gf_levelCaster[level];
        casterDraw = uvec4(min(blades, caster.y) * grassIndicesPerBlade(grassCasterLod(lod)), 1u, caster.x, 0u);
    }
}

void main()
{
    const uint slot = gl_GlobalInvocationID.x; // < GRASS_MAX_PATCHES: the dispatch is the slots
    uvec4 draw = uvec4(0u), casterDraw = uvec4(0u);
    grassCell(slot, draw, casterDraw);
    for (uint w = 0u; w < 4u; ++w)
    {
        out_commands[slot * 5u + w] = draw[w];
        out_nearShadowCommands[slot * 5u + w] = casterDraw[w];
    }
    out_commands[slot * 5u + 4u] = slot;
    out_nearShadowCommands[slot * 5u + 4u] = slot;
}
