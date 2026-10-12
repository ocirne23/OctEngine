// Shared single-scattering Rayleigh + Mie atmosphere. The coefficients come from the UBO (u_sky_*), so the visible sky
// (sky.fs.glsl) and all indirect sky light (GI miss rays, fog ambient, the surface fallback) use the same settings, and
// the in-scatter is sourced by the same u_sunColor as direct lighting: no extra intensity multipliers in this path.
// Requires ubo.inc.glsl and PI (include via shared.inc.glsl).

#ifndef ATMOSPHERE_INC_GLSL
#define ATMOSPHERE_INC_GLSL

// ATMOS_R_PLANET and ATMOS_BETA_OZONE are MIRRORED on the CPU (RendererUbo.cpp, the ground sun transmittance in
// buildUboSky): keep in sync.
const float ATMOS_R_PLANET = 6371e3;
const float ATMOS_R_ATMOS  = 6451e3;
const float ATMOS_OBSERVE_HEIGHT = 2.0;

// Tweakable shape (u_sky, Sky/Atmosphere).
#define ATMOS_H_RAY   u_sky_rayleighHeight // Rayleigh scale height (m)
#define ATMOS_H_MIE   u_sky_mieHeight      // Mie scale height (m)
#define ATMOS_MIE_EXT u_sky_mieExtinction  // Mie extinction/scattering ratio (absorption)
#define ATMOS_OZONE   u_sky_ozone          // ozone absorption strength (1 = Earth-like)

// Ozone absorption (Bruneton coefficients, 1/m), proportional to the Rayleigh density, absorption only. It absorbs
// green/yellow most, which removes the vivid green horizon band single scattering otherwise makes (blue-heavy Rayleigh
// source x blue-removing extinction peaks in green at mid optical depths).
const vec3 ATMOS_BETA_OZONE = vec3(0.650e-6, 1.881e-6, 0.085e-6);

vec3 atmosTau(vec2 od) // total per-channel optical thickness from (Rayleigh, Mie) optical depths
{
	return u_sky_betaRayleigh * od.x + vec3(u_sky_betaMie * ATMOS_MIE_EXT) * od.y + ATMOS_BETA_OZONE * (ATMOS_OZONE * od.x);
}

// Near/far intersection distances of a ray with a sphere of radius r centered at the origin.
vec2 atmosRaySphere(vec3 ro, vec3 rd, float r)
{
	float b = dot(ro, rd);
	float c = dot(ro, ro) - r * r;
	float d = b * b - c;
	if (d < 0.0) return vec2(1e20, -1e20);
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}

float phaseRayleigh(float mu) { return 3.0 / (16.0 * PI) * (1.0 + mu * mu); }

