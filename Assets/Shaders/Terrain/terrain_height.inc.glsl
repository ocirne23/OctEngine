// Camera-centered terrain data cascades (CPU-baked by TerrainStreamer, Renderer::setTerrainDataMap): an RGBA16 UNORM 2D
// array of THREE layers (Procedural's packTerrainDataGpu), one shared world center, every channel a plain number
// (hardware bilinear everywhere):
//   layer 0 = the NEAR cascade, layer 1 = the FAR cascade (same resolution over a much larger range):
//     R = raw terrain surface height (world Y, m)
//     G = water surface level (world Y, m; sea level over open ocean, sunk where no ocean reaches)
//     B = regional fog thickness [0,1]
//     A = MACRO ALTITUDE (m above sea level): splits "mountain" (height far above it) from "high flatland"
//     R, G and A map through the cascade's own range: m = lo + unorm * scale (u_terrain_mapDecode: near .xy, far .zw).
//   layer 2 = the CLIMATE of both cascades at their own texel grids: near (.x sea-level temperature 0..1 over
//     [-25, +50] C, .y humidity), far (.z temperature, .w humidity). Read via terrainClimateAt / terrainClimateFastAt.
// The bake's flow directions are not on the GPU (no shader reads them; the CPU copy keeps them).
// Consumers: the fog (vol_scatter.cs.glsl), the ocean's depth / water level (ocean_wave.inc.glsl), and the long-range sun
// shadow march below (shared by the fog and every lit surface).
// Layer 0 is the mesh's own field (Full detail); layer 1 is the generator's COARSE stage (terrainNearWeight).
//
// UBO (requires ubo.inc.glsl): u_terrain_mapCentre = world center XZ, mapInvNearSize = 1 / near cascade size (0 = no
// map), mapInvFarSize = 1 / far cascade size (0 = near only), mapDecode. The includer defines TERRAIN_HEIGHT_BINDING.

#ifndef TERRAIN_HEIGHT_INC_GLSL
#define TERRAIN_HEIGHT_INC_GLSL

layout (binding = TERRAIN_HEIGHT_BINDING) uniform sampler2DArray u_terrainHeight;

bool terrainHeightMapPresent() { return u_terrain_mapInvNearSize > 0.0; }

// The near cascade's share at near-map uv: 1 inside, 0 past its outer band (0.42 .. 0.48 of the map from its centre).
float terrainNearBlend(vec2 uv0)
{
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    return u_terrain_mapInvFarSize > 0.0 ? 1.0 - smoothstep(0.42, 0.48, edge) : 1.0;
}

// One cascade's texel, decoded: x = height, y = water level, z = fog thickness, w = macro altitude.
vec4 terrainDataLayer(vec2 uv, float layer, vec2 decode)
{
    const vec4 v = textureLod(u_terrainHeight, vec3(uv, layer), 0.0);
    return vec4(decode.x + v.xy * decode.y, v.z, decode.x + v.w * decode.y);
}

// The full texel at worldXZ (x = height, y = water level, z = fog thickness, w = macro altitude); fades to the far
// cascade over the near map's outer band (no seam). Beyond the far cascade, clamp-to-edge extends the edge values. Only
// meaningful while terrainHeightMapPresent().
vec4 terrainDataAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_terrain_mapCentre;
    const vec2 uv0 = rel * u_terrain_mapInvNearSize + 0.5;
    const float nearW = terrainNearBlend(uv0);
    vec4 d = vec4(0.0);
    if (nearW < 1.0)
        d = terrainDataLayer(rel * u_terrain_mapInvFarSize + 0.5, 1.0, u_terrain_mapDecode.zw);
    if (nearW > 0.0)
        d = mix(d, terrainDataLayer(uv0, 0.0, u_terrain_mapDecode.xy), nearW);
    return d;
}

// Raw terrain surface height (world Y, m) at worldXZ.
float terrainHeightAt(vec2 worldXZ) { return terrainDataAt(worldXZ).x; }

