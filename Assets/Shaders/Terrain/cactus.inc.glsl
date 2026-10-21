// THE CACTUS SKIN - the rocks' `Surface Cactus` (instanced_indirect_rock.fs.glsl) and the clutter's `Kind Cactus`
// (clutter.fs.glsl); Docs/Context/Procedural/Rocks.md, Clutter.md. The ribs are the mesh's own; this draws an AREOLE on
// every rib's crest - a greyish felt pad with a star of pale spines - about a rib's width apart along it, every other rib
// half a step on.

#ifndef CACTUS_INC_GLSL
#define CACTUS_INC_GLSL

// The angle around a part (0..1) from its (cos, sin) - any length. Procedural's rib crests (ribIndent: cos(ribs x
// (angle + pi)) = 1) lie at its whole multiples of 1 / ribs. `pixel` = the width of a pixel in it: at the wrap, the
// derivative of a copy that wraps on the far side.
float cactusAround(vec2 cs, out float pixel)
{
    const float around = atan(cs.y, cs.x) * (1.0 / 6.28318531) + 0.5;
    pixel = min(fwidth(around), fwidth(fract(around + 0.5)));
    return around;
}

// The skin at `cell`: x across the ribs (the crests at whole numbers), y along them, both in rib widths; `ribs` wraps x
// (0 = no wrap: a pad's lattice). `pixel` = the width of a pixel in rib widths. Returns the albedo; `cover` = how much of
// the pixel the felt pads and the spines cover (the roughness follows it). The pattern fades to its mean before it
// aliases, and a spine thinner than a pixel draws a pixel wide, as faint as it is thin.
vec3 cactusSkin(vec2 cell, float ribs, float pixel, vec3 skin, vec3 spineColour, out float cover)
{
    float rib = floor(cell.x + 0.5);
    if (ribs > 0.5 && rib >= ribs)
        rib -= ribs;
    const float pitch = 0.9; // areoles along a crest, rib widths apart
    const float step = cell.y / pitch + 0.5 * mod(rib, 2.0);
    const vec2 d = vec2(cell.x - floor(cell.x + 0.5), (fract(step) - 0.5) * pitch); // from the nearest areole
    const float r = length(d);
    const float pad = 1.0 - smoothstep(0.06, 0.09, r);
    // 7 spines out of the pad to ~0.4 rib widths, the star turned per areole; the distance to the nearest one.
    const float spoke = (fract(atan(d.y, d.x) * (7.0 / 6.28318531) + 0.37 * (rib + floor(step))) - 0.5) * r * (6.28318531 / 7.0);
    const float width = max(0.012, pixel);
    const float spines = (1.0 - smoothstep(0.5 * width, width, abs(spoke))) * (0.012 / width)
        * (1.0 - smoothstep(0.25, 0.42, r)) * smoothstep(0.05, 0.08, r);
    // The valleys a little darker; each rib its own shade.
    const float shade = (0.9 + 0.1 * fract(sin(rib * 12.9898) * 43758.5453)) * mix(1.0, 0.82, smoothstep(0.3, 0.5, abs(d.x)));
    vec3 albedo = skin * shade;
    albedo = mix(albedo, mix(skin, spineColour, 0.5) * 0.75, pad);
    albedo = mix(albedo, spineColour, 0.7 * spines);
    // Past ~1 / 4 rib per pixel the pads alias: the mean.
    const float fade = smoothstep(0.08, 0.3, pixel);
    cover = mix(max(pad, spines), 0.15, fade);
    return mix(albedo, skin * 0.9 + spineColour * 0.04, fade);
}

#endif
