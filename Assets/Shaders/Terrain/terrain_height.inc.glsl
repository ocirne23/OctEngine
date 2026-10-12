// Camera-centered terrain data cascades (CPU-baked by TerrainStreamer, setFogTerrainHeightMap): an RGBA32F 2D array,
// layer 0 = near/fine, layer 1 = far/coarse (same resolution over a much larger range), one shared world center:
//   R = raw terrain surface height (world Y, m)                                                - bilinear-safe
//   G = water surface level (world Y, m; sea level over open ocean, higher over lakes/rivers)  - bilinear-safe
//   B = PACKED 4x8 bits BIT-CAST into the float (NaN patterns possible: floatBitsToUint only, never float arithmetic
//       or hardware filtering on the raw value):
//         bits 0-7   fog thickness [0,1]
//         bits 8-15  water FLOW direction: 0 = none, 1..255 = angle/2pi in XZ (toward land at the coast, downhill
//                    elsewhere - HeightMapBaker's applyFlowField). Angles wrap: NEAREST texel only (terrainFlowEncAt).
//         bits 16-23 SEA-LEVEL temperature, CELSIUS over [-25, +50] (terrainTemperatureAt)
//         bits 24-31 humidity [0,1]
//       Read via terrainClimateAt (manual bilinear of decoded texels) / terrainClimateNearestAt.
//   A = MACRO ALTITUDE (m above sea level): splits "mountain" (height far above it) from "high flatland"; bilinear-safe
// Consumers: the fog (vol_scatter.cs.glsl), the ocean's depth/water-level fallback outside its shore map
// (ocean_wave.inc.glsl), and the long-range sun shadow march below (shared by the fog and every lit surface).
//
// UBO (requires ubo.inc.glsl): u_terrain_mapCentre = world center XZ, mapInvNearSize = 1 / near cascade size (0 = no
// map), mapInvFarSize = 1 / far cascade size (0 = near only). The includer defines TERRAIN_HEIGHT_BINDING.

#ifndef TERRAIN_HEIGHT_INC_GLSL
#define TERRAIN_HEIGHT_INC_GLSL

layout (binding = TERRAIN_HEIGHT_BINDING) uniform sampler2DArray u_terrainHeight;

bool terrainHeightMapPresent() { return u_terrain_mapInvNearSize > 0.0; }

// The full texel at worldXZ; fades to the far cascade over the near map's outer band (no seam). Beyond the far
// cascade, clamp-to-edge extends the edge values. Only meaningful while terrainHeightMapPresent().
vec4 terrainDataAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_terrain_mapCentre;
    const vec2 uv0 = rel * u_terrain_mapInvNearSize + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const float nearW = u_terrain_mapInvFarSize > 0.0 ? 1.0 - smoothstep(0.42, 0.48, edge) : 1.0;
    vec4 d = vec4(0.0);
    if (nearW < 1.0)
        d = textureLod(u_terrainHeight, vec3(rel * u_terrain_mapInvFarSize + 0.5, 1.0), 0.0);
    if (nearW > 0.0)
        d = mix(d, textureLod(u_terrainHeight, vec3(uv0, 0.0), 0.0), nearW);
    return d;
}

// Raw terrain surface height (world Y, m) at worldXZ.
float terrainHeightAt(vec2 worldXZ) { return terrainDataAt(worldXZ).x; }

// Sun visibility marched off the height cascades, for receivers past the PCSS cascades and the RT/TLAS range (the
// terrain mesh ring runs to ~33 km); distant ground would otherwise sit uniformly lit behind a hard terminator.
// Steps DOUBLE from startT (reach startT * 2^steps). The test is a SOFT horizon over a penumbra that widens with t, so
// the widening spacing matches it: a cone trace. Deterministic (no jitter: far shadows hold still under camera motion),
// and an occluder missed in a late gap degrades into a softer edge rather than popping.
//   startT = first sample distance (m). On a SURFACE receiver this is the self-shadow bias: it must clear the map's
//            texel size (8 m near / 132 m far) and the sub-texel relief the map does not carry.
//   spread = penumbra growth per metre. Floor it well above the true sun disc (~0.005): the softness stops the map's
//            texels resolving as stair-steps.
float terrainSunVisibility(vec3 pos, vec3 sunDir, float startT, int steps, float spread, float bias)
{
    if (sunDir.y <= 0.0)
        return 0.0; // sun below the horizon
    float vis = 1.0;
    float t = startT;
    for (int i = 0; i < steps; ++i)
    {
        const vec3 p = pos + sunDir * t;
        vis = min(vis, clamp((p.y - terrainHeightAt(p.xz)) / (t * spread + bias), 0.0, 1.0));
        if (vis <= 0.0)
            return 0.0;
        t *= 2.0;
    }
    return vis;
}

