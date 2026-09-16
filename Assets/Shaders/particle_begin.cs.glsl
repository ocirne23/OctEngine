#version 460

// Particle frame setup. Two modes, one indirect dispatch (group count CPU-written per frame):
//  - reset (pool init / "Reset particles" tweak): every thread refills the dead stack, thread 0 zeroes
//    the counters and writes the fixed draw-arg fields. Dispatched at ceil(MAX_PARTICLES / 256).
//  - per-frame prepare: thread 0 only (1 group): zeroes this frame's OUT alive count and sizes the sim
//    dispatch to cover last frame's survivors (the IN list) plus everything the emit pass will append.

#include "particle.inc.glsl"

layout (local_size_x = 256) in;

layout (binding = 0, std140) uniform ParticleParams
{
    float p_dt; uint p_spawnCount; uint p_parity; uint p_reset;
    uint p_frameIndex; uint p_collision; uint p_padA; uint p_padB;
};
layout (binding = 1, std430) buffer Counters { PARTICLE_COUNTERS_BLOCK };
layout (binding = 2, std430) writeonly buffer DeadList { uint pd_deadList[]; };

void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    if (p_reset != 0u)
    {
        if (gid < MAX_PARTICLES)
            pd_deadList[gid] = gid;
        if (gid == 0u)
        {
            c_deadCount = int(MAX_PARTICLES);
            // Still cover this frame's spawns: the emit pass runs right after the reset, and without
            // this the freshly-spawned particles would sit unsimulated in the IN list and be orphaned
            // when the next frame's prepare zeroes it.
            c_simGroups = uvec4((p_spawnCount + PARTICLE_SIM_GROUP_SIZE - 1u) / PARTICLE_SIM_GROUP_SIZE, 1u, 1u, 0u);
            for (uint i = 0u; i < 2u; ++i)
            {
                c_draw[i * 4u + 0u] = 6u; // vertexCount: one quad (two triangles) per particle
                c_draw[i * 4u + 1u] = 0u; // instanceCount = alive count
                c_draw[i * 4u + 2u] = 0u;
                c_draw[i * 4u + 3u] = 0u;
            }
            // GPU spawn path: requests made before the reset are dropped with the pool.
            c_gpuSpawnCount = 0u;
            c_gpuSpawnConsume = 0u;
            c_gpuEmitGroups = uvec4(0u, 1u, 1u, 0u);
            c_dropSpawns = 0u;
            c_dropBroken = 0u;
        }
        return;
    }
    if (gid != 0u)
        return;
    const uint outIdx = 1u - p_parity;
    c_draw[outIdx * 4u + 1u] = 0u;
    // This frame's diagnostic counters: the emit pass adds a dropped spawn (pool exhausted), the sim
    // pass a retired non-finite particle. Zeroed HERE, read back at the end of the sim pass, so the
    // readback carries exactly this frame's totals.
    c_dropSpawns = 0u;
    c_dropBroken = 0u;
    // GPU spawn path: latch this frame's producer count (clamped to the request buffer), size the GPU
    // emit dispatch, and zero the counter for the NEXT frame's producers (they run after this frame's
    // whole particle chain on the same queue).
    const uint gpuSpawns = min(c_gpuSpawnCount, MAX_PARTICLE_GPU_SPAWNS);
    c_gpuSpawnConsume = gpuSpawns;
    c_gpuSpawnCount = 0u;
    c_gpuEmitGroups = uvec4((gpuSpawns + 63u) / 64u, 1u, 1u, 0u);
    const uint total = c_draw[p_parity * 4u + 1u] + p_spawnCount + gpuSpawns;
    c_simGroups.x = (total + PARTICLE_SIM_GROUP_SIZE - 1u) / PARTICLE_SIM_GROUP_SIZE;
    c_simGroups.y = 1u;
    c_simGroups.z = 1u;
}
