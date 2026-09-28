// The cloud view-ray march, shared by the screen march (cloud_march.cs.glsl) and the sky-map clouds
// (cloud_sky.cs.glsl). Requires shared.inc.glsl, clouds.inc.glsl, cloud_shadow.inc.glsl and a
// `sampler2DArray u_skyMap` (the GI sky bake: its CLEAR layer is the ambient, so the clouds never light
// themselves through their own image in the sky map).

#ifndef CLOUD_RAYMARCH_INC_GLSL
#define CLOUD_RAYMARCH_INC_GLSL

// Optical depth toward the sun from a camera-relative point: quadratically growing steps out to the
// light distance. Only the first (short) step carries the detail noise: the longer ones integrate over
// the detail scale anyway, and each detail sample costs two or three more fetches (curl, detail, near).
float cloudLightOpticalDepth(vec3 rel, vec2 nxz, float camAlt, vec3 L, float lodBase, float lodDetail, float detailWeight)
{
    const int n = int(u_cloudMarch1.x);
    const float reach = u_cloudMarch1.y;
    const float invN = 1.0 / u_cloudMarch1.x;
    float od = 0.0;
    float tPrev = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float f = float(i + 1) * invN;
        const float t1 = reach * f * f;
        const float tm = 0.5 * (tPrev + t1);
        const vec3 p = rel + L * tm;
        const float alt = cloudAltitude(p, camAlt);
        if (alt > u_cloudShape0.y)
            break;
        od += cloudDensity(nxz + L.xz * tm, alt, 1e30, i == 0 ? detailWeight : 0.0, lodBase, lodDetail) * (t1 - tPrev);
        tPrev = t1;
    }
    return od * u_cloudShape1.w;
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
    const float ms = u_cloudLight1.w;
    // The multiple-scattering octaves in CLOSED FORM (per ray; per sample then 2 exp, no loop): the sum over ALL
    // isotropic octaves i >= 1 of a^i e^(-od a^i) (Wrenninge, a = b = "Multi-scatter"), as its first octave exactly
    // plus the whole geometric tail a^2 / (1 - a) through ONE effective extinction a^(1 + 1/(1 - a)) (the tail's
    // mean octave), x "Multi-scatter strength" (u_cloudLight2.w, non-physical above 1). With the sun BEHIND the
    // viewer the lit side is seen near 180 degrees, where the droplet phase is ~0, so its brightness is this term
    // alone: two octaves (the old fixed count, ~0.14 of the sunlight) left thick sunlit clouds gray; all of them
    // at a = 0.9 give ~0.7.
    const float msTailScale = ms * ms / max(1.0 - ms, 0.05);
    const float msTailExt = pow(ms, 1.0 + 1.0 / max(1.0 - ms, 0.05));
    const float msStrength = u_cloudLight2.w;
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
    // (u_cloudShape4.y = 1 / "Ground light depth"), so it lights the undersides and dies within that depth. (A
    // linear 1 - hf lit the whole cloud up to the shell top.) hf = the height within the sample's own layer.
    float sumSunLow = 0.0;    // sum(absorbed * sun * (1 - hf))
    float sumSunHigh = 0.0;   // sum(absorbed * sun * hf)
    float sumAmbHigh = 0.0;   // sum(absorbed * hf)
    float sumAmbGround = 0.0; // sum(absorbed * exp(-h * k)), h = metres above the main layer's base

    // The self-shadow comes from the shadow map where it covers the sample (one fetch instead of a sun march);
    // the map is relative to the CENTRE view's camera. CLOUD_SELF_SHADOW_MAP is baked; u_cloudShadow4.x =
    // the map was rendered this frame (the sun is up).
#ifdef CLOUD_SELF_SHADOW_MAP
    const bool mapShadow = u_cloudShadow4.x > 0.5;
#else
    const bool mapShadow = false;