// The NEAR cascade's weight at worldXZ: 1 inside it, 0 past it (terrainDataAt's blend band). The far cascade is the
// generator's COARSE stage: a different surface from the drawn mesh, with its shoreline hundreds of metres off. A
// consumer that needs the DRAWN ground (a waterline) scales its use of the height by this.
float terrainNearWeight(vec2 worldXZ)
{
    const vec2 d = abs((worldXZ - u_terrain_mapCentre) * u_terrain_mapInvNearSize);
    return 1.0 - smoothstep(0.42, 0.48, max(d.x, d.y));
}

// Sun visibility marched off the height cascades, for receivers past the PCSS cascades and the RT/TLAS range (the
// terrain mesh ring runs to ~33 km); distant ground would otherwise sit uniformly lit behind a hard terminator.
// Steps DOUBLE from startT (reach startT * 2^steps). The test is a SOFT horizon over a penumbra that widens with t, so
// the widening spacing matches it: a cone trace. Deterministic (no jitter: far shadows hold still under camera motion),
// and an occluder missed in a late gap degrades into a softer edge rather than popping.
//   startT = first sample distance (m). On a SURFACE receiver this is the self-shadow bias: it must clear the map's
//            texel size (4 m near / 100 m far at the defaults) and the sub-texel relief the map does not carry.
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

// Temperature (C) at a world height, from a climate vec4 (terrainClimateAt: .z = the SEA-LEVEL baseline): THE way to read
// temperature out of this map. Every consumer passes the height IT shades, so the two cascades cannot disagree (they bake
// different heights for the same spot: the far one a coarse average, which cannot know a peak exists). The rate is the
// generator's own (u_terrain_lapseRate, C per WORLD metre), not baked per texel. Clamped at sea level: the generator does
// not warm the seabed.
float terrainTemperatureAt(vec4 climate, float worldY)
{
    return climate.z + u_terrain_lapseRate * max(worldY - u_terrain_seaLevel, 0.0);
}

// A climate vec4 from (fog thickness, temperature 0..1, humidity): x = fog thickness [0,1], y = unused, z = SEA-LEVEL
// temperature (C, evaluate with terrainTemperatureAt), w = humidity [0,1].
vec4 terrainClimateDecode(float fog, vec2 tempHum)
{
    return vec4(fog, 0.0, tempHum.x * 75.0 - 25.0, tempHum.y);
}

// Climate fields at worldXZ, SMOOTH: bilinear + the same near/far cascade blend as terrainDataAt. One climate-layer fetch
// per cascade; the fog thickness rides the cascade's data layer (a consumer that ignores .x pays no fetch for it).
vec4 terrainClimateAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_terrain_mapCentre;
    const vec2 uv0 = rel * u_terrain_mapInvNearSize + 0.5;
    const float nearW = terrainNearBlend(uv0);
    vec3 c = vec3(0.0); // fog, temperature 0..1, humidity
    if (nearW < 1.0)
    {
        const vec2 uv = rel * u_terrain_mapInvFarSize + 0.5;
        c = vec3(textureLod(u_terrainHeight, vec3(uv, 1.0), 0.0).z, textureLod(u_terrainHeight, vec3(uv, 2.0), 0.0).zw);
    }
    if (nearW > 0.0)
        c = mix(c, vec3(textureLod(u_terrainHeight, vec3(uv0, 0.0), 0.0).z, textureLod(u_terrainHeight, vec3(uv0, 2.0), 0.0).xy), nearW);
    return terrainClimateDecode(c.x, c.yz);
}

// The cheap variant for the froxel fog and the water: ONE cascade (the far one past 0.45 of the near map), no blend.
// Regional fog and the climate are km-scale, and the fog grid samples this a million times a frame.
vec4 terrainClimateFastAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_terrain_mapCentre;
    const vec2 uv0 = rel * u_terrain_mapInvNearSize + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const bool useFar = u_terrain_mapInvFarSize > 0.0 && edge > 0.45;
    const vec2 uv = useFar ? rel * u_terrain_mapInvFarSize + 0.5 : uv0;
    const vec4 k = textureLod(u_terrainHeight, vec3(uv, 2.0), 0.0);
    return terrainClimateDecode(textureLod(u_terrainHeight, vec3(uv, useFar ? 1.0 : 0.0), 0.0).z, useFar ? k.zw : k.xy);
}

#endif
