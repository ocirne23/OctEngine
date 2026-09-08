// Light grid data structure accessors and utilities
// ReadAPI: Required defines: GRID_DATA_NAME, TABLE_SIZE_NAME, GRID_TABLE_NAME
ivec3 getGridPos(vec3 pos);
ivec3 getGridMin(uint gridIdx);
uint getTableIdx(ivec3 gridPos);
uint getNextTableIdx(uint idx);

uint getGridIdx(uint tableIdx);
uint getCellOffset(uint gridIdx, uint cellIdx);
uint getCellSize(uint gridIdx);
uint getNumLightsForCell(uint cellOffset);
uint getLightId(uint cellOffset, uint lightIdx);

uint getLargeLightCount(uint gridIdx);
uint getLargeLightId(uint gridIdx, uint lightIdx);

#ifdef GRID_DATA_WRITE
// WriteAPI: Required defines: GRID_DATA_WRITE, GRID_DATA_NAME, TABLE_SIZE_NAME, GRID_TABLE_NAME, GRID_DATA_COUNTER_NAME, NUM_GRIDS_NAME
void setGridMin(uint gridIdx, ivec3 gridMin);
void setCellSize(uint gridIdx, uint cellSize);
void addLightToCell(uint gridIdx, uint cellIdx, uint lightId);
void addLargeLight(uint gridIdx, uint lightId);
uint getGridMemoryUsage(uint cellSize);
uint getOrInsertGrid(ivec3 gridPos, uint cellSize);
void addLightToGrid(uint gridIdx, uint lightId, ivec3 minCell, ivec3 maxCell, vec3 sphereCenter, float sphereRadius);
#endif

#define MAX_LARGE_LIGHTS_PER_GRID 14 // Must be even
#define MAX_LIGHTCELL_LIGHTS 16      // Must be even
#define GRID_HEADER_SIZE (4 + (MAX_LARGE_LIGHTS_PER_GRID / 2 + 1)) // uints before the cells
// GRID_SIZE is defined in hash_grid.inc.glsl (included below).

// GRID DATA MEMORY LAYOUT:
// {
//     ivec3 gridMin;
//     uint gridSize;
//     uint numLargeLights;
//     uint16_t largeLightIds[MAX_LARGE_LIGHTS_PER_GRID];
//     struct
//     {
//         uint numLights;
//         uint16_t lightIds[MAX_LIGHTCELL_LIGHTS];
//     } cells[(GRID_SIZE / cellSize)^3];
// }

#ifndef GRID_DATA_NAME
#error "GRID_DATA_NAME not set"
#endif

#ifndef TABLE_SIZE_NAME
#error "TABLE_SIZE_NAME not set"
#endif

#ifndef GRID_TABLE_NAME
#error "GRID_TABLE_NAME not set"
#endif

// Hashing, table probing, GRID_SIZE and getGridPos are shared with the GI probe grid.
#include "hash_grid.inc.glsl"

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

#ifdef GRID_DATA_WRITE

#ifndef GRID_DATA_COUNTER_NAME
#error "GRID_DATA_COUNTER_NAME not set"
#endif
#ifndef NUM_GRIDS_NAME
#error "NUM_GRIDS_NAME not set"
#endif

void setGridMin(uint gridIdx, ivec3 gridMin)
{
	GRID_DATA_NAME[gridIdx + 0] = gridMin.x;
	GRID_DATA_NAME[gridIdx + 1] = gridMin.y;
	GRID_DATA_NAME[gridIdx + 2] = gridMin.z;
}

void setCellSize(uint gridIdx, uint cellSize)
{
	GRID_DATA_NAME[gridIdx + 3] = cellSize;
}

void addLightToCell(uint gridIdx, uint cellIdx, uint lightId)
{
	const uint cellOffset = getCellOffset(gridIdx, cellIdx);
	const uint lightIdx = atomicAdd(GRID_DATA_NAME[cellOffset], uint(1));
	if (lightIdx < MAX_LIGHTCELL_LIGHTS)
	{
		atomicOr(GRID_DATA_NAME[cellOffset + 1 + lightIdx / 2], uint(lightId) << ((lightIdx & 1u) == 0u ? 0 : 16));
	}
}

