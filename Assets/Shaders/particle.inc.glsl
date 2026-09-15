#ifndef PARTICLE_INC_GLSL
#define PARTICLE_INC_GLSL

// GPU particle system shared declarations (ParticlePipeline). MAX_PARTICLES, MAX_PARTICLE_EMITTERS,
// PARTICLE_SIM_GROUP_SIZE, PARTICLE_FLAG_* and PARTICLE_TEX_NONE are injected by the engine from
// RendererVKLayout (Layout.ixx). Structs must stay in sync with Layout.ixx.

// One live particle in the persistent pool (48 bytes).
struct Particle
{
    vec4 posAge;   // xyz = world position, w = age (s)
    vec4 velLife;  // xyz = velocity (m/s), w = lifetime (s)
    uvec4 misc;    // x = emitter slot, y = RNG seed, z = rotation (float bits), w = spin rad/s (float bits)
};

// Mirror of RendererVKLayout::ParticleEmitterGpu (208 bytes).
struct ParticleEmitter
{
    vec4 posSpawnRadius;  // xyz = world position, w = spawn radius (m)
    vec4 rotation;        // quat; spawn cone axis = local +Y
    vec4 velocityInherit; // xyz = emitter velocity (m/s), w = inherit factor
    vec4 spawnParams;     // x = cone angle (rad), y = speed min, z = speed max, w = spawn on shell
    vec4 lifeParams;      // x = life min (s), y = life max, z = gravity (m/s^2, -Y), w = drag (1/s)
    vec4 noiseParams;     // x = turbulence accel (m/s^2), y = frequency (1/m), z = scroll (m/s), w = bounce
    vec4 sizeParams;      // x = size start (m), y = size end, z = size variance, w = velocity stretch (s)
    vec4 colorStart;      // rgb = linear color * intensity, a = start alpha
    vec4 colorEnd;
    vec4 fadeParams;      // x = fade-in end (life frac), y = fade-out start, z = additivity, w = soft fade dist (m)
    vec4 spinParams;      // x = max spin (rad/s), y = random initial rotation (0/1), z = lit emissive floor, w = ground fade height (m)
    uvec4 texFlags;       // x = texture idx, y = PARTICLE_FLAG_* bits, z = flipbook cols | rows << 16, w = flipbook fps (float bits)
    vec4 volumeParams;    // PARTICLE_FLAG_VOLUME: xyz = box half extents (m) around posSpawnRadius.xyz, w = wind response (1/s)
};

// Weather volume helpers (PARTICLE_FLAG_VOLUME). The box is centred on the emitter position, which
// follows the camera; a particle leaving one face re-enters through the opposite one, so the box
// never drains and the count stays constant regardless of how far the camera travels.
vec3 particleVolumeWrap(vec3 rel, vec3 halfExtents)
{
    const vec3 size = 2.0 * halfExtents;
    return mod(rel + halfExtents, size) - halfExtents;
}
// Alpha fade over the outer band of the box's XZ footprint, so the wrap seam at the sides never pops.
float particleVolumeEdgeFade(vec3 rel, vec3 halfExtents)
{
    const vec2 edge = abs(rel.xz) / max(halfExtents.xz, vec2(1e-3));
    return 1.0 - smoothstep(0.8, 1.0, max(edge.x, edge.y));
}

// GPU counters block: sim dispatch args + per-parity draw args (instanceCount IS the alive count) +
// the dead-stack top + the GPU spawn path's counter, latched count and emit dispatch args. Bound as one
// buffer that is also the indirect dispatch/draw source (80 bytes; ParticlePipeline.cpp offsets).
// c_draw[parity * 4 + 0..3] = vertexCount(6), instanceCount, firstVertex(0), firstInstance(0).
// c_gpuSpawnCount: producers append here (unbounded; the begin pass clamps to MAX_PARTICLE_GPU_SPAWNS
// into c_gpuSpawnConsume, which the GPU emit pass reads, then zeroes it for the next frame's producers).
#define PARTICLE_COUNTERS_BLOCK \
    uvec4 c_simGroups;          \
    uint  c_draw[8];            \
    int   c_deadCount;          \
    uint  c_gpuSpawnCount;      \
    uint  c_gpuSpawnConsume;    \
    uint  c_pad2;               \
    uvec4 c_gpuEmitGroups;

