// Volumetric fog froxel grid helpers, shared by vol_scatter / vol_integrate / vol_apply. The grid is frustum-aligned:
// XY = viewport UV, Z = exponential view-depth slices from VOL_FOG_NEAR to u_fog_range. VOL_FROXEL_X/Y/Z are injected
// from RendererVKLayout (Layout.ixx).
//
// The less obvious u_fog fields (ubo.inc.glsl):
//   albedo: fog albedo * intensity (> 1 = non-physical gain); anisotropy: phase g
//   noiseScale (1/m) / noiseStrength: drift with the wind (u_weather_windSpeed m/s, u_weather_windDirection)
//   terrainFollow: fraction of the terrain height added to the height fog base (height map: u_terrain_mapCentre /
//                  mapInvNearSize (0 = no map) / mapInvFarSize (0 = near only) / mapSeaLevel (fog clamp floor))
//   terrainShadowDistance: beyond it froxels sun-shadow against the terrain height map (TLAS rays / cascades run out)
//   regionStrength: baked regional thickness / falloff modulation (0 = uniform)
//   underwaterDensity: multiplier on the density below the local water surface
//   farField*: past the froxel volume (vol_apply): farFieldFalloffScale x the near heightFalloff, farFieldSteps = ground
//              samples per ray

#ifndef VOL_FOG_INC_GLSL
#define VOL_FOG_INC_GLSL

#define VOL_FOG_NEAR 1.25

float volFogFar() { return u_fog_range; } // >= VOL_FOG_NEAR + 1 on the CPU

// Exponential slices: more resolution near the camera. The slice power (u_fog_slicePower) warps the normalized slice
// first: < 1 thickens the near slices and thins the far ones, which buys back far Z resolution at long fog ranges.
float volSliceToViewZ(float w)
{
    const float f = volFogFar();
    return VOL_FOG_NEAR * pow(f / VOL_FOG_NEAR, pow(w, u_fog_slicePower));
}

float volViewZToSlice(float viewZ)
{
    const float f = volFogFar();
    return pow(log(max(viewZ, VOL_FOG_NEAR) / VOL_FOG_NEAR) / log(f / VOL_FOG_NEAR), 1.0 / u_fog_slicePower);
}

// Henyey-Greenstein phase. Both directions are negated (camera -> froxel, light -> froxel), so the negations cancel:
// callers pass dot(viewDir, dirTowardLight). Every caller's |g| <= 0.95 (u_fog_anisotropy clamped on the CPU, or a mix
// of it toward 0.78), so the denominator stays >= 0.0025.
float volPhaseHG(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// ---- Analytic height fog (the far field beyond the froxel volume; vol_apply) ----
// Density p(y) = 1 for y <= base, exp(-k * (y - base)) above. MUST MATCH heightFogMean in vol_scatter, plateau included,
// or the handover at the volume's far plane steps. Antiderivative in height: G(0) = 0, G'(h) = p(base + h).
float volHeightAntideriv(float h, float k)
{
    return (h <= 0.0 || k < 1e-6) ? h : (1.0 - exp(-k * h)) / k; // k -> 0 degenerates to G(h) = h
}

// Optical depth over a stretch where the height above the base runs linearly: from hStart, `slope` metres of height
// per metre, for `len` metres. Only linearity in h is needed, so a linearly varying base (terrain interpolated between
// two samples) just subtracts its slope and the result stays exact: volFarField follows terrain without sampling density.
float volAnalyticOpticalDepthLinear(float hStart, float slope, float len, float k, float density)
{
    if (len <= 0.0)
        return 0.0;

    // Near-horizontal is the common case (the horizon): without this the 1/slope NaNs a band across the screen.
    if (abs(slope) < 1e-5)
        return density * len * (hStart <= 0.0 ? 1.0 : exp(-k * hStart));
    return density * (volHeightAntideriv(hStart + slope * len, k) - volHeightAntideriv(hStart, k)) / slope;
}

// Flat-based height fog between t0 and t1 along the ray. Correct for any t1 including VOL_FAR_INFINITY: an upward ray
// converges on G = 1/k, a level or downward one saturates to zero transmittance.
float volAnalyticOpticalDepth(vec3 camPos, vec3 dir, float t0, float t1, float base, float k, float density)
{
    return volAnalyticOpticalDepthLinear(camPos.y + dir.y * t0 - base, dir.y, max(t1 - t0, 0.0), k, density);
}

// "Infinitely far" (sky pixels): large enough that the limits above resolve exactly in fp32, small enough that
// density * t stays in range.
#define VOL_FAR_INFINITY 1.0e7

#endif
