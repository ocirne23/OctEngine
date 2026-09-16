#version 460

// Ocean spray: the first PRODUCER on the particle GPU spawn path (particle_spawn.inc.glsl). Runs as the
// last OceanSimulationPipeline step, after the maps' mip chain, over a world-space grid of
// OCEAN_SPRAY_GRID^2 cells around the scene focus ("Ocean/Spray radius"). Each cell jitters one sample
// point, rebuilds the fold Jacobian + crest acceleration the water shader's whitecap foam keys on
// (explicit LOD at the cell's footprint - this is compute, no derivatives), and where the crest is
// breaking rolls a hashed dice at "Spray rate" x cell area x dt x breaking. A hit appends spawn
// requests at the displaced surface with the crest's forward motion (along the wind) plus an upward
// kick; the particle chain later this frame turns them into particles of the ONE emitter slot the CPU
// published (the first emitter of ParticleSystem's Effects/ocean_spray.pfx instance, u_oceanSpray0.x).
//
// Land is skipped through the shore data (depth <= 0), and the grid ORIGIN is snapped to whole cells so
// the sample lattice does not swim with the focus.

#include "shared.inc.glsl"
#define OCEAN_MAPS_BINDING 1
#define TERRAIN_HEIGHT_BINDING 2
#include "ocean_wave.inc.glsl"
#include "particle.inc.glsl"
#define PARTICLE_COUNTERS_BINDING 3
#define PARTICLE_REQUESTS_BINDING 4
#include "particle_spawn.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// oceanSampleSurface's Jacobian + acceleration with EXPLICIT LOD at the cell footprint (the shared
// helper samples with implicit derivatives, which compute has none of). Same depth weighting.
void sprayCrest(vec2 worldXZ, float cell, vec2 shoreHW, out float jacobian, out float accel, out float turbulence)
{
    const float chop = u_oceanParams0.w;
    const float depth = oceanEffectiveDepth(worldXZ, shoreHW.y - shoreHW.x);
    const vec2 fr = oceanFlowRotation(worldXZ);
    const vec2 sampleXZ = oceanFlowSamplePos(worldXZ, fr);
    float sxx = 0.0, szz = 0.0, sxz = 0.0;
    accel = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float L = u_oceanParams2[c];
        const vec3 uv = vec3(sampleXZ / L, 0.0);
        const float lod = oceanVertexLod(cell, 0.0, L);
        const vec4 g = textureLod(u_oceanMaps, uv + vec3(0.0, 0.0, float(OCEAN_CASCADES + c)), lod);
        const vec4 d = textureLod(u_oceanMaps, uv + vec3(0.0, 0.0, float(c)), lod);
        const vec4 m = textureLod(u_oceanMaps, uv + vec3(0.0, 0.0, float(2 * OCEAN_CASCADES + c)), lod);
        sxx += g.z; szz += g.w; sxz += d.w;
        accel += m.z;
        if (c == 0)
            turbulence = m.w;
    }
    const float w = oceanSurfaceWeight(depth, shoreHW.y);
    sxx *= w; szz *= w; sxz *= w;
    accel *= w;
    const float jxx = 1.0 + chop * sxx;
    const float jzz = 1.0 + chop * szz;
    const float jxz = chop * sxz;
    jacobian = jxx * jzz - jxz * jxz;
}

void main()
{
    const uint slot = floatBitsToUint(u_oceanSpray0.x);
    const float rate = u_oceanSpray0.y;
    if (slot == 0xFFFFFFFFu || rate <= 0.0)
        return;
    const uvec2 id = gl_GlobalInvocationID.xy;
    if (any(greaterThanEqual(id, uvec2(OCEAN_SPRAY_GRID))))
        return;

    const float radius = max(u_oceanSpray0.z, 1.0);
    const float cell = 2.0 * radius / float(OCEAN_SPRAY_GRID);
    const vec2 origin = floor(u_sceneFocus.xz / cell) * cell - vec2(radius);
    uint seed = particlePcg(u_frameIndex * 0x9E3779B9u + id.x * 0x85EBCA6Bu + id.y * 0xC2B2AE35u);
    const vec2 worldXZ = origin + (vec2(id) + vec2(particleRand(seed), particleRand(seed))) * cell;

    // Fade toward the grid edge so the spray field has no square rim.
    const float edge = max(abs(worldXZ.x - u_sceneFocus.x), abs(worldXZ.y - u_sceneFocus.z)) / radius;
    const float edgeFade = 1.0 - smoothstep(0.75, 1.0, edge);
    if (edgeFade <= 0.0)
        return;

    const vec2 shoreHW = oceanSampleShoreData(worldXZ); // (terrain height, water level)
    if (shoreHW.y - shoreHW.x <= 0.0)
        return; // land

    float jacobian, accel, turbulence;
    sprayCrest(worldXZ, cell, shoreHW, jacobian, accel, turbulence);
    // The water shader's displayed foam (turbulence-relaxed threshold): spray where the whitecaps are.
    const float foam = oceanInstantFoam(jacobian, accel, turbulence * u_oceanParams5.x);
    const float breaking = smoothstep(u_oceanSpray1.x, 1.0, foam);
    if (breaking <= 0.0)
        return;

    const float expected = rate * cell * cell * u_oceanSpray0.w * breaking * edgeFade;
    uint count = uint(expected);
    if (particleRand(seed) < fract(expected))
        ++count;
    count = min(count, 8u);
    if (count == 0u)
        return;

    const vec3 disp = oceanSampleDisplacement(worldXZ, cell, 0.0, shoreHW);
    // The simulated field TRAVELS AGAINST u_oceanParams0.xy (see oceanFlowRotation's note), so the
    // crests move along -wind: throw the spray with them.
    const vec2 wind = -u_oceanParams0.xy; // unit
    // Spawn AHEAD of the crest: the lip breaks forward, so the spray leaves from the front face, not
    // from the top or the back. "Spray forward offset" m of lead along the travel direction, and
    // "Spray height offset" m above the surface (so a fresh particle is not depth-cut by the wave).
    const vec2 lead = wind * u_oceanSpray1.w;
    const vec3 surface = vec3(worldXZ.x + disp.x + lead.x, shoreHW.y + disp.y + u_oceanSpray2.x, worldXZ.y + disp.z + lead.y);
    // Energy: the stronger the breaking, the higher and faster the spray is thrown.
    const float energy = 0.6 + 0.8 * breaking;
    for (uint i = 0u; i < count; ++i)
    {
        // Jitter biased forward too: the box runs from the lip out to two cells ahead of it.
        const vec2 j2 = vec2(particleRand(seed) - 0.5, particleRand(seed) - 0.5) * cell;
        const vec3 jitter = vec3(j2.x + wind.x * (particleRand(seed) * cell), 0.0, j2.y + wind.y * (particleRand(seed) * cell));
        // Atomised mist: carried forward with the crest, barely lifting - it hangs at the lip rather
        // than arcing away like a thrown droplet would.
        const vec2 side = (vec2(particleRand(seed), particleRand(seed)) * 2.0 - 1.0) * 0.6;
        const float fwd = u_oceanSpray1.z * (0.4 + 0.4 * particleRand(seed)) * energy;
        const float up = u_oceanSpray1.y * (0.05 + 0.1 * particleRand(seed)) * energy;
        const vec3 vel = vec3(wind.x * fwd + side.x, up, wind.y * fwd + side.y);
        if (!particleRequestSpawn(surface + jitter, vel, slot))
            return; // this frame's request buffer is full
    }
}
