// The cloud view-ray march, shared by the screen march (cloud_march.cs.glsl) and the sky-map clouds
// (cloud_sky.cs.glsl). Requires shared.inc.glsl, clouds.inc.glsl, cloud_shadow.inc.glsl and a
// `sampler2DArray u_skyMap` (the GI sky bake: its CLEAR layer is the ambient, so the clouds never light
// themselves through their own image in the sky map).

#ifndef CLOUD_RAYMARCH_INC_GLSL
#define CLOUD_RAYMARCH_INC_GLSL

// Optical depth toward the sun from a camera-relative point: quadratically growing steps out to the
// light distance. Only the first (short) step carries the detail noise: the longer ones integrate over
// the detail scale anyway, and each detail sample costs two or three more fetches (curl, detail, near).
float cloudLightOpticalDepth(vec3 rel, vec2 nxz, float camAlt, vec3 L, float lodBase, float lodDetail)
{
    const int n = int(u_cloudMarch1.x);
    const float reach = u_cloudMarch1.y;
    float od = 0.0;
    float tPrev = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float f = float(i + 1) / float(n);
        const float t1 = reach * f * f;
        const float tm = 0.5 * (tPrev + t1);
        const vec3 p = rel + L * tm;
        const float alt = cloudAltitude(p, camAlt);
        if (alt > u_cloudShape0.y)
            break;
        od += cloudDensity(nxz + L.xz * tm, alt, 1e30, i == 0, lodBase, lodDetail) * (t1 - tPrev);
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
    // Octave 0 (single scattering) keeps the droplet phase: its narrow forward spike is the silver lining.
    // The multiple-scattering octaves are ISOTROPIC: after a few scattering events the direction is nearly
    // random, which is what keeps the side of a cloud away from the sun bright. A flattened copy of the
    // droplet phase (g x 0.66 = 0.66 at 20 um) stayed so forward that the anti-sun side went dark.
    const float isotropic = 1.0 / (4.0 * PI);
    const vec3 phase = vec3(cloudPhase(mu, 1.0), isotropic, isotropic);

    // Sun radiance at the cloud: the atmosphere transmittance at the shell bottom and top, along the LOCAL
    // up where the ray enters (the planet curves: at 100 km the local sun elevation differs by ~1 degree,
    // which is the whole sunset on distant clouds).
    const float tEntry = seg0.y > seg0.x ? seg0.x : seg1.x;
    const vec3 entryRel = origin + dir * tEntry;
    const vec3 localUp = normalize(vec3(entryRel.x, entryRel.y + camAlt + ATMOS_R_PLANET, entryRel.z));
    const vec3 sunColor = u_sunColor * u_eclipseParams.x;
    const vec3 sunBottom = sunColor * atmosTransmittanceToLight(u_cloudShape0.x, L, localUp);
    const vec3 sunTop = sunColor * atmosTransmittanceToLight(u_cloudShape0.y, L, localUp);

    // Ambient: the CLEAR sky hemisphere from the sky map (up + four at 30 degrees), and the ground bounce
    // under the shell (albedo x (sun + sky) irradiance / PI). Blended by the height in the shell.
    vec3 ambSky = textureLod(u_skyMap, vec3(skyMapUV(vec3(0.0, 1.0, 0.0)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(0.866, 0.5, 0.0)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(-0.866, 0.5, 0.0)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(0.0, 0.5, 0.866)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky += textureLod(u_skyMap, vec3(skyMapUV(vec3(0.0, 0.5, -0.866)), SKY_MAP_LAYER_CLEAR), 0.0).rgb;
    ambSky *= 0.2 * u_cloudLight1.x;
    const vec3 ambGround = u_cloudLight1.y * (sunColor * u_sunTransmittance * (max(L.y, 0.0) * INV_PI) + ambSky);

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
    float tSum = 0.0;
    float wSum = 0.0;
    for (int s = 0; s < 2; ++s)
    {
        const vec2 seg = s == 0 ? seg0 : seg1;
        float t = seg.x;
        bool coarse = true;
        int emptyRun = 0;
        while (t < seg.y && r.steps < maxSteps && r.transmittance > 0.005)
        {
            // Fine near the origin, growing with distance - but never so fine that the budget cannot reach
            // the end of the segment.
            float dt = max(u_cloudMarch0.z, t * u_cloudMarch0.w);
            dt = max(dt, (seg.y - t) / float(max(maxSteps - r.steps, 1)));
            if (coarse)
                dt *= CLOUD_COARSE_MULT;
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
                if (cloudDensity(nxz, alt, ts, false, lodBase, lodDetail) > 0.0)
                {
                    coarse = false; // back up: this interval is marched again, finely
                    emptyRun = 0;
                }
                else
                    t += dt;
                continue;
            }
            t += dt;
            const float dens = cloudDensity(nxz, alt, ts, true, lodBase, lodDetail);
            if (dens <= 0.0)
            {
                if (++emptyRun >= CLOUD_COARSE_AFTER)
                    coarse = true;
                continue;
            }
            emptyRun = 0;

            const float sigmaT = dens * u_cloudShape1.w;
            const float hf = clamp(cloudHeightFraction(alt), 0.0, 1.0);
            vec3 inScatter = vec3(1.0);
            if (!densityOnly)
            {
                float od;
                const vec2 map = mapShadow ? cloudShadowSample(rel + toCentreView) : vec2(0.0);
                if (map.y >= 1.0)
                    od = map.x;
                else
                    od = mix(cloudLightOpticalDepth(rel, nxz, camAlt, L, lodBase, lodDetail), map.x, map.y);
                // Multiple scattering (Wrenninge): octave i scatters a^i as much, through b^i of the
                // optical depth; a = b = the "Multi-scatter" tweak, the octaves' phase isotropic (above).
                const float sunTerm = phase.x * exp(-od) + ms * phase.y * exp(-od * ms) + ms * ms * phase.z * exp(-od * ms * ms);
                const float powder = mix(1.0, 1.0 - exp(-dens * 6.0), u_cloudLight1.z);
                inScatter = mix(sunBottom, sunTop, hf) * (sunTerm * powder) + mix(ambGround, ambSky, hf);
            }

            // Energy-conserving step (Hillaire 2015): the in-scatter integrated over the step's own
            // extinction, so the result does not depend on the step length. Albedo 1.
            const float stepT = exp(-sigmaT * dt);
            const float absorbed = r.transmittance * (1.0 - stepT);
            r.inScatter += inScatter * absorbed;
            if (r.front < 0.0)
                r.front = ts;
            tSum += ts * absorbed;
            wSum += absorbed;
            r.transmittance *= stepT;
        }
    }
    if (wSum <= 1e-5)
    {
        r.front = -1.0;
        return r;
    }
    r.weighted = tSum / wSum;

    // Aerial perspective between the origin and the cloud: the cloud dims through the air, and the air in
    // front of it keeps the in-scatter the sky behind it had (the sky carries the whole ray's).
    if (!densityOnly)
    {
        vec3 airT;
        const vec3 ro = origin + vec3(0.0, camAlt + ATMOS_R_PLANET, 0.0); // planet-centred
        const vec3 air = cloudAerialScatter(ro, dir, r.weighted, L, airT) * sunColor;
        r.inScatter = r.inScatter * airT + air * (1.0 - r.transmittance);
    }
    return r;
}

#endif
