#version 460

// Particle simulate: one thread per IN-list entry (survivors of last frame + this frame's spawns; the
// dispatch is GPU-sized by the begin pass). Integrates gravity/drag/turbulence, optionally collides
// against last frame's G-buffer depth (screen-space; particles outside the view just fly on), and
// compacts survivors into the OUT alive list - whose count is directly this frame's draw instanceCount.
// Expired particles return their pool index to the dead stack. Emitters whose slot was destroyed carry
// PARTICLE_FLAG_KILL, which retires their particles on the next sim step.

#define UBO_BINDING 1
#include "shared.inc.glsl"
#include "particle.inc.glsl"

layout (local_size_x = 64) in;

layout (binding = 0, std140) uniform ParticleParams
{
    float p_dt; uint p_spawnCount; uint p_parity; uint p_reset;
    uint p_frameIndex; uint p_collision; uint p_padA; uint p_padB;
};
layout (binding = 2, std430) buffer Particles { Particle pp_particles[]; };
layout (binding = 3, std430) buffer AliveIn { uint pa_aliveIn[]; };
layout (binding = 4, std430) buffer AliveOut { uint pa_aliveOut[]; };
layout (binding = 5, std430) buffer DeadList { uint pd_deadList[]; };
layout (binding = 6, std430) buffer Counters { PARTICLE_COUNTERS_BLOCK };
layout (binding = 7, std430) readonly buffer Emitters { ParticleEmitter pe_emitters[]; };
layout (binding = 8) uniform sampler2D u_prevDepth;   // last frame's G-buffer depth (centre/left view)
layout (binding = 9) uniform sampler2D u_prevNormal;  // last frame's G-buffer world normal

void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    if (gid >= c_draw[p_parity * 4u + 1u])
        return;

    const uint particleIdx = pa_aliveIn[gid];
    Particle particle = pp_particles[particleIdx];
    const ParticleEmitter e = pe_emitters[particle.misc.x];

    particle.posAge.w += p_dt;
    const bool killed = (e.texFlags.y & PARTICLE_FLAG_KILL) != 0u;
    if (particle.posAge.w >= particle.velLife.w || killed)
    {
        const int deadSlot = atomicAdd(c_deadCount, 1);
        pd_deadList[deadSlot] = particleIdx;
        return;
    }

    // Integrate.
    vec3 vel = particle.velLife.xyz;
    vel.y -= e.lifeParams.z * p_dt;
    vel *= max(0.0, 1.0 - e.lifeParams.w * p_dt);
    if (e.noiseParams.x > 0.0)
    {
        const vec3 noisePos = particle.posAge.xyz * e.noiseParams.y
            + vec3(0.0, -u_timeSeconds * e.noiseParams.z * e.noiseParams.y, 0.0)
            + vec3(float(particle.misc.y & 1023u)); // per-particle field offset breaks up lockstep motion
        vel += particleTurbulence(noisePos) * (e.noiseParams.x * p_dt);
    }
    vec3 pos = particle.posAge.xyz + vel * p_dt;

    // Screen-space depth collision against last frame's G-buffer (centre view).
    if (p_collision != 0u && (e.texFlags.y & PARTICLE_FLAG_COLLIDE) != 0u)
    {
        const vec4 clip = u_views[VIEW_CENTER].mvp * vec4(pos, 1.0);
        if (clip.w > 0.0)
        {
            const vec2 ndc = clip.xy / clip.w;
            if (all(lessThan(abs(ndc), vec2(1.0))))
            {
                const vec2 vpUv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                const vec2 uv = u_viewportRect.xy + vpUv * u_viewportRect.zw;
                const float depth = texture(u_prevDepth, uv).r;
                if (depth > 0.0) // reversed-Z: 0 = far plane / sky
                {
                    const vec3 scenePos = worldPosFromDepthMat(uv, depth, u_views[VIEW_CENTER].invMvp);
                    const float sceneW = (u_views[VIEW_CENTER].mvp * vec4(scenePos, 1.0)).w;
                    const float thickness = max(0.5, e.sizeParams.x);
                    if (clip.w > sceneW && clip.w - sceneW < thickness)
                    {
                        vec3 n = normalize(texture(u_prevNormal, uv).xyz + vec3(0.0, 1e-4, 0.0));
                        const float vn = dot(vel, n);
                        if (vn < 0.0)
                        {
                            vel = (vel - 2.0 * vn * n) * e.noiseParams.w;
                            pos = scenePos + n * 0.02;
                        }
                    }
                }
            }
        }
    }

    particle.posAge.xyz = pos;
    particle.velLife.xyz = vel;
    pp_particles[particleIdx] = particle;

    const uint outSlot = atomicAdd(c_draw[(1u - p_parity) * 4u + 1u], 1u);
    pa_aliveOut[outSlot] = particleIdx;
}