// One GPU spawn request (RendererVKLayout::ParticleSpawnRequestGpu, 32 bytes): see particle_spawn.inc.glsl.
struct ParticleSpawnRequest
{
    vec4 posSize;    // xyz = world position, w reserved
    vec4 velEmitter; // xyz = velocity (m/s), w = emitter slot (uint bits)
};

// ---- RNG (pcg) ----
uint particlePcg(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}
float particleRand(inout uint state) // [0,1)
{
    state = particlePcg(state);
    return float(state) * (1.0 / 4294967296.0);
}

vec3 particleQuatRotate(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// ---- cheap 3D value noise (turbulence) ----
float particleHash1(uvec3 v)
{
    uint h = v.x * 1597334673u + v.y * 3812015801u + v.z * 2798796415u;
    h = particlePcg(h);
    return float(h) * (1.0 / 4294967296.0);
}
float particleNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    uvec3 b = uvec3(ivec3(i) + 0x8000);
    float n000 = particleHash1(b + uvec3(0u, 0u, 0u)), n100 = particleHash1(b + uvec3(1u, 0u, 0u));
    float n010 = particleHash1(b + uvec3(0u, 1u, 0u)), n110 = particleHash1(b + uvec3(1u, 1u, 0u));
    float n001 = particleHash1(b + uvec3(0u, 0u, 1u)), n101 = particleHash1(b + uvec3(1u, 0u, 1u));
    float n011 = particleHash1(b + uvec3(0u, 1u, 1u)), n111 = particleHash1(b + uvec3(1u, 1u, 1u));
    float nx00 = mix(n000, n100, f.x), nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x), nx11 = mix(n011, n111, f.x);
    return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}
// Pseudo-turbulence acceleration: three decorrelated value noises in [-1,1] (not divergence-free, but
// visually adequate for smoke/ember wander at a fraction of a real curl evaluation's cost).
vec3 particleTurbulence(vec3 p)
{
    return vec3(particleNoise(p) * 2.0 - 1.0,
                particleNoise(p + vec3(31.416, 27.183, 12.793)) * 2.0 - 1.0,
                particleNoise(p + vec3(-17.321, 41.421, -23.606)) * 2.0 - 1.0);
}

// ---- weather wind (the UBO's u_weatherWind0/1; only for includers that have the UBO - the sim + draw) ----
#ifdef UBO_INC_GLSL
// A noise field that TRAVELS along the wind direction - at the sheet drift speed plus half the mean
// wind, like a gust front, so it sweeps even in light wind - and evolves in time. Sampled in the
// horizontal plane; returns [-1, 1].
float weatherTravellingNoise(vec2 worldXZ, float invSize, float time, float phase)
{
    const vec2 travel = u_weatherWind2.xy * (u_weatherWind1.w * time) + u_weatherWind0.xz * (time * 0.5);
    const vec2 p = (worldXZ - travel) * invSize;
    return particleNoise(vec3(p.x, time * 0.35 + phase, p.y)) * 2.0 - 1.0;
}
// The local wind velocity at a point: the mean wind plus a 2D gust vector of the gust strength (two
// decorrelated fields), so flurries swirl in calm air and lean with the wind in a storm. The fields
// are large-scale and travelling, so a gust front moving through the volume packs the drops into a
// band ahead of it - the density "waves" of a storm - before the wrap evens them out again.
vec3 weatherWindAt(vec3 pos, float time)
{
    const float g = u_weatherWind0.w;
    if (g <= 0.0)
        return u_weatherWind0.xyz;
    const vec2 gust = vec2(weatherTravellingNoise(pos.xz, u_weatherWind1.x, time, 0.0),
                           weatherTravellingNoise(pos.xz, u_weatherWind1.x, time, 53.0));
    return u_weatherWind0.xyz + vec3(gust.x, 0.0, gust.y) * g;
}
// Alpha multiplier for the drawn drops: sheets of denser and thinner rain sweeping through. Contrast
// 0 = uniform; at 1 the thinnest sheet is nearly empty and the densest twice as bright.
float weatherSheet(vec3 pos, float time)
{
    const float c = u_weatherWind1.y;
    if (c <= 0.0)
        return 1.0;
    const float n = weatherTravellingNoise(pos.xz, u_weatherWind1.z, time, 37.0);
    return clamp(1.0 + c * n * 1.5, 0.0, 2.0);
}
#endif

#endif
