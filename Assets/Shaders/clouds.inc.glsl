// Volumetric cloud density model, shared by every cloud pass (march, and later the shadow map and the sky
// map). Requires shared.inc.glsl. The includer defines CLOUD_NOISE_BINDING, the first of FOUR consecutive
// combined image samplers (all generated once by cloud_noise.cs.glsl, repeat addressing, mipmapped 3D):
//   +0 base   3D 128^3: r = Perlin-Worley, gba = Worley fBm at 4 / 8 / 16 cells per tile
//   +1 detail 3D  32^3: rgb = Worley fBm at 2 / 4 / 8 cells per tile
//   +2 weather 2D 512^2: r = coverage field, g = cloud type field, b = density field
//   +3 curl   2D 128^2: rg = curl vector * 0.5 + 0.5
//
// THE SHELL: altitudes [u_clouds_shellBottom, u_clouds_shellTop] above world Y 0, on a sphere of ATMOS_R_PLANET
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
    return u_cloudsLive_noiseOrigin + (u_viewPos - u_views_viewPos[VIEW_CENTER].xyz).xz;
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

// The ray's (at most two) intervals inside the band [bottom, top], clamped to [0, tMax], for a ray from a point
// at altitude originAlt with b = cloudRayB. The ground (altitude 0) ends the ray. An empty interval has y <= x.
void cloudBandIntervals(float originAlt, float b, float tMax, float bottom, float top, out vec2 seg0, out vec2 seg1)
{
    const vec2 tTop = cloudRaySphere(originAlt, b, top);
    const vec2 tBottom = cloudRaySphere(originAlt, b, bottom);
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
// The SHELL: the union of both layers' bands (u_clouds_shellBottom / shellTop) - what the view march covers.
void cloudShellIntervals(float originAlt, float b, float tMax, out vec2 seg0, out vec2 seg1)
{
    cloudBandIntervals(originAlt, b, tMax, u_clouds_shellBottom, u_clouds_shellTop, seg0, seg1);
}
// The MAIN layer's band alone (u_clouds_mainBottom, u_clouds_mainInvHeight).
float cloudMainTop() { return u_clouds_mainBottom + 1.0 / u_clouds_mainInvHeight; }
void cloudMainIntervals(float originAlt, float b, float tMax, out vec2 seg0, out vec2 seg1)
{
    cloudBandIntervals(originAlt, b, tMax, u_clouds_mainBottom, cloudMainTop(), seg0, seg1);
}

// Vertical density profile for a cloud type (0 = stratus, 0.5 = cumulus, 1 = cumulonimbus) at the
// normalized column height hf: xy = the bottom ramp, zw = the top ramp.
// "Base sharpness" (0..1) shortens the bottom ramp toward a flat base. "Top roundness" (the superellipse exponent p,
// 1 = the plain taper) shapes the top ramp as (1 - s^p)^(1/p): the profile - which RAISES the coverage threshold -
// holds near 1 over most of the ramp and falls only near its end, so a cloud stays wide and rounds off into a dome
// instead of thinning into a cone.
float cloudHeightProfile(float hf, float type, float roundnessP, float baseSharpness)
{
    const vec4 stratus      = vec4(0.00, 0.06, 0.12, 0.22);
    const vec4 cumulus      = vec4(0.00, 0.10, 0.35, 0.60);
    const vec4 cumulonimbus = vec4(0.00, 0.08, 0.75, 1.00);
    vec4 g = type < 0.5 ? mix(stratus, cumulus, type * 2.0) : mix(cumulus, cumulonimbus, type * 2.0 - 1.0);
    g.y = mix(g.y, g.x + 0.005, baseSharpness);
    const float s = smoothstep(g.z, g.w, hf);
    return smoothstep(g.x, g.y, hf) * pow(max(1.0 - pow(s, roundnessP), 0.0), 1.0 / roundnessP);
}

// The tower-height signal (0..1) of a column. The tower field alone (weather.a) is smooth and broad, so most clouds
// sat on a slope of it and their tops tilted alike - ramps. "Tower core link" ties the top to the cloud's OWN
// coverage instead: its core rises highest and its edges stay low (central towers, domes), with the tower field
// scaling that per cloud (x 0.5 .. 1.5) so some clouds tower and others stay flat.
float cloudTowerSignal(float coverage, float tower, float coreLink)
{
    return mix(tower, clamp(coverage * (0.5 + tower), 0.0, 1.0), coreLink);
}

// A column's coverage: the layer's base coverage plus the weather map's spread ("Coverage variation",
// u_clouds_coverageVariation). The spread fades in over the first CLOUD_VARIATION_RAMP of the base coverage: with a
// constant spread a coverage of 0 still left the weather map's peaks as clouds (only variation 0 cleared the
// sky). At 0 now: nothing; from the ramp up: exactly the old sum.
const float CLOUD_VARIATION_RAMP = 0.25;
float cloudColumnCoverage(float baseCoverage, float weatherR)
{
    const float spread = u_clouds_coverageVariation * min(baseCoverage * (1.0 / CLOUD_VARIATION_RAMP), 1.0);
    return clamp(baseCoverage + (weatherR - 0.5) * spread, 0.0, 1.0);
}

float cloudHeightFraction(float alt)
{
    return (alt - u_clouds_shellBottom) * u_clouds_invShellHeight;
}

// The height (0..1) within the LAYER a sample belongs to - the lighting's (sky ambient from above, the sun's
// bottom / top blend): with two layers the shell is tall, and a main-layer top at a fraction of it lost its sky
// light. Inside the upper band (when on), that band's; else the main band's, clamped.
float cloudLayerHeightFraction(float alt)
{
    const float hfUpper = (alt - u_clouds_upperBottom) * u_clouds_upperInvHeight;
    if (u_clouds_upperEnabled > 0.5 && hfUpper >= 0.0 && hfUpper <= 1.0)
        return hfUpper;
    return clamp((alt - u_clouds_mainBottom) * u_clouds_mainInvHeight, 0.0, 1.0);
}

// The shape both layers share: the base noise thresholded by coverage x profile, then eroded by the detail.
// noiseAlt = the altitude the noise is read at, hfCloud = the height within the cloud's own column (0..1).
float cloudLayerShape(vec2 nxz, float noiseAlt, float hfCloud, float coverage, float profile, float camDist, float detail, float lodBase, float lodDetail)
{
    const vec4 b = textureLod(u_cloudBaseNoise, vec3(nxz.x, noiseAlt, nxz.y) * u_clouds_baseFrequency, lodBase);
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
        const vec2 curl = textureLod(u_cloudCurl, nxz * (u_clouds_baseFrequency * 4.0), 0.0).xy * 2.0 - 1.0;
        const vec2 dxz = nxz + curl * (u_clouds_curl * (1.0 - hfCloud));
        const vec3 pd = vec3(dxz.x, noiseAlt + u_cloudsLive_detailDrift, dxz.y) * u_clouds_detailFrequency;
        float hfFbm = dot(textureLod(u_cloudDetailNoise, pd, lodDetail).rgb, vec3(0.625, 0.25, 0.125));
        if (camDist < u_clouds_nearDetailRadius)
        {
            const float nearFbm = dot(textureLod(u_cloudDetailNoise, pd * CLOUD_NEAR_DETAIL_MULT, 0.0).rg, vec2(0.7, 0.3));
            hfFbm = mix(hfFbm, hfFbm * 0.7 + nearFbm * 0.3, 1.0 - camDist * u_clouds_invNearDetailRadius);
        }
        // Wispy (inverted) at the base, billowy at the top.
        const float erodeBy = mix(hfFbm, 1.0 - hfFbm, clamp(hfCloud * 5.0, 0.0, 1.0));
        d = clamp(cloudRemap(d, erodeBy * (u_clouds_erosion * detail), 1.0, 0.0, 1.0), 0.0, 1.0);
    }
    // EROSION CUTOFF ("Erosion cutoff", u_clouds_erosionCutoff): the erosion's remap leaves a thin rest wherever the detail
    // fBm is low (a small threshold), and at the extinction scale over kilometres of ray that rest read as haze
    // in the open, eroded areas. Remapped out here: under the cutoff = nothing, the cores stay at 1. Monotonic, so
    // the cheap shape (detail 0) still bounds the full one - the march's coarse test stays conservative.
    const float cut = u_clouds_erosionCutoff;
    return clamp((d - cut) / (1.0 - cut), 0.0, 1.0);
}

