// Sunlight transmittance through the wavy water surface, for points BELOW the local water level.
// Shared by the froxel fog (vol_scatter.cs.glsl — turns per-froxel sun light into volumetric light
// shafts) and the lit surface pass (instanced_indirect.fs.glsl — caustics on underwater geometry).
// Two physically-motivated terms, both from data that already exists:
//   - CAUSTIC FOCUS: the fold Jacobian of the FFT wave field at the point where the sun ray crossed
//     the surface (entryXZ = worldXZ + sunDir.xz/sunDir.y * depth). Converging wavefronts (J -> 0)
//     focus light into the bright moving filaments; diverging ones dim it. Same Jacobian terms the
//     whitecap foam uses, so the pattern matches the drawn surface exactly.
//   - BEER-LAMBERT: exp(-u_oceanAbsorption.rgb * pathLength) — the blue-green shift with depth that
//     makes accumulated froxel light read as colored shafts.
// Tweaks ride u_fogParams7: z = caustic strength (0 disables the focus term, absorption remains),
// w = caustic depth fade (1/m: contrast decay with depth, approximating defocus — paired with a mip
// that coarsens with depth so deep caustics blur out instead of aliasing).
//
// The includer defines UNDERWATER_OCEAN_BINDING for the FFT maps (fog binds them at 11, the forward
// set carries them at 7) before including. Requires ubo.inc.glsl (via shared.inc.glsl).

#ifndef UNDERWATER_LIGHT_INC_GLSL
#define UNDERWATER_LIGHT_INC_GLSL

layout (binding = UNDERWATER_OCEAN_BINDING) uniform sampler2DArray u_uwOceanMaps;

// worldXZ/depthBelow locate the shaded point below the CALM water level; footprint = world size of one
// receiver sample (froxel width for fog, ~0 for surface pixels) — it floors the sampling mip so the
// pattern never aliases against what the receiver can represent. reach (>= 1) divides the depth the
// ATTENUATION terms see (absorption + contrast fade + defocus), stretching how far shafts survive
// without moving the pattern (the geometric entry point uses the true depth); 1 = physical. The fog
// passes sqrt of its shaft boost so brightness and length grow together; surfaces pass 1.
// Live wave height (m, relative to the calm local water level): mirrors the vertex displacement's
// VERTICAL logic — the raw cascade sum times the ocean's surface weight (no flow rotation / choppy
// XZ, close enough for gating). Lets callers test "underwater" against the INSTANTANEOUS surface
// instead of the calm level, so sand exposed by a receding swash reads as dry (no caustics/absorption)
// and the run-up tongue reads as covered. columnDepth = calm water depth at the point.
float underwaterLiveWaveY(vec2 worldXZ, float columnDepth, float waterLevel)
{
    // The shore weights — mirror oceanSwashBase / oceanSwashFadeIn / oceanSwashWeight
    // (ocean_wave.inc.glsl): amplitude x sea-connection fade (landlocked water at an elevated level
    // gets no swell) x land-height fade, and the approach fade-in across the swash band.
    const float seaFade = 1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_oceanParams2.w));
    const float reach = max(u_oceanParams7.w, 0.01);
    const float landFade = clamp(1.0 + min(columnDepth, 0.0) / reach, 0.0, 1.0);
    const float fadeIn = 1.0 - smoothstep(0.0, max(2.0 * reach, u_oceanParams4.z * u_oceanParams2.y), columnDepth);
    const float sw = u_oceanParams7.z * seaFade * landFade * fadeIn;

    // Swash backflow moves the water HORIZONTALLY (displaced = source + chop offset), so the surface
    // above this ground point originates from x - offset: first-order inverse — evaluate the offset
    // here, then sample the height field at the offset position. Without this the gate tests the wrong
    // water column and paints caustics on dry sand just in front of a receding tongue.
    vec2 sampleXZ = worldXZ;
    const float flow = u_oceanParams0.w * u_oceanParams8.z * sw; // chop * backflow * swash weight
    if (flow > 0.0)
    {
        vec2 off = vec2(0.0);
        float rawY0 = 0.0;
        for (int c = 0; c < OCEAN_CASCADES; ++c)
        {
            const float L = u_oceanParams2[c];
            const vec4 d = textureLod(u_uwOceanMaps, vec3(worldXZ / L, float(c)), 0.0);
            off += d.xz;
            rawY0 += d.y;
        }
        // Same thickness gate + reach soft-cap as the displacement: a buried tongue doesn't slide, and
        // the slide distance stays bounded.
        vec2 flowOff = off * (flow * smoothstep(0.0, 0.35, rawY0 * sw + columnDepth));
        const float flowCap = clamp(0.5 * u_oceanParams7.w, 0.25, 1.0);
        sampleXZ -= flowOff * (flowCap / (flowCap + length(flowOff)));
    }

    // The raw cascade sum times the ONE depth weight the displacement uses (mirrors oceanSurfaceWeight):
    // 1 in open water, easing to the swash base (amplitude x sea x land fades) across the approach band.
    float rawY = 0.0;
    for (int c = 0; c < OCEAN_CASCADES; ++c)
        rawY += textureLod(u_uwOceanMaps, vec3(sampleXZ / u_oceanParams2[c], float(c)), 0.0).y;
    const float base = u_oceanParams7.z * seaFade * landFade;
    return rawY * (1.0 - fadeIn * (1.0 - base));
}

