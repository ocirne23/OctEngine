#version 460

// FFT ocean, pass 4/5: temporal TURBULENCE accumulation.
//
// The accumulated field is not foam - it is the churn energy breaking events leave in the water. It is
// injected by the SAME instant-foam function the water shader displays (oceanInstantFoam at base
// thresholds: Jacobian folding + Longuet-Higgins downward crest acceleration, evaluated with
// mip-filtered inputs matched to this field's texel footprint) and decays exponentially:
//   turbulence = max(turbulence_prev * decay, instant)
// The water shader then derives the visuals from it: aged foam via fold-threshold relaxation (so the
// pattern comes from the LIVE geometry's convergence lines, not from this coarse map), entrained-bubble
// milkiness, and extra surface roughness. The previous field is read through a wrapped 3x3 diffusion
// tent ("Spread"): turbulence spreads outward as it lives, which also keeps the stored field free of
// texel structure. Two layers = ping/pong (read last frame's layer, write the other; frame slots
// alternate strictly).
//
// Point-sampling the fine cascades' full-res gradients instead of mip-filtering them aliases badly (a
// 0.75m mask texel reading one ~2.5cm steepness sample flickers per frame). The mips sampled here are
// last frame's for the fine cascades (this pass runs before the blit) - one frame of latency, invisible.
//
// The result is stashed into the cascade-0 moments layer's w channel BEFORE the mip blit, so the water
// shader samples mip-filtered trail coverage with everything else. Decay is per frame (u_oceanParams3.z).

#include "ubo.inc.glsl"
#define OCEAN_MAPS_BINDING 3
#include "ocean_wave.inc.glsl" // oceanInstantFoam + the maps sampler (binding 3 here)

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 1, rgba16f) uniform image2DArray u_maps; // mip 0 storage: write moments[c0].w
layout (binding = 2, r16f) uniform image2DArray u_foam;    // trail ping/pong (2 layers, cascade 0 patch)

layout (push_constant) uniform FoamPC { uint u_writeLayer; }; // write this layer, read/diffuse the other

void main()
{
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const float N = float(OCEAN_FFT_SIZE);
    const float chop = u_oceanParams0.w;
    const float L0 = u_oceanParams2.x;

    // World-space offset of this cascade-0 texel; each cascade sampled at its own (wrapping) patch UV,
    // filtered down to this mask texel's footprint.
    const vec2 worldOfs = (vec2(xy) + 0.5) * (L0 / N);

    float sxx = 0.0, szz = 0.0, sxz = 0.0, accel = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
    {
        const float Lc = u_oceanParams2[c];
        const vec2 uv = worldOfs / Lc;
        const float lod = max(log2(L0 / Lc), 0.0); // texel footprint match: cascade texel -> mask texel
        const vec4 g = textureLod(u_oceanMaps, vec3(uv, float(OCEAN_CASCADES + c)), lod);
        const vec4 d = textureLod(u_oceanMaps, vec3(uv, float(c)), lod);
        sxx += g.z; szz += g.w; sxz += d.w;
        accel += textureLod(u_oceanMaps, vec3(uv, float(2 * OCEAN_CASCADES + c)), lod).z;
    }
    const float jxx = 1.0 + chop * sxx;
    const float jzz = 1.0 + chop * szz;
    const float jxz = chop * sxz;
    const float jacobian = jxx * jzz - jxz * jxz;

    // The injection is the shared instant-foam function at BASE thresholds (biasBoost 0): only genuine
    // breaking adds turbulence - the display-side threshold relaxation must not feed back into itself.
    const float instant = oceanInstantFoam(jacobian, accel, 0.0);

    // Previous turbulence, DIFFUSED: a wrapped 3x3 tent of last frame's layer, blended by the spread rate.
    const int readLayer = int(1u - u_writeLayer);
    const int Ni = OCEAN_FFT_SIZE;
    float tent = 0.0;
    for (int j = -1; j <= 1; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            const ivec2 tc = (xy + ivec2(i, j) + ivec2(Ni)) & ivec2(Ni - 1); // patch tiles: wrap
            tent += imageLoad(u_foam, ivec3(tc, readLayer)).x * float((2 - abs(i)) * (2 - abs(j)));
        }
    }
    const float center = imageLoad(u_foam, ivec3(xy, readLayer)).x;
    const float prev = mix(center, tent * (1.0 / 16.0), u_oceanParams4.y);

    const float turbulence = max(prev * u_oceanParams3.z, instant);
    imageStore(u_foam, ivec3(xy, int(u_writeLayer)), vec4(turbulence));

    vec4 moments = imageLoad(u_maps, ivec3(xy, 2 * OCEAN_CASCADES));
    moments.w = turbulence;
    imageStore(u_maps, ivec3(xy, 2 * OCEAN_CASCADES), moments);
}
