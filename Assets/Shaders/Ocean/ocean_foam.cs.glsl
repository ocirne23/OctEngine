#version 460

// FFT ocean, pass 4/5: the WORLD-SPACE FOAM FIELD (ocean_foam_field.inc.glsl: the levels, the drifted
// rest coordinates, and what the one amount draws). One invocation per texel per level (z = level):
//   foam = max(foam_prev * "Foam decay", instant) - no spread: it keeps the crest's shape
// Injected by the SAME instant-foam function the water shader draws its crest foam with (oceanInstantFoam:
// Jacobian folding + Longuet-Higgins downward crest acceleration), evaluated at the texel's REST position
// with every cascade mip-filtered to this level's texel footprint.
//
// Last frame's state is read from the ping/pong image at xy + shift (u_ocean_foamLevels[l].zw: whole
// texels the level's origin moved since last frame as the camera travelled); texels that scrolled in
// start from the next coarser level (see main). The result is written to the ping/pong layer AND to the maps' foam layer, whose mip chain
// the blit then builds with everything else.

#include "ubo.inc.glsl"
#define OCEAN_MAPS_BINDING 3
#include "ocean_wave.inc.glsl" // oceanInstantFoam + the maps sampler (binding 3 here)
#include "ocean_foam_field.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 1, rgba16f) uniform image2DArray u_maps; // mip 0 storage: write the foam layers
layout (binding = 2, r16f) uniform image2DArray u_foam;    // ping/pong: layer = level * 2 + (0 | 1)

layout (push_constant) uniform FoamPC { uint u_writeLayer; }; // write this slot, read the other

// Last frame's amount of a level at a texel of LAST frame's grid; 0 outside it.
float loadPrev(int level, ivec2 xy)
{
    if (any(lessThan(xy, ivec2(0))) || any(greaterThanEqual(xy, ivec2(OCEAN_FFT_SIZE))))
        return 0.0;
    return imageLoad(u_foam, ivec3(xy, level * 2 + int(1u - u_writeLayer))).x;
}

// Last frame's amount of a level at drifted coordinate q, bilinear. Its last-frame origin is this frame's
// minus the shift (whole texels).
float samplePrev(int level, vec2 q)
{
    const float texel = oceanFoamTexel(level);
    const vec2 prevOrigin = u_ocean_foamLevels[level].xy - u_ocean_foamLevels[level].zw * texel;
    const vec2 st = (q - prevOrigin) / texel - 0.5;
    const ivec2 i = ivec2(floor(st));
    const vec2 f = st - vec2(i);
    return mix(mix(loadPrev(level, i), loadPrev(level, i + ivec2(1, 0)), f.x),
               mix(loadPrev(level, i + ivec2(0, 1)), loadPrev(level, i + ivec2(1, 1)), f.x), f.y);
}

void main()
{
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const int level = int(gl_GlobalInvocationID.z);
    const float N = float(OCEAN_FFT_SIZE);
    const float chop = u_ocean_choppiness;
    const float texel = oceanFoamTexel(level);

    // This texel's rest position: its drifted coordinate plus the drift.
    const vec2 restXZ = u_ocean_foamLevels[level].xy + (vec2(xy) + 0.5) * texel + u_ocean_foamDrift;

    float sxx = 0.0, szz = 0.0, sxz = 0.0, accel = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float Lc = u_ocean_cascadeSizes[c];
        const vec2 uv = restXZ / Lc;
        const float lod = max(log2(texel * N / Lc), 0.0); // texel footprint match: cascade texel -> field texel
        const vec4 g = textureLod(u_oceanMaps, vec3(uv, float(OCEAN_CASCADES + c)), lod);
        const vec4 d = textureLod(u_oceanMaps, vec3(uv, float(c)), lod);
        sxx += g.z; szz += g.w; sxz += d.w;
        accel += textureLod(u_oceanMaps, vec3(uv, float(2 * OCEAN_CASCADES + c)), lod).z;
    }
    const float jxx = 1.0 + chop * sxx;
    const float jzz = 1.0 + chop * szz;
    const float jxz = chop * sxz;
    const float instant = oceanInstantFoam(jxx * jzz - jxz * jxz, accel);

    // Last frame's amount at this texel's drifted position. A texel that SCROLLED IN (outside last frame's
    // grid) starts from the next coarser level's last-frame state there instead of empty: that level has
    // covered this water for a while, so the new strip carries the right amount (softer, a 4x4 average) and
    // no emptier band trails the moving camera. The outermost level has nothing coarser: it starts empty
    // (and fades out anyway). Reading the other slot of any level is safe: this dispatch writes only its own.
    const ivec2 prevXY = xy + ivec2(u_ocean_foamLevels[level].zw);
    float prev;
    if (all(greaterThanEqual(prevXY, ivec2(0))) && all(lessThan(prevXY, ivec2(OCEAN_FFT_SIZE))))
        prev = loadPrev(level, prevXY);
    else if (level + 1 < OCEAN_FOAM_LEVELS)
        prev = samplePrev(level + 1, u_ocean_foamLevels[level].xy + (vec2(xy) + 0.5) * texel);
    else
        prev = 0.0;

    const float foam = max(prev * u_ocean_foamDecay, instant);
    imageStore(u_foam, ivec3(xy, level * 2 + int(u_writeLayer)), vec4(foam));
    imageStore(u_maps, ivec3(xy, OCEAN_FOAM_LAYER + level), vec4(foam, 0.0, 0.0, 0.0));
}
