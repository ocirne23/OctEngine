// Forcefield emitter hash grid (a user of hash_grid.inc.glsl, like the light grid and the GI probe grid): UNIFORM world
// space FORCE_GRID_CELL_SIZE cells, NOT camera-adaptive (gameplay evaluation happens anywhere in the world). An emitter is
// inserted into every cell its reach bounds overlap, so a cell lists every emitter whose compact support can reach it -
// an EMPTY cell provably has zero field (the union march's skip relies on this).
//
// BUILT ON THE CPU (ForceFieldPipeline::buildGrid) and written straight into these buffers: this include is READ-ONLY.
//
// TABLE BUFFER: { uint numCells; uint dataCounter; uint tableSize; uint pad; uint table[]; }
//   table[] holds SLOT indices (the CPU claim table, memcpy'd); a cell's record starts at slot * FORCE_CELL_UINTS.
// DATA BUFFER, per cell (FORCE_CELL_UINTS uints):
//   { ivec3 cellPos; uint count; uint16 emitterIds[FORCE_CELL_MAX_EMITTERS]; }
//   (count is the TRUE candidate count - it may exceed the cap; forceCellCount clamps)

#ifndef FORCE_GRID_INC_GLSL
#define FORCE_GRID_INC_GLSL

#include "hash_grid.inc.glsl"

// Finer than the shared GRID_SIZE (32) of the light grid: the gather cost scales with how many small emitters one cell
// collects, and a swarm battle packs dozens of unit bubbles into a 32 m cell. Also the union march's empty-space skip
// granularity. MIRRORED in ForceFieldPipeline.cpp.
#define FORCE_GRID_CELL_SIZE 16.0
ivec3 forceGridPos(vec3 pos) { return ivec3(floor(pos / FORCE_GRID_CELL_SIZE)); }

#ifndef FORCE_TABLE_BINDING
#define FORCE_TABLE_BINDING 3
#endif
#ifndef FORCE_DATA_BINDING
#define FORCE_DATA_BINDING 4
#endif

layout (binding = FORCE_TABLE_BINDING, std430) readonly buffer ForceGridTable
{
    uint fg_numCells;
    uint fg_dataCounter;
    uint fg_tableSize;
    uint fg_tablePad;
    uint fg_table[];
};
layout (binding = FORCE_DATA_BINDING, std430) readonly buffer ForceGridData
{
    uint fg_data[];
};

#define FORCE_CELL_UINTS (4u + FORCE_CELL_MAX_EMITTERS / 2u)
#define FORCE_INVALID_CELL 0xFFFFFFFFu

uint forceTableIdx(ivec3 gridPos) { return hashTableIndex(gridPos, fg_tableSize); }
uint forceNextTableIdx(uint idx)  { return hashNextIndex(idx, fg_tableSize); }

ivec3 forceCellPos(uint cellIdx)
{
    return ivec3(int(fg_data[cellIdx]), int(fg_data[cellIdx + 1u]), int(fg_data[cellIdx + 2u]));
}

uint forceCellCount(uint cellIdx) { return min(fg_data[cellIdx + 3u], FORCE_CELL_MAX_EMITTERS); }

uint forceCellEmitter(uint cellIdx, uint k)
{
    const uint packed = fg_data[cellIdx + 4u + k / 2u];
    return (k & 1u) == 0u ? (packed & 0xFFFFu) : (packed >> 16);
}

// The cell containing gridPos, FORCE_INVALID_CELL when unoccupied. The CPU writes the table at <= 25% load, so chains
// stay short; the probe count is bounded for safety.
uint forceFindCell(ivec3 gridPos)
{
    uint idx = forceTableIdx(gridPos);
    for (uint probes = 0u; probes < 64u; ++probes)
    {
        const uint slot = fg_table[idx];
        if (slot == EMPTY_ENTRY)
            return FORCE_INVALID_CELL;
        const uint cellIdx = slot * FORCE_CELL_UINTS;
        if (forceCellPos(cellIdx) == gridPos)
            return cellIdx;
        idx = forceNextTableIdx(idx);
    }
    return FORCE_INVALID_CELL;
}

#endif
