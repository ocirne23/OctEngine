#ifndef OCEAN_FOAM_FIELD_INC_GLSL
#define OCEAN_FOAM_FIELD_INC_GLSL

// The WORLD-SPACE foam field (ocean_foam.cs.glsl): OCEAN_FOAM_LEVELS camera-centred clipmap levels of
// OCEAN_FFT_SIZE^2 texels, level l's texel = base x 4^l, stored as the ocean maps' last layers
// (OCEAN_FOAM_LAYER + l, mipped with the rest). ONE quantity, x = the FOAM AMOUNT breaking left stuck to
// the water: above "Foam threshold" (its density, oceanStuckFoamCoverage) it draws white foam, and the
// same amount is the bubble cloud under it (milk + roughness) - so decaying foam fades into
// the turquoise glow instead of vanishing.
//
// Indexed in DRIFTED REST coordinates q = rest XZ - drift: the rest (undisplaced, Lagrangian) lattice is
// where a water parcel sits between its orbits, so foam on it rides the waves' horizontal motion and
// stays behind as a crest passes under it; the drift (u_oceanFoamField.xy, accumulated on the CPU) moves
// the whole frame downwind without resampling, so the field never blurs from advection.
// Requires ubo.inc.glsl. fp32 only (the compute pass includes it too).

#define OCEAN_FOAM_LAYER (3 * OCEAN_CASCADES)
#define OCEAN_FOAM_MAX_MIP 6 // the blurred read's coarsest mip (8 x 8 texels: a blur never needs more)

float oceanFoamTexel(int level)
{
    return u_oceanFoamField.z * float(1 << (2 * level));
}

// Cubic B-spline reconstruction of one mip from 4 bilinear taps (Sigg & Hadwiger, GPU Gems 2 ch. 20): a
// magnified field read bilinearly shows every texel as a diamond; the B-spline is C2-smooth across them.
float oceanFoamBicubic(sampler2DArray maps, vec2 uv, float layer, float mip)
{
    const float size = float(OCEAN_FFT_SIZE >> int(mip));
    const vec2 st = uv * size - 0.5;
    const vec2 i = floor(st);
    const vec2 f = st - i;
    const vec2 f2 = f * f, f3 = f2 * f;
    const vec2 w0 = (1.0 / 6.0) * (1.0 - 3.0 * f + 3.0 * f2 - f3);
    const vec2 w1 = (1.0 / 6.0) * (4.0 - 6.0 * f2 + 3.0 * f3);
    const vec2 w2 = (1.0 / 6.0) * (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3);
    const vec2 w3 = (1.0 / 6.0) * f3;
    const vec2 s0 = w0 + w1, s1 = w2 + w3;
    const vec2 p0 = (i - 0.5 + w1 / s0) / size; // i - 1 + w1/s0, back to texel-centre UV (+0.5)
    const vec2 p1 = (i + 1.5 + w3 / s1) / size;
    return s0.y * (s0.x * textureLod(maps, vec3(p0.x, p0.y, layer), mip).x + s1.x * textureLod(maps, vec3(p1.x, p0.y, layer), mip).x)
         + s1.y * (s0.x * textureLod(maps, vec3(p0.x, p1.y, layer), mip).x + s1.x * textureLod(maps, vec3(p1.x, p1.y, layer), mip).x);
}

// The foam amount at a REST position. footprint = world size of one pixel there; blur = a wider world
// size to read the field at (0 = none): the bubble cloud reads it blurred, a diffuse volume under the
// foam, so the separate breaking events merge into clouds instead of texel-sized spots.
// Finest level that covers it, cross-faded into the next over its outer 20%; 0 past the last one. Where a
// level is MAGNIFIED it reads through the B-spline above: near the camera (LOD < 1, crossfading into plain
// trilinear by LOD 1 - four taps), and wherever the blur picked a mip coarser than the pixel (two mips'
// B-splines blended - eight taps, or a coarse mip shows its texels as diamonds).
// Explicit LOD: safe in non-uniform control flow.
float oceanSampleFoamField(sampler2DArray maps, vec2 restXZ, float footprint, float blur)
{
    const vec2 q = restXZ - u_oceanFoamField.xy;
    float result = 0.0;
    float remaining = 1.0;
    for (int level = 0; level < OCEAN_FOAM_LEVELS; ++level)
    {
        const float texel = oceanFoamTexel(level);
        const vec2 uv = (q - u_oceanFoamLevels[level].xy) / (texel * float(OCEAN_FFT_SIZE));
        const vec2 edge = abs(uv - 0.5);
        const float w = 1.0 - smoothstep(0.40, 0.48, max(edge.x, edge.y));
        if (w <= 0.0)
            continue;
        const float layer = float(OCEAN_FOAM_LAYER + level);
        float v;
        if (blur > footprint)
        {
            const float lod = max(log2(blur / texel), 0.0);
            const float mip = min(floor(lod), float(OCEAN_FOAM_MAX_MIP));
            v = oceanFoamBicubic(maps, uv, layer, mip);
            if (lod > mip && mip < float(OCEAN_FOAM_MAX_MIP))
                v = mix(v, oceanFoamBicubic(maps, uv, layer, mip + 1.0), lod - mip);
        }
        else
        {
            const float lod = max(log2(max(footprint, 1e-4) / texel), 0.0);
            v = textureLod(maps, vec3(uv, layer), lod).x;
            if (lod < 1.0)
                v = mix(oceanFoamBicubic(maps, uv, layer, 0.0), v, lod);
        }
        result += v * (w * remaining);
        remaining *= 1.0 - w;
        if (remaining <= 0.0)
            break;
    }
    return result;
}

// The stuck foam's surface DENSITY: its field amount per rest area over the live fold Jacobian J (the
// displaced area per rest area). Foam rides the water, so where the surface converges (J < 1: the
// troughs' gathering lines, just ahead of a crest) it packs, and where it stretches (J > 1: the wave faces)
// it thins and tears. Every cascade is in J, so the finest waves give it detail down to centimetres - all
// from the simulation, no pattern.
float oceanStuckFoamDensity(float amount, float jacobian)
{
    return amount / max(jacobian, 0.1);
}

// Its COVERAGE: a threshold on the density ("Foam threshold", u_oceanFoamField1.y) with a narrow edge
// ("Foam edge", 1.z) plus the caller's AA widening (the density's screen derivative, or a footprint
// estimate where there are none) - crisp foam edges, not a gradient.
float oceanStuckFoamCoverage(float density, float aa)
{
    const float threshold = u_oceanFoamField1.y;
    const float edge = u_oceanFoamField1.z + aa;
    return smoothstep(threshold - edge, threshold + edge, density);
}

#endif
