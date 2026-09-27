// Volumetric cloud density model, shared by every cloud pass (march, and later the shadow map and the sky
// map). Requires shared.inc.glsl. The includer defines CLOUD_NOISE_BINDING, the first of FOUR consecutive
// combined image samplers (all generated once by cloud_noise.cs.glsl, repeat addressing, mipmapped 3D):
//   +0 base   3D 128^3: r = Perlin-Worley, gba = Worley fBm at 4 / 8 / 16 cells per tile
//   +1 detail 3D  32^3: rgb = Worley fBm at 2 / 4 / 8 cells per tile
//   +2 weather 2D 512^2: r = coverage field, g = cloud type field, b = density field
//   +3 curl   2D 128^2: rg = curl vector * 0.5 + 0.5
//
// THE SHELL: altitudes [u_cloudShape0.x, u_cloudShape0.y] above world Y 0, on a sphere of ATMOS_R_PLANET
// whose centre lies straight under the camera. Everything is CAMERA-RELATIVE: a point is `rel` from the
// camera, and its noise-space XZ is rel.xz + cloudNoiseOffset() - the CPU wraps (camera - wind) by the
// weather period in double (Renderer::buildUboClouds), so the coordinates stay small anywhere. Base and
// detail repeat an INTEGER number of times per weather tile, so that one wrap keeps every texture
// continuous. The noise height axis is the altitude, so the shapes follow the curved shell.

#ifndef CLOUDS_INC_GLSL
#define CLOUDS_INC_GLSL

layout (binding = CLOUD_NOISE_BINDING + 0) uniform sampler3D u_cloudBaseNoise;
layout (binding = CLOUD_NOISE_BINDING + 1) uniform sampler3D u_cloudDetailNoise;
layout (binding = CLOUD_NOISE_BINDING + 2) uniform sampler2D u_cloudWeather;
layout (binding = CLOUD_NOISE_BINDING + 3) uniform sampler2D u_cloudCurl;

const float CLOUD_BASE_RES = 128.0;
const float CLOUD_DETAIL_RES = 32.0;
const float CLOUD_NEAR_DETAIL_MULT = 8.0; // integer: the near octave must tile with the detail texture

float cloudRemap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) * (newHi - newLo) / (hi - lo);
}

// Noise-space XZ offset of THIS view: the CPU origin is built from the centre view's camera, so an eye
// adds its own offset from it (0 on desktop).
vec2 cloudNoiseOffset()
{
    return u_cloudNoiseOrigin.xz + (u_viewPos - u_views[VIEW_CENTER].viewPos.xyz).xz;
}

// Altitude of a camera-relative point. h = (r^2 - R^2) / (r + R): no cancellation between two ~6.4e6 values.
// r + R is taken to first order, 2R + y: the dropped term xz^2 / (2 (R + y)) is a relative error of about
// xz^2 / 4R^2 in h (1.4e-4 at 150 km: under a metre), and it saves the sqrt on every march, sun-march and
// shadow-map sample.
float cloudAltitude(vec3 rel, float camAlt)
{
    const float y = rel.y + camAlt;
    return (dot(rel.xz, rel.xz) + y * (y + 2.0 * ATMOS_R_PLANET)) / (2.0 * ATMOS_R_PLANET + y);
}

// The two roots (near, far) of a ray from a point at altitude originAlt against the sphere at altitude H;
// b = dot(planet-centred origin, direction). (1e30, -1e30) = no hit. The stable quadratic (Ray Tracing
// Gems ch. 7): c is formed from the altitude difference, and the smaller root comes from c / q instead of
// a cancelling subtraction.
vec2 cloudRaySphere(float originAlt, float b, float H)
{
    const float c = (originAlt - H) * (2.0 * ATMOS_R_PLANET + originAlt + H);
    const float disc = b * b - c;
    if (disc < 0.0)
        return vec2(1e30, -1e30);
    const float s = sqrt(disc);
    const float q = (b >= 0.0) ? -b - s : -b + s;
    const float t1 = (q != 0.0) ? c / q : 0.0;
    return vec2(min(q, t1), max(q, t1));
}

// b for a ray from a camera-relative point along dir (the planet centre lies straight under the camera).
float cloudRayB(vec3 rel, vec3 dir, float camAlt)
{
    return dot(rel.xz, dir.xz) + (rel.y + camAlt + ATMOS_R_PLANET) * dir.y;
}

