#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable

#include "shared.inc.glsl"

// GATHER pass of the light grid build. The CPU (LightGridComputePipeline::build) already decided
// which GRID_SIZE^3 grids exist, their cell size, their data offset, the hash table, and the list of
// lights that touch each grid (split into per-cell candidates and LARGE lights). One workgroup covers
// LIGHT_GRID_GATHER_GROUP consecutive cells of one grid; every thread owns ONE cell, loops the grid's
// candidate list, and writes the cell's count + packed ids in one go - no atomics, no spin, full
// occupancy, and a deterministic light order (the CPU's light index order).

// Per light, written by the CPU: [0] = box min.xyz, sphere radius (0 = box test only);
// [1] = box max.xyz, unused; [2] = sphere centre.xyz, unused.
layout (binding = 1, std430) readonly buffer InLightCull
{
    vec4 in_lightCull[];
};
// Per grid, LIGHT_GRID_JOB_STRIDE uints: gridMin.xyz, cellSize, dataOffset, lightBegin, lightCount,
// largeBegin, largeCount (LightGridComputePipeline::GridJob).
layout (binding = 2, std430) readonly buffer InGridJobs
{
    uint in_gridJobs[];
};
// Per workgroup: grid slot, first cell.
layout (binding = 3, std430) readonly buffer InWorkgroups
{
    uint in_workgroups[];
};
// The concatenated per-grid light id lists (cell candidates, then large lights, per grid).
layout (binding = 4, std430) readonly buffer InLightList
{
    uint in_lightList[];
};
layout (binding = 5, std430) buffer OutLightGrids
{
    uint out_gridData[];
};

#define GRID_DATA_NAME out_gridData
#include "light_grid.inc.glsl"

#define LIGHT_GRID_JOB_STRIDE 9
#define LIGHT_GRID_GATHER_GROUP 64

layout(local_size_x=LIGHT_GRID_GATHER_GROUP, local_size_y=1, local_size_z=1) in;

void main()
{
    const uint workgroup = gl_WorkGroupID.x;
    const uint gridSlot  = in_workgroups[workgroup * 2 + 0];
    const uint cellBegin = in_workgroups[workgroup * 2 + 1];

    const uint job = gridSlot * LIGHT_GRID_JOB_STRIDE;
    const ivec3 gridPos    = ivec3(in_gridJobs[job + 0], in_gridJobs[job + 1], in_gridJobs[job + 2]);
    const uint cellSize    = in_gridJobs[job + 3];
    const uint gridIdx     = in_gridJobs[job + 4];
    const uint lightBegin  = in_gridJobs[job + 5];
    const uint lightCount  = in_gridJobs[job + 6];
    const uint largeBegin  = in_gridJobs[job + 7];
    const uint largeCount  = in_gridJobs[job + 8];

    // The grid header, once per grid (the first workgroup's first thread).
    if (cellBegin == 0u && gl_LocalInvocationID.x == 0u)
    {
        out_gridData[gridIdx + 0] = uint(gridPos.x);
        out_gridData[gridIdx + 1] = uint(gridPos.y);
        out_gridData[gridIdx + 2] = uint(gridPos.z);
        out_gridData[gridIdx + 3] = cellSize;
        out_gridData[gridIdx + 4] = largeCount;
        for (uint i = 0u; i < MAX_LARGE_LIGHTS_PER_GRID / 2; ++i)
        {
            const uint lo = 2u * i, hi = 2u * i + 1u;
            uint packed = 0u;
            if (lo < largeCount) packed |= in_lightList[largeBegin + lo] & 0xFFFFu;
            if (hi < largeCount) packed |= (in_lightList[largeBegin + hi] & 0xFFFFu) << 16;
            out_gridData[gridIdx + 5 + i] = packed;
        }
    }

    const uint numCells   = GRID_SIZE / cellSize;
    const uint numCellsSq = numCells * numCells;
    const uint cellIdx    = cellBegin + gl_LocalInvocationID.x;
    if (cellIdx >= numCellsSq * numCells)
        return;

    // Same cell order as getCellIdx: x + y * numCells + z * numCells^2.
    const uvec3 cellPos = uvec3(cellIdx % numCells, (cellIdx / numCells) % numCells, cellIdx / numCellsSq);
    const vec3 cellMin  = vec3(gridPos * GRID_SIZE) + vec3(cellPos) * float(cellSize);
    const vec3 cellMax  = cellMin + vec3(float(cellSize));

    uint count = 0u;
    uint packed[MAX_LIGHTCELL_LIGHTS / 2];
    for (uint i = 0u; i < MAX_LIGHTCELL_LIGHTS / 2; ++i)
        packed[i] = 0u;

    for (uint i = 0u; i < lightCount; ++i)
    {
        const uint lightId = in_lightList[lightBegin + i];
        const vec4 boxMin  = in_lightCull[lightId * 3 + 0];
        const vec4 boxMax  = in_lightCull[lightId * 3 + 1];
        if (any(greaterThan(cellMin, boxMax.xyz)) || any(lessThan(cellMax, boxMin.xyz)))
            continue;
        if (boxMin.w > 0.0) // point / spot: the range sphere skips the box's corners (~half the cells)
        {
            const vec3 centre = in_lightCull[lightId * 3 + 2].xyz;
            const vec3 d = centre - clamp(centre, cellMin, cellMax);
            if (dot(d, d) > boxMin.w * boxMin.w)
                continue;
        }
        if (count < MAX_LIGHTCELL_LIGHTS)
            packed[count / 2] |= (lightId & 0xFFFFu) << ((count & 1u) == 0u ? 0 : 16);
        ++count; // the true demand: the readers clamp, the debug heat view shows saturation
    }

    const uint cellOffset = getCellOffset(gridIdx, cellIdx);
    out_gridData[cellOffset] = count;
    for (uint i = 0u; i < MAX_LIGHTCELL_LIGHTS / 2; ++i)
        out_gridData[cellOffset + 1 + i] = packed[i];
}
