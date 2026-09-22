// Terrain wetness clipmap - read side. A single persistent R16F image, TERRAIN_WET_RES^2 texels of
// u_terrainWetParams1.x metres, stored TOROIDALLY around the scene focus like the GI probe clipmap
// (gi_probe.inc.glsl): a texel's storage slot is its integer lattice coord & (RES-1), so a texel that
// stays in the window keeps its slot (and its wetness) frame after frame while the window scrolls, and
// the coords that scroll out are overwritten by the ones wrapping in. terrain_wetness.cs.glsl writes it
// (decay + swash/rain injection); the terrain fragment shader reads it to darken and gloss the ground.
//
// UBO packing (requires ubo.inc.glsl):
//   u_terrainWetParams0.xy = window origin lattice coord (min corner), u_terrainWetParams1.xy = texel size
//   and its inverse, u_terrainWetParams2.x = present (0/1).
//
// The includer defines TERRAIN_WET_BINDING before including (the image lives in GENERAL layout).

#ifndef TERRAIN_WETNESS_INC_GLSL
#define TERRAIN_WETNESS_INC_GLSL

layout (binding = TERRAIN_WET_BINDING) uniform sampler2DArray u_terrainWet; // 2 layers = ping/pong

bool terrainWetPresent() { return u_terrainWetParams2.x > 0.5; }

// The layer the compute pass wrote THIS frame (the other holds last frame's field).
int terrainWetLayer() { return int(u_terrainWetParams3.x); }

// Lattice coord of the window's min corner. The floats carry exact integers (|coord| << 2^24).
ivec2 terrainWetOrigin() { return ivec2(u_terrainWetParams0.xy); }

// Storage slot of an absolute lattice coord: & mask is a true mod for power-of-two RES, correct for
// negative coords under two's complement.
ivec2 terrainWetSlot(ivec2 lc) { return lc & (TERRAIN_WET_RES - 1); }

// One texel; 0 outside the window (a slot there holds some OTHER coord's wetness).
float terrainWetTexel(ivec2 lc, ivec2 origin)
{
    const ivec2 rel = lc - origin;
    if (any(lessThan(rel, ivec2(0))) || any(greaterThanEqual(rel, ivec2(TERRAIN_WET_RES))))
        return 0.0;
    return texelFetch(u_terrainWet, ivec3(terrainWetSlot(lc), terrainWetLayer()), 0).r;
}

// Wetness [0,1] at worldXZ: manual bilinear of the four surrounding texels (hardware filtering would
// blend across the wrap seam), faded to 0 over the window's outer band so the coverage edge never shows.
float terrainWetnessAt(vec2 worldXZ)
{
    if (!terrainWetPresent())
        return 0.0;
    const ivec2 origin = terrainWetOrigin();
    const vec2 p = worldXZ * u_terrainWetParams1.y - 0.5; // texel centres sit at (lc + 0.5) * texel
    const vec2 fl = floor(p);
    const vec2 f = p - fl;
    const ivec2 i0 = ivec2(fl);
    const float w00 = terrainWetTexel(i0, origin);
    const float w10 = terrainWetTexel(i0 + ivec2(1, 0), origin);
    const float w01 = terrainWetTexel(i0 + ivec2(0, 1), origin);
    const float w11 = terrainWetTexel(i0 + ivec2(1, 1), origin);
    const float w = mix(mix(w00, w10, f.x), mix(w01, w11, f.x), f.y);
    const vec2 c = (p + 0.5 - vec2(origin)) * (1.0 / float(TERRAIN_WET_RES)); // 0..1 across the window
    const float edge = max(abs(c.x - 0.5), abs(c.y - 0.5));
    return w * (1.0 - smoothstep(0.44, 0.49, edge));
}

// THE WATER LEVEL this wetness fills the splat relief to (height 0..1): 0 = the relief's low points,
// 1 = its top (every crevice full, the surface flat over the relief). Water stands where the relief height
// is below it, so rain first shows in the crevices and spreads as the ground keeps wetting.
// "Fill start" / "Fill full" are the wetnesses that bracket it, "Fill curve" the exponent between them
// (1 = linear, > 1 = fills late, < 1 = early). CLAMPED to 1: the level is a height INSIDE the relief -
// unclamped, the film rises off the ground and floats.
// SLOPE ("Film max slope" / "Film slope fade", u_terrainWetParams6.yz as mesh normal.y): water does not
// stand on a slope, so the level sinks with it - a pool drains back into the relief's low points and is gone
// at the max slope. A smooth recede instead of an alpha fade; the slope drain only thins the wetness, so a
// wet enough slope still filled its relief. normalY = the SMOOTH mesh normal (the slope drain's slope).
// Shared by the film's coverage (the terrain FS) and its surface (terrain_tess.tes.glsl), so the two are
// always the same water.
float terrainPoolLevel(float wet, float normalY)
{
    const float start = u_terrainWetParams4.x, full = u_terrainWetParams4.y;
    const float t = clamp((wet - start) / max(full - start, 1e-3), 0.0, 1.0);
    const float slope = smoothstep(u_terrainWetParams6.y, u_terrainWetParams6.z, normalY);
    return clamp(pow(t, max(u_terrainWetParams4.z, 1e-3)), 0.0, 1.0) * slope;
}

#endif
