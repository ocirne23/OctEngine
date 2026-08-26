#version 460

// Forcefield grid insert: one thread per compacted live emitter, inserting its index into every
// uniform FORCE_GRID_CELL_SIZE (16 m) hash-grid cell its directional reach bounds overlap. Big emitters (FORCE_FLAG_BIG,
// flagged by the CPU compaction against the big-reach threshold) ride the emitter buffer header's
// global list instead and are skipped here. The table/data buffers were fill-cleared earlier in
// this command buffer; the read side (shell FS, force/query compute) runs after the barrier.

// ONE thread per workgroup, like light_grid.cs.glsl — NOT FORCE_SIM_GROUP_SIZE. The cell-claim
// protocol (forceGetOrInsertCell) spins on a CAS loser until the winner publishes; with contending
// threads in the same warp the loser's spin can starve the winner forever (intra-warp lockstep) and
// the first overlapping emitters hang the GPU. Single-thread workgroups are independently scheduled,
// which is exactly why the light grid dispatches this way.
layout (local_size_x = 1) in;

#include "shared.inc.glsl" // EMPTY_ENTRY/INITIALIZING_ENTRY sentinels (+ the UBO at binding 0)
#define FORCE_GRID
#define FORCE_GRID_WRITE
#include "force_field.inc.glsl"

void main()
{
    const uint i = gl_GlobalInvocationID.x;
    if (i >= fe_count)
        return;
    const ForceEmitterData e = fe_emitters[i];
    if ((e.teamFlags.y & FORCE_FLAG_BIG) != 0u)
        return;

    // World-space AABB of the oriented reach box (|basis| * halfExtents around the box center).
    float side, forward, back;
    forceEmitterBounds(e, side, forward, back);
    const mat3 basis = forceEmitterBasis(e.dirFocus.xyz);
    const vec3 center = e.posReach.xyz + e.dirFocus.xyz * (forward - back) * 0.5;
    const vec3 halfExtents = vec3(side, side, (forward + back) * 0.5);
    const vec3 worldExtent = abs(basis[0]) * halfExtents.x + abs(basis[1]) * halfExtents.y + abs(basis[2]) * halfExtents.z;

    const ivec3 minCell = forceGridPos(center - worldExtent);
    const ivec3 maxCell = forceGridPos(center + worldExtent);
    for (int x = minCell.x; x <= maxCell.x; ++x)
    for (int y = minCell.y; y <= maxCell.y; ++y)
    for (int z = minCell.z; z <= maxCell.z; ++z)
    {
        const uint cellIdx = forceGetOrInsertCell(ivec3(x, y, z));
        if (cellIdx != FORCE_INVALID_CELL)
            forceAddEmitterToCell(cellIdx, i);
    }
}
