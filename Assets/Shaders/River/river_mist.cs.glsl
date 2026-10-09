#version 460

// River mist: the particle GPU spawn path's second producer (particle_spawn.inc.glsl), at the head of the particle sim
// pass. Procedural's RiverSystem hands up the stretches of whitewater (rapids, falls) near the camera - each a point on
// the water surface with the channel's half-width, the flow's velocity, the whitewater (already x the river's size) and
// the stretch's length. One thread per stretch rolls a hashed dice at "Mist rate" x its area x dt x the whitewater past
// "Mist threshold"; a hit spawns over the stretch (along it and across the channel), drifting downstream with the
// water and kicked upward, into the ONE emitter slot the CPU published (Effects/river_mist.pfx, u_river_mistEmitter).

#include "shared.inc.glsl"
#include "particle.inc.glsl"
#define PARTICLE_COUNTERS_BINDING 1
#define PARTICLE_REQUESTS_BINDING 2
#include "particle_spawn.inc.glsl"

struct RiverMistSource
{
    vec4 posHalf;  // xyz = world position on the water surface, w = the channel's half-width (m)
    vec4 flowFoam; // xy = the flow's velocity in XZ (m/s), z = the whitewater 0..1, w = the stretch's length (m)
};
layout (binding = 3, std430) readonly buffer RiverMistSources
{
    uint s_count;
    uint s_pad0, s_pad1, s_pad2;
    RiverMistSource s_sources[];
};

layout (local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    const uint slot = u_river_mistEmitter;
    const float rate = u_river_mistRate;
    if (slot == 0xFFFFFFFFu || rate <= 0.0)
        return;
    const uint i = gl_GlobalInvocationID.x;
    if (i >= s_count)
        return;
    const RiverMistSource src = s_sources[i];
    const float strength = pow(smoothstep(u_river_mistThreshold, 1.0, src.flowFoam.z), u_river_mistCurve);
    if (strength <= 0.0)
        return;

    const float halfWidth = max(src.posHalf.w, 0.1);
    const float len = max(src.flowFoam.w, 0.1);
    const float expected = rate * (2.0 * halfWidth * len) * u_ocean_sprayDt * strength;
    uint seed = particlePcg(u_frameIndex * 0x9E3779B9u + i * 0x85EBCA6Bu + 0x27D4EB2Fu);
    uint count = uint(expected);
    if (particleRand(seed) < fract(expected))
        ++count;
    count = min(count, 16u);
    if (count == 0u)
        return;

    const vec2 flow = src.flowFoam.xy;
    const float speed = length(flow);
    const vec2 dir = speed > 1e-4 ? flow / speed : vec2(1.0, 0.0);
    const vec2 side = vec2(-dir.y, dir.x);
    // The stronger the whitewater, the higher the mist is thrown.
    const float energy = 0.5 + strength;
    for (uint k = 0u; k < count; ++k)
    {
        const float a = (particleRand(seed) - 0.5) * len;
        // Across the channel: |u|^(1 + "Mist centering"), so it gathers toward the centre line (0 = evenly).
        const float u = particleRand(seed) * 2.0 - 1.0;
        const float c = sign(u) * pow(abs(u), 1.0 + u_river_mistCentering) * halfWidth;
        const vec3 pos = vec3(src.posHalf.x + dir.x * a + side.x * c,
                              src.posHalf.y + u_river_mistHeight,
                              src.posHalf.z + dir.y * a + side.y * c);
        const vec2 drift = flow * u_river_mistSpeed * (0.6 + 0.6 * particleRand(seed))
                         + side * ((particleRand(seed) * 2.0 - 1.0) * 0.3 * speed);
        const float up = u_river_mistKick * energy * (0.4 + 0.8 * particleRand(seed));
        if (!particleRequestSpawn(pos, vec3(drift.x, up, drift.y), slot))
            return; // this frame's request buffer is full
    }
}
