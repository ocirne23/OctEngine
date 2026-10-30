// The cloud view-ray march, shared by the screen march (cloud_march.cs.glsl) and the sky-map clouds (cloud_sky.cs.glsl).
// Requires shared.inc.glsl, clouds.inc.glsl, cloud_shadow.inc.glsl and a `sampler2DArray u_skyMap` (the GI sky bake:
// its CLEAR layer is the ambient, so the clouds never light themselves through their own image in the sky map).

#ifndef CLOUD_RAYMARCH_INC_GLSL
#define CLOUD_RAYMARCH_INC_GLSL

const float CLOUD_LOG2E = 1.44269504089; // exp(x) = exp2(x * CLOUD_LOG2E), folded into per-ray values: exp's own multiply leaves the loops

// The step jitter: a per-pixel hash, advanced by the golden ratio each frame (R1: well spread per pixel).
// Where the history is missing it shows as grain, not as the diagonal lines of interleaved-gradient noise.
float cloudJitter(ivec2 p)
{
    uint h = uint(p.x) * 1597334673u ^ uint(p.y) * 3812015801u;
    h = (h ^ (h >> 16u)) * 0x7feb352du;
    h ^= h >> 15u;
    return fract(float(h) * (1.0 / 4294967296.0) + float(u_frameIndex & 1023u) * 0.61803398875);
}

// Optical depth toward the sun from a camera-relative point at altitude sampleAlt, in quadratically growing steps:
//  - THE REACH is the way out of the sample's OWN layer toward the sun, capped by u_clouds_lightDistance for a low
//    sun. Not the shell's top: the gap between the layers would take steps.
//  - Only the first (short) step reads the detail: the longer ones integrate over the detail scale anyway.
//  - Each step reads the base noise at the mip of ITS OWN LENGTH (at least the view sample's): a fine mip only
//    thrashes the texture cache (and aliases).
//  - EARLY OUT at cut (normalized density x metres): past it every multi-scattering octave is gone.
float cloudLightOpticalDepth(vec3 rel, vec2 nxz, float camAlt, float sampleAlt, vec3 L, float lodBase, float lodDetail, float detailWeight, float cut)
{
    const int n = int(u_clouds_lightSteps);
    const bool inUpper = u_clouds_upperEnabled > 0.5 && sampleAlt >= u_clouds_upperBottom;
    const float layerTop = inUpper ? u_clouds_upperBottom + 1.0 / u_clouds_upperInvHeight : cloudMainTop();
    const float reach = clamp((layerTop - sampleAlt) * u_clouds_invSunUpLight, 1.0, u_clouds_lightDistance); // L is the sun
    const float invN = u_clouds_invLightSteps;
    const float stepLodBias = log2(u_clouds_baseFrequency * CLOUD_BASE_RES);

    float od = 0.0;
    float tPrev = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float f = float(i + 1) * invN;
        const float t1 = reach * f * f;
        const float tm = 0.5 * (tPrev + t1);
        const vec3 p = rel + L * tm;
        const float alt = cloudAltitude(p, camAlt);
        if (alt > u_clouds_shellTop)
            break;
        const float len = t1 - tPrev;
        const float lodStep = max(lodBase, log2(len) + stepLodBias);
        od += cloudDensity(nxz + L.xz * tm, alt, 1e30, i == 0 ? detailWeight : 0.0, lodStep, lodDetail) * len;
        if (od > cut)
            break;
        tPrev = t1;
    }
    return od * u_clouds_extinction;
}

// The clouds' sun visibility of the AIR at a camera-relative point (this view's camera), for the aerial perspective.
// Inside the shadow map: the map. PAST it, cloudSunTransmittance gives one sky-wide mean (the air toward far cloud
// bases lay half-lit under a dense deck: a blue sheen), so it reads the WEATHER MAP: the coverage of the column where
// the sun ray from the point crosses the main layer's middle.
float cloudAirSunVis(vec3 rel, float camAlt, vec3 L)
{
#ifdef CLOUD_SHADOWS
    if (u_clouds_shadowRendered < 0.5)
        return 1.0;

    const vec2 s = cloudShadowSample(rel + (u_viewPos - u_views_viewPos[VIEW_CENTER].xyz), false);
    float farT = 1.0;
    if (s.y < 1.0)
    {
        const float mid = u_clouds_mainBottom + 0.5 / u_clouds_mainInvHeight;
        const float tSun = max(mid - cloudAltitude(rel, camAlt), 0.0) * u_clouds_invSunUpAir; // L is the sun
        const vec2 nxz = (rel + L * tSun).xz + cloudNoiseOffset();
        const float c = cloudColumnCoverage(u_clouds_coverage, textureLod(u_cloudWeather, nxz * u_clouds_invWeatherPeriod, 0.0).r, nxz);
        farT = 1.0 - smoothstep(0.35, 0.75, c);
    }

    const float T = mix(farT, exp(-s.x), s.y);
    return mix(1.0, T, u_clouds_shadowStrength);
#else
    return 1.0;
#endif
}

