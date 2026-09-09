#version 460

// Terrain wetness clipmap — write side (TerrainWetnessPipeline; read side + addressing in
// terrain_wetness.inc.glsl). One thread per storage slot of the toroidal window: decode the slot's
// lattice coord for THIS frame's window, carry last frame's wetness if the coord was inside LAST frame's
// window (a coord that scrolled in starts dry — its slot holds a stale coord's value), decay it, then
// inject:
//   - ocean: ground under the LIVE water surface wets up. Same predicate the lit core uses for
//     underwater sunlight — calm depth from the terrain-data map, plus the swash run-up residual within
//     the swash band (underwaterLiveWaveY) — so the wet tongue is where the water was drawn. The
//     wetting TARGET ramps 0 -> 1 over the first film-depth metres of water (a thin tongue edge wets
//     less than the body) and the texel RISES toward it at the wet-in rate rather than jumping, so an
//     advancing front reads as a gradient in time and space instead of texels popping to 1. Permanently
//     submerged ground reaches 1 and holds it, so a receding drawdown reveals a wet seabed that dries.
//   - rain: a uniform per-frame addition (u_terrainWetParams1.w; 0 = no rain).
// Decay is exponential in sim time (factor precomputed per frame on the CPU), sharpened on warm ground
// by the map's own climate (u_terrainWetParams2.w). Two layers = ping/pong: read last frame's layer,
// write the other (frame slots alternate strictly; u_terrainWetParams3.x names the written layer).

// --- Diffusion: reads last frame's value through a wrapped 3x3 tent so wetness spreads sideways as it
// lives (the same trick the ocean foam mask uses), which also smooths the advancing front off the texel
// grid. The toggle is BAKED from the "Terrain/Wetness" Diffusion tweak (TerrainWetnessPipeline::
// buildLayout; a change reloads this shader — this is the fallback). The SPREAD is live and framerate
// independent: u_terrainWetParams3.w = 1 - exp(-rate * dt), the fraction of the tent replacing the
// centre this frame, so the front advances ~rate * texel per second whatever the fps. It is a
// one-way (max) spread: it wets the fringe without draining the body (see below).
#ifndef WET_DIFFUSION
#define WET_DIFFUSION 1
#endif

#include "ubo.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 1, r16f) uniform image2DArray u_wet;
#define TERRAIN_HEIGHT_BINDING 2
#include "terrain_height.inc.glsl"
#define UNDERWATER_OCEAN_BINDING 3
#include "underwater_light.inc.glsl"

const int WET_MASK = TERRAIN_WET_RES - 1;

// Last frame's wetness of an absolute lattice coord; `fallback` where the coord was outside last frame's
// window (its slot holds a stale coord's value).
float prevWet(ivec2 lc, ivec2 prevOrigin, int prevLayer, float fallback)
{
    const ivec2 rel = lc - prevOrigin;
    if (any(lessThan(rel, ivec2(0))) || any(greaterThanEqual(rel, ivec2(TERRAIN_WET_RES))))
        return fallback;
    return imageLoad(u_wet, ivec3(lc & WET_MASK, prevLayer)).r;
}

