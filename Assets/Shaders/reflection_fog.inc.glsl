// Cheap fog for the water's scene rays (ocean.fs.glsl, the terrain water film). The screen-space fog pass
// only knows the primary ray to the water, so what a ray shows beyond it would otherwise read crisp inside
// a hazy scene.
//
// MIRROR RULE (applyReflectionFog / applyReflectionFogSky): a reflection carries the fog its SOURCE carries
// when seen directly - the viewer compares a reflected shore with that shore a few pixels up the screen:
//     tau = tau(camera -> what the ray shows) - tau(camera -> the water point)
// The second term is what the screen-space pass lays over the water pixel afterwards. Deliberately not the
// literal path from the water point: height fog is densest at its base (sea level over water), so a grazing
// mirror ray stays in the densest metre while the camera's ray to the same shore runs a camera height above
// it. That buries every reflection, by a factor that changes with the fog settings.
//
// LITERAL PATH (applyRayFog / applyRayFogSky): the fog along the ray itself, for a ray whose source the
// viewer cannot see directly - the ocean's underside looking up through Snell's window.
//
// Both: the far field's closed-form height fog (vol_fog.inc.glsl), near medium, each stretch capped at the
// fog range. No march, no fetch - terrain follow is approximated from the heights the ray already knows
// (reflectionFogBase), linear between two points, which is exactly what the closed form needs. No regional
// climate, no noise, no shadowing. In-scatter is the far field's single-scatter collapse: lighting constant
// over the path, so the integral is 1 - T.
//
// Needs the UBO, PI (shared.inc.glsl) and gi_probe.inc.glsl WITH GI_PROBE_HALF (+ the fp16 extension): the
// blend is half math, on the half sky SH read. The optical depths stay 32-bit (heights, distances).
// sunRadiance = the caller's sun tint (transmittance x colour x eclipse), ambientSky = its skyRadiance(up)
// fetch (the GI-off fallback).

#ifndef REFLECTION_FOG_INC_GLSL
#define REFLECTION_FOG_INC_GLSL

#include "vol_fog.inc.glsl"

// A point's height stands in for the macro altitude the real fog follows, and overshoots it (the macro band
// is the smooth ground under the relief). At the full height the base rises as fast as the ray, and the
// height falloff never acts.
const float REFLECTION_FOG_ALTITUDE_SHARE = 0.5;

// The layer's base at a point of height pointY: 0 rise on the ocean, the followed ground under an inland film.
float reflectionFogBase(float pointY)
{
    return u_fogParams0.y + u_fogParams3.x * (u_fogParams5.w + REFLECTION_FOG_ALTITUDE_SHARE * max(pointY - u_fogParams5.w, 0.0));
}

// "Ocean/RT/Reflection fog" (u_oceanParams8.x) x the scene's density; 0 with the fog off.
float reflectionFogDensity()
{
    return u_fogParams3.z < 0.5 ? 0.0 : u_fogParams0.x * u_oceanParams8.x;
}

// Optical depth from the camera to `target`, the base running from baseCam to baseTarget.
float reflectionFogTau(vec3 target, float baseCam, float baseTarget, float density)
{
    const float len = max(distance(u_viewPos, target), 1e-3);
    const float h0 = u_viewPos.y - baseCam;
    const float slope = ((target.y - baseTarget) - h0) / len;
    return volAnalyticOpticalDepthLinear(h0, slope, min(len, max(u_fogParams0.w, 1.0)), u_fogParams0.z, density);
}