// Single-scattered atmosphere from a camera-relative origin to origin + dir * tEnd and its transmittance: the aerial
// perspective in front of a cloud. Returns the sun part per unit sun radiance (multiply by the sun colour); skyLight =
// the SKY-LIGHT part (the "Sky Radiance" light from straight up, as aerial_lut.cs.glsl: most of the blue), radiance.
// Each step's sun is cloud-shadowed (cloudAirSunVis); the sky light is not, as in the LUT. The 8 steps sit at the ray's
// per-pixel / per-frame JITTER, which the temporal pass averages into soft shafts (fixed points stamped the openings'
// lit air onto the clouds as blobs).
vec3 cloudAerialScatter(vec3 origin, float camAlt, vec3 dir, float tEnd, vec3 lightDir, float jitter, out vec3 transmittance, out vec3 skyLight)
{
    const int steps = 8;
    const vec3 ro = origin + vec3(0.0, camAlt + ATMOS_R_PLANET, 0.0); // planet-centred
    const float mu = dot(dir, lightDir);
    const float pR = phaseRayleigh(mu);
    const float pM = phaseHG(mu, u_sky_mieG);
    const bool sky = dot(u_sky_radiance, u_sky_radiance) > 0.0; // uniform: a UBO value
    const AtmosRay ray = atmosRayBegin(ro, dir); // the origin's Chapman values once, not per step
    const vec2 densExp2 = -CLOUD_LOG2E / vec2(ATMOS_H_RAY, ATMOS_H_MIE); // exp(-h / H) as exp2(h * densExp2)
    const float dt = tEnd / float(steps);

    vec3 sumR = vec3(0.0);
    vec3 sumM = vec3(0.0);
    vec3 skyR = vec3(0.0);
    vec3 skyM = vec3(0.0);
    float t = jitter * dt;
    for (int i = 0; i < steps; ++i)
    {
        const vec3 p = ro + dir * t;
        const float h = length(p) - ATMOS_R_PLANET;
        const vec2 dens = exp2(max(h, 0.0) * densExp2) * dt;
        const vec2 viewOD = atmosRayOD(ray, t);
        const vec3 atten = exp(-atmosTau(viewOD + atmosLightOpticalDepth(p, lightDir)))
                         * cloudAirSunVis(origin + dir * t, camAlt, lightDir);
        sumR += atten * dens.x;
        sumM += atten * dens.y;
        if (sky)
        {
            const vec3 skyAtt = exp(-atmosTau(viewOD + atmosLightOpticalDepth(p, p / length(p))));
            skyR += skyAtt * dens.x;
            skyM += skyAtt * dens.y;
        }
        t += dt;
    }

    transmittance = exp(-atmosTau(atmosRayOD(ray, tEnd)));
    skyLight = vec3(0.0);
    if (sky)
    {
        const float muSky = dir.y;
        skyLight = (skyR * u_sky_betaRayleigh * phaseRayleigh(muSky) + skyM * vec3(u_sky_betaMie) * phaseHG(muSky, u_sky_mieG))
                 * u_sky_scatterBoost * u_sky_radiance;
    }
    return (sumR * u_sky_betaRayleigh * pR + sumM * vec3(u_sky_betaMie) * pM) * u_sky_scatterBoost;
}

struct CloudMarchResult
{
    vec3 inScatter;      // incl. the aerial perspective in front of the cloud
    float transmittance;
    float front;         // distance of the first cloud sample (< 0 = no cloud)
    float weighted;      // transmittance-weighted cloud distance
    int steps;
};

