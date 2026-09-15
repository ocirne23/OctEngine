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
// The live ocean surface (PARTICLE_FLAG_WATER_FLOOR): the FFT maps + the terrain-data cascades for the
// shore weighting, the same field the water is drawn with.
#define OCEAN_MAPS_BINDING 11
#define TERRAIN_HEIGHT_BINDING 12
#include "ocean_wave.inc.glsl"

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
layout (binding = 10) uniform sampler2DArray u_rainOcclusion; // THIS frame's top-down rain occlusion depth, one layer (standard Z; border 1 = open sky)

// Weather volume shelter test: true when the particle sits deeper than the occlusion map's surface at
// its XZ by more than the tolerance (i.e. under a roof). Outside the map the border depth (1 = far)
// never shelters.
bool rainSheltered(vec3 pos)
{
    const vec4 clip = u_rainOcclusionViewProj * vec4(pos, 1.0); // ortho: w = 1
    const vec2 uv = clip.xy * 0.5 + 0.5;
    const float surface = texture(u_rainOcclusion, vec3(uv, 0.0)).r;
    return (clip.z - surface) > u_rainOcclusionParams.z * u_rainOcclusionParams.y;
}

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
    const bool volume = (e.texFlags.y & PARTICLE_FLAG_VOLUME) != 0u; // never ages out: it wraps instead
    if ((particle.posAge.w >= particle.velLife.w && !volume) || killed)
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

    // Water floor: a particle that reaches the live wave surface lands on it, stops, and is pushed to
    // its fade-out so it dissolves ON the water instead of sinking through it (spray rejoining the sea).
    // The surface height is read at the particle's own XZ (the horizontal chop displacement is ignored:
    // an error of a few cm at the crests, invisible on a landing droplet).
    if ((e.texFlags.y & PARTICLE_FLAG_WATER_FLOOR) != 0u)
    {
        const vec2 shoreHW = oceanSampleShoreData(pos.xz);
        const float surfaceY = shoreHW.y + oceanSampleDisplacement(pos.xz, 0.25, 0.0, shoreHW).y;
        if (pos.y < surfaceY)
        {
            pos.y = surfaceY + 0.01;
            vel = vec3(vel.x, 0.0, vel.z) * 0.3;
            particle.posAge.w = max(particle.posAge.w, particle.velLife.w * e.fadeParams.y);
        }
    }

    if (volume)
    {
        // Weather volume: the box rides the emitter (the camera). A drop under the occlusion map's
        // surface restarts at the box top at a fresh random XZ - the same XZ would just drop it back
        // onto the roof, and a box top that is itself indoors would pin it there. Then wrap.
        const vec3 halfExt = max(e.volumeParams.xyz, vec3(1e-3));
        // Wind: the horizontal velocity relaxes onto the LOCAL wind (mean wind swung by the gust field) at
        // the emitter's wind response rate - heavy drops lean slowly, flakes follow at once.
        if (e.volumeParams.w > 0.0)
        {
            const vec3 wind = weatherWindAt(pos, u_timeSeconds);
            const float k = min(1.0, e.volumeParams.w * p_dt);
            vel.xz += (wind.xz - vel.xz) * k;
            pos.xz += (wind.xz - particle.velLife.xz) * k * p_dt * 0.5; // half-step: the relaxed part of this frame's motion
        }
        vec3 rel = pos - e.posSpawnRadius.xyz;
        // A drop restarts at the box top at a FRESH RANDOM XZ when it is sheltered, and also when it
        // falls out through the bottom: the gust field has divergence, so drops pile up where the wind
        // converges, and a per-axis Y wrap would keep each one in its sink forever - the whole volume
        // would drain into "waterfalls". Fresh XZ per fall = new rain from the cloud, uniformly spread,
        // so clustering is bounded by what one fall through the box can do (as in reality).
        const bool sheltered = (e.texFlags.y & PARTICLE_FLAG_OCCLUDE) != 0u && u_rainOcclusionParams.x > 0.5 && rainSheltered(pos);
        if (sheltered || rel.y < -halfExt.y)
        {
            uint seed = particle.misc.y;
            rel.x = (particleRand(seed) * 2.0 - 1.0) * halfExt.x;
            rel.z = (particleRand(seed) * 2.0 - 1.0) * halfExt.z;
            // Just inside the top face: exactly ON it, the wrap below folds it onto the bottom face.
            rel.y = halfExt.y * 0.999;
            particle.misc.y = seed;
        }
        // Underwater volume (silt, bubbles): a particle above the live water surface is put back at a
        // random depth under it (a bubble reaching the surface pops and re-forms below). When the whole
        // box is above the water nothing fits; the draw hides the volume while the camera is above sea
        // level anyway.
        if ((e.texFlags.y & (PARTICLE_FLAG_UNDERWATER | PARTICLE_FLAG_ABOVE_WATER)) != 0u)
        {
            const vec2 shoreHW = oceanSampleShoreData(pos.xz);
            const float surfaceY = shoreHW.y + oceanSampleDisplacement(pos.xz, 0.25, 0.0, shoreHW).y;
            if ((e.texFlags.y & PARTICLE_FLAG_UNDERWATER) != 0u)
            {
                const float topRel = min(surfaceY - 0.05 - e.posSpawnRadius.y, halfExt.y * 0.999);
                if (rel.y > topRel && topRel > -halfExt.y)
                {
                    uint seed = particle.misc.y;
                    rel.y = mix(-halfExt.y * 0.999, topRel, particleRand(seed));
                    particle.misc.y = seed;
                }
            }
            else
            {   // above-water volume (dust): the inverse - a particle under the surface goes back up over it.
                const float bottomRel = max(surfaceY + 0.05 - e.posSpawnRadius.y, -halfExt.y * 0.999);
                if (rel.y < bottomRel && bottomRel < halfExt.y)
                {
                    uint seed = particle.misc.y;
                    rel.y = mix(bottomRel, halfExt.y * 0.999, particleRand(seed));
                    particle.misc.y = seed;
                }
            }
        }
        pos = e.posSpawnRadius.xyz + particleVolumeWrap(rel, halfExt);
    }

    particle.posAge.xyz = pos;
    particle.velLife.xyz = vel;
    pp_particles[particleIdx] = particle;

    const uint outSlot = atomicAdd(c_draw[(1u - p_parity) * 4u + 1u], 1u);
    pa_aliveOut[outSlot] = particleIdx;
}
