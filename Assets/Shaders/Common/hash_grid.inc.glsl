// Shared world-space spatial hash-grid helpers.
//
// World space is partitioned into GRID_SIZE^3 cubes. Each occupied cube is keyed by its integer grid
// position and inserted (open-addressed, linear probing) into a power-of-two-sized table. Both the light
// grid (light_grid.inc.glsl) and the GI probe grid (gi_probe.inc.glsl) build on this so they share one
// hashing + table-probing scheme.
//
// The core helpers take the table size as an argument (rather than a macro) so a single shader can use
// two independently-sized grids at once. Each grid defines its own thin getTableIdx/getNextTableIdx
// wrappers that bind their own table size. EMPTY_ENTRY / INITIALIZING_ENTRY come from shared.inc.glsl.

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