void addLargeLight(uint gridIdx, uint lightId)
{
	const uint numLargeLightsOffset = gridIdx + 4;
	const uint largeLightIdx = atomicAdd(GRID_DATA_NAME[numLargeLightsOffset], uint(1));
	if (largeLightIdx < MAX_LARGE_LIGHTS_PER_GRID)
	{
		atomicOr(GRID_DATA_NAME[numLargeLightsOffset + 1 + largeLightIdx / 2], uint(lightId) << ((largeLightIdx & 1u) == 0u ? 0 : 16));
	}
}

uint getGridMemoryUsage(uint cellSize)
{
	const uint numCells = GRID_SIZE / cellSize;
	return GRID_HEADER_SIZE + numCells * numCells * numCells * (MAX_LIGHTCELL_LIGHTS / 2 + 1);
}

// Returned when the table or grid data buffer is out of space: the light is dropped for this frame.
// The CPU reads the (deliberately still-incremented) usage counters back and grows the buffers to fit,
// so the overflow lasts one frame instead of writing out of bounds (-> device lost).
#define INVALID_GRID 0xFFFFFFFFu

uint getOrInsertGrid(ivec3 gridPos, uint cellSize)
{
	// Stop inserting past 75% table load: keeps EMPTY entries so read/write probe loops terminate
	// (the CPU grows the table at 50% load, so this only triggers on a burst within one frame).
	if (NUM_GRIDS_NAME * 4u >= TABLE_SIZE_NAME * 3u)
		return INVALID_GRID;
	uint idx = getTableIdx(gridPos);
	while (true)
	{
		memoryBarrierBuffer();
		uint gridIdx = GRID_TABLE_NAME[idx];
		if (gridIdx < INITIALIZING_ENTRY && gridIdx < GRID_DATA_COUNTER_NAME)
		{
			if (getGridMin(gridIdx) == gridPos)
			{
				return gridIdx;
			}
			idx = getNextTableIdx(idx);
		}
		else if (atomicCompSwap(GRID_TABLE_NAME[idx], EMPTY_ENTRY, INITIALIZING_ENTRY) == EMPTY_ENTRY)
		{
			const uint memSize = getGridMemoryUsage(cellSize);
			const uint newGridIdx = atomicAdd(GRID_DATA_COUNTER_NAME, memSize);
			if (newGridIdx + memSize > uint(GRID_DATA_NAME.length()))
			{
				// Out of grid data memory: release the claimed table slot and drop the light. The
				// counter stays inflated on purpose - the CPU readback uses it to size the growth.
				atomicExchange(GRID_TABLE_NAME[idx], EMPTY_ENTRY);
				return INVALID_GRID;
			}
			setGridMin(newGridIdx, gridPos);
			setCellSize(newGridIdx, cellSize);
			memoryBarrierBuffer();
			atomicExchange(GRID_TABLE_NAME[idx], newGridIdx);
			atomicAdd(NUM_GRIDS_NAME, uint(1));
			return newGridIdx;
		}
	}
}

// Adds the light to the cells [minCell, maxCell] (cell coordinates inside this grid, already
// clamped by the caller). sphereRadius > 0: cells whose box misses the sphere are skipped — the
// corners of a range box are ~48% of its volume, and every skipped cell is two atomics saved.
void addLightToGrid(uint gridIdx, uint lightId, ivec3 minCell, ivec3 maxCell, vec3 sphereCenter, float sphereRadius)
{
	const vec3 gridMinW  = vec3(getGridMin(gridIdx) * GRID_SIZE);
	const uint cellSize  = getCellSize(gridIdx);
	const float cellSizeF = float(cellSize);
	const uint numCells  = GRID_SIZE / cellSize;
	const uint numCellsSq = numCells * numCells;
	const float radiusSq = sphereRadius * sphereRadius;
	for (int x = minCell.x; x <= maxCell.x; ++x)
	{
		for (int y = minCell.y; y <= maxCell.y; ++y)
		{
			for (int z = minCell.z; z <= maxCell.z; ++z)
			{
				if (sphereRadius > 0.0)
				{
					const vec3 cellMin = gridMinW + vec3(x, y, z) * cellSizeF;
					const vec3 d = sphereCenter - clamp(sphereCenter, cellMin, cellMin + cellSizeF);
					if (dot(d, d) > radiusSq)
						continue;
				}
				const uint cellIdx = uint(x) + uint(y) * numCells + uint(z) * numCellsSq;
				addLightToCell(gridIdx, cellIdx, lightId);
			}
		}
	}
}

#endif