// Camera-centered terrain data cascades (CPU-baked by Procedural::TerrainStreamer, uploaded via
// Renderer::setFogTerrainHeightMap): an RGBA32F 2D array, layer 0 = near/fine cascade, layer 1 = far/
// coarse cascade (same resolution over a much larger range), one shared world center. Per texel:
//   R = raw terrain surface height (world Y, m)              - bilinear-safe
//   G = water surface level (world Y, m; sea level over open ocean, higher over lakes/rivers) - bilinear-safe
//   B = PACKED 4x8 bits BIT-CAST into the float (NaN patterns possible - floatBitsToUint only, never
//       float arithmetic or hardware filtering on the raw value):
//         bits 0-7   fog thickness, decodes to [0,1]
//         bits 8-15  water FLOW direction: 0 = none, 1..255 = angle/2pi in XZ (toward nearest land where
//                    the ocean meets the coast, downhill elsewhere - see HeightMapBaker's applyFlowField).
//                    Angles wrap: NEAREST texel only (terrainFlowEncAt), never any form of bilinear.
//         bits 16-23 temperature, decodes to CELSIUS over [-25, +50]
//         bits 24-31 humidity, decodes to [0,1] (0 -> 0.0, 255 -> 1.0)
//       Read via terrainClimateAt (manual bilinear of decoded texels) / terrainClimateNearestAt.
//   A = MACRO ALTITUDE (m above sea level, pass-1.5 field) - the terrain coloring splits "mountain"
//       (height far above it) from "high-altitude flatland" (height ~ altitude); bilinear-safe
// Consumers: the volumetric fog's terrain-following height base + regional density
// (vol_scatter.cs.glsl), the ocean's depth/water-level fallback outside its own shore map
// (ocean_wave.inc.glsl), and the long-range sun shadow march below (terrainSunVisibility - shared by the
// fog and every lit surface, which is why it lives here rather than in either consumer).
//
// UBO packing (requires ubo.inc.glsl):
//   u_fogParams5.xy = world center XZ, u_fogParams3.y = 1 / near cascade world size (0 = no map),
//   u_fogParams5.z  = 1 / far cascade world size (0 = near only), u_fogParams5.w = baked terrain sea level
//
// The includer defines TERRAIN_HEIGHT_BINDING before including to bind the sampler.

#ifndef TERRAIN_HEIGHT_INC_GLSL
#define TERRAIN_HEIGHT_INC_GLSL

layout (binding = TERRAIN_HEIGHT_BINDING) uniform sampler2DArray u_terrainHeight;

bool terrainHeightMapPresent() { return u_fogParams3.y > 0.0; }

// Full texel (height, water level, fog thickness, spare) at worldXZ; fades to the far cascade over the
// near map's outer band so the handover never shows a seam. Beyond the far cascade, clamp-to-edge extends
// the edge values. Only meaningful while terrainHeightMapPresent().
vec4 terrainDataAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_fogParams5.xy;
    const vec2 uv0 = rel * u_fogParams3.y + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const float nearW = u_fogParams5.z > 0.0 ? 1.0 - smoothstep(0.42, 0.48, edge) : 1.0;
    vec4 d = vec4(0.0);
    if (nearW < 1.0)
        d = textureLod(u_terrainHeight, vec3(rel * u_fogParams5.z + 0.5, 1.0), 0.0);
    if (nearW > 0.0)
        d = mix(d, textureLod(u_terrainHeight, vec3(uv0, 0.0), 0.0), nearW);
    return d;
}

// Raw terrain surface height (world Y, m) at worldXZ.
float terrainHeightAt(vec2 worldXZ) { return terrainDataAt(worldXZ).x; }

// Sun visibility marched off the height cascades, for receivers the ordinary sun shadow sources cannot
// reach: the PCSS cascades end at Shadows/Max distance and the RT path's instances are bounded by RT/TLAS
// Range, both far short of a terrain mesh ring that runs to ~33 km, so distant ground would otherwise sit
// uniformly lit behind a hard terminator. The map still has data out to its far cascade.
//
// Steps DOUBLE from startT, so the reach is startT * 2^steps and the sample spacing grows with distance.
// That is not an approximation to apologise for: the occlusion test is a SOFT horizon - how far the ray
// clears the terrain, over a penumbra that widens with t - so the widening spacing stays matched to the
// widening penumbra and the whole thing behaves as a cone trace. Two consequences worth relying on:
// it is fully deterministic (no jitter, so no temporal integration, so far shadows hold perfectly still
// under camera motion - unlike a jittered binary march), and an occluder missed inside a late gap
// degrades into a softer edge rather than popping.
//   startT = first sample distance (m). On a SURFACE receiver this is the self-shadow bias: the sample
//            must clear both the map's own texel size (8 m near / 132 m far) and the sub-texel relief the
//            map does not carry at all (the generator's slope-masked noise is finer than either cascade).
//   spread = penumbra growth per metre. Floor it well above the true sun disc (~0.005): the softness is
//            what stops the map's texels resolving as stair-steps.
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

