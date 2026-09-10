// Shared single-scattering Rayleigh + Mie atmosphere. Earth-ish geometry constants; the scattering
// coefficients come from the UBO (u_betaRayleigh / u_betaMie), so the visible sky (sky.fs.glsl) and all
// indirect sky lighting (GI probe miss rays, fog ambient, the surface fallback outside the probe volume)
// are driven by the same physical settings: sun color * intensity, atmosphere lighting (moonlight), and
// the Rayleigh/Mie coefficients. There are no extra intensity multipliers anywhere in this path: the
// in-scatter integral is sourced by the same u_sunColor that drives direct surface lighting.
//
// Requires ubo.inc.glsl and PI (include via shared.inc.glsl).

#ifndef ATMOSPHERE_INC_GLSL
#define ATMOSPHERE_INC_GLSL

const float ATMOS_R_PLANET = 6371e3;
const float ATMOS_R_ATMOS  = 6451e3;
const float ATMOS_OBSERVE_HEIGHT = 2.0;
// Tweakable atmosphere shape (u_atmosParams, Sky/Atmosphere in the TweakPanel).
#define ATMOS_H_RAY   u_atmosParams.x // Rayleigh scale height (m)
#define ATMOS_H_MIE   u_atmosParams.y // Mie scale height (m)
#define ATMOS_MIE_EXT u_atmosParams.z // Mie extinction/scattering ratio (absorption)
#define ATMOS_OZONE   u_atmosParams.w // ozone absorption strength (1 = Earth-like)

// Ozone absorption (Bruneton-style coefficients, 1/m). Absorbs green/yellow strongest — this is what
// kills the vivid green band single scattering otherwise produces at the horizon (the blue-heavy
// Rayleigh source times blue-killing extinction peaks in green at mid optical depths; on Earth the
// ozone layer absorbs exactly that, leaving the familiar orange/red horizon and blue twilight).
// Approximated as proportional to the Rayleigh density (absorption only, no scattering).
const vec3 ATMOS_BETA_OZONE = vec3(0.650e-6, 1.881e-6, 0.085e-6);