vec3 underwaterSunTransmittance(vec2 worldXZ, float depthBelow, float footprint, float reach)
{
    const vec3 sunDir = normalize(u_sunDirection.xyz);
    const float sy = max(sunDir.y, 0.08); // grazing sun: cap the path-length blow-up
    const float dEff = depthBelow / max(reach, 1.0);
    const float pathLen = dEff / sy;

    float focus = 1.0;
    if (u_fogParams7.z > 0.0)
    {
        const vec2 entryXZ = worldXZ + sunDir.xz * (depthBelow / sy); // TRUE surface entry point
        const float chop = u_oceanParams0.w;
        const float defocusLod = min(dEff * 0.1, 5.0); // deeper = softer, wider pattern
        float sxx = 0.0, szz = 0.0, sxz = 0.0;
        for (int c = 0; c < OCEAN_CASCADES; ++c)
        {
            const float L = u_oceanParams2[c];
            const float lod = max(log2(max(footprint, 1e-3) * float(OCEAN_FFT_SIZE) / L), 0.0) + defocusLod;
            const vec2 uv = entryXZ / L;
            const vec4 g = textureLod(u_uwOceanMaps, vec3(uv, float(OCEAN_CASCADES + c)), lod); // (dh/dx, dh/dz, dDx/dx, dDz/dz)
            sxx += g.z;
            szz += g.w;
            sxz += textureLod(u_uwOceanMaps, vec3(uv, float(c)), lod).w; // displacement layer w = dDx/dz
        }
        // Fold Jacobian (Tessendorf): < 1 converging (bright), > 1 diverging (dim). Applied as a CONTRAST
        // EXPONENT — "Caustic strength" steepens the response, so converging zones spike into hot
        // filaments (up to 8x) instead of a gentle modulation, which is what makes fog columns read as
        // distinct rays. The exponent decays with depth (defocus), flattening focus toward 1.
        const float J = (1.0 + chop * sxx) * (1.0 + chop * szz) - chop * sxz * chop * sxz;
        const float depthFade = exp(-dEff * u_fogParams7.w);
        // Shoreline fade: contrast ramps in over the first meters of TRUE depth, so the pattern
        // dissolves at the terrain-waterline intersection instead of cutting off there (a caustic
        // needs a water column to focus through; at depth 0 there is none).
        const float shoreFade = u_fogParams8.y > 0.0 ? smoothstep(0.0, u_fogParams8.y, depthBelow) : 1.0;
        focus = pow(clamp(1.0 / max(J, 0.125), 0.02, 8.0), 1.5 * u_fogParams7.z * depthFade * shoreFade);
    }

    return exp(-u_oceanAbsorption.rgb * pathLen) * focus;
}

#endif