// THE UPPER LAYER ("Sky/Clouds/Upper layer", u_clouds_upper*): an independent band above (or
// anywhere around) the main one, with its own coverage and type - a stratiform / altocumulus deck over the
// cumulus instead of one tall shell that stretched every column into a peak. Its weather is the same map
// ROTATED -90 degrees and offset (scale 1: still tiles with the weather period), so its gaps do not follow the
// main layer's. Tops vary per column like the main layer's (its alpha), over [0.6, 1] of the band.
float cloudUpperDensity(vec2 nxz, float alt, float camDist, float detail, float lodBase, float lodDetail)
{
    const float hf = (alt - u_clouds_upperBottom) * u_clouds_upperInvHeight;
    if (hf <= 0.0 || hf >= 1.0)
        return 0.0;
    const vec4 weather = textureLod(u_cloudWeather, vec2(-nxz.y, nxz.x) * u_clouds_invWeatherPeriod + vec2(0.61, 0.13), 0.0);
    const float coverage = cloudColumnCoverage(u_clouds_upperCoverage, weather.r);
    if (coverage <= 0.001)
        return 0.0;
    const float type = clamp(u_clouds_upperType + (weather.g - 0.5) * (0.5 * u_clouds_typeVariation), 0.0, 1.0);
    // Per-column LIFT ("Upper layer/Height variation", u_clouds_upperHeightVariation), as the main layer's: the whole column - the
    // profile and the noise it thresholds - rises by up to that fraction of the band, and its height shrinks to
    // (1 - v) so it stays inside. Without it every sheet sat at the band's bottom: one altitude for the whole deck.
    // Its own field (the tower field NEGATED and offset: scale 1 still tiles; uncorrelated with the main layer's
    // lift and with this layer's weather) at a coarse mip, so the height drifts over kilometres.
    const float variation = u_clouds_upperHeightVariation;
    float lift = 0.0; // fraction of the band
    if (variation > 0.0)
        lift = variation * smoothstep(0.2, 0.8, textureLod(u_cloudWeather, -nxz * u_clouds_invWeatherPeriod + vec2(0.23, 0.71), 2.0).a);
    const float hfCloud = (hf - lift) / (1.0 - variation);
    if (hfCloud <= 0.0 || hfCloud >= 1.0)
        return 0.0;
    // The main layer's profile settings, the tower variation milder.
    const float columnTop = mix(1.0 - 0.7 * u_clouds_towerVariation, 1.0, cloudTowerSignal(coverage, weather.a, u_clouds_towerCoreLink));
    const float profile = cloudHeightProfile(hfCloud / columnTop, type, u_clouds_topRoundness, u_clouds_baseSharpness);
    if (profile <= 0.0)
        return 0.0;
    const float noiseAlt = alt - lift / u_clouds_upperInvHeight; // the noise rides up with the column (band height = 1 / upperInvHeight)
    return cloudLayerShape(nxz, noiseAlt, hfCloud, coverage, profile, camDist, detail, lodBase, lodDetail)
        * (u_clouds_upperDensity * mix(0.6, 1.4, weather.b));
}