// The ray's (at most two) intervals inside the shell, clamped to [0, tMax], for a ray from a point at
// altitude originAlt with b = cloudRayB. The ground (altitude 0) ends the ray. An empty interval has y <= x.
void cloudShellIntervals(float originAlt, float b, float tMax, out vec2 seg0, out vec2 seg1)
{
    const vec2 tTop = cloudRaySphere(originAlt, b, u_cloudShape0.y);
    const vec2 tBottom = cloudRaySphere(originAlt, b, u_cloudShape0.x);
    const vec2 tGround = cloudRaySphere(originAlt, b, 0.0);
    float tEnd = tMax;
    if (tGround.x > 0.0 && tGround.x < tGround.y)
        tEnd = min(tEnd, tGround.x);
    seg0 = vec2(max(tTop.x, 0.0), min(tTop.y, tEnd));
    seg1 = vec2(1.0, 0.0);
    if (tBottom.x < tBottom.y) // carve the inside of the bottom sphere out
    {
        seg1 = vec2(max(tBottom.y, seg0.x), seg0.y);
        seg0.y = min(seg0.y, tBottom.x);
    }
}

// Vertical density profile for a cloud type (0 = stratus, 0.5 = cumulus, 1 = cumulonimbus) at the
// normalized shell height hf: xy = the bottom ramp, zw = the top ramp.
float cloudHeightProfile(float hf, float type)
{
    const vec4 stratus      = vec4(0.00, 0.06, 0.12, 0.22);
    const vec4 cumulus      = vec4(0.00, 0.10, 0.35, 0.60);
    const vec4 cumulonimbus = vec4(0.00, 0.08, 0.75, 1.00);
    const vec4 g = type < 0.5 ? mix(stratus, cumulus, type * 2.0) : mix(cumulus, cumulonimbus, type * 2.0 - 1.0);
    return smoothstep(g.x, g.y, hf) * (1.0 - smoothstep(g.z, g.w, hf));
}

float cloudHeightFraction(float alt)
{
    return (alt - u_cloudShape0.x) * u_cloudShape3.z; // z = 1 / (top - bottom)
}