void main()
{
    const ivec2 slot = ivec2(gl_GlobalInvocationID.xy);
    const int writeLayer = int(u_terrainWetParams3.x);
    if (u_terrainWetParams2.x < 0.5)
    {
        imageStore(u_wet, ivec3(slot, writeLayer), vec4(0.0)); // disabled: leave nothing behind for a later enable
        return;
    }
    const int prevLayer = 1 - writeLayer;
    const ivec2 origin = ivec2(u_terrainWetParams0.xy);
    const ivec2 prevOrigin = ivec2(u_terrainWetParams0.zw);
    // The one lattice coord in [origin, origin + RES) that maps to this slot.
    const ivec2 lc = origin + ((slot - origin) & WET_MASK);

    float wet = prevWet(lc, prevOrigin, prevLayer, 0.0);
#if WET_DIFFUSION
    {
        // 1-2-1 tent over the 8 neighbours; a neighbour outside last frame's window contributes the
        // centre's own value so the window edge neither darkens nor brightens.
        float tent = 0.0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const float w = (dx == 0 ? 2.0 : 1.0) * (dz == 0 ? 2.0 : 1.0);
                tent += w * ((dx == 0 && dz == 0) ? wet : prevWet(lc + ivec2(dx, dz), prevOrigin, prevLayer, wet));
            }
        // ADDITIVE, not conserving: a texel only ever rises toward wetter neighbours, never falls toward
        // drier ones — a plain blur would drain the wet body to feed its fringe, drying it faster than
        // the decay says. Water that spreads is not lost here; the decay alone dries the ground.
        wet = max(wet, mix(wet, tent * (1.0 / 16.0), u_terrainWetParams3.w));
    }
#endif

    const vec2 worldXZ = (vec2(lc) + 0.5) * u_terrainWetParams1.x;
    float decay = u_terrainWetParams1.z;
    float target = 0.0;   // wetting target under water this frame
    float soak = 1.0;     // accumulation rate scale (slope drain: water runs off a face before it soaks in)
    if (terrainHeightMapPresent())
    {
        const vec4 d = terrainDataAt(worldXZ);
        float depth = d.y - d.x; // calm water level above ground (negative on dry land)
        // Swash band: gate against the LIVE displaced surface (see doSunLight in the lit core). Ground
        // deeper than the reach is under water at any wave phase and skips the wave taps; u_oceanParams7.w
        // is 0 with the swash or the ocean off, so this costs nothing then.
        const float reach = u_oceanParams7.w;
        if (reach > 0.0 && abs(depth) < reach)
            depth += underwaterLiveWaveY(worldXZ, depth, d.y);
        target = smoothstep(0.0, max(u_terrainWetParams3.z, 1e-3), depth);
        if (target <= 0.0 && u_terrainWetParams2.w > 0.0)
        {
            // Warm ground dries faster: the decay exponent scales with the temperature above 15 C
            // (colder than that = the base dry time). Evaluated at the map's own height.
            const float tempC = terrainTemperatureAt(terrainClimateAt(worldXZ), d.x);
            decay = pow(decay, 1.0 + max(tempC - 15.0, 0.0) * u_terrainWetParams2.w);
        }
        // Slope drain on the ACCUMULATION side: the same drain factor the terrain shader applies to the
        // decay (wetness^(1 + slope * drain)) divides the wet-in and rain rates here, so a cliff face
        // takes that much longer to soak. The slope comes from the map's height gradient — its 8 m
        // texels see cliffs and steep banks, not sub-metre ledges (those only drain faster, per pixel).
        // Only paid where something is accumulating.
        if (u_terrainWetParams5.x > 0.0 && (target > 0.0 || u_terrainWetParams1.w > 0.0))
        {
            const float h = 4.0; // half the near cascade's texel: central differences on its bilinear field
            const float gx = (terrainHeightAt(worldXZ + vec2(h, 0.0)) - terrainHeightAt(worldXZ - vec2(h, 0.0))) * (0.5 / h);
            const float gz = (terrainHeightAt(worldXZ + vec2(0.0, h)) - terrainHeightAt(worldXZ - vec2(0.0, h))) * (0.5 / h);
            const float ny = inversesqrt(1.0 + gx * gx + gz * gz); // the surface normal's Y
            soak = 1.0 / (1.0 + (1.0 - ny) * u_terrainWetParams5.x);
        }
    }
    // Rise toward the target at the wet-in rate, never fall below the decayed carry.
    wet = max(wet * decay, min(wet + u_terrainWetParams3.y * soak, target));
    wet += u_terrainWetParams1.w * soak; // rain
    imageStore(u_wet, ivec3(slot, writeLayer), vec4(clamp(wet, 0.0, 1.0)));
}