// THE MAIN LAYER ("Bottom" / "Top", the "Sky/Clouds" settings).
float cloudMainDensity(vec2 nxz, float alt, float camDist, float detail, float lodBase, float lodDetail)
{
    const float invH = u_clouds_mainInvHeight;
    const float hf = (alt - u_clouds_mainBottom) * invH;
    if (hf <= 0.0 || hf >= 1.0)
        return 0.0;
    const vec2 wuv = nxz * u_clouds_invWeatherPeriod;
    const vec4 weather = textureLod(u_cloudWeather, wuv, 0.0);
    const float coverage = cloudColumnCoverage(u_clouds_coverage, weather.r);
    if (coverage <= 0.001)
        return 0.0;
    const float type = clamp(u_clouds_type + (weather.g - 0.5) * u_clouds_typeVariation, 0.0, 1.0);
    // Per-column tower height: the profile is stretched over [0, columnTop] of the column, so neighbouring clouds
    // end at different heights. "Tower variation" (u_clouds_towerVariation) = how far a top may drop below the layer top.
    const float columnTop = mix(1.0 - u_clouds_towerVariation, 1.0, cloudTowerSignal(coverage, weather.a, u_clouds_towerCoreLink));
    // Per-column LIFT ("Base height variation", v): the WHOLE column rises by up to v of the layer - the profile
    // AND the noise it thresholds - and the column's height shrinks to (1 - v) so it stays inside the shell. A
    // cloud keeps its own rounded base and only sits higher. (Cutting the bottom off instead - a per-column base
    // over the same profile - left the low part of each cloud as thin tendrils down to the shell bottom: a
    // "peak" under every cloud.) The field: the tower field rotated 90 degrees (uncorrelated with the tops; scale
    // 1 still tiles with the weather period) at a COARSE mip, so the height drifts over kilometres - a cloud's
    // base barely tilts, neighbours sit at similar heights, distant ones differ.
    const float variation = u_clouds_baseVariation;
    float lift = 0.0; // fraction of the layer
    if (variation > 0.0)
        lift = variation * smoothstep(0.2, 0.8, textureLod(u_cloudWeather, vec2(wuv.y, -wuv.x) + 0.37, 2.0).a);
    const float hfCloud = (hf - lift) / (1.0 - variation); // 0..1 over THIS column (the wisps and the curl follow it)
    if (hfCloud <= 0.0 || hfCloud >= 1.0)
        return 0.0;
    float profile = cloudHeightProfile(hfCloud / columnTop, type, u_clouds_topRoundness, u_clouds_baseSharpness);
    if (profile <= 0.0)
        return 0.0;
    // SHELVES ("Shelf count / strength / thickness", u_clouds_shelf*): stable layers (inversions) where a rising cloud
    // spreads out sideways into a flat tier - stratocumulus cumulogenitus; at the top of a storm, the anvil. They sit
    // at FIXED heights of the layer ("Shelf spacing" between them, the stack centred in the layer: u_clouds_shelfLowest =
    // the lowest, from the CPU, u_clouds_shelfSpacing = the spacing; hf, not the column's own height), so every cloud spreads at the same altitude, as under a real inversion. Around each the profile is raised past 1, which lowers the coverage
    // threshold: the cloud widens there. Only where the cloud already exists (profile > 0 above) - a column that stays
    // below a shelf does not grow one, so the tiers follow the coverage of the clouds under them.
    const int shelves = int(u_clouds_shelfCount);
    if (shelves > 0)
    {
        float shelf = 0.0;
        for (int i = 0; i < shelves; ++i)
            shelf = max(shelf, 1.0 - smoothstep(0.0, u_clouds_shelfHalfThickness, abs(hf - (u_clouds_shelfLowest + float(i) * u_clouds_shelfSpacing))));
        profile += u_clouds_shelfStrength * shelf;
    }
    const float noiseAlt = alt - lift / invH; // the noise rides up with the column (layer height = 1 / invH)
    return cloudLayerShape(nxz, noiseAlt, hfCloud, coverage, profile, camDist, detail, lodBase, lodDetail) * mix(0.6, 1.4, weather.b);
}

