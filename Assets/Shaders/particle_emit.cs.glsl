#version 460

// Particle emit: one thread per requested spawn. The spawn map (CPU-written per frame) holds the
// emitter slot of each spawn; a thread pops a free index off the dead stack, initializes the particle
// from its emitter's config and appends it to the IN alive list, where this frame's sim pass picks it
// up (the begin pass sized the sim dispatch to cover survivors + spawns). Dead-stack exhaustion
// (pool full) silently drops spawns.
//
// PARTICLE_GPU_SPAWN variant: the GPU spawn path (particle_spawn.inc.glsl). Binding 6 is the request
// buffer other compute passes appended to; the count is the begin pass's latched c_gpuSpawnConsume and
// the dispatch is GPU-sized (c_gpuEmitGroups). A request carries its own position and velocity, so the
// emitter's spawn shape / cone / speed are skipped - everything else (life, size, colour, flags) still
// comes from the named emitter slot.

#include "particle.inc.glsl"

layout (local_size_x = 64) in;

layout (binding = 0, std140) uniform ParticleParams
{
    float p_dt; uint p_spawnCount; uint p_parity; uint p_reset;
    uint p_frameIndex; uint p_collision; uint p_padA; uint p_padB;
};
layout (binding = 1, std430) buffer Particles { Particle pp_particles[]; };
layout (binding = 2, std430) buffer AliveIn { uint pa_aliveIn[]; };
layout (binding = 3, std430) buffer DeadList { uint pd_deadList[]; };
layout (binding = 4, std430) buffer Counters { PARTICLE_COUNTERS_BLOCK };
layout (binding = 5, std430) readonly buffer Emitters { ParticleEmitter pe_emitters[]; };
#ifdef PARTICLE_GPU_SPAWN
layout (binding = 6, std430) readonly buffer SpawnRequests { ParticleSpawnRequest pr_requests[]; };
#else
layout (binding = 6, std430) readonly buffer SpawnMap { uint ps_spawnMap[]; };
#endif

const float PARTICLE_PI = 3.14159265359;

void main()
{
    const uint gid = gl_GlobalInvocationID.x;
#ifdef PARTICLE_GPU_SPAWN
    if (gid >= c_gpuSpawnConsume)
        return;
    const ParticleSpawnRequest req = pr_requests[gid];
    const uint emitterIdx = floatBitsToUint(req.velEmitter.w);
    if (emitterIdx >= MAX_PARTICLE_EMITTERS)
        return; // a producer named a dead slot: drop it before touching the dead stack
#else
    if (gid >= p_spawnCount)
        return;
    const uint emitterIdx = ps_spawnMap[gid];
#endif

    // Pop a free pool index; underflow = pool exhausted, drop the spawn. Counted (c_dropSpawns, zeroed
    // per frame by the begin pass and read back for the log): an exhausted pool silently stops EVERY
    // emitter, which is indistinguishable from the particle system being switched off.
    const int deadSlot = atomicAdd(c_deadCount, -1);
    if (deadSlot <= 0)
    {
        atomicAdd(c_deadCount, 1);
        atomicAdd(c_dropSpawns, 1u);
        return;
    }
    const uint particleIdx = pd_deadList[deadSlot - 1];

    const ParticleEmitter e = pe_emitters[emitterIdx];

#ifdef PARTICLE_GPU_SPAWN
    uint seed = particlePcg(p_frameIndex * 0x9E3779B9u + gid * 0xC2B2AE35u + particleIdx + 0x68BC21EBu); // distinct stream from the CPU emit
#else
    uint seed = particlePcg(p_frameIndex * 0x9E3779B9u + gid * 0x85EBCA6Bu + particleIdx);
#endif

    // Spawn position: random point in (or on) the emitter's sphere - or, for a weather volume,
    // uniformly inside its box (the box fills at once instead of raining in from a point).
    vec3 pos;
#ifdef PARTICLE_GPU_SPAWN
    pos = req.posSize.xyz;
#else
    if ((e.texFlags.y & PARTICLE_FLAG_VOLUME) != 0u)
    {
        const vec3 r = vec3(particleRand(seed), particleRand(seed), particleRand(seed)) * 2.0 - 1.0;
        pos = e.posSpawnRadius.xyz + r * e.volumeParams.xyz;
    }
    else
    {
        vec3 offDir = normalize(vec3(particleRand(seed), particleRand(seed), particleRand(seed)) * 2.0 - 1.0 + 1e-5);
        float offR = e.posSpawnRadius.w * mix(pow(particleRand(seed), 1.0 / 3.0), 1.0, e.spawnParams.w);
        pos = e.posSpawnRadius.xyz + offDir * offR;
    }
#endif

#ifdef PARTICLE_GPU_SPAWN
    // The producer decided the velocity; the emitter's own velocity inheritance still applies.
    const vec3 vel = req.velEmitter.xyz + e.velocityInherit.xyz * e.velocityInherit.w;
#else
    // Direction: uniform within the cone around the emitter's local +Y.
    const float cosCone = cos(clamp(e.spawnParams.x, 0.0, PARTICLE_PI));
    const float cosTheta = mix(cosCone, 1.0, particleRand(seed));
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi = particleRand(seed) * 2.0 * PARTICLE_PI;
    const vec3 localDir = vec3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
    const vec3 dir = particleQuatRotate(e.rotation, localDir);

    const float speed = mix(e.spawnParams.y, e.spawnParams.z, particleRand(seed));
    const vec3 vel = dir * speed + e.velocityInherit.xyz * e.velocityInherit.w;
#endif

    const float lifetime = max(0.01, mix(e.lifeParams.x, e.lifeParams.y, particleRand(seed)));
    const float rotation = e.spinParams.y > 0.5 ? particleRand(seed) * 2.0 * PARTICLE_PI : 0.0;
    const float spin = e.spinParams.x * (particleRand(seed) * 2.0 - 1.0);

    Particle particle;
    particle.posAge = vec4(pos, 0.0);
    particle.velLife = vec4(vel, lifetime);
    particle.misc = uvec4(emitterIdx, seed, floatBitsToUint(rotation), floatBitsToUint(spin));
    pp_particles[particleIdx] = particle;

    const uint aliveIdx = atomicAdd(c_draw[p_parity * 4u + 1u], 1u);
    pa_aliveIn[aliveIdx] = particleIdx;
}