// Lays the fog's light over `radiance` for an optical depth tau along `dir`.
vec3 reflectionFogBlend(vec3 radiance, float tau, vec3 dir, vec3 sunRadiance, vec3 L, vec3 ambientSky)
{
    const float T = exp(-tau);
    if (T >= 0.9999)
        return radiance;
    // The far field's light (vol_apply volFarField): HG-phased sun + the GI sky probe toward the viewer.
    // Not skyRadiance(up) - the zenith is the darkest patch of a sunlit sky - except with GI off, where
    // u_aoParams.y is 0 and the SH is stale.
    const f16vec3 skyLight = u_aoParams.y > 0.0 ? giEvalSkySHH(f16vec3(-dir)) * float16_t(u_aoParams.y * INV_PI) : f16vec3(ambientSky);
    const f16vec3 inLight = f16vec3(sunRadiance * volPhaseHG(dot(dir, L), u_fogParams1.w)) + skyLight + f16vec3(u_ambientColor);
    const float16_t Th = float16_t(T);
    return vec3(f16vec3(radiance) * Th + f16vec3(u_fogParams1.rgb) * inLight * (float16_t(1.0) - Th));
}

// shown = the point the ray shows, baseShown = the base there.
vec3 reflectionFogMirror(vec3 radiance, vec3 origin, vec3 dir, vec3 shown, float baseShown, vec3 sunRadiance, vec3 L, vec3 ambientSky)
{
    const float density = reflectionFogDensity();
    if (density <= 1e-7)
        return radiance;
    // The camera's base takes the origin's (no fetch): the camera stands over or beside the water it looks at.
    const float baseOrigin = reflectionFogBase(origin.y);
    const float tau = max(reflectionFogTau(shown, baseOrigin, baseShown, density)
                        - reflectionFogTau(origin, baseOrigin, baseOrigin, density), 0.0);
    return reflectionFogBlend(radiance, tau, dir, sunRadiance, L, ambientSky);
}

// Mirror rule, a scene hit at distance t.
vec3 applyReflectionFog(vec3 radiance, vec3 origin, vec3 dir, float t, vec3 sunRadiance, vec3 L, vec3 ambientSky)
{
    const vec3 hit = origin + dir * t;
    return reflectionFogMirror(radiance, origin, dir, hit, reflectionFogBase(hit.y), sunRadiance, L, ambientSky);
}

// Mirror rule, the reflected sky: the sky seen directly along the ray - a point past the fog range (the
// stretch is capped there), over the origin's base.
vec3 applyReflectionFogSky(vec3 radiance, vec3 origin, vec3 dir, vec3 sunRadiance, vec3 L, vec3 ambientSky)
{
    const vec3 far = u_viewPos + dir * (2.0 * max(u_fogParams0.w, 1.0));
    return reflectionFogMirror(radiance, origin, dir, far, reflectionFogBase(origin.y), sunRadiance, L, ambientSky);
}

// Literal path, a scene hit at distance t.
vec3 applyRayFog(vec3 radiance, vec3 origin, vec3 dir, float t, vec3 sunRadiance, vec3 L, vec3 ambientSky)
{
    const float density = reflectionFogDensity();
    if (density <= 1e-7)
        return radiance;
    const float baseOrigin = reflectionFogBase(origin.y);
    const float hitY = origin.y + dir.y * t;
    const float slope = ((hitY - reflectionFogBase(hitY)) - (origin.y - baseOrigin)) / max(t, 1e-3);
    const float tau = volAnalyticOpticalDepthLinear(origin.y - baseOrigin, slope, min(t, max(u_fogParams0.w, 1.0)), u_fogParams0.z, density);
    return reflectionFogBlend(radiance, tau, dir, sunRadiance, L, ambientSky);
}

// Literal path, the sky: the origin's base held flat (one local height must not govern an unbounded stretch).
vec3 applyRayFogSky(vec3 radiance, vec3 origin, vec3 dir, vec3 sunRadiance, vec3 L, vec3 ambientSky)
{
    const float density = reflectionFogDensity();
    if (density <= 1e-7)
        return radiance;
    const float tau = volAnalyticOpticalDepthLinear(origin.y - reflectionFogBase(origin.y), dir.y, max(u_fogParams0.w, 1.0), u_fogParams0.z, density);
    return reflectionFogBlend(radiance, tau, dir, sunRadiance, L, ambientSky);
}

#endif