// Normalized density [0, 1+] at a noise-space XZ and an altitude. Multiply by u_cloudShape1.w for the
// extinction (1/m). detail = the weight of the detail erosion (0 = the cheap shape, weather + base only, no
// detail fetches; the march fades it out with distance). lodBase / lodDetail are the explicit mip levels (the
// march derives them from the pixel footprint); camDist fades in the near octave.
float cloudDensity(vec2 nxz, float alt, float camDist, float detail, float lodBase, float lodDetail)
{
    const float hf = cloudHeightFraction(alt);
    if (hf <= 0.0 || hf >= 1.0)
        return 0.0;
    const vec4 weather = textureLod(u_cloudWeather, nxz * u_cloudShape1.x, 0.0);
    const float coverage = clamp(u_cloudShape0.z + (weather.r - 0.5) * u_cloudShape3.x, 0.0, 1.0);
    if (coverage <= 0.001)
        return 0.0;
    const float type = clamp(u_cloudShape2.x + (weather.g - 0.5) * u_cloudShape2.y, 0.0, 1.0);
    // Per-column tower height (weather.a): the profile is stretched over [0, columnTop] of the shell, so
    // neighbouring clouds end at different heights.
    const float columnTop = mix(0.45, 1.0, weather.a);
    const float profile = cloudHeightProfile(hf / columnTop, type);
    if (profile <= 0.0)
        return 0.0;

    const vec4 b = textureLod(u_cloudBaseNoise, vec3(nxz.x, alt, nxz.y) * u_cloudShape1.y, lodBase);
    const float lowFbm = dot(b.gba, vec3(0.625, 0.25, 0.125));
    // The Perlin-Worley remap lands in [0.5, 1] (r >= its own Worley term), so the coverage threshold
    // sweeps that range: coverage 0 = nothing passes, 1 = everything. The profile RAISES the threshold
    // instead of only scaling the density: toward the top (and the base) only the strongest noise passes,
    // so the clouds round into domes. A profile times the density against one fixed threshold cut every
    // cloud at the same height - flat tops.
    const float baseShape = clamp(cloudRemap(b.r, lowFbm - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0);
    float d = clamp(cloudRemap(baseShape, 1.0 - 0.5 * coverage * profile, 1.0, 0.0, 1.0), 0.0, 1.0) * min(profile * 4.0, 1.0);
    if (d <= 0.0)
        return 0.0;

    if (detail > 0.0)
    {
        // Curl-distorted detail: stronger toward the base (wispy undersides), rising with the evolve drift.
        const vec2 curl = textureLod(u_cloudCurl, nxz * (u_cloudShape1.y * 4.0), 0.0).xy * 2.0 - 1.0;
        const vec2 dxz = nxz + curl * (u_cloudShape2.w * (1.0 - hf));
        const vec3 pd = vec3(dxz.x, alt + u_cloudNoiseOrigin.w, dxz.y) * u_cloudShape1.z;
        float hfFbm = dot(textureLod(u_cloudDetailNoise, pd, lodDetail).rgb, vec3(0.625, 0.25, 0.125));
        if (camDist < u_cloudShape3.y)
        {
            const float nearFbm = dot(textureLod(u_cloudDetailNoise, pd * CLOUD_NEAR_DETAIL_MULT, 0.0).rg, vec2(0.7, 0.3));
            hfFbm = mix(hfFbm, hfFbm * 0.7 + nearFbm * 0.3, 1.0 - camDist * u_cloudShape3.w);
        }
        // Wispy (inverted) at the base, billowy at the top.
        const float erodeBy = mix(hfFbm, 1.0 - hfFbm, clamp(hf * 5.0, 0.0, 1.0));
        d = clamp(cloudRemap(d, erodeBy * (u_cloudShape2.z * detail), 1.0, 0.0, 1.0), 0.0, 1.0);
    }
    return d * mix(0.6, 1.4, weather.b);
}

// Draine's phase function (normalized): HG with an extra alpha * mu^2 term (alpha 1, g 0 = Rayleigh).
float phaseDraine(float mu, float g, float alpha)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * mu, 1.5))
         * (1.0 + alpha * mu * mu) / (1.0 + alpha * (1.0 + 2.0 * g2) / 3.0);
}

// The HG + Draine fit to Mie scattering on water droplets (Jendersie & d'Eon 2023; the CPU turns the
// droplet size into u_cloudLight0): the sharp forward peak (the silver lining) and the fogbow. k scales
// both g (the multiple-scattering octaves flatten the phase).
float cloudPhase(float mu, float k)
{
    return mix(phaseHG(mu, u_cloudLight0.x * k), phaseDraine(mu, u_cloudLight0.y * k, u_cloudLight0.z), u_cloudLight0.w);
}

// Single-scattered atmosphere between a planet-centred origin ro and ro + dir * tEnd, per unit sun
// radiance (multiply by the sun colour), and its transmittance: the aerial perspective in front of a
// cloud, from any altitude, to a finite distance, in any direction.
vec3 cloudAerialScatter(vec3 ro, vec3 dir, float tEnd, vec3 lightDir, out vec3 transmittance)
{
    const int steps = 4;
    const float mu = dot(dir, lightDir);
    const float pR = phaseRayleigh(mu);
    const float pM = phaseHG(mu, u_skySunParams.y);
    const AtmosRay ray = atmosRayBegin(ro, dir); // the origin's Chapman values once, not per step
    vec3 sumR = vec3(0.0), sumM = vec3(0.0);
    const float dt = tEnd / float(steps);
    float t = 0.5 * dt;
    for (int i = 0; i < steps; ++i)
    {
        const vec3 p = ro + dir * t;
        const float h = length(p) - ATMOS_R_PLANET;
        const vec2 dens = exp(-max(h, 0.0) / vec2(ATMOS_H_RAY, ATMOS_H_MIE)) * dt;
        const vec3 atten = exp(-atmosTau(atmosRayOD(ray, t) + atmosLightOpticalDepth(p, lightDir)));
        sumR += atten * dens.x;
        sumM += atten * dens.y;
        t += dt;
    }
    transmittance = exp(-atmosTau(atmosRayOD(ray, tEnd)));
    return (sumR * u_betaRayleigh * pR + sumM * vec3(u_betaMie) * pM) * u_skySunParams.x;
}

#endif