// Henyey-Greenstein: the Mie lobe and the sun halo.
float phaseHG(float mu, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

// Chapman grazing-incidence approximation (Schüler): closed-form optical depth to space through an exponential
// atmosphere. Exact at the zenith, ~airmass 38 at the horizon, handles the below-horizon case.
float atmosChapman(float X, float cosChi) // X = (planet radius + height) / scale height
{
	float c = sqrt(1.5707963 * X); // sqrt(pi/2 * X)
	if (cosChi >= 0.0)
		return c / ((c - 1.0) * cosChi + 1.0);

	float sinChi = sqrt(max(1.0 - cosChi * cosChi, 1e-6));
	float X0 = X * sinChi;
	return 2.0 * sqrt(1.5707963 * X0) * exp(min(X - X0, 60.0)) - c / ((c - 1.0) * (-cosChi) + 1.0);
}

// (Rayleigh, Mie) optical depth from pos (planet-centered) to the atmosphere edge toward a light.
vec2 atmosLightOpticalDepth(vec3 pos, vec3 lightDir)
{
	float r = length(pos);
	float h = max(r - ATMOS_R_PLANET, 0.0);
	float cosChi = dot(pos, lightDir) / r;
	float odR = exp(-h / ATMOS_H_RAY) * ATMOS_H_RAY * atmosChapman(r / ATMOS_H_RAY, cosChi);
	float odM = exp(-h / ATMOS_H_MIE) * ATMOS_H_MIE * atmosChapman(r / ATMOS_H_MIE, cosChi);
	return vec2(odR, odM);
}

// Transmittance from a point at `height` above the surface toward a light: white at midday, warm at the horizon, ~0
// below it.
vec3 atmosTransmittanceToLight(float height, vec3 lightDir, vec3 up)
{
	return exp(-atmosTau(atmosLightOpticalDepth(up * (ATMOS_R_PLANET + height), lightDir)));
}

// (Rayleigh, Mie) optical depth between a planet-centred ro and ro + dir * tEnd, as a difference of two Chapman
// evaluations taken in the UPWARD direction of each half. A downward ray's "to space" depth goes through the planet
// (up to ~e^60), and a difference of two of those is float noise: a descending ray is measured backwards, and one that
// dips and rises again is split at its lowest point.
// AtmosRay is the per-ray part, for a march that asks many distances along one ray: the Chapman values at the origin
// and the lowest point are constant, so each distance costs ONE evaluation.
struct AtmosRay
{
	vec3 ro;
	vec3 dir;
	float tLow;     // the ray's lowest point (<= 0: rising all the way)
	vec2 odRoFwd;   // rising: origin -> space along dir
	vec2 odRoBack;  // descending: origin -> space along -dir
	vec2 odLowPart; // descending: origin -> lowest point
	vec2 odLowFwd;  // descending: lowest point -> space along dir
};

AtmosRay atmosRayBegin(vec3 ro, vec3 dir)
{
	AtmosRay r;
	r.ro = ro;
	r.dir = dir;
	r.tLow = -dot(ro, dir);
	r.odRoFwd = vec2(0.0);
	r.odRoBack = vec2(0.0);
	r.odLowPart = vec2(0.0);
	r.odLowFwd = vec2(0.0);

	if (r.tLow <= 0.0)
		r.odRoFwd = atmosLightOpticalDepth(ro, dir);
	else
	{
		r.odRoBack = atmosLightOpticalDepth(ro, -dir);
		const vec3 m = ro + dir * r.tLow;
		r.odLowPart = max(atmosLightOpticalDepth(m, -dir) - r.odRoBack, vec2(0.0));
		r.odLowFwd = atmosLightOpticalDepth(m, dir);
	}
	return r;
}

// (Rayleigh, Mie) optical depth from the ray's origin to distance t (atmosSegmentOD's three cases).
vec2 atmosRayOD(AtmosRay r, float t)
{
	const vec3 p = r.ro + r.dir * t;
	if (r.tLow <= 0.0) // rising all the way
		return max(r.odRoFwd - atmosLightOpticalDepth(p, r.dir), vec2(0.0));
	if (t <= r.tLow) // still descending: measured from p back up to the origin
		return max(atmosLightOpticalDepth(p, -r.dir) - r.odRoBack, vec2(0.0));
	return r.odLowPart + max(r.odLowFwd - atmosLightOpticalDepth(p, r.dir), vec2(0.0));
}

vec2 atmosSegmentOD(vec3 ro, vec3 dir, float tEnd)
{
	return atmosRayOD(atmosRayBegin(ro, dir), tEnd);
}

// In-scattered radiance along dir for one directional light of unit radiance (the caller multiplies the light colour),
// plus the view ray's total transmittance (for the sun disc / stars), for an observer `observerHeight` metres above the
// ground (the visible sky passes the camera altitude: the sky darkens and the horizon dips). The ray ends at the
// atmosphere edge or the ground. The sky uses 12 steps.
vec3 atmosphereScatter(vec3 dir, vec3 lightDir, vec3 up, int steps, float observerHeight, out vec3 transmittance)
{
	vec3 ro = up * (ATMOS_R_PLANET + observerHeight);
	float tFar = max(atmosRaySphere(ro, dir, ATMOS_R_ATMOS).y, 0.0);
	const vec2 tGround = atmosRaySphere(ro, dir, ATMOS_R_PLANET);
	if (tGround.x > 0.0 && tGround.x < tGround.y)
		tFar = min(tFar, tGround.x);

	float mu = dot(dir, lightDir);
	float pR = phaseRayleigh(mu);
	float pM = phaseHG(mu, u_sky_mieG);

	// View optical depth per sample in closed form (atmosRayOD), exact for an exponential atmosphere: an accumulated one
	// diverges per channel at grazing angles (rainbow / green hues), worse with fewer steps.
	const AtmosRay ray = atmosRayBegin(ro, dir);
	vec3 sumR = vec3(0.0), sumM = vec3(0.0);
	float dt = tFar / float(steps);
	float t = 0.5 * dt;
	for (int i = 0; i < steps; ++i)
	{
		vec3 p = ro + dir * t;
		float h = length(p) - ATMOS_R_PLANET;
		vec2 dens = exp(-max(h, 0.0) / vec2(ATMOS_H_RAY, ATMOS_H_MIE)) * dt;
		vec2 odView = atmosRayOD(ray, t);
		vec2 odSun = atmosLightOpticalDepth(p, lightDir);
		vec3 atten = exp(-atmosTau(odView + odSun));
		sumR += atten * dens.x;
		sumM += atten * dens.y;
		t += dt;
	}

	transmittance = exp(-atmosTau(atmosRayOD(ray, tFar)));
	// u_sky_scatterBoost scales the in-scatter without the transmittance; here, so every consumer scales the same.
	return (sumR * u_sky_betaRayleigh * pR + sumM * vec3(u_sky_betaMie) * pM) * u_sky_scatterBoost;
}

// Low-step scatter for the indirect paths, from the ground observer (ATMOS_OBSERVE_HEIGHT), the ray to the atmosphere
// edge only. The view optical depth is closed form as in atmosphereScatter, so the quadrature error only touches the
// smooth source term and a handful of samples stays hue-stable.
vec3 atmosphereScatterCheap(vec3 dir, vec3 lightDir, vec3 up, int steps)
{
	vec3 ro = up * (ATMOS_R_PLANET + ATMOS_OBSERVE_HEIGHT);
	float tFar = max(atmosRaySphere(ro, dir, ATMOS_R_ATMOS).y, 0.0);

	float mu = dot(dir, lightDir);
	float pR = phaseRayleigh(mu);
	float pM = phaseHG(mu, u_sky_mieG);

	vec2 odViewFull = atmosLightOpticalDepth(ro, dir); // ro -> space along the view ray, closed form
	vec3 sumR = vec3(0.0), sumM = vec3(0.0);
	float dt = tFar / float(steps);
	float t = 0.5 * dt;
	for (int i = 0; i < steps; ++i)
	{
		vec3 p = ro + dir * t;
		float h = length(p) - ATMOS_R_PLANET;
		vec2 dens = exp(-max(h, 0.0) / vec2(ATMOS_H_RAY, ATMOS_H_MIE)) * dt;
		vec2 odView = max(odViewFull - atmosLightOpticalDepth(p, dir), vec2(0.0)); // ro -> p, exact
		vec2 odSun = atmosLightOpticalDepth(p, lightDir);
		vec3 atten = exp(-atmosTau(odView + odSun));
		sumR += atten * dens.x;
		sumM += atten * dens.y;
		t += dt;
	}

	return (sumR * u_sky_betaRayleigh * pR + sumM * vec3(u_sky_betaMie) * pM) * u_sky_scatterBoost;
}

// The below-horizon ground's direct sun, albedo / PI * E_sun (constant over directions). The GI trace counts it as SUN
// light on its downward misses, so the lookup's cloud dimming reaches it (GI_SUN_V4 in gi_probe.inc.glsl). up is always
// u_skyUp, so the whole term is the CPU's per-frame u_sky_groundSun; up is unused (kept for the callers).
vec3 skyGroundSun(vec3 up)
{
	return u_sky_groundSun;
}

// Sky radiance for indirect lighting (GI miss rays, fog ambient, surface fallback): a low-step version of the sky's
// integral, sourced by the sun plus the sky-radiance light (moonlight / space light, along u_skyUp). Below the horizon
// a Lambertian ground stands in: a grazing sky sample x the ground albedo, PLUS the direct sun (skyGroundSun), the
// dominant daytime term - without it downward GI rays sit well below the sunlit ground the probes see.
vec3 skyRadiance(vec3 dir)
{
	const vec3 up = u_skyUp;
	vec3 groundAtten = vec3(1.0);
	vec3 groundSun = vec3(0.0);
	float cosUp = dot(dir, up);
	if (cosUp < 0.0)
	{
		dir = normalize(dir - up * (cosUp - 0.02));
		groundAtten = u_sky_groundAlbedo;
		groundSun = skyGroundSun(up);
	}

	vec3 radiance = atmosphereScatterCheap(dir, u_sunDirection.xyz, up, 4) * u_sunColor.rgb * u_sunVisible;
	if (dot(u_sky_radiance, u_sky_radiance) > 0.0)
		radiance += atmosphereScatterCheap(dir, up, up, 2) * u_sky_radiance;
	return radiance * groundAtten + groundSun;
}

// Sky for a MIRROR ray (ocean, terrain wet film): matched to sky.fs.glsl (12 view steps + the eclipse saturation
// curve); skyRadiance() under-samples the long grazing paths reflections look along. No sun disc (the GGX glint is its
// reflection), no ground term.
vec3 mirrorSkyRadiance(vec3 dir)
{
	const vec3 up = u_skyUp;
	const float eclipse = u_sunVisible;
	const vec3 luminosity = vec3(0.2126, 0.7152, 0.0722);
	vec3 color = atmosphereScatterCheap(dir, u_sunDirection.xyz, up, 12) * u_sunColor.rgb;
	color = mix(vec3(dot(color, luminosity)), color, 2.0 * (2.0 - eclipse)) * eclipse;
	if (dot(u_sky_radiance, u_sky_radiance) > 0.0)
		color += atmosphereScatterCheap(dir, up, up, 4) * u_sky_radiance;
	return color;
}

// THE SKY MAP (gi_sky_map.cs.glsl, GIProbePipeline): both functions above baked once per frame into a lat-long layer
// array: SKY_MAP_LAYER_GI = skyRadiance, SKY_MAP_LAYER_MIRROR = mirrorSkyRadiance, both with the volumetric clouds.
// Consumers declare `sampler2DArray u_skyMap` on their own binding and sample textureLod(u_skyMap, vec3(skyMapUV(dir),
// layer), 0.0). World +Y pole: u = atan(d.x, d.z) / 2pi + 0.5 (the sampler repeats U), v = acos(d.y) / pi (clamped V).
// Layer 2 holds per-frame CONSTANTS, one texel each, written by the bake's invocation (0, 0, 0) (texelFetch them):
//   SKY_MAP_CLOUD_AMBIENT_TEXEL - skyRadiance WITHOUT clouds, the zenith + 4 at 30 degrees, averaged: the clouds' own
//                                 ambient, so they never light themselves through their image
//   SKY_MAP_TREE_SKY_TEXEL      - the GI layer (WITH clouds), cosine-weighted over the zenith (0.4) + 4 at 45 degrees
//                                 (0.15 each): the far trees' sky light
//   SKY_MAP_GI_ZENITH_TEXEL     - the GI layer along u_skyUp: the GI-off sky ambient (terrain, ocean, wet film)
// THE GI LAYER IS LOW-RES: only the SKY_MAP_GI_WIDTH x SKY_MAP_GI_HEIGHT corner of layer 0 holds it, likewise of the
// sky clouds' GI layer (cloud_sky.cs.glsl layer 1); its readers reduce it to L1 or the constants above. Read it with
// skyMapGISample, never with textureLod / skyMapUV (the corner wraps in U by hand).
#define SKY_MAP_LAYER_GI     0.0
#define SKY_MAP_LAYER_MIRROR 1.0
#define SKY_MAP_LAYER_CLEAR  2.0
// Layer 3: the CLEAR mirror sky (no clouds), the bake's own cache. The atmosphere changes only with the sun and the
// sky tweaks, so each frame re-marches ONE texel of every 2x2 block and composites the clouds over the cached rest
// (alpha 0 = not baked yet: marched at once).
#define SKY_MAP_LAYER_MIRROR_CACHE 3
#define SKY_MAP_CLOUD_AMBIENT_TEXEL ivec3(0, 0, 2)
#define SKY_MAP_TREE_SKY_TEXEL      ivec3(1, 0, 2)
#define SKY_MAP_GI_ZENITH_TEXEL     ivec3(2, 0, 2)
#define SKY_MAP_GI_WIDTH  64
#define SKY_MAP_GI_HEIGHT 32

vec2 skyMapUV(vec3 d)
{
	return vec2(atan(d.x, d.z) * (0.5 / PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) * (1.0 / PI));
}

vec3 skyMapDir(vec2 uv) // texel centre uv -> direction (the bake's inverse of skyMapUV)
{
	const float phi = (uv.x - 0.5) * 2.0 * PI;
	const float theta = uv.y * PI;
	const float st = sin(theta);
	return vec3(st * sin(phi), cos(theta), st * cos(phi));
}

// Bilinear read of a low-res GI corner: four texelFetches, U wrapped inside the corner (the sampler's REPEAT wraps the
// whole image), V clamped.
vec4 skyMapGISample(sampler2DArray tex, vec3 dir, int layer)
{
	const vec2 st = skyMapUV(dir) * vec2(SKY_MAP_GI_WIDTH, SKY_MAP_GI_HEIGHT) - 0.5;
	const ivec2 i0 = ivec2(floor(st));
	const vec2 f = st - vec2(i0);
	const int x0 = (i0.x % SKY_MAP_GI_WIDTH + SKY_MAP_GI_WIDTH) % SKY_MAP_GI_WIDTH;
	const int x1 = (x0 + 1) % SKY_MAP_GI_WIDTH;
	const int y0 = clamp(i0.y, 0, SKY_MAP_GI_HEIGHT - 1);
	const int y1 = clamp(i0.y + 1, 0, SKY_MAP_GI_HEIGHT - 1);
	return mix(mix(texelFetch(tex, ivec3(x0, y0, layer), 0), texelFetch(tex, ivec3(x1, y0, layer), 0), f.x),
	           mix(texelFetch(tex, ivec3(x0, y1, layer), 0), texelFetch(tex, ivec3(x1, y1, layer), 0), f.x), f.y);
}

// The SURFACE out-of-field fallback is the virtual sky probe: gi_probe_trace.cs.glsl projects skyRadiance into an SH-L1
// slot (GI_SKY_SH_BASE, giEvalSkySH) evaluated like a real probe, so the handover at the probe field's edge matches.

#endif
