#ifndef PARTICLE_SPAWN_INC_GLSL
#define PARTICLE_SPAWN_INC_GLSL

// The particle GPU SPAWN PATH, producer side. Any compute pass that runs before the particle sim in
// the frame (see the frame order in RendererVK/CONTEXT.md) binds the particle counters + request
// buffers (Renderer hands them out from the ParticlePipeline) and appends spawn requests here; the
// particle begin pass latches the count and the GPU emit dispatch turns every request into a particle
// of the named emitter slot (an ordinary .pfx emitter instance whose slot the CPU published to the
// producer). The includer defines PARTICLE_COUNTERS_BINDING and PARTICLE_REQUESTS_BINDING and has
// included particle.inc.glsl.
//
// Capacity: MAX_PARTICLE_GPU_SPAWNS per frame across ALL producers; past it requests are dropped (the
// counter keeps counting, the begin pass clamps). The particle pool (MAX_PARTICLES) is shared with the
// CPU emitters too - a producer must budget its rate.

layout (binding = PARTICLE_COUNTERS_BINDING, std430) buffer ParticleCounters { PARTICLE_COUNTERS_BLOCK };
layout (binding = PARTICLE_REQUESTS_BINDING, std430) writeonly buffer ParticleSpawnRequests { ParticleSpawnRequest pr_requests[]; };

// Appends one request. Returns false when this frame's request capacity is exhausted.
bool particleRequestSpawn(vec3 pos, vec3 vel, uint emitterSlot)
{
    const uint idx = atomicAdd(c_gpuSpawnCount, 1u);
    if (idx >= MAX_PARTICLE_GPU_SPAWNS)
        return false;
    pr_requests[idx].posSize = vec4(pos, 0.0);
    pr_requests[idx].velEmitter = vec4(vel, uintBitsToFloat(emitterSlot));
    return true;
}

#endif
