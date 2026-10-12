// Sunlight transmittance through the wavy water surface, for points BELOW the local water level. Shared by the froxel fog
// (vol_scatter.cs.glsl: volumetric light shafts) and the lit surface pass (instanced_indirect.fs.glsl: caustics on
// underwater geometry). Two terms, both from existing data:
//   - CAUSTIC FOCUS: the FFT field's fold Jacobian where the sun ray crossed the surface (entryXZ = worldXZ + sunDir.xz /
//     sunDir.y * depth). Converging wavefronts (J -> 0) focus light into bright moving filaments; diverging ones dim it.
//     The whitecap foam's Jacobian terms, so the pattern matches the drawn surface.
//   - BEER-LAMBERT: exp(-u_ocean_absorption * pathLength) - the blue-green shift that makes froxel light read as shafts.
// Tweaks: u_fog_causticStrength (>= 0.05 on the CPU), u_fog_causticDepthFade (1/m: contrast decay with depth, approximating
// defocus - paired with a mip that coarsens with depth, so deep caustics blur out instead of aliasing).
// The includer defines UNDERWATER_OCEAN_BINDING for the FFT maps first (fog: 11, the forward set: 7). Requires ubo.inc.glsl.

#ifndef UNDERWATER_LIGHT_INC_GLSL
#define UNDERWATER_LIGHT_INC_GLSL

layout (binding = UNDERWATER_OCEAN_BINDING) uniform sampler2DArray u_uwOceanMaps;

// THE shore weights for every consumer that mirrors the ocean's vertex displacement outside the ocean shader (MIRROR
// oceanSwashBase / oceanSwashFadeIn / oceanSwashWeight / oceanSurfaceWeight in ocean_wave.inc.glsl). columnDepth = the CALM
// water depth (negative on dry land), waterLevel = the calm local level.
//   swash   = amplitude x sea x land x fadeIn                (the tongue: drives the backflow / flow offset)
//   surface = 1 - fadeIn x (1 - amplitude x sea x land)      (the ONE depth weight on the raw cascade sum)
void oceanShoreWeights(float columnDepth, float waterLevel, out float swash, out float surface)
{
    const float seaFade = 1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_ocean_seaLevel));
    const float landFade = clamp(1.0 + min(columnDepth, 0.0) * u_ocean_swashInvReach, 0.0, 1.0);
    const float fadeIn = 1.0 - smoothstep(0.0, u_ocean_swashFadeBand, columnDepth);
    const float base = u_ocean_swashAmp * seaFade * landFade;
    swash = base * fadeIn;
    surface = 1.0 - fadeIn * (1.0 - base);
}

// Live wave height (m, relative to the calm local level): the vertex displacement's VERTICAL logic (no flow rotation /
// choppy XZ, close enough for gating). Callers test "underwater" against the INSTANTANEOUS surface, so sand a receding
// swash exposes reads dry (no caustics/absorption) and the run-up tongue reads covered. columnDepth = calm water depth.
float underwaterLiveWaveY(vec2 worldXZ, float columnDepth, float waterLevel)
{
    float sw, surfaceW;
    oceanShoreWeights(columnDepth, waterLevel, sw, surfaceW);

    // Swash backflow moves the water HORIZONTALLY, so the surface above this point comes from x - offset: a first-order
    // inverse. Without it the gate tests the wrong column and paints caustics on dry sand in front of a receding tongue.
    vec2 sampleXZ = worldXZ;
    const float flow = u_ocean_choppiness * u_ocean_swashFlow * sw; // chop * backflow * swash weight
    if (flow > 0.0)
    {
        vec2 off = vec2(0.0);
        float rawY0 = 0.0;
        for (int c = 0; c < OCEAN_CASCADES; ++c)
        {
            const float L = u_ocean_cascadeSizes[c];
            const vec4 d = textureLod(u_uwOceanMaps, vec3(worldXZ / L, float(c)), 0.0);
            off += d.xz;
            rawY0 += d.y;
        }
        // Same thickness gate + reach soft-cap as the displacement.
        vec2 flowOff = off * (flow * smoothstep(0.0, 0.35, rawY0 * sw + columnDepth));
        const float flowCap = u_ocean_swashFlowCap;
        sampleXZ -= flowOff * (flowCap / (flowCap + length(flowOff)));
    }

    // The raw cascade sum times the ONE depth weight the displacement uses.
    float rawY = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
        rawY += textureLod(u_uwOceanMaps, vec3(sampleXZ / u_ocean_cascadeSizes[c], float(c)), 0.0).y;
    return rawY * surfaceW;
}

