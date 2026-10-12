#version 460

// Ocean spray: the first PRODUCER on the particle GPU spawn path (particle_spawn.inc.glsl). The last
// OceanSimulationPipeline step (after the mip chain), over OCEAN_SPRAY_GRID^2 world cells around the scene focus ("Ocean/
// Spray radius"). Each cell jitters one sample, rebuilds the fold Jacobian + crest acceleration the whitecap foam keys on,
// and where the crest breaks rolls a hashed dice at "Spray rate" x cell area x dt x breaking. A hit appends spawn requests
// at the displaced surface, moving with the crest plus an upward kick, for the ONE emitter slot the CPU published
// (u_ocean_sprayEmitter: the first emitter of ParticleSystem's Effects/ocean_spray.pfx instance).
// Land is skipped through the shore data; the grid ORIGIN snaps to whole cells so the lattice does not swim with the focus.

#include "shared.inc.glsl"
#define OCEAN_MAPS_BINDING 1
#define TERRAIN_HEIGHT_BINDING 2
#include "ocean_wave.inc.glsl"
#include "particle.inc.glsl"
#define PARTICLE_COUNTERS_BINDING 3
#define PARTICLE_REQUESTS_BINDING 4
#include "particle_spawn.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// oceanSampleSurface's Jacobian + acceleration with EXPLICIT LOD at the cell footprint (compute has no derivatives).
// Same depth weighting.
void sprayCrest(vec2 worldXZ, float cell, vec2 shoreHW, out float jacobian, out float accel)
{
    const float chop = u_ocean_choppiness;
    const float depth = oceanEffectiveDepth(worldXZ, shoreHW.y - shoreHW.x);
    const vec2 fr = oceanFlowRotation(worldXZ);
    const vec2 sampleXZ = oceanFlowSamplePos(worldXZ, fr);
    float sxx = 0.0, szz = 0.0, sxz = 0.0;
    accel = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float L = u_ocean_cascadeSizes[c];
        const vec3 uv = vec3(sampleXZ / L, 0.0);
        const float lod = oceanVertexLod(cell, 0.0, L);
        const vec4 g = textureLod(u_oceanMaps, uv + vec3(0.0, 0.0, float(OCEAN_CASCADES + c)), lod);
        const vec4 d = textureLod(u_oceanMaps, uv + vec3(0.0, 0.0, float(c)), lod);
        const vec4 m = textureLod(u_oceanMaps, uv + vec3(0.0, 0.0, float(2 * OCEAN_CASCADES + c)), lod);
        sxx += g.z; szz += g.w; sxz += d.w;
        accel += m.z;
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
    const uint slot = u_ocean_sprayEmitter;
    const float rate = u_ocean_sprayRate;
    if (slot == 0xFFFFFFFFu || rate <= 0.0)
        return;
    const uvec2 id = gl_GlobalInvocationID.xy;
    if (any(greaterThanEqual(id, uvec2(OCEAN_SPRAY_GRID))))
        return;

    const float radius = u_ocean_sprayRadius;
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

    float jacobian, accel;
    sprayCrest(worldXZ, cell, shoreHW, jacobian, accel);
    const float foam = oceanInstantFoam(jacobian, accel);
    const float breaking = smoothstep(u_ocean_sprayThreshold, 1.0, foam);
    if (breaking <= 0.0)
        return;

    const float expected = rate * cell * cell * u_ocean_sprayDt * breaking * edgeFade;
    uint count = uint(expected);
    if (particleRand(seed) < fract(expected))
        ++count;
    count = min(count, 8u);
    if (count == 0u)
        return;

    const vec3 disp = oceanSampleDisplacement(worldXZ, cell, 0.0, shoreHW);

    // The field TRAVELS AGAINST u_ocean_windDirection (see oceanFlowRotation), so the crests move along -wind.
    const vec2 wind = -u_ocean_windDirection; // unit
    // Spawn AHEAD of the crest (the lip breaks forward): "Spray forward offset" m of lead, and "Spray height offset" m up
    // so a fresh particle is not depth-cut by the wave.
    const vec2 lead = wind * u_ocean_sprayForward;
    const vec3 surface = vec3(worldXZ.x + disp.x + lead.x, shoreHW.y + disp.y + u_ocean_sprayHeight, worldXZ.y + disp.z + lead.y);
    const float energy = 0.6 + 0.8 * breaking; // stronger breaking throws higher and faster
    for (uint i = 0u; i < count; ++i)
    {
        // Jitter biased forward too: the box runs from half a cell behind the lip to 1.5 cells ahead of it.
        const vec2 j2 = vec2(particleRand(seed) - 0.5, particleRand(seed) - 0.5) * cell;
        const vec3 jitter = vec3(j2.x + wind.x * (particleRand(seed) * cell), 0.0, j2.y + wind.y * (particleRand(seed) * cell));
        // Atomised mist: carried with the crest, barely lifting - it hangs at the lip, not arcing away like a droplet.
        const vec2 side = (vec2(particleRand(seed), particleRand(seed)) * 2.0 - 1.0) * (0.6 * u_ocean_worldScale);
        const float fwd = u_ocean_spraySpeed * (0.4 + 0.4 * particleRand(seed)) * energy;
        const float up = u_ocean_sprayKick * (0.05 + 0.1 * particleRand(seed)) * energy;
        const vec3 vel = vec3(wind.x * fwd + side.x, up, wind.y * fwd + side.y);
        if (!particleRequestSpawn(surface + jitter, vel, slot))
            return; // this frame's request buffer is full
    }
}
