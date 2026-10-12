#version 460

// Terrain wetness clipmap - write side (TerrainWetnessPipeline; read side + addressing in terrain_wetness.inc.glsl).
// One thread per storage slot of the toroidal window: decode the slot's lattice coord for THIS frame's window, carry
// last frame's wetness if the coord was inside LAST frame's window (else it starts dry), decay it, then inject:
//   - ocean: ground under the LIVE water surface (the lit core's underwater predicate: calm depth + the swash run-up
//     in the swash band) wets up, so the wet tongue is where the water was drawn. The texel RISES toward the target
//     at the wet-in rate rather than jumping, so an advancing front reads as a gradient instead of popping to 1.
//     Submerged ground reaches 1 and holds it, so a receding drawdown reveals a wet seabed that dries.
//   - rivers / lakes: under their water and one map texel past its edge, up to the river wetness.
//   - rain: a uniform per-frame addition (u_terrain_wetRain; 0 = no rain).
// Decay is exponential in sim time (factor from the CPU), sharpened on warm ground by the map's climate. Two layers =
// ping/pong: read last frame's, write the other (u_terrain_wetLayer names the written layer).

// Diffusion: last frame's value through a wrapped 3x3 tent, so wetness spreads sideways (as the ocean foam mask) and
// the front smooths off the texel grid. BAKED from the "Terrain/Wetness" Diffusion tweak (this is the fallback). The
// SPREAD is live and framerate independent: u_terrain_wetSpread = 1 - exp(-rate * dt). One-way (max): it wets the
// fringe without draining the body.
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

// The inland water map (RiverSystem's bake, setRiverWaterMap): the river / lake surface Y per texel, RIVER_WATER_NONE
// elsewhere. The terrain-data map's water channel is the sea's only.
layout (binding = 4, std430) readonly buffer InRiverWaterMap
{
    vec2 rw_origin;
    float rw_invTexel;
    uint rw_dim;
    float rw_height[];
};

bool riverWaterAt(vec2 worldXZ)
{
    if (rw_dim == 0u)
        return false;
    const ivec2 t = ivec2(floor((worldXZ - rw_origin) * rw_invTexel));
    if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(int(rw_dim)))))
        return false;
    return rw_height[uint(t.y) * rw_dim + uint(t.x)] > -1.0e8;
}

const int WET_MASK = TERRAIN_WET_RES - 1;

// Last frame's wetness of an absolute lattice coord; `fallback` where the coord was outside last frame's window.
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
    const int writeLayer = int(u_terrain_wetLayer);
    if (u_terrainWater_enabled < 0.5)
    {
        imageStore(u_wet, ivec3(slot, writeLayer), vec4(0.0)); // disabled: leave nothing behind for a later enable
        return;
    }

    const int prevLayer = 1 - writeLayer;
    const ivec2 origin = ivec2(u_terrain_wetOrigin);
    const ivec2 prevOrigin = ivec2(u_terrain_wetPrevOrigin);
    // The one lattice coord in [origin, origin + RES) that maps to this slot.
    const ivec2 lc = origin + ((slot - origin) & WET_MASK);

    float wet = prevWet(lc, prevOrigin, prevLayer, 0.0);
#if WET_DIFFUSION
    {
        // 1-2-1 tent; a neighbour outside last frame's window contributes the centre's own value (no edge darkening).
        float tent = 0.0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const float w = (dx == 0 ? 2.0 : 1.0) * (dz == 0 ? 2.0 : 1.0);
                tent += w * ((dx == 0 && dz == 0) ? wet : prevWet(lc + ivec2(dx, dz), prevOrigin, prevLayer, wet));
            }
        // ADDITIVE, not conserving: a texel only rises toward wetter neighbours - a plain blur would drain the wet
        // body to feed its fringe, drying it faster than the decay says.
        wet = max(wet, mix(wet, tent * (1.0 / 16.0), u_terrain_wetSpread));
    }
#endif

    const vec2 worldXZ = (vec2(lc) + 0.5) * u_terrainWater_texelSize;
    float decay = u_terrain_wetDecay;
    float dryMul = 1.0;   // warm-ground multiplier on the decay exponent
    float target = 0.0;   // wetting target under water this frame
    float soak = 1.0;     // accumulation rate scale (slope drain: water runs off a face before it soaks in)
    if (terrainHeightMapPresent())
    {
        const vec4 d = terrainDataAt(worldXZ);
        float depth = d.y - d.x; // calm water level above ground (negative on dry land)
        // Swash band: gate against the LIVE displaced surface (doSunLight). Deeper ground is under water at any wave
        // phase and skips the wave taps (u_ocean_swashReach >= 0.01 on the CPU).
        const float reach = u_ocean_swashReach;
        if (abs(depth) < reach)
            depth += underwaterLiveWaveY(worldXZ, depth, d.y);
        target = depth > 0.0 ? 1.0 : 0.0; // under the live surface = fully wet

        if (target <= 0.0)
        {
            // Warm ground dries faster: the decay exponent scales with the temperature above 15 C, at the map's height.
            const float tempC = terrainTemperatureAt(terrainClimateAt(worldXZ), d.x);
            dryMul = 1.0 + max(tempC - 15.0, 0.0) * u_terrainWater_dryTempSensitivity;
            decay = pow(decay, dryMul);
        }

        // Slope drain on the ACCUMULATION side: the terrain FS's drain factor (wetness^(1 + slope * drain)) divides
        // the wet-in and rain rates, so a cliff takes that much longer to soak. The slope is the map's height
        // gradient (8 m texels: cliffs and steep banks, not ledges). Only paid where something is accumulating.
        if (target > 0.0 || u_terrain_wetRain > 0.0)
        {
            const float h = 4.0; // half the near cascade's texel: central differences on its bilinear field
            const float gx = (terrainHeightAt(worldXZ + vec2(h, 0.0)) - terrainHeightAt(worldXZ - vec2(h, 0.0))) * (0.5 / h);
            const float gz = (terrainHeightAt(worldXZ + vec2(0.0, h)) - terrainHeightAt(worldXZ - vec2(0.0, h))) * (0.5 / h);
            const float ny = inversesqrt(1.0 + gx * gx + gz * gz); // the surface normal's Y
            soak = 1.0 / (1.0 + (1.0 - ny) * u_terrainWater_slopeDrain);
        }
    }

    // Rivers and lakes: under their water plus a one-texel bank band, up to "Terrain/Rivers/Surface/Wetness"; the
    // diffusion spreads the fringe up the banks. Independent of the terrain-data map.
    if (target < u_river_wetness)
    {
        const float r = 1.0 / rw_invTexel; // the CPU writes it > 0, with or without a map
        if (riverWaterAt(worldXZ) || riverWaterAt(worldXZ + vec2(r, 0.0)) || riverWaterAt(worldXZ - vec2(r, 0.0)) ||
            riverWaterAt(worldXZ + vec2(0.0, r)) || riverWaterAt(worldXZ - vec2(0.0, r)))
        {
            target = u_river_wetness;
            dryMul = 1.0;
            decay = u_terrain_wetDecay;
        }
    }

    // Rise toward the target at the wet-in rate, never fall below the decayed carry: d(wet)/dt = rain - wet / dryTime,
    // so rain settles the ground at rain x dryTime.
    wet = max(wet * decay, min(wet + u_terrain_wetIn * soak, target));
    wet += u_terrain_wetRain * soak; // rain
    imageStore(u_wet, ivec3(slot, writeLayer), vec4(clamp(wet, 0.0, 1.0)));
}