// worldXZ / depthBelow locate the point below the water; depthBelow is the LIVE depth (path length + entry point).
// footprint = world size of one receiver sample (froxel width for fog, ~0 for surface pixels): it floors the mip so the
// pattern never aliases. reach (>= 1) divides the depth the ATTENUATION terms see (absorption, contrast fade, defocus), so
// shafts survive further without moving the pattern; 1 = physical (the fog passes sqrt of its shaft boost).
// columnDepth / waterLevel = the CALM depth and level: the caustic's waves take the ocean's surface weight, the damping
// the drawn surface applies (without it the surf zone focused full open-water waves under a damped surface).
vec3 underwaterSunTransmittance(vec2 worldXZ, float depthBelow, float footprint, float reach, float columnDepth, float waterLevel)
{
    // u_fog_causticSunStep: xy = sun.xz / sy, z = 1 / sy, sy = max(sun.y, 0.08) (grazing sun: caps the path-length blow-up)
    const float dEff = depthBelow / max(reach, 1.0);
    const float pathLen = dEff * u_fog_causticSunStep.z;

    float focus = 1.0;
    {
        const vec2 entryXZ = worldXZ + u_fog_causticSunStep.xy * depthBelow; // TRUE surface entry point
        const float chop = u_ocean_choppiness;
        const float defocusLod = min(dEff * 0.1, 5.0); // deeper = softer, wider pattern
        float sxx = 0.0, szz = 0.0, sxz = 0.0;
        for (int c = 0; c < OCEAN_CASCADES; ++c)
        {
            const float L = u_ocean_cascadeSizes[c];
            const float lod = max(log2(max(footprint, 1e-3) * float(OCEAN_FFT_SIZE) / L), 0.0) + defocusLod;
            const vec2 uv = entryXZ / L;
            const vec4 g = textureLod(u_uwOceanMaps, vec3(uv, float(OCEAN_CASCADES + c)), lod); // (dh/dx, dh/dz, dDx/dx, dDz/dz)
            sxx += g.z;
            szz += g.w;
            sxz += textureLod(u_uwOceanMaps, vec3(uv, float(c)), lod).w; // displacement layer w = dDx/dz
        }

        // The Jacobian terms are linear in the displacement, so the shore weight multiplies straight in. The entry
        // point's column depth is unknown; the shaded point's stands in (within a few metres).
        float swashW, surfaceW;
        oceanShoreWeights(columnDepth, waterLevel, swashW, surfaceW);
        sxx *= surfaceW; szz *= surfaceW; sxz *= surfaceW;

        // Fold Jacobian (Tessendorf): < 1 converging (bright), > 1 diverging (dim), as a CONTRAST EXPONENT: "Caustic
        // strength" steepens it so converging zones spike into hot filaments (up to 8x) - distinct rays in the fog. The
        // exponent decays with depth (defocus).
        const float J = (1.0 + chop * sxx) * (1.0 + chop * szz) - chop * sxz * chop * sxz;
        const float depthFade = exp(-dEff * u_fog_causticDepthFade);
        // Shoreline fade over the first metres of TRUE depth: no water column to focus through at depth 0.
        const float shoreFade = smoothstep(0.0, u_fog_causticShoreFade, depthBelow);
        focus = pow(max(1.0 / max(J, 0.125), 0.02), 1.5 * u_fog_causticStrength * depthFade * shoreFade);
    }

    return exp(-u_ocean_absorption * pathLen) * focus;
}

#endif
