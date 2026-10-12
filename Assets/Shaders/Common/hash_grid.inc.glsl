// World-space spatial hash grid: GRID_SIZE^3 cubes, keyed by integer grid position, in a power-of-two table
// (open addressing, linear probing). Shared by the light grid, the GI probe grid and the force grid.
// The table size is an argument, not a macro, so one shader can use two grids of different size; each grid wraps
// these in its own getTableIdx / getNextTableIdx. EMPTY_ENTRY / INITIALIZING_ENTRY come from shared.inc.glsl.

#ifndef HASH_GRID_INC_GLSL
#define HASH_GRID_INC_GLSL

#define GRID_SIZE 32

uint getPositionHash(ivec3 p)
{
    uvec3 q = uvec3(p);
    q = q * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = q.x ^ q.y ^ q.z;
    // pcg hash
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    n = (n >> 22u) ^ n;
    return n;
}

uint hashTableIndex(ivec3 gridPos, uint tableSize)
{
    return getPositionHash(gridPos) & (tableSize - 1u);
}

uint hashNextIndex(uint idx, uint tableSize)
{
    return (idx + 1u) & (tableSize - 1u);
}

ivec3 getGridPos(vec3 pos)
{
    return ivec3(floor(pos / GRID_SIZE));
}

#endif