vec3 atmosTau(vec2 od) // total per-channel optical thickness from (Rayleigh, Mie) optical depths
{
	return u_betaRayleigh * od.x + vec3(u_betaMie * ATMOS_MIE_EXT) * od.y
	     + ATMOS_BETA_OZONE * (ATMOS_OZONE * od.x);
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
// Henyey-Greenstein, used both for the Mie scattering lobe and the sun halo.
float phaseHG(float mu, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

// Chapman grazing-incidence approximation (Schüler): the closed form of the optical-depth integral
// along a ray to space through an exponential atmosphere. Exact at the zenith, ~airmass 38 at the
// horizon, and handles the below-horizon case.
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

// Transmittance from a point at `height` above the surface toward a light (sun/moon). Used to tint
// direct light at cloud height: white at midday, warm at the horizon, ~0 below it.
vec3 atmosTransmittanceToLight(float height, vec3 lightDir, vec3 up)
{
	return exp(-atmosTau(atmosLightOpticalDepth(up * (ATMOS_R_PLANET + height), lightDir)));
}

// In-scattered radiance along dir for one directional light of unit radiance (multiply by the light
// color outside), plus the view ray's total transmittance (attenuates the sun disc / stars behind the
// atmosphere). `steps` trades quality for cost: the sky raymarch uses 12, the indirect paths fewer.
vec3 atmosphereScatter(vec3 dir, vec3 lightDir, vec3 up, int steps, out vec3 transmittance)
{
	vec3 ro = up * (ATMOS_R_PLANET + ATMOS_OBSERVE_HEIGHT);
	float tFar = max(atmosRaySphere(ro, dir, ATMOS_R_ATMOS).y, 0.0);

	float mu = dot(dir, lightDir);
	float pR = phaseRayleigh(mu);
	float pM = phaseHG(mu, u_skySunParams.y);

	// View optical depth per sample as a difference of two Chapman evaluations — exact for an
	// exponential atmosphere. The numerically-accumulated version diverges per channel at grazing
	// angles (rainbow/green hue artifacts), and gets worse the fewer steps are taken.
	vec2 odViewFull = atmosLightOpticalDepth(ro, dir);
	vec3 sumR = vec3(0.0), sumM = vec3(0.0);
	float dt = tFar / float(steps);
	float t = 0.5 * dt;
	for (int i = 0; i < steps; ++i)
	{
		vec3 p = ro + dir * t;
		float h = length(p) - ATMOS_R_PLANET;
		vec2 dens = exp(-max(h, 0.0) / vec2(ATMOS_H_RAY, ATMOS_H_MIE)) * dt;
		vec2 odView = max(odViewFull - atmosLightOpticalDepth(p, dir), vec2(0.0));
		vec2 odSun = atmosLightOpticalDepth(p, lightDir);
		vec3 atten = exp(-atmosTau(odView + odSun));
		sumR += atten * dens.x;
		sumM += atten * dens.y;
		t += dt;
	}
	vec2 odViewEnd = max(odViewFull - atmosLightOpticalDepth(ro + dir * tFar, dir), vec2(0.0));
	transmittance = exp(-atmosTau(odViewEnd));
	// Scatter boost (u_skySunParams.x) scales how much of the light gets in-scattered — more indirect
	// sky light — without touching the transmittance. Applied here so every consumer (visible sky,
	// GI miss rays, fog ambient, surface fallback) scales consistently.
	return (sumR * u_betaRayleigh * pR + sumM * vec3(u_betaMie) * pM) * u_skySunParams.x;
}

// Low-step scatter for the indirect paths. Unlike atmosphereScatter, the VIEW optical depth at each
// sample is computed analytically as a difference of two Chapman evaluations (exact for an exponential
// atmosphere) instead of accumulated numerically: with only a few samples the accumulated form gets the
// per-channel exp(-beta*tau) extinction badly wrong at grazing angles, which shows up as vivid
// rainbow/green hue artifacts in GI and fog. The quadrature error now only touches the smooth source
// term, so a handful of samples stays hue-stable.
vec3 atmosphereScatterCheap(vec3 dir, vec3 lightDir, vec3 up, int steps)
{
	vec3 ro = up * (ATMOS_R_PLANET + ATMOS_OBSERVE_HEIGHT);
	float tFar = max(atmosRaySphere(ro, dir, ATMOS_R_ATMOS).y, 0.0);

	float mu = dot(dir, lightDir);
	float pR = phaseRayleigh(mu);
	float pM = phaseHG(mu, u_skySunParams.y);

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
	return (sumR * u_betaRayleigh * pR + sumM * vec3(u_betaMie) * pM) * u_skySunParams.x;
}

// Sky radiance for indirect lighting (GI miss rays, fog ambient, surface fallback): a cheap, low-step
// version of the exact same scatter integral the sky renders with, sourced by the sun plus the
// directional sky-radiance light (moonlight / space light, along u_skyUp). Below the horizon a
// Lambertian ground stands in: a graze-direction sky sample scaled by the ground albedo (the sky's
// bounce) PLUS the sun's direct contribution albedo/PI * E_sun — the dominant daytime term. Without the
// sun term, downward GI rays and the out-of-field ambient fallback sat well below the brightness of the
// sunlit ground the probes actually see.
vec3 skyRadiance(vec3 dir)
{
	const vec3 up = normalize(u_skyUp);
	vec3 groundAtten = vec3(1.0);
	vec3 groundSun = vec3(0.0);
	float cosUp = dot(dir, up);
	if (cosUp < 0.0)
	{
		dir = normalize(dir - up * (cosUp - 0.02));
		groundAtten = u_groundParams.rgb;
		const vec3 sunDir = normalize(u_sunDirection.xyz);
		groundSun = u_groundParams.rgb * atmosTransmittanceToLight(0.0, sunDir, up) * u_sunColor.rgb
			* (u_eclipseParams.x * max(dot(sunDir, up), 0.0) / PI);
	}
	vec3 radiance = atmosphereScatterCheap(dir, normalize(u_sunDirection.xyz), up, 4) * u_sunColor.rgb * u_eclipseParams.x;
	if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
		radiance += atmosphereScatterCheap(dir, up, up, 2) * u_skyRadianceColor;
	return radiance * groundAtten + groundSun;
}

// Sky for a MIRROR ray (ocean surface, terrain wet film): matched to sky.fs.glsl (12 view steps + the
// eclipse saturation curve). skyRadiance() is GI-grade and under-samples the long grazing paths
// reflections look along. No sun disc (the GGX glint is its reflection) and no ground term.
vec3 mirrorSkyRadiance(vec3 dir)
{
	const vec3 up = normalize(u_skyUp);
	const float eclipse = u_eclipseParams.x;
	const vec3 luminosity = vec3(0.2126, 0.7152, 0.0722);
	vec3 color = atmosphereScatterCheap(dir, normalize(u_sunDirection.xyz), up, 12) * u_sunColor.rgb;
	color = mix(vec3(dot(color, luminosity)), color, 2.0 * (2.0 - eclipse)) * eclipse;
	if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
		color += atmosphereScatterCheap(dir, up, up, 4) * u_skyRadianceColor;
	return color;
}

// THE SKY MAP (gi_sky_map.cs.glsl, GIProbePipeline): both functions above baked once per frame into a
// lat-long 2-layer array — layer SKY_MAP_LAYER_GI = skyRadiance, SKY_MAP_LAYER_MIRROR =
// mirrorSkyRadiance. Consumers declare `sampler2DArray u_skyMap` on their own binding and sample
// textureLod(u_skyMap, vec3(skyMapUV(dir), layer), 0.0). World +Y pole: u = atan(d.x, d.z) / 2pi + 0.5
// (the sampler repeats U), v = acos(d.y) / pi (clamped V).
#define SKY_MAP_LAYER_GI     0.0
#define SKY_MAP_LAYER_MIRROR 1.0
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

//vec3 skyRadiance(vec3 dir)
//{
//	const vec3 up = normalize(u_skyUp);
//	vec3 groundAtten = vec3(1.0);
//	const vec3 sunDir = normalize(u_sunDirection.xyz);
//
//	float cosUp = dot(dir, up);
//	if (cosUp < 0.0)
//	{
//		dir = normalize(dir - up * (cosUp - 0.02));
//		groundAtten = u_groundParams.rgb * atmosTransmittanceToLight(0.0, sunDir, up) * u_sunColor.rgb * (u_eclipseParams.x * max(dot(sunDir, up), 0.0));
//	}
//	vec3 radiance = atmosphereScatterCheap(dir, sunDir, up, 4) * u_sunColor.rgb * u_eclipseParams.x;
//	if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
//		radiance += atmosphereScatterCheap(dir, up, up, 2) * u_skyRadianceColor;
//	return radiance * groundAtten;
//}

// The SURFACE out-of-field fallback is the virtual sky probe: gi_probe_trace.cs.glsl projects
// skyRadiance into an SH-L1 slot (GI_SKY_SH_BASE, gi_probe.inc.glsl giEvalSkySH) evaluated exactly like
// a real probe, so the handover at the probe field's edge matches by construction.

#endif