// Marches from a camera-relative origin (this view's camera) along dir through the shell intervals seg0 / seg1
// (cloudShellIntervals from that origin). jitter offsets every sample within its step; pixelAngle sets the mip
// level from the footprint. densityOnly = the debug view (white, unlit). jitter < 0 = the ray ends in the SKY (see
// "Aerial fade" at the end; cloudSkyJitter): the flag rides in the sign of a value the march holds anyway - as its own
// bool, live through the whole march, it cost cloud_march 64 -> 72 registers. |jitter| is a free source modifier.
float cloudSkyJitter(float jitter, bool skyBehind) { return skyBehind ? -max(jitter, 1e-7) : jitter; }

CloudMarchResult cloudRaymarch(vec3 origin, vec3 dir, vec2 seg0, vec2 seg1, int maxSteps, float jitter, float pixelAngle, bool densityOnly)
{
    CloudMarchResult r;
    r.inScatter = vec3(0.0);
    r.transmittance = 1.0;
    r.front = -1.0;
    r.weighted = 0.0;
    r.steps = 0;

    const float camAlt = u_viewPos.y;
    const vec2 noiseOffset = cloudNoiseOffset();
    const vec3 L = u_sunDirection;
    const float mu = dot(dir, L);

    // MULTIPLE SCATTERING in CLOSED FORM (per sample 2 exp, no loop): the sum over all isotropic octaves i >= 1 of
    // a^i e^(-od a^i) (Wrenninge, a = "Multi-scatter") as its first octave exactly, plus the geometric tail
    // a^2 / (1 - a) through ONE effective extinction a^(1 + 1/(1 - a)) (the tail's mean octave). The octaves are
    // ISOTROPIC: a flattened droplet phase stayed so forward that the anti-sun side went dark.
    const float ms = u_clouds_multiScatterAttenuation;
    const float msTailScale = ms * ms / (1.0 - ms);
    const float msTailExt = pow(ms, 1.0 + 1.0 / (1.0 - ms));
    const float msStrength = u_clouds_multiScatterStrength;
    // The sun march's early out: the slowest octave falls as exp(-od * min(msTailExt, ms)), so past od = 7 / that it
    // is under 0.1 % - and so is every faster term. Normalized density x metres (u_clouds_sunOdCut, from the CPU).
    const float odCut = u_clouds_sunOdCut;
    // Octave 0 (single scattering) keeps the droplet phase: its forward spike is the silver lining.
    const float isotropic = 1.0 / (4.0 * PI);
    const float phase0 = cloudPhase(mu, 1.0);

    // THE LIGHT COLOURS ARE APPLIED AFTER THE LOOP (registers). A sample's in-scatter is linear in four per-ray colours:
    //   sunBottom * (sun * (1 - hf)) + sunTop * (sun * hf) + ambGround * exp(-h * k) + ambSky * hf
    // (sun = the phase-weighted, shadowed sun term, hf = the height in the sample's own layer), so the loop sums four
    // scalar weights instead of twelve colour floats. The sun's (1 - hf) part is sumSun - sumSunHigh after the loop.
    // The ground bounce falls off EXPONENTIALLY with h = metres above the main layer's base (k =
    // u_clouds_invGroundLightDepth): it lights the undersides only.
    float sumSun       = 0.0; // sum(absorbed * sun)
    float sumSunHigh   = 0.0; // sum(absorbed * sun * hf)
    float sumAmbHigh   = 0.0; // sum(absorbed * hf)
    float sumAmbGround = 0.0; // sum(absorbed * exp(-h * k))

    // The self-shadow comes from the shadow map where it covers the sample (one fetch instead of a sun march); the map
    // is relative to the CENTRE view's camera. u_clouds_shadowRendered = the map was rendered this frame.
#ifdef CLOUD_SELF_SHADOW_MAP
    const bool mapShadow = u_clouds_shadowRendered > 0.5;
#else
    const bool mapShadow = false;
#endif
    const vec3 toCentreView = u_viewPos - u_views_viewPos[VIEW_CENTER].xyz;

    // The mip levels are log2(distance) + a per-ray constant each: one log2 per step.
    const float lodBaseBias   = log2(pixelAngle * u_clouds_baseFrequency * CLOUD_BASE_RES);
    const float lodDetailBias = log2(pixelAngle * u_clouds_detailFrequency * CLOUD_DETAIL_RES);

    // EMPTY-SPACE SKIPPING: in clear air the march takes COARSE steps that test only the cheap shape (weather + base).
    // The detail only ERODES that shape, so a zero there is a zero of the full density. On a hit the march backs up
    // (the coarse step is not taken) and marches finely; CLOUD_COARSE_AFTER empty fine steps return it to coarse.
    const float CLOUD_COARSE_MULT = 3.0;
    const int CLOUD_COARSE_AFTER = 4;
    // EARLY OUT at transmittance CLOUD_T_END; below CLOUD_T_THICK the fine steps double (those samples are weighted by T).
    const float CLOUD_T_END = 0.02;
    const float CLOUD_T_THICK = 0.3;
    // The detail erosion fades out over the last 20 % of "Detail distance"; past it the detail fetches are skipped.
    const float detailInvDist = u_clouds_invDetailDistance;

    // THE STEP SCHEDULE: dt = max(near step, g * t). The growth g is SOLVED PER RAY so the ray takes u_clouds_stepsPerRay
    // steps (capped at ~75 % of maxSteps: the rest absorbs the coarse back-ups) over [sStart, sEnd]. The step count
    // at rate g, with n = the near step, solved by a fixed-point iteration (the log moves slowly):
    //   N(g) = max(0, n / g - sStart) / n + ln(sEnd / max(sStart, n / g)) / g
    // THE NEAR STEP SCALES WITH THE SPAN: clamp(span / target, "Min step", "Near step"), so a short span (up through a
    // thin layer) still takes `target` steps.
    // With the upper layer on, the solve span is the MAIN band's (over the union shell the main layer lost steps to the
    // gap and the upper band); the gap is crossed by the coarse steps. tStart / tEnd stay the SHELL's (the air term).
    const float tStart = max(seg0.y > seg0.x ? seg0.x : seg1.x, 1.0);
    const float tEnd = max(seg1.y > seg1.x ? seg1.y : seg0.y, tStart + 1.0);
    float sStart = tStart;
    float sEnd = tEnd;
    if (u_clouds_upperEnabled > 0.5)
    {
        vec2 m0, m1;
        cloudMainIntervals(cloudAltitude(origin, camAlt), cloudRayB(origin, dir, camAlt), tEnd, m0, m1);
        if (m0.y > m0.x || m1.y > m1.x)
        {
            sStart = max(m0.y > m0.x ? m0.x : m1.x, 1.0);
            sEnd = max(m1.y > m1.x ? m1.y : m0.y, sStart + 1.0);
        }
    }

    const float target = min(u_clouds_stepsPerRay, 0.75 * float(maxSteps));
    const float nearStep = max(min(u_clouds_nearStep, (sEnd - sStart) / target), u_clouds_minStep);
    float growth = 0.0; // near steps throughout
    if ((sEnd - sStart) / nearStep > target)
    {
        growth = log(sEnd / sStart) / target; // the pure geometric schedule: the start point
        for (int i = 0; i < 4; ++i)
        {
            const float tLinear = nearStep / max(growth, 1e-6); // where g * t overtakes the near step
            growth = max((max(1.0 - sStart / tLinear, 0.0) + log(sEnd / max(sStart, tLinear))) / target, 1e-6);
        }
    }

    // THE AERIAL PERSPECTIVE'S DISTANCE: a ray holds cloud at many distances (flat through the layer: near fog AND clouds
    // tens of km out), and at the weighted cloud distance the near cloud was dimmed by the far cloud's air. So the loop
    // averages the air transmittance exp(-kAir * t) weighted like the in-scatter (kAir = the ray's mean air extinction,
    // 1/m, channel mean), and the haze is taken at the distance with that transmittance. kAir2 = kAir * log2(e).
    const float kAir2 = dot(atmosTau(atmosSegmentOD(origin + vec3(0.0, camAlt + ATMOS_R_PLANET, 0.0), dir, tEnd)), vec3(CLOUD_LOG2E / 3.0)) / tEnd;
    float airSum = 0.0; // sum(absorbed * exp(-kAir * t))

    float tSum = 0.0;
    float wSum = 0.0;
    for (int s = 0; s < 2; ++s)
    {
        const vec2 seg = s == 0 ? seg0 : seg1;
        float t = seg.x;
        bool coarse = true;
        int emptyRun = 0;
        while (t < seg.y && r.steps < maxSteps && r.transmittance > CLOUD_T_END)
        {
            // The uniform floor (distance left / steps left) is only the safety net for a budget the back-ups used up:
            // as the rule it made every step of a long ray long.
            float dt = max(nearStep, t * growth);
            dt = max(dt, (seg.y - t) / float(max(maxSteps - r.steps, 1)));
            if (coarse)
                dt *= CLOUD_COARSE_MULT;
            else if (r.transmittance < CLOUD_T_THICK)
                dt *= 2.0;
            dt = min(dt, seg.y - t);
            const float ts = t + dt * abs(jitter);
            ++r.steps;

            const vec3 rel = origin + dir * ts;
            const float alt = cloudAltitude(rel, camAlt);
            const vec2 nxz = rel.xz + noiseOffset;
            const float logDist = log2(ts);
            const float lodBase = logDist + lodBaseBias; // < 0 is fine: the noise sampler's minLod is 0 (CloudPipeline.cpp)
            if (coarse)
            {
                if (cloudDensity(nxz, alt, ts, 0.0, lodBase, 0.0) > 0.0) // detail 0: no erosion (bounds the full shape), no fetch
                {
                    coarse = false; // back up: this interval is marched again, finely
                    emptyRun = 0;
                }
                else
                    t += dt;
                continue;
            }

            t += dt;
            const float lodDetail = logDist + lodDetailBias;
            // Past "Detail distance": the fetch threshold's half - the detail's mean erosion, no fetches (clouds.inc.glsl).
            const float detailWeight = clamp(5.0 - 5.0 * ts * detailInvDist, 0.5 * CLOUD_DETAIL_FETCH_MIN, 1.0);
            const float dens = cloudDensity(nxz, alt, ts, detailWeight, lodBase, lodDetail);
            if (dens <= 0.0)
            {
                if (++emptyRun >= CLOUD_COARSE_AFTER)
                    coarse = true;
                continue;
            }
            emptyRun = 0;

            // Energy-conserving step (Hillaire 2015): the in-scatter integrated over the step's own extinction, so the
            // result does not depend on the step length. Albedo 1.
            const float stepT = exp(-dens * u_clouds_extinction * dt);
            const float absorbed = r.transmittance * (1.0 - stepT);
            if (!densityOnly)
            {
                const float hf = cloudLayerHeightFraction(alt);
                float od;
                const vec2 map = mapShadow ? cloudShadowSample(rel + toCentreView, false) : vec2(0.0);
                if (map.y >= 1.0)
                    od = map.x;
                else
                    od = mix(cloudLightOpticalDepth(rel, nxz, camAlt, alt, L, lodBase, lodDetail,
                        detailWeight > CLOUD_DETAIL_FETCH_MIN ? detailWeight : 0.0, odCut), map.x, map.y); // the sun: no mean erosion
                const float od2 = od * CLOUD_LOG2E; // the three exp as exp2: one multiply instead of three
                const float msSum = ms * exp2(-od2 * ms) + msTailScale * exp2(-od2 * msTailExt);
                const float sunTerm = phase0 * exp2(-od2) + isotropic * (msStrength * msSum);
#ifdef CLOUD_POWDER
                const float powder = mix(1.0, 1.0 - exp2(dens * (-6.0 * CLOUD_LOG2E)), u_clouds_powder);
                const float sunWeight = absorbed * sunTerm * powder;
#else
                const float sunWeight = absorbed * sunTerm;
#endif
                sumSun       += sunWeight;
                sumSunHigh   += sunWeight * hf;
                sumAmbHigh   += absorbed * hf;
                sumAmbGround += absorbed * exp(-max(alt - u_clouds_mainBottom, 0.0) * u_clouds_invGroundLightDepth);
            }

            if (r.front < 0.0)
                r.front = ts;
            tSum += ts * absorbed;
            wSum += absorbed;
            airSum += absorbed * exp2(-kAir2 * ts);
            r.transmittance *= stepT;
        }
    }

    // The early out leaves up to CLOUD_T_END, and the composite passes scene x transmittance: 2 % of the SUN DISC still
    // shone through any cloud. Remapped so the early-out level is fully opaque (continuous; 1 stays 1).
    r.transmittance = clamp((r.transmittance - CLOUD_T_END) * (1.0 / (1.0 - CLOUD_T_END)), 0.0, 1.0);
    if (wSum <= 1e-5)
    {
        r.front = -1.0;
        return r;
    }
    r.weighted = tSum / wSum;
    if (densityOnly)
    {
        r.inScatter = vec3(wSum); // white, unlit: in-scatter 1 per absorbed fraction
        return r;
    }

    // Sun radiance at the cloud: the atmosphere transmittance at the shell bottom and top, along the LOCAL up at the
    // first cloud (at 100 km the local sun elevation differs by ~1 degree: the sunset on distant clouds).
    const vec3 frontRel = origin + dir * r.front;
    const vec3 localUp = normalize(vec3(frontRel.x, frontRel.y + camAlt + CLOUD_R, frontRel.z));
    const vec3 sunColor = u_sunColor * u_sunVisible;
    const vec3 sunBottom = sunColor * atmosTransmittanceToLight(u_clouds_shellBottom, L, localUp);
    const vec3 sunTop = sunColor * atmosTransmittanceToLight(u_clouds_shellTop, L, localUp);

    // Ambient: the CLEAR sky (a 5-direction mean baked once per frame into one texel by gi_sky_map.cs.glsl),
    // and the ground bounce: the sky's ground colour x the cloud albedo x (sun + sky) irradiance / PI.
    const vec3 ambSky = texelFetch(u_skyMap, SKY_MAP_CLOUD_AMBIENT_TEXEL, 0).rgb * u_clouds_ambient;
    const vec3 ambGround = u_clouds_groundBounceAlbedo * (sunColor * u_sunTransmittance * (max(L.y, 0.0) * INV_PI) + ambSky);

    r.inScatter = sunBottom * (sumSun - sumSunHigh) + sunTop * sumSunHigh + ambGround * sumAmbGround + ambSky * sumAmbHigh;

    // Aerial perspective between the origin and the cloud, taken at the distance whose air transmittance is the
    // in-scatter-weighted mean (airSum). With almost no air along the ray (up, from altitude) that distance is
    // ill-conditioned, and the weighted distance serves. -log2(T) / kAir2 = -ln(T) / kAir.
    const float airMean = airSum / wSum;
    const float tAir = kAir2 * tEnd > 1e-3 * CLOUD_LOG2E ? clamp(-log2(max(airMean, 1e-6)) / kAir2, 0.0, tEnd) : r.weighted;

    // The air in front lies in the CLOUDS' SHADOW where the sun is behind them; unshadowed, its Mie forward peak drew a
    // sun glow on top of any cloud.
    vec3 airT, airSky;
    const vec3 air = cloudAerialScatter(origin, camAlt, dir, tAir, L, abs(jitter), airT, airSky) * sunColor + airSky;
    // u_clouds_aerialStrength scales the added air light; "Aerial dimming" the air's optical depth on the cloud's own light
    // (per colour: the far cloud warms and darkens). "Aerial fade" takes the air's MEAN optical depth off the cloud's
    // opacity: the world is flat, so far clouds never sink under a horizon - they fade into the sky behind instead.
    const vec3 airTau = -log(max(airT, vec3(1e-6)));
    r.inScatter = r.inScatter * exp(-airTau * u_clouds_aerialDimming) + air * ((1.0 - r.transmittance) * u_clouds_aerialStrength);
    const float fade = exp(-dot(airTau, vec3(1.0 / 3.0)) * u_clouds_aerialFade);
    r.inScatter *= fade;
    const float fadeOpacity = (1.0 - r.transmittance) * (1.0 - fade);
    // A SKY pixel holds the sun disc and halo, and the composite passes scene x transmittance: the opacity the fade took
    // off let the disc shine through any cloud at any density. There the cloud stays opaque and fades into the clear sky
    // instead (the mirror cache: no disc, seen from the ground). Done here, not by the caller: a second value returned
    // past the aerial scatter cost cloud_march 64 -> 72 registers.
    if (jitter < 0.0) // the ray ends in the sky (cloudSkyJitter)
        r.inScatter += fadeOpacity * textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR_CACHE), 0.0).rgb;
    else
        r.transmittance += fadeOpacity;
    return r;
}

#endif