// Normalized density [0, 1+] at a noise-space XZ and an altitude: the MAIN layer + the upper layer. Multiply by
// u_clouds_extinction for the extinction (1/m). detail = the weight of the detail erosion (0 = the cheap shape,
// weather + base only, no detail fetches; the march fades it out with distance). lodBase / lodDetail are the
// explicit mip levels (the march derives them from the pixel footprint); camDist fades in the near octave.
// The SHELL (u_clouds_shellBottom / shellTop) is the union of the two bands - what the march and the shadow map cover.
float cloudDensity(vec2 nxz, float alt, float camDist, float detail, float lodBase, float lodDetail)
{
    float d = cloudMainDensity(nxz, alt, camDist, detail, lodBase, lodDetail);
    if (u_clouds_upperEnabled > 0.5)
        d += cloudUpperDensity(nxz, alt, camDist, detail, lodBase, lodDetail);
    return d;
}

// Draine's phase function (normalized): HG with an extra alpha * mu^2 term (alpha 1, g 0 = Rayleigh).
float phaseDraine(float mu, float g, float alpha)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * mu, 1.5))
         * (1.0 + alpha * mu * mu) / (1.0 + alpha * (1.0 + 2.0 * g2) / 3.0);
}

// The HG + Draine fit to Mie scattering on water droplets (Jendersie & d'Eon 2023; the CPU turns the
// droplet size into u_clouds_hgG / draineG / draineAlpha / draineWeight): the sharp forward peak (the silver
// lining) and the fogbow. k scales both g (the multiple-scattering octaves flatten the phase).
float cloudPhase(float mu, float k)
{
    return mix(phaseHG(mu, u_clouds_hgG * k), phaseDraine(mu, u_clouds_draineG * k, u_clouds_draineAlpha), u_clouds_draineWeight);
}

#endif
