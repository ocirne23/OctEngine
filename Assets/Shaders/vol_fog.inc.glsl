// Volumetric fog froxel grid helpers, shared by vol_scatter / vol_integrate / vol_apply.
// The grid is a view-frustum-aligned 3D texture: XY = viewport UV, Z = exponential view-depth slices
// from VOL_FOG_NEAR to u_fogParams0.w. Dims must match RendererVKLayout::VOL_FROXEL_*.
//
// UBO fog parameter packing (ubo.inc.glsl):
//   u_fogParams0: x = global density (extinction 1/m), y = height fog base (world up), z = height falloff (1/m), w = fog range (m)
//   u_fogParams1: rgb = fog albedo * intensity (scattering tint; > 1 = non-physical gain), w = phase anisotropy g
//   u_fogParams2: x = noise scale (1/m), y = noise strength, z = wind speed (m/s), w = temporal history weight
//   u_fogParams3: x = terrain follow (fraction of the terrain height added to the height fog base),
//                 y = 1 / near terrain map world size (0 = no map), z = enabled, w = light shadow rays (> 0.5)
//   u_fogParams4: x = sun shadow rays per froxel, y = spatial filter (> 0.5), z = GI ambient (> 0.5), w = sun shadow cone half-angle (rad)
//   u_fogParams5: xy = terrain height map world center XZ (shared by both cascades),
//                 z = 1 / far cascade world size (0 = near only), w = baked terrain sea level (fog clamp floor)
//   u_fogParams6: x = slice power (exponent on the normalized slice before the exponential mapping:
//                 1 = plain exponential, < 1 shifts Z resolution from near to far - useful at long ranges),
//                 y = terrain shadow distance (froxels beyond it sun-shadow against the terrain height map
//                 instead of TLAS rays / cascade taps, which both run out of data at distance),
//                 z = regional fog strength (baked thickness/falloff modulation, 0 = uniform),
//                 w = underwater density multiplier (on the global density, below the local water surface)
//   u_fogParams9: far field (past the froxel volume; vol_apply) - x = enabled (> 0.5), y = density scale,
//                 z = multiplier on the near field's height falloff (fogParams0.z), w = ground samples per ray

#ifndef VOL_FOG_INC_GLSL
#define VOL_FOG_INC_GLSL

// VOL_FROXEL_X/Y/Z are injected by the engine from RendererVKLayout (Layout.ixx)
#define VOL_FOG_NEAR 1.25

float volFogFar() { return max(u_fogParams0.w, VOL_FOG_NEAR + 1.0); }

// Exponential slice distribution: more resolution near the camera, where fog detail is visible.
// The slice power (u_fogParams6.x) warps the normalized slice first: < 1 thickens the nearest slices
// and thins the far ones (the last slice's depth extent scales by the power), which buys back far-field
// Z resolution at long fog ranges without touching the froxel count.
float volSliceToViewZ(float w)
{
    const float f = volFogFar();
    return VOL_FOG_NEAR * pow(f / VOL_FOG_NEAR, pow(w, u_fogParams6.x));
}

float volViewZToSlice(float viewZ)
{
    const float f = volFogFar();
    return pow(log(max(viewZ, VOL_FOG_NEAR) / VOL_FOG_NEAR) / log(f / VOL_FOG_NEAR), 1.0 / u_fogParams6.x);
}

// Henyey-Greenstein phase. cosTheta = dot(photon travel direction, out-scatter direction); with both the
// view ray (camera -> froxel) and the light direction (light -> froxel) negated, the negations cancel,
// so callers pass dot(viewDir, dirTowardLight).
float volPhaseHG(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// ---- Analytic height fog (the far field beyond the froxel volume; vol_apply) ---------------------------
// Density profile, matching heightFogMean in vol_scatter including the below-base plateau (they must agree
// or the handover at the volume's far plane steps):
//   p(y) = 1 for y <= base, exp(-k * (y - base)) above it
// Antiderivative in height, G(0) = 0, G'(h) = p(base + h).
float volHeightAntideriv(float h, float k)
{
    return (h <= 0.0 || k < 1e-6) ? h : (1.0 - exp(-k * h)) / k; // k -> 0 degenerates to G(h) = h
}

// Optical depth over a stretch where the height above the base runs linearly: from hStart, at `slope`
// metres of height per metre travelled, for `len` metres.
//
// Linearity in h is what this needs, not a straight ray through a flat layer - so if the base itself
// varies linearly (a terrain-following layer over ground interpolated between two samples), its slope
// just subtracts from the ray's and the result stays exact. That is what lets volFarField follow terrain
// piecewise without ever sampling density.
float volAnalyticOpticalDepthLinear(float hStart, float slope, float len, float k, float density)
{
    if (len <= 0.0)
        return 0.0;
    // Near-horizontal is the common case (the horizon), not a rare edge: without this the 1/slope below
    // NaNs a band across the middle of the screen.
    if (abs(slope) < 1e-5)
        return density * len * (hStart <= 0.0 ? 1.0 : exp(-k * hStart));
    return density * (volHeightAntideriv(hStart + slope * len, k) - volHeightAntideriv(hStart, k)) / slope;
}

// Flat-based height fog between distances t0 and t1 along the ray. Correct for any t1 including
// VOL_FAR_INFINITY: an upward ray converges on G = 1/k, a level or downward one saturates to zero
// transmittance.
float volAnalyticOpticalDepth(vec3 camPos, vec3 dir, float t0, float t1, float base, float k, float density)
{
    return volAnalyticOpticalDepthLinear(camPos.y + dir.y * t0 - base, dir.y, max(t1 - t0, 0.0), k, density);
}

// "Infinitely far" (sky pixels): large enough that the limits above resolve exactly in fp32, small enough
// that density * t stays in range.
#define VOL_FAR_INFINITY 1.0e7

#endif
