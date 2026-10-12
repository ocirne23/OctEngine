// Terrain wetness clipmap - read side. A persistent R16F image, TERRAIN_WET_RES^2 texels of u_terrainWater_texelSize
// metres, stored TOROIDALLY around the scene focus like the GI probe clipmap: a texel's slot is its lattice coord &
// (RES-1), so a texel that stays in the window keeps its slot (and its wetness) while the window scrolls.
// terrain_wetness.cs.glsl writes it (decay + swash/rain injection); the terrain FS darkens and glosses the ground.
// UBO (requires ubo.inc.glsl): u_terrain_wetOrigin = the window's min-corner lattice coord, u_terrainWater_texelSize /
// invTexelSize, u_terrainWater_enabled. The includer defines TERRAIN_WET_BINDING (the image is in GENERAL layout).

#ifndef TERRAIN_WETNESS_INC_GLSL
#define TERRAIN_WETNESS_INC_GLSL

layout (binding = TERRAIN_WET_BINDING) uniform sampler2DArray u_terrainWet; // 2 layers = ping/pong

bool terrainWetPresent() { return u_terrainWater_enabled > 0.5; }

// The layer the compute pass wrote THIS frame (the other holds last frame's field).
int terrainWetLayer() { return int(u_terrain_wetLayer); }

// Lattice coord of the window's min corner. The floats carry exact integers (|coord| << 2^24).
ivec2 terrainWetOrigin() { return ivec2(u_terrain_wetOrigin); }

// Storage slot of an absolute lattice coord: & mask is a true mod for power-of-two RES, negative coords too.
ivec2 terrainWetSlot(ivec2 lc) { return lc & (TERRAIN_WET_RES - 1); }

// One texel; 0 outside the window (a slot there holds some OTHER coord's wetness).
float terrainWetTexel(ivec2 lc, ivec2 origin)
{
    const ivec2 rel = lc - origin;
    if (any(lessThan(rel, ivec2(0))) || any(greaterThanEqual(rel, ivec2(TERRAIN_WET_RES))))
        return 0.0;
    return texelFetch(u_terrainWet, ivec3(terrainWetSlot(lc), terrainWetLayer()), 0).r;
}

// Wetness [0,1] at worldXZ: manual bilinear (hardware filtering would blend across the wrap seam), faded to 0 over
// the window's outer band so the coverage edge never shows.
float terrainWetnessAt(vec2 worldXZ)
{
    if (!terrainWetPresent())
        return 0.0;
    const ivec2 origin = terrainWetOrigin();
    const vec2 p = worldXZ * u_terrainWater_invTexelSize - 0.5; // texel centres sit at (lc + 0.5) * texel
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

// THE WATER LEVEL this wetness fills the splat relief to (height 0..1): 0 = the relief's low points, 1 = its top
// (every crevice full). Water stands where the relief is below it, so rain first shows in the crevices.
// "Fill start" / "Fill full" bracket it, "Fill curve" is the exponent between them. CLAMPED to 1: the level is a
// height INSIDE the relief - unclamped, the film floats off the ground.
// SLOPE ("Film max slope" / "Film slope fade", as mesh normal.y): water does not stand on a slope, so the level sinks
// with it (a smooth recede, not an alpha fade; the slope drain alone only thins the wetness). normalY = the SMOOTH
// mesh normal. Shared by the film's coverage (the terrain FS) and its surface (the TES): always the same water.
float terrainPoolLevel(float wet, float normalY)
{
    const float start = u_terrainWater_fillStart, full = u_terrainWater_fillFull;
    const float t = clamp((wet - start) / (full - start), 0.0, 1.0);
    const float slope = smoothstep(u_terrainWater_slopeCut, u_terrainWater_slopeFull, normalY);
    return pow(t, u_terrainWater_fillCurve) * slope;
}

#endif
