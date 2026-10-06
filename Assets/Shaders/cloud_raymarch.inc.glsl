// The cloud view-ray march, shared by the screen march (cloud_march.cs.glsl) and the sky-map clouds
// (cloud_sky.cs.glsl). Requires shared.inc.glsl, clouds.inc.glsl, cloud_shadow.inc.glsl and a
// `sampler2DArray u_skyMap` (the GI sky bake: its CLEAR layer is the ambient, so the clouds never light
// themselves through their own image in the sky map).

#ifndef CLOUD_RAYMARCH_INC_GLSL
#define CLOUD_RAYMARCH_INC_GLSL

// Optical depth toward the sun from a camera-relative point at altitude sampleAlt: quadratically growing steps
// out to the reach. Only the first (short) step carries the detail noise: the longer ones integrate over
// the detail scale anyway, and each detail sample costs two or three more fetches (curl, detail, near).
// Each step reads the base noise at the mip of ITS OWN LENGTH (at least the view sample's): a step hundreds of
// metres long integrates over that much noise, and the view sample's fine mip only thrashed the texture cache
// (and aliased). EARLY OUT at odCut (normalized, the caller's): past it every multi-scattering octave is gone.
// THE REACH is the way out of the sample's OWN LAYER toward the sun, (layer top - altitude) / L.y, capped by
// "Light distance (m)" (u_clouds_lightDistance) for a low sun. A fixed reach spent steps in the empty air above a sample
// near the top, and one near the base stopped short of the tower over it. The own layer's top, not the shell's:
// the gap between the layers would take steps for nothing.
float cloudLightOpticalDepth(vec3 rel, vec2 nxz, float camAlt, float sampleAlt, vec3 L, float lodBase, float lodDetail, float detailWeight, float odCut)
{
    const int n = int(u_clouds_lightSteps);
    const bool inUpper = u_clouds_upperEnabled > 0.5 && sampleAlt >= u_clouds_upperBottom;
    const float layerTop = inUpper ? u_clouds_upperBottom + 1.0 / u_clouds_upperInvHeight : u_clouds_mainBottom + 1.0 / u_clouds_mainInvHeight;
    const float reach = clamp((layerTop - sampleAlt) / max(L.y, 0.02), 1.0, u_clouds_lightDistance);
    const float invN = 1.0 / u_clouds_lightSteps;
    const float stepLodBias = log2(u_clouds_baseFrequency * CLOUD_BASE_RES);
    const float cut = odCut / u_clouds_extinction;
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
// Inside the shadow map: the map (one bilinear fetch). PAST it, cloudSunTransmittance fades to one constant mean for
// the whole sky (mix(1, 0.3, coverage)), so the air toward far cloud bases - 10-40 km of it at a low angle - was
// half-lit under a dense deck, and its blue Rayleigh light lay over the dark bases as a sheen. Here, past the map,
// the march estimates it from the WEATHER MAP instead (bound here, not in the other consumers): the coverage of the
// column where the sun ray from the point crosses the main layer's middle - a gap lights the air, dense weather
// shades it.
float cloudAirSunVis(vec3 rel, float camAlt, vec3 L)
{
#ifdef CLOUD_SHADOWS
    if (u_cloudsLive_shadowRendered < 0.5)
        return 1.0;
    const vec2 s = cloudShadowSample(rel + (u_viewPos - u_views[VIEW_CENTER].viewPos.xyz), false);
    float farT = 1.0;
    if (s.y < 1.0)
    {
        const float mid = u_clouds_mainBottom + 0.5 / u_clouds_mainInvHeight;
        const float tSun = max(mid - cloudAltitude(rel, camAlt), 0.0) / max(L.y, 0.05);
        const vec2 nxz = (rel + L * tSun).xz + cloudNoiseOffset();
        const float c = cloudColumnCoverage(u_clouds_coverage, textureLod(u_cloudWeather, nxz * u_clouds_invWeatherPeriod, 0.0).r);
        farT = 1.0 - smoothstep(0.35, 0.75, c);
    }
    const float T = mix(farT, exp(-s.x), s.y);
    return mix(1.0, T, u_clouds_shadowStrength);
#else
    return 1.0;
#endif
}

// Single-scattered atmosphere from a camera-relative origin to origin + dir * tEnd, per unit sun radiance (multiply
// by the sun colour), and its transmittance: the aerial perspective in front of a cloud, from any altitude, to a
// finite distance, in any direction. Each step's sun is CLOUD-SHADOWED (cloudAirSunVis): one visibility for the
// whole segment (the old mean of two taps) could not tell the shaded air under a deck from the lit air past it.
// 8 steps at the ray's own JITTER (per pixel and frame; the temporal pass averages them): with 4 fixed points over
// tens of km each pixel caught the lit air columns under the openings at one of the same 4 depths as every other
// pixel - the openings' shapes stamped onto the clouds beside them as blue blobs instead of soft shafts.
vec3 cloudAerialScatter(vec3 origin, float camAlt, vec3 dir, float tEnd, vec3 lightDir, float jitter, out vec3 transmittance)
{
    const int steps = 8;
    const vec3 ro = origin + vec3(0.0, camAlt + ATMOS_R_PLANET, 0.0); // planet-centred
    const float mu = dot(dir, lightDir);
    const float pR = phaseRayleigh(mu);
    const float pM = phaseHG(mu, u_sky_mieG);
    const AtmosRay ray = atmosRayBegin(ro, dir); // the origin's Chapman values once, not per step
    vec3 sumR = vec3(0.0), sumM = vec3(0.0);
    const float dt = tEnd / float(steps);
    float t = jitter * dt;
    for (int i = 0; i < steps; ++i)
    {
        const vec3 p = ro + dir * t;
        const float h = length(p) - ATMOS_R_PLANET;
        const vec2 dens = exp(-max(h, 0.0) / vec2(ATMOS_H_RAY, ATMOS_H_MIE)) * dt;
        const vec3 atten = exp(-atmosTau(atmosRayOD(ray, t) + atmosLightOpticalDepth(p, lightDir)))
                         * cloudAirSunVis(origin + dir * t, camAlt, lightDir);
        sumR += atten * dens.x;
        sumM += atten * dens.y;
        t += dt;
    }
    transmittance = exp(-atmosTau(atmosRayOD(ray, tEnd)));
    return (sumR * u_sky_betaRayleigh * pR + sumM * vec3(u_sky_betaMie) * pM) * u_sky_scatterBoost;
}

struct CloudMarchResult
{
    vec3 inScatter;   // incl. the aerial perspective in front of the cloud
    float transmittance;
    float front;      // distance of the first cloud sample (< 0 = no cloud)
    float weighted;   // transmittance-weighted cloud distance
    int steps;
};

// Marches from a camera-relative origin (this view's camera) along dir through the shell intervals seg0 /
// seg1 (cloudShellIntervals from that origin). jitter offsets every sample within its step; pixelAngle sets
// the mip level from the footprint. densityOnly = the debug view (white, unlit).
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
    const float ms = u_clouds_multiScatterAttenuation;
    // The multiple-scattering octaves in CLOSED FORM (per ray; per sample then 2 exp, no loop): the sum over ALL
    // isotropic octaves i >= 1 of a^i e^(-od a^i) (Wrenninge, a = b = "Multi-scatter"), as its first octave exactly
    // plus the whole geometric tail a^2 / (1 - a) through ONE effective extinction a^(1 + 1/(1 - a)) (the tail's
    // mean octave), x "Multi-scatter strength" (u_clouds_multiScatterStrength, non-physical above 1). With the sun BEHIND the
    // viewer the lit side is seen near 180 degrees, where the droplet phase is ~0, so its brightness is this term
    // alone: two octaves (the old fixed count, ~0.14 of the sunlight) left thick sunlit clouds gray; all of them
    // at a = 0.9 give ~0.7.
    const float msTailScale = ms * ms / max(1.0 - ms, 0.05);
    const float msTailExt = pow(ms, 1.0 + 1.0 / max(1.0 - ms, 0.05));
    const float msStrength = u_clouds_multiScatterStrength;
    // The sun march's early out: the slowest octave falls as exp(-od * msTailExt), so past od = 7 / msTailExt it
    // is under 0.1 % - and so is every faster term.
    const float odCut = 7.0 / max(min(msTailExt, ms), 0.02);
    // Octave 0 (single scattering) keeps the droplet phase: its narrow forward spike is the silver lining.
    // The multiple-scattering octaves are ISOTROPIC: after a few scattering events the direction is nearly
    // random, which is what keeps the side of a cloud away from the sun bright. A flattened copy of the
    // droplet phase (g x 0.66 = 0.66 at 20 um) stayed so forward that the anti-sun side went dark.
    const float isotropic = 1.0 / (4.0 * PI);
    const float phase0 = cloudPhase(mu, 1.0);

    // THE LIGHT COLOURS ARE APPLIED AFTER THE LOOP. A sample's in-scatter is linear in four per-ray colours -
    //   sunBottom * (sun * (1 - hf)) + sunTop * (sun * hf) + ambGround * exp(-h * k) + ambSky * hf
    // (sun = the phase-weighted, shadowed sun term, hf = the height in the shell) - so the loop only sums the
    // four scalar weights x the absorbed fraction: four scalars live across the loop instead of twelve colour
    // floats + the vec3 sum.
    // THE GROUND BOUNCE COMES FROM BELOW: it falls off EXPONENTIALLY with the metres above the main layer's base
    // (u_clouds_invGroundLightDepth = 1 / "Ground light depth"), so it lights the undersides and dies within that depth. (A
    // linear 1 - hf lit the whole cloud up to the shell top.) hf = the height within the sample's own layer.
    float sumSunLow = 0.0;    // sum(absorbed * sun * (1 - hf))
    float sumSunHigh = 0.0;   // sum(absorbed * sun * hf)
    float sumAmbHigh = 0.0;   // sum(absorbed * hf)
    float sumAmbGround = 0.0; // sum(absorbed * exp(-h * k)), h = metres above the main layer's base

    // The self-shadow comes from the shadow map where it covers the sample (one fetch instead of a sun march);
    // the map is relative to the CENTRE view's camera. CLOUD_SELF_SHADOW_MAP is baked; u_cloudsLive_shadowRendered =
    // the map was rendered this frame (the sun is up).
#ifdef CLOUD_SELF_SHADOW_MAP
    const bool mapShadow = u_cloudsLive_shadowRendered > 0.5;
#else
    const bool mapShadow = false;
#endif
    const vec3 toCentreView = u_viewPos - u_views[VIEW_CENTER].viewPos.xyz;

    // The mip levels are log2(distance) + a per-ray constant each: one log2 per step.
    const float lodBaseBias = log2(pixelAngle * u_clouds_baseFrequency * CLOUD_BASE_RES);
    const float lodDetailBias = log2(pixelAngle * u_clouds_detailFrequency * CLOUD_DETAIL_RES);

    // EMPTY-SPACE SKIPPING: in clear air the march takes COARSE steps (CLOUD_COARSE_MULT x the step) that
    // test only the cheap shape (weather + base, no detail). The detail only ERODES that shape, so a zero
    // there is a zero of the full density: the coarse test never misses a cloud the fine march would see,
    // it can only step over one thinner than a coarse step. On a hit the march backs up (the coarse step is
    // not taken) and marches finely; CLOUD_COARSE_AFTER empty fine steps in a row return it to coarse.
    const float CLOUD_COARSE_MULT = 3.0;
    const int CLOUD_COARSE_AFTER = 4;
    // EARLY OUT: the march ends at transmittance CLOUD_T_END (what lies behind adds under 2 %), and once the
    // ray is mostly opaque (CLOUD_T_THICK) the fine steps double - the samples there are weighted by T.
    const float CLOUD_T_END = 0.02;
    const float CLOUD_T_THICK = 0.3;
    // DETAIL FADE: the detail erosion fades out over the last 20 % of "Detail distance" (u_clouds_invDetailDistance =
    // 1 / that distance); past it the samples skip the detail fetches (their mips are nearly flat there).
    const float detailInvDist = u_clouds_invDetailDistance;

    // THE STEP SCHEDULE: dt = max(near step, g * t), the growth rate g PER RAY from the shell. Steps for
    // [tStart, tEnd] at rate g, with n = the near step:
    //   N(g) = max(0, n / g - tStart) / n + ln(tEnd / max(tStart, n / g)) / g
    // A FIXED g ("Step growth", before 2026-09-29) gave a ray crossing the shell from below ln(top / bottom) / g
    // steps at any angle - so the step count through the clouds followed the RATIO of the layer's top and bottom
    // (300-5000 m: ~280 steps; 1500-3000 m: ~70), and every top / bottom change needed a new growth. Now g is SOLVED
    // so every ray takes "Steps per ray" (u_clouds_stepsPerRay) steps over its shell span: the same quality through the
    // layer whatever its top and bottom, and the same cost per ray. Capped at ~75 % of the budget (the rest absorbs
    // the coarse back-ups; the sky map's 64-step rays). A fixed-point iteration on g = (the terms) / target - the log
    // moves slowly, so a few rounds settle.
    // THE NEAR STEP SCALES WITH THE SPAN: clamp(span / target, "Min step", "Near step"). A fixed near step gave a SHORT
    // span (up through a thin layer: 1000 m at 15 m = 67 steps) fewer steps than the target - lower quality exactly
    // where the path is short. Now such a span is marched in `target` uniform steps; a long span keeps "Near step" at
    // the camera. "Min step" (u_clouds_minStep) is the floor: steps finer than the noise detail only re-read the same
    // texels, so a very short span takes fewer steps instead of paying the full target for nothing.
    // (Before the per-ray solve a long flat ray overran the budget, and a uniform floor of (distance left / steps
    // left) made EVERY step long, inside the nearby cloud too: a grainy band at the camera's altitude.)
    // THE SOLVE SPAN IS THE MAIN LAYER'S (sStart, sEnd) while the upper layer is on: over the union shell the target
    // was spread across the gap and the upper band too, and the main layer got fewer steps whenever the upper layer
    // was on. With the growth fitted to the main band the upper layer - farther out - takes its steps ON TOP (at the
    // same growth: ~ln(upper top / upper bottom) / g more), and the gap is crossed with the coarse empty-space steps.
    // A ray that misses the main band (only the upper one) solves over the shell. tStart / tEnd stay the SHELL's
    // (the air term below).
    const float tStart = max(seg0.y > seg0.x ? seg0.x : seg1.x, 1.0);
    const float tEnd = max(seg1.y > seg1.x ? seg1.y : seg0.y, tStart + 1.0);
    float sStart = tStart, sEnd = tEnd;
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
    const float target = max(min(u_clouds_stepsPerRay, 0.75 * float(maxSteps)), 1.0);
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

    // THE AERIAL PERSPECTIVE'S DISTANCE: the air in front of the cloud is applied once per ray (after the loop),
    // but a ray holds cloud at many distances - flat through the layer, the fog around the camera AND clouds
    // tens of km out. At the transmittance-weighted distance the near cloud's light was dimmed by the far
    // cloud's air (a dark band at the camera's altitude). Instead the loop averages the air's transmittance
    // itself, exp(-kAir * t) weighted like the in-scatter, with kAir = the ray's mean air extinction (1/m,
    // channel mean); the haze is then taken at the distance with that transmittance.
    const float kAir = dot(atmosTau(atmosSegmentOD(origin + vec3(0.0, camAlt + ATMOS_R_PLANET, 0.0), dir, tEnd)), vec3(1.0 / 3.0)) / tEnd;
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
            // Fine near the origin, growing with distance at this ray's rate (above); the uniform floor is only
            // the safety net for a budget the back-ups used up.
            float dt = max(nearStep, t * growth);
            dt = max(dt, (seg.y - t) / float(max(maxSteps - r.steps, 1)));
            if (coarse)
                dt *= CLOUD_COARSE_MULT;
            else if (r.transmittance < CLOUD_T_THICK)
                dt *= 2.0;
            dt = min(dt, seg.y - t);
            const float ts = t + dt * jitter;
            ++r.steps;

            const vec3 rel = origin + dir * ts;
            const float alt = cloudAltitude(rel, camAlt);
            const vec2 nxz = rel.xz + noiseOffset;
            const float logDist = log2(ts);
            const float lodBase = max(logDist + lodBaseBias, 0.0);
            const float lodDetail = max(logDist + lodDetailBias, 0.0);
            if (coarse)
            {
                if (cloudDensity(nxz, alt, ts, 0.0, lodBase, lodDetail) > 0.0)
                {
                    coarse = false; // back up: this interval is marched again, finely
                    emptyRun = 0;
                }
                else
                    t += dt;
                continue;
            }
            t += dt;
            const float detailWeight = clamp(5.0 - 5.0 * ts * detailInvDist, 0.0, 1.0);
            const float dens = cloudDensity(nxz, alt, ts, detailWeight, lodBase, lodDetail);
            if (dens <= 0.0)
            {
                if (++emptyRun >= CLOUD_COARSE_AFTER)
                    coarse = true;
                continue;
            }
            emptyRun = 0;

            const float sigmaT = dens * u_clouds_extinction;
            // Energy-conserving step (Hillaire 2015): the in-scatter integrated over the step's own
            // extinction, so the result does not depend on the step length. Albedo 1.
            const float stepT = exp(-sigmaT * dt);
            const float absorbed = r.transmittance * (1.0 - stepT);
            if (!densityOnly)
            {
                const float hf = cloudLayerHeightFraction(alt); // within its own layer (clouds.inc.glsl)
                float od;
                const vec2 map = mapShadow ? cloudShadowSample(rel + toCentreView, false) : vec2(0.0);
                if (map.y >= 1.0)
                    od = map.x;
                else
                    od = mix(cloudLightOpticalDepth(rel, nxz, camAlt, alt, L, lodBase, lodDetail, detailWeight, odCut), map.x, map.y);
                // Multiple scattering: the closed-form octave sum (above the loop), isotropic.
                const float msSum = ms * exp(-od * ms) + msTailScale * exp(-od * msTailExt);
                const float sunTerm = phase0 * exp(-od) + isotropic * (msStrength * msSum);
#ifdef CLOUD_POWDER // baked: "Powder" above 0
                const float powder = mix(1.0, 1.0 - exp(-dens * 6.0), u_clouds_powder);
                const float sunWeight = absorbed * sunTerm * powder;
#else
                const float sunWeight = absorbed * sunTerm;
#endif
                sumSunHigh += sunWeight * hf;
                sumSunLow += sunWeight * (1.0 - hf);
                sumAmbHigh += absorbed * hf;
                sumAmbGround += absorbed * exp(-max(alt - u_clouds_mainBottom, 0.0) * u_clouds_invGroundLightDepth); // metres above the main layer's base
            }
            if (r.front < 0.0)
                r.front = ts;
            tSum += ts * absorbed;
            wSum += absorbed;
            airSum += absorbed * exp(-kAir * ts);
            r.transmittance *= stepT;
        }
    }
    // The early out leaves up to CLOUD_T_END of transmittance, and the composite passes scene x transmittance:
    // 2 % of the SUN DISC (thousands of times the sky) still shone through any cloud, however dense. Remapped
    // so the early-out level is fully opaque - continuous (no edge where a cloud just reaches it), and 1 stays 1.
    r.transmittance = clamp((r.transmittance - CLOUD_T_END) / (1.0 - CLOUD_T_END), 0.0, 1.0);
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

    // Sun radiance at the cloud: the atmosphere transmittance at the shell bottom and top, along the LOCAL
    // up at the first cloud (the planet curves: at 100 km the local sun elevation differs by ~1 degree, which
    // is the whole sunset on distant clouds).
    const vec3 frontRel = origin + dir * r.front;
    const vec3 localUp = normalize(vec3(frontRel.x, frontRel.y + camAlt + ATMOS_R_PLANET, frontRel.z));
    const vec3 sunColor = u_sunColor * u_sunVisible;
    const vec3 sunBottom = sunColor * atmosTransmittanceToLight(u_clouds_shellBottom, L, localUp);
    const vec3 sunTop = sunColor * atmosTransmittanceToLight(u_clouds_shellTop, L, localUp);

    // Ambient: the CLEAR sky hemisphere from the sky map (up + four at 30 degrees), weighted by the height in the
    // shell, and the ground bounce under the shell (albedo x (sun + sky) irradiance / PI), falling off from below.
    // The 5-direction mean is baked once per frame into the clear layer's texel (0, 0) (gi_sky_map.cs.glsl).
    const vec3 ambSky = texelFetch(u_skyMap, SKY_MAP_CLOUD_AMBIENT_TEXEL, 0).rgb * u_clouds_ambient;
    const vec3 ambGround = u_cloudsLive_groundBounceAlbedo * (sunColor * u_sunTransmittance * (max(L.y, 0.0) * INV_PI) + ambSky); // the sky's ground colour x the cloud albedo

    r.inScatter = sunBottom * sumSunLow + sunTop * sumSunHigh + ambGround * sumAmbGround + ambSky * sumAmbHigh;
    if (u_clouds_aerialStrength <= 0.0) // "Lighting/Aerial perspective strength" 0: the cloud as lit, no air in front
        return r;

    // Aerial perspective between the origin and the cloud: the cloud dims through the air, and the air in
    // front of it keeps the in-scatter the sky behind it had (the sky carries the whole ray's). Taken at the
    // distance whose air transmittance is the in-scatter-weighted mean (airSum, above); with almost no air
    // along the ray (up, from altitude) that distance is ill-conditioned and the weighted distance serves.
    const float airMean = airSum / wSum;
    const float tAir = kAir * tEnd > 1e-3 ? clamp(-log(max(airMean, 1e-6)) / kAir, 0.0, tEnd) : r.weighted;
    // The air in front of the cloud lies in the CLOUDS' SHADOW where the sun is behind them (under an overcast,
    // all of it): cloudAerialScatter shadows each of its steps (cloudAirSunVis). Unshadowed, its Mie forward peak
    // drew a sun glow on top of any cloud, however dense. (A ramp to the VIEW ray's transmittance within ~10 degrees
    // of the sun left the rest of the halo: a black hole in a ring.)
    vec3 airT;
    const vec3 air = cloudAerialScatter(origin, camAlt, dir, tAir, L, jitter, airT) * sunColor;
    // "Lighting/Aerial perspective strength" (u_clouds_aerialStrength) scales the added air light only; the cloud's
    // dimming through the air (airT) stays physical.
    r.inScatter = r.inScatter * airT + air * ((1.0 - r.transmittance) * u_clouds_aerialStrength);
    return r;
}

#endif
