// Forcefield emitter hash grid - a third user of hash_grid.inc.glsl (after the light grid and the
// GI probe grid), UNIFORM world-space FORCE_GRID_CELL_SIZE (16 m) cells (NOT camera-adaptive: gameplay force/query
// evaluation happens anywhere in the world, not just near the camera). Each occupied cell stores a
// fixed-capacity list of compact emitter indices; an emitter is inserted into every cell its reach
// bounds overlap, so a point's containing cell lists every emitter whose compact support can reach
// it - an EMPTY cell therefore provably has zero field (the union march's skip relies on this).
//
// TABLE BUFFER: { uint numCells; uint dataCounter; uint tableSize; uint pad; uint table[]; }
//   (numCells/dataCounter are the CPU capacity readback - on overflow the counter keeps
//   incrementing so the readback measures true demand, light-grid contract)
// DATA BUFFER, per cell (FORCE_CELL_UINTS uints):
//   { ivec3 cellPos; uint count; uint16 emitterIds[FORCE_CELL_MAX_EMITTERS]; }
//
// FORCE_GRID_WRITE additionally declares the insert side (force_grid.cs.glsl only).

#ifndef FORCE_GRID_INC_GLSL
#define FORCE_GRID_INC_GLSL

#include "hash_grid.inc.glsl"

// The FORCE grid's OWN cell size - finer than the shared GRID_SIZE (32) the light grid keeps:
// every evaluation's gather cost scales with how many small emitters one cell collects, and a
// swarm battle packs dozens of unit bubbles into a 32 m cell. 16 m quarters the gathered set;
// the insert side (an emitter touches ~8x the cells) is a single-thread-per-emitter pass that
// stays trivial next to any evaluation. Also the union march's empty-space skip granularity.
#define FORCE_GRID_CELL_SIZE 16.0
ivec3 forceGridPos(vec3 pos) { return ivec3(floor(pos / FORCE_GRID_CELL_SIZE)); }

#ifndef FORCE_TABLE_BINDING
#define FORCE_TABLE_BINDING 3
#endif
#ifndef FORCE_DATA_BINDING
#define FORCE_DATA_BINDING 4
#endif

// The insert pass MUST see these buffers coherent (light_grid.cs.glsl does the same): the cell-claim
// spin re-reads the table with a PLAIN load, and without coherent that load may be cached per
// invocation - the CAS loser then never observes the winner's atomicExchange publish and spins until
// the device is lost. Read-only consumers run strictly after the insert barrier and skip the
// qualifier (coherent bypasses caching).
#ifdef FORCE_GRID_WRITE
#define FORCE_GRID_QUALIFIER coherent
#else
#define FORCE_GRID_QUALIFIER
#endif

layout (binding = FORCE_TABLE_BINDING, std430) FORCE_GRID_QUALIFIER buffer ForceGridTable
{
    uint fg_numCells;
    uint fg_dataCounter;
    uint fg_tableSize;
    uint fg_tablePad;
    uint fg_table[];
};
layout (binding = FORCE_DATA_BINDING, std430) FORCE_GRID_QUALIFIER buffer ForceGridData
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

// Read side: the cell containing gridPos, FORCE_INVALID_CELL when unoccupied. Runs strictly after
// the insert pass's barrier, so the table holds only EMPTY or published entries. The probe count is
// bounded for safety (insert stops at 75% load, so chains stay short).
uint forceFindCell(ivec3 gridPos)
{
    uint idx = forceTableIdx(gridPos);
    for (uint probes = 0u; probes < 64u; ++probes)
    {
        const uint cellIdx = fg_table[idx];
        if (cellIdx == EMPTY_ENTRY)
            return FORCE_INVALID_CELL;
        if (cellIdx < INITIALIZING_ENTRY && forceCellPos(cellIdx) == gridPos)
            return cellIdx;
        idx = forceNextTableIdx(idx);
    }
    return FORCE_INVALID_CELL;
}

#ifdef FORCE_GRID_WRITE

// getOrInsertGrid clone (light_grid.inc.glsl): CAS-claim the table slot, bump-allocate the cell,
// publish; on data overflow release the slot but leave the counter inflated so the CPU readback
// measures true demand and grows the buffers (the cell's emitters drop for ONE frame).
uint forceGetOrInsertCell(ivec3 gridPos)
{
    // Stop inserting past 75% table load: keeps EMPTY entries so probe loops terminate.
    if (fg_numCells * 4u >= fg_tableSize * 3u)
        return FORCE_INVALID_CELL;
    uint idx = forceTableIdx(gridPos);
    // Bounded, unlike the light grid's: if a publish is ever not observed (visibility bug, extreme
    // contention) the emitter drops from this cell for ONE frame instead of spinning to device-lost.
    for (uint spin = 0u; spin < 4096u; ++spin)
    {
        memoryBarrierBuffer();
        const uint cellIdx = fg_table[idx];
        if (cellIdx < INITIALIZING_ENTRY && cellIdx < fg_dataCounter)
        {
            if (forceCellPos(cellIdx) == gridPos)
                return cellIdx;
            idx = forceNextTableIdx(idx);
        }
        else if (atomicCompSwap(fg_table[idx], EMPTY_ENTRY, INITIALIZING_ENTRY) == EMPTY_ENTRY)
        {
            const uint newIdx = atomicAdd(fg_dataCounter, FORCE_CELL_UINTS);
            if (newIdx + FORCE_CELL_UINTS > uint(fg_data.length()))
            {
                atomicExchange(fg_table[idx], EMPTY_ENTRY);
                return FORCE_INVALID_CELL;
            }
            fg_data[newIdx + 0u] = uint(gridPos.x);
            fg_data[newIdx + 1u] = uint(gridPos.y);
            fg_data[newIdx + 2u] = uint(gridPos.z);
            // count at +3 is already 0 (data buffer is fill-cleared each frame)
            memoryBarrierBuffer();
            atomicExchange(fg_table[idx], newIdx);
            atomicAdd(fg_numCells, 1u);
            return newIdx;
        }
    }
    return FORCE_INVALID_CELL;
}

void forceAddEmitterToCell(uint cellIdx, uint emitterIdx)
{
    const uint k = atomicAdd(fg_data[cellIdx + 3u], 1u);
    if (k < FORCE_CELL_MAX_EMITTERS)
        atomicOr(fg_data[cellIdx + 4u + k / 2u], emitterIdx << ((k & 1u) == 0u ? 0 : 16));
}

#endif // FORCE_GRID_WRITE

#endif