#endif
    const vec3 toCentreView = u_viewPos - u_views[VIEW_CENTER].viewPos.xyz;

    // The mip levels are log2(distance) + a per-ray constant each: one log2 per step.
    const float lodBaseBias = log2(pixelAngle * u_cloudShape1.y * CLOUD_BASE_RES);
    const float lodDetailBias = log2(pixelAngle * u_cloudShape1.z * CLOUD_DETAIL_RES);

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
    // DETAIL FADE: the detail erosion fades out over the last 20 % of "Detail distance" (u_cloudMarch1.w =
    // 1 / that distance); past it the samples skip the detail fetches (their mips are nearly flat there).
    const float detailInvDist = u_cloudMarch1.w;

    // THE STEP SCHEDULE: dt = max(near step, g * t). With the "Step growth" g a long ray (flat through the
    // layer: up to the max distance) needs more steps than the budget, and a uniform floor of (distance left /
    // steps left) then made EVERY step long - 400 m from the camera on, inside the nearby cloud: one density
    // sample per step, opaque at once, a grainy band at the camera's altitude. Instead the growth rate rises
    // per ray until the schedule fits ~75 % of the budget (the rest absorbs the coarse back-ups): short steps
    // near the camera, faster growth far away. Steps for [tStart, tEnd] at rate g, with n = the near step:
    //   N(g) = max(0, n / g - tStart) / n + ln(tEnd / max(tStart, n / g)) / g
    // solved by a fixed-point iteration on g = (the same terms) / budget - the log moves slowly, so a few
    // rounds settle.
    const float nearStep = u_cloudMarch0.z;
    const float tStart = max(seg0.y > seg0.x ? seg0.x : seg1.x, 1.0);
    const float tEnd = max(seg1.y > seg1.x ? seg1.y : seg0.y, tStart + 1.0);
    const float budget = 0.75 * float(maxSteps);
    float growth = u_cloudMarch0.w;
    for (int i = 0; i < 4; ++i)
    {
        const float tLinear = nearStep / max(growth, 1e-6); // where g * t overtakes the near step
        const float needed = max(tLinear - tStart, 0.0) / nearStep + log(tEnd / max(tStart, tLinear)) / max(growth, 1e-6);
        if (needed <= budget)
            break;
        growth = max(growth, (max(1.0 - tStart / tLinear, 0.0) + log(tEnd / max(tStart, tLinear))) / budget);
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

            const float sigmaT = dens * u_cloudShape1.w;
            // Energy-conserving step (Hillaire 2015): the in-scatter integrated over the step's own
            // extinction, so the result does not depend on the step length. Albedo 1.
            const float stepT = exp(-sigmaT * dt);
            const float absorbed = r.transmittance * (1.0 - stepT);
            if (!densityOnly)
            {
                const float hf = cloudLayerHeightFraction(alt); // within its own layer (clouds.inc.glsl)
                float od;
                const vec2 map = mapShadow ? cloudShadowSample(rel + toCentreView) : vec2(0.0);
                if (map.y >= 1.0)
                    od = map.x;
                else
                    od = mix(cloudLightOpticalDepth(rel, nxz, camAlt, L, lodBase, lodDetail, detailWeight), map.x, map.y);
                // Multiple scattering: the closed-form octave sum (above the loop), isotropic.
                const float msSum = ms * exp(-od * ms) + msTailScale * exp(-od * msTailExt);
                const float sunTerm = phase0 * exp(-od) + isotropic * (msStrength * msSum);
#ifdef CLOUD_POWDER // baked: "Powder" above 0
                const float powder = mix(1.0, 1.0 - exp(-dens * 6.0), u_cloudLight1.z);
                const float sunWeight = absorbed * sunTerm * powder;
#else
                const float sunWeight = absorbed * sunTerm;
#endif
                sumSunHigh += sunWeight * hf;
                sumSunLow += sunWeight * (1.0 - hf);
                sumAmbHigh += absorbed * hf;
                sumAmbGround += absorbed * exp(-max(alt - u_cloudLayer0.x, 0.0) * u_cloudShape4.y); // metres above the main layer's base
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
    const vec3 sunColor = u_sunColor * u_eclipseParams.x;
    const vec3 sunBottom = sunColor * atmosTransmittanceToLight(u_cloudShape0.x, L, localUp);
    const vec3 sunTop = sunColor * atmosTransmittanceToLight(u_cloudShape0.y, L, localUp);

    // Ambient: the CLEAR sky hemisphere from the sky map (up + four at 30 degrees), weighted by the height in the
    // shell, and the ground bounce under the shell (albedo x (sun + sky) irradiance / PI), falling off from below.
    vec3 ambSky = textureLod(u_skyMap, vec3(skyMapUV(vec3(0.0, 1.0, 0.0)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(0.866, 0.5, 0.0)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(-0.866, 0.5, 0.0)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(0.0, 0.5, 0.866)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(0.0, 0.5, -0.866)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky *= 0.2 * u_cloudLight1.x;
    const vec3 ambGround = u_cloudLight2.rgb * (sunColor * u_sunTransmittance * (max(L.y, 0.0) * INV_PI) + ambSky); // the sky's ground colour x the cloud albedo

    r.inScatter = sunBottom * sumSunLow + sunTop * sumSunHigh + ambGround * sumAmbGround + ambSky * sumAmbHigh;

    // Aerial perspective between the origin and the cloud: the cloud dims through the air, and the air in
    // front of it keeps the in-scatter the sky behind it had (the sky carries the whole ray's). Taken at the
    // distance whose air transmittance is the in-scatter-weighted mean (airSum, above); with almost no air
    // along the ray (up, from altitude) that distance is ill-conditioned and the weighted distance serves.
    const float airMean = airSum / wSum;
    const float tAir = kAir * tEnd > 1e-3 ? clamp(-log(max(airMean, 1e-6)) / kAir, 0.0, tEnd) : r.weighted;
    vec3 airT;
    const vec3 roPlanet = origin + vec3(0.0, camAlt + ATMOS_R_PLANET, 0.0); // re-derived, not held across the loop
    const vec3 air = cloudAerialScatter(roPlanet, dir, tAir, L, airT) * sunColor;
    // The air in front of the cloud lies in the CLOUDS' SHADOW where the sun is behind them (under an overcast,
    // all of it): its sun term takes the cloud shadow map's sun transmittance, averaged at 1/4 and 3/4 of the air
    // segment. Unshadowed, its Mie forward peak drew a sun glow on top of any cloud, however dense. (A ramp to the
    // VIEW ray's transmittance within ~10 degrees of the sun left the rest of the halo: a black hole in a ring.)
    const vec3 airWorld = u_viewPos + origin;
    const float airSunVis = 0.5 * (cloudSunTransmittance(airWorld + dir * (0.25 * tAir))
                                 + cloudSunTransmittance(airWorld + dir * (0.75 * tAir)));
    r.inScatter = r.inScatter * airT + air * ((1.0 - r.transmittance) * airSunVis);
    return r;
}

#endif
