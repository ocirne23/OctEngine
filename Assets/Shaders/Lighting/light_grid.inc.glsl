// Light grid data structure accessors (READ side). The grid set, the hash table and the per-grid
// candidate lists are built on the CPU (LightGridComputePipeline::build); light_grid.cs.glsl gathers
// the per-cell lists from them, so no shader ever inserts into this structure.
//
// Required defines: GRID_DATA_NAME. Optional: TABLE_SIZE_NAME + GRID_TABLE_NAME for the hash lookup
// (getTableIdx / getNextTableIdx / getGridIdx); the gather shader does without them.
ivec3 getGridPos(vec3 pos);
ivec3 getGridMin(uint gridIdx);

uint getCellOffset(uint gridIdx, uint cellIdx);
uint getCellSize(uint gridIdx);
uint getNumLightsForCell(uint cellOffset);
uint getLightId(uint cellOffset, uint lightIdx);

uint getLargeLightCount(uint gridIdx);
uint getLargeLightId(uint gridIdx, uint lightIdx);

#define MAX_LARGE_LIGHTS_PER_GRID 14 // Must be even
#define MAX_LIGHTCELL_LIGHTS 16      // Must be even
#define GRID_HEADER_SIZE (4 + (MAX_LARGE_LIGHTS_PER_GRID / 2 + 1)) // uints before the cells
// GRID_SIZE is defined in hash_grid.inc.glsl (included below).
// LightGridComputePipeline.cpp mirrors these three constants and the layout below: keep them in sync.

// GRID DATA MEMORY LAYOUT:
// {
//     ivec3 gridMin;
//     uint cellSize;
//     uint numLargeLights;
//     uint16_t largeLightIds[MAX_LARGE_LIGHTS_PER_GRID];
//     struct
//     {
//         uint numLights;
//         uint16_t lightIds[MAX_LIGHTCELL_LIGHTS];
//     } cells[(GRID_SIZE / cellSize)^3];
// }
// numLights / numLargeLights hold the TRUE candidate count (may exceed the caps): readers clamp.

#ifndef GRID_DATA_NAME
#error "GRID_DATA_NAME not set"
#endif

// Hashing, table probing, GRID_SIZE and getGridPos are shared with the GI probe grid.
#include "hash_grid.inc.glsl"

#if defined(TABLE_SIZE_NAME) && defined(GRID_TABLE_NAME)
// Thin wrappers binding the shared hash helpers to this grid's table size.
uint getTableIdx(ivec3 gridPos)
{
	return hashTableIndex(gridPos, TABLE_SIZE_NAME);
}

uint getNextTableIdx(uint idx)
{
	return hashNextIndex(idx, TABLE_SIZE_NAME);
}

uint getGridIdx(uint tableIdx)
{
	return GRID_TABLE_NAME[tableIdx];
}
#endif

ivec3 getGridMin(uint gridIdx)
{
	return ivec3(GRID_DATA_NAME[gridIdx + 0], GRID_DATA_NAME[gridIdx + 1], GRID_DATA_NAME[gridIdx + 2]);
}

uint getCellOffset(uint gridIdx, uint cellIdx)
{
	return gridIdx + GRID_HEADER_SIZE + cellIdx * (MAX_LIGHTCELL_LIGHTS / 2 + 1);
}

uint getCellSize(uint gridIdx)
{
	return GRID_DATA_NAME[gridIdx + 3];
}

uint getCellIdx(uint gridIdx, ivec3 gridMin, vec3 pos)
{
	const uint cellSize   = getCellSize(gridIdx);
	const uint numCells   = GRID_SIZE / cellSize;
	const ivec3 cellPos   = (ivec3(floor(pos)) - gridMin * GRID_SIZE) / int(cellSize);
	const uint numCellsSq = numCells * numCells;
	const uint cellIdx    = cellPos.x + cellPos.y * numCells + cellPos.z * numCellsSq;
	return cellIdx;
}

uint calcCellOffset(uint gridIdx, ivec3 gridMin, vec3 pos)
{
	const uint cellIdx = getCellIdx(gridIdx, gridMin, pos);
	return getCellOffset(gridIdx, cellIdx);
}

uint getLargeLightCount(uint gridIdx)
{
	return GRID_DATA_NAME[gridIdx + 4];
}

uint getNumLightsForCell(uint cellOffset)
{
	return GRID_DATA_NAME[cellOffset];
}

uint getLightId(uint cellOffset, uint lightIdx)
{
	const uint packed = GRID_DATA_NAME[cellOffset + 1 + lightIdx / 2];
	return (lightIdx & 1u) == 0u ? uint(packed & 0x0000FFFF) : uint(packed >> 16);
}

uint getLargeLightId(uint gridIdx, uint lightIdx)
{
	const uint packed = GRID_DATA_NAME[gridIdx + 5 + lightIdx / 2];
	return (lightIdx & 1u) == 0u ? uint(packed & 0x0000FFFF) : uint(packed >> 16);
}