// Fog height-falloff from temperature: cold air hugs the ground, warm air lets fog tower. Recomputed
// rather than baked - it is a pure function of temperature, so storing it wasted 8 bits of the packed
// channel that the LAPSE RATE genuinely needs. Mirrors Procedural's fogFalloffFromTemperature.
float fogFalloffFromTemperature(float celsius)
{
    return clamp(1.8 - 1.2 * clamp((celsius + 10.0) * (1.0 / 40.0), 0.0, 1.0), 0.0, 4.0);
}

// One decoded climate texel from the bit-packed B channel at integer texel coords:
//   x = fog thickness [0,1], y = UNUSED (bits 8-15 hold the flow direction, which must never pass
//   through the bilinear below - angles wrap; read it via terrainFlowEncAt), z = SEA-LEVEL temperature
//   in CELSIUS [-25, +50], w = humidity [0,1].
// .z is NOT the temperature here - it is the baseline at sea level. Evaluate with terrainTemperatureAt at
// whatever height you care about; that is the whole point of storing the model's parameterisation (a
// baseline and a slope) rather than a sample of it. A sample is only valid at the height it was taken
// from, and the two cascades bake different heights for the same spot - the near one the full-detail
// surface, the far one its 7.68 km average, which cannot know a peak exists.
vec4 terrainClimateTexel(ivec2 t, int layer, ivec2 res)
{
    const uint packed = floatBitsToUint(texelFetch(u_terrainHeight, ivec3(clamp(t, ivec2(0), res - 1), layer), 0).z);
    return vec4(float(packed & 255u) * (1.0 / 255.0),
                0.0, // bits 8-15 free
                float((packed >> 16) & 255u) * (75.0 / 255.0) - 25.0,
                float((packed >> 24) & 255u) * (1.0 / 255.0));
}

// Temperature (C) at a world height, from a decoded climate texel. THE way to read temperature out of
// this map: .z alone is the sea-level baseline and means nothing on its own.
// Every consumer passes the height IT shades - the terrain shader its vertex, the fog the ground under a
// froxel - so the two cascades cannot disagree: they carry the same baseline and slope, and the height
// comes from the caller, not from whatever each cascade happened to bake.
// The rate is the generator's own, published once as u_terrainParams.y (C per WORLD metre) rather than
// baked per texel: measured, a per-texel rate bought no near/far agreement at all - both cascades regress
// the same data, so their rates agreed and the disagreement was entirely in the baselines. Clamped at sea
// level because that is where the generator stops applying it (it does not warm the seabed).
float terrainTemperatureAt(vec4 climate, float worldY)
{
    return climate.z + u_terrainParams.y * max(worldY - u_terrainParams.z, 0.0);
}

// Manual bilinear of one cascade's climate: hardware filtering would blend the PACKED bits into
// garbage, so decode the 4 neighboring texels and blend the decoded values.
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

// Climate fields at worldXZ, SMOOTH: manual bilinear + the same near/far cascade blend as terrainDataAt,
// so temperature/humidity gradients have the same effective resolution as the height channel (terrain
// coloring showed the raw texels as visible pixels at climate borders otherwise).
vec4 terrainClimateAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_fogParams5.xy;
    const ivec2 res = textureSize(u_terrainHeight, 0).xy;
    const vec2 uv0 = rel * u_fogParams3.y + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const float nearW = u_fogParams5.z > 0.0 ? 1.0 - smoothstep(0.42, 0.48, edge) : 1.0;
    vec4 c = vec4(0.0);
    if (nearW < 1.0)
        c = terrainClimateBilinear(rel * u_fogParams5.z + 0.5, 1, res);
    if (nearW > 0.0)
        c = mix(c, terrainClimateBilinear(uv0, 0, res), nearW);
    return c;
}

// Raw 8-bit flow direction at worldXZ (bits 8-15 of the packed channel): 0 = no direction, 1..255 =
// angle/2pi of where the water moves - toward land through the surf zone, downhill elsewhere. NEAREST
// texel, near cascade preferred: encoded angles wrap, so no form of bilinear may ever touch them.
uint terrainFlowEncAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_fogParams5.xy;
    const vec2 uv0 = rel * u_fogParams3.y + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const bool useFar = u_fogParams5.z > 0.0 && edge > 0.45;
    const vec2 uv = useFar ? rel * u_fogParams5.z + 0.5 : uv0;
    const ivec2 res = textureSize(u_terrainHeight, 0).xy;
    const ivec2 t = clamp(ivec2(uv * vec2(res)), ivec2(0), res - 1);
    return (floatBitsToUint(texelFetch(u_terrainHeight, ivec3(t, useFar ? 1 : 0), 0).z) >> 8) & 255u;
}


// Cheap nearest-texel variant for volume consumers (froxel fog): regional fog is km-scale, texel
// granularity never shows there, and the froxel grid samples this a million times a frame.
vec4 terrainClimateNearestAt(vec2 worldXZ)
{
    const vec2 rel = worldXZ - u_fogParams5.xy;
    const vec2 uv0 = rel * u_fogParams3.y + 0.5;
    const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
    const bool useFar = u_fogParams5.z > 0.0 && edge > 0.45;
    const vec2 uv = useFar ? rel * u_fogParams5.z + 0.5 : uv0;
    const ivec2 res = textureSize(u_terrainHeight, 0).xy;
    return terrainClimateTexel(ivec2(uv * vec2(res)), useFar ? 1 : 0, res);
}

#endif