// Fog height-falloff from temperature: cold air hugs the ground, warm air lets fog tower. A pure function of
// temperature, so not baked. Mirrors Procedural's fogFalloffFromTemperature.
float fogFalloffFromTemperature(float celsius)
{
    return 1.8 - 1.2 * clamp((celsius + 10.0) * (1.0 / 40.0), 0.0, 1.0); // [0.6, 1.8]
}

// One decoded climate texel from the packed B channel: x = fog thickness [0,1], y = UNUSED (the flow direction must
// never pass through the bilinear below: terrainFlowEncAt), z = SEA-LEVEL temperature (C), w = humidity [0,1].
// .z is a baseline, not a sample: evaluate it with terrainTemperatureAt at the height you shade. The two cascades bake
// different heights for the same spot (the far one a 7.68 km average, which cannot know a peak exists).
vec4 terrainClimateTexel(ivec2 t, int layer, ivec2 res)
{
    const uint packed = floatBitsToUint(texelFetch(u_terrainHeight, ivec3(clamp(t, ivec2(0), res - 1), layer), 0).z);
    return vec4(float(packed & 255u) * (1.0 / 255.0),
                0.0, // bits 8-15 free
                float((packed >> 16) & 255u) * (75.0 / 255.0) - 25.0,
                float((packed >> 24) & 255u) * (1.0 / 255.0));
}

// Temperature (C) at a world height, from a decoded climate texel: THE way to read temperature out of this map. Every
// consumer passes the height IT shades, so the two cascades cannot disagree. The rate is the generator's own
// (u_terrain_lapseRate, C per WORLD metre), not baked per texel. Clamped at sea level: the generator does not warm
// the seabed.
float terrainTemperatureAt(vec4 climate, float worldY)
{
    return climate.z + u_terrain_lapseRate * max(worldY - u_terrain_seaLevel, 0.0);
}

// Manual bilinear of one cascade's climate: hardware filtering would blend the PACKED bits into garbage.
vec4 terrainClimateBilinear(vec2 uv, int layer, ivec2 res)
{
    const vec2 st = uv * vec2(res) - 0.5;
    const vec2 f = fract(st);
    const ivec2 b = ivec2(floor(st));
    const vec4 c00 = terrainClimateTexel(b, layer, res);
    const vec4 c10 = terrainClimateTexel(b + ivec2(1, 0), layer, res);
    const vec4 c01 = terrainClimateTexel(b + ivec2(0, 1), layer, res);
    const vec4 c11 = terrainClimateTexel(b + ivec2(1, 1), layer, res);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

// Climate fields at worldXZ, SMOOTH: manual bilinear + the same near/far cascade blend as terrainDataAt (raw texels
// show as pixels at climate borders).
vec4 terrainClimateAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_terrain_mapCentre;
    const ivec2 res = textureSize(u_terrainHeight, 0).xy;
    const vec2 uv0 = rel * u_terrain_mapInvNearSize + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const float nearW = u_terrain_mapInvFarSize > 0.0 ? 1.0 - smoothstep(0.42, 0.48, edge) : 1.0;
    vec4 c = vec4(0.0);
    if (nearW < 1.0)
        c = terrainClimateBilinear(rel * u_terrain_mapInvFarSize + 0.5, 1, res);
    if (nearW > 0.0)
        c = mix(c, terrainClimateBilinear(uv0, 0, res), nearW);
    return c;
}

// Raw 8-bit flow direction at worldXZ (0 = none, 1..255 = angle/2pi). NEAREST texel, near cascade preferred: encoded
// angles wrap, so no form of bilinear may ever touch them.
uint terrainFlowEncAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_terrain_mapCentre;
    const vec2 uv0 = rel * u_terrain_mapInvNearSize + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const bool useFar = u_terrain_mapInvFarSize > 0.0 && edge > 0.45;
    const vec2 uv = useFar ? rel * u_terrain_mapInvFarSize + 0.5 : uv0;
    const ivec2 res = textureSize(u_terrainHeight, 0).xy;
    const ivec2 t = clamp(ivec2(uv * vec2(res)), ivec2(0), res - 1);
    return (floatBitsToUint(texelFetch(u_terrainHeight, ivec3(t, useFar ? 1 : 0), 0).z) >> 8) & 255u;
}

// Cheap nearest-texel variant for the froxel fog: regional fog is km-scale, and the grid samples this a million times
// a frame.
vec4 terrainClimateNearestAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_terrain_mapCentre;
    const vec2 uv0 = rel * u_terrain_mapInvNearSize + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const bool useFar = u_terrain_mapInvFarSize > 0.0 && edge > 0.45;
    const vec2 uv = useFar ? rel * u_terrain_mapInvFarSize + 0.5 : uv0;
    const ivec2 res = textureSize(u_terrainHeight, 0).xy;
    return terrainClimateTexel(ivec2(uv * vec2(res)), useFar ? 1 : 0, res);
}

#endif
