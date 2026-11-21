// The shared Rayleigh (air) + Mie (haze) atmosphere: the sun's and the sky-radiance light's single scattering marched,
// every further order from the CPU (atmosMultiScatter). The coefficients come from the UBO (u_sky_*; "Humidity" is in
// u_sky_betaMie / u_sky_mieExtinction), so the visible sky (sky.fs.glsl) and all indirect sky light (GI miss rays, fog
// ambient, the surface fallback) use the same settings, and the in-scatter is sourced by the same u_sunColor as direct
// lighting. Requires ubo.inc.glsl and PI (include via shared.inc.glsl).

#ifndef ATMOSPHERE_INC_GLSL
#define ATMOSPHERE_INC_GLSL

// THE MODEL IS MIRRORED on the CPU (RendererUbo.cpp SkyAtmosphere / SkyRay: the radii, ATMOS_BETA_OZONE and the ozone
// layer, atmosOzonePath, atmosDensity, atmosChapman, atmosTau, atmosRayOD, the ATMOS_MS_* heights) for u_sunTransmittance
// and the multiple-scattering light: keep in sync.
const float ATMOS_R_PLANET = 6371e3;
const float ATMOS_R_ATMOS  = 6451e3;
const float ATMOS_OBSERVE_HEIGHT = 2.0;

// Tweakable shape (u_sky, Sky/Atmosphere).
#define ATMOS_H_RAY   u_sky_rayleighHeight // Rayleigh scale height (m)
#define ATMOS_H_MIE   u_sky_mieHeight      // Mie scale height (m)
#define ATMOS_MIE_EXT u_sky_mieExtinction  // Mie extinction/scattering ratio (absorption)
#define ATMOS_OZONE   u_sky_ozone          // ozone absorption strength (1 = Earth-like)

vec3 atmosTau(vec2 od) // per-channel optical thickness of the air and the haze from (Rayleigh, Mie) optical depths; the ozone is apart
{
	return u_sky_betaRayleigh * od.x + vec3(u_sky_betaMie * ATMOS_MIE_EXT) * od.y;
}

// OZONE: absorption only, in a LAYER 15 - 35 km up (uniform; Bruneton's peak coefficients x 0.75 = his 15 km of tent
// over these 20 km, the same depth straight up). It absorbs green / yellow most. It is NOT a share of the air: near the
// ground there is none, so a level ray (the aerial perspective, the air in front of the clouds) only gets it on the
// LIGHT's way in. A layer has no Chapman form; its optical depth is a LENGTH through it (atmosOzonePath), which a march
// adds to the exponent of its light: the light's way to the sample + the view's way back from it. The view
// transmittance a march steps with (the air and the haze) does not hold it, so (source / extinction) stays right.
const float ATMOS_OZONE_R_BOTTOM = ATMOS_R_PLANET + 15e3;
const float ATMOS_OZONE_R_TOP    = ATMOS_R_PLANET + 35e3;
const vec3 ATMOS_BETA_OZONE = vec3(0.650e-6, 1.881e-6, 0.085e-6) * 0.75;

// The length (m) inside the ozone layer of the ray from p (planet-centred) to space along dir. s runs along the ray, 0
// at its closest point to the planet's centre: the layer is where |s| lies between the two half chords. Any p, any dir
// (a ray from above that dips through the layer crosses it twice).
float atmosOzonePath(vec3 p, vec3 dir)
{
	const float s = dot(p, dir);
	const float closestSq = dot(p, p) - s * s;
	const float inner = sqrt(max(ATMOS_OZONE_R_BOTTOM * ATMOS_OZONE_R_BOTTOM - closestSq, 0.0));
	const float outer = sqrt(max(ATMOS_OZONE_R_TOP * ATMOS_OZONE_R_TOP - closestSq, 0.0));
	return max(outer - max(s, inner), 0.0) + max(min(-s, outer) - inner, 0.0);
}

// The same for a PART of a ray: from where its coordinate along itself is sFrom (an AtmosRay's origin: -tLow) to p.
// It needs no value of the ray's origin but that one float, which a march holds already.
float atmosOzoneSegment(vec3 p, vec3 dir, float sFrom)
{
	const float s = dot(p, dir);
	const float closestSq = dot(p, p) - s * s;
	const float inner = sqrt(max(ATMOS_OZONE_R_BOTTOM * ATMOS_OZONE_R_BOTTOM - closestSq, 0.0));
	const float outer = sqrt(max(ATMOS_OZONE_R_TOP * ATMOS_OZONE_R_TOP - closestSq, 0.0));
	return max(min(s, outer) - max(sFrom, inner), 0.0) + max(min(s, -inner) - max(sFrom, -outer), 0.0);
}

vec3 atmosOzoneTau(float path) { return ATMOS_BETA_OZONE * (ATMOS_OZONE * path); }

// The ozone between a point UNDER the layer and a light straight up: the whole layer once.
vec3 atmosOzoneZenith() { return exp(-atmosOzoneTau(ATMOS_OZONE_R_TOP - ATMOS_OZONE_R_BOTTOM)); }

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

// Henyey-Greenstein: the haze's lobes and the sun halo.
float phaseHG(float mu, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

// THE HAZE'S PHASE toward a light (mode = a constant). Haze is particles of two sizes, and one lobe cannot hold both:
// the fine ones scatter WIDE (u_sky_mieG: the dull glow over a large part of the sky), the coarse ones SHARP
// (u_sky_hazeSharpG, their share of the scattering u_sky_hazeSharpShare: a bright glow a few degrees round the sun).
// * ATMOS_HAZE_SUN      - the sun, both lobes: the sky, the mirror sky, the aerial LUT, the air in front of the clouds.
// * ATMOS_HAZE_SUN_WIDE - the sun, all of the haze in the wide lobe (the same energy): the low-res GI sky and every
//                         ambient. The sharp lobe is narrower than a GI texel (5.6 degrees): a sun that moves over the
//                         texel centres would make the sky light pulse.
// * ATMOS_HAZE_DIFFUSE  - the sky-radiance light: no lobe. It stands for a diffuse glow, and a lobe would draw a bright
//                         spot round the zenith in a thick haze.
#define ATMOS_HAZE_SUN      0
#define ATMOS_HAZE_SUN_WIDE 1
#define ATMOS_HAZE_DIFFUSE  2
float atmosHazePhase(float mu, int mode)
{
	if (mode == ATMOS_HAZE_DIFFUSE)
		return 1.0 / (4.0 * PI);
	const float wide = phaseHG(mu, u_sky_mieG);
	if (mode == ATMOS_HAZE_SUN_WIDE)
		return wide;
	return mix(wide, phaseHG(mu, u_sky_hazeSharpG), u_sky_hazeSharpShare);
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

// THE HAZE HAS TWO HEIGHTS: its low layer (ATMOS_H_MIE), and a BACKGROUND share of the ground's density
// (u_sky_hazeBackground) that thins with the AIR - haze above the low layer. The background has the air's scale height
// on purpose: its density and its optical depth are then a mix with the air's own, with no exponential and no Chapman
// evaluation of its own. Every (Rayleigh, Mie) density and optical depth below holds it in its Mie part.
vec2 atmosDensity(float h) // (Rayleigh, Mie) at height h (m), 1 at the ground
{
	const vec2 d = exp(-h / vec2(ATMOS_H_RAY, ATMOS_H_MIE));
	return vec2(d.x, mix(d.y, d.x, u_sky_hazeBackground));
}

// (Rayleigh, Mie) optical depth from pos (planet-centered) to the atmosphere edge toward a light.
vec2 atmosLightOpticalDepth(vec3 pos, vec3 lightDir)
{
	float r = length(pos);
	float h = max(r - ATMOS_R_PLANET, 0.0);
	float cosChi = dot(pos, lightDir) / r;
	float odR = exp(-h / ATMOS_H_RAY) * ATMOS_H_RAY * atmosChapman(r / ATMOS_H_RAY, cosChi);
	float odM = exp(-h / ATMOS_H_MIE) * ATMOS_H_MIE * atmosChapman(r / ATMOS_H_MIE, cosChi);
	return vec2(odR, mix(odM, odR, u_sky_hazeBackground));
}

// The same straight UP from height h (m): the column above it, scale height x density (the Chapman function is 1 at the
// zenith). No Chapman evaluation, and its exponentials are atmosDensity's own.
vec2 atmosZenithOpticalDepth(float h)
{
	const vec2 od = exp(-h / vec2(ATMOS_H_RAY, ATMOS_H_MIE)) * vec2(ATMOS_H_RAY, ATMOS_H_MIE);
	return vec2(od.x, mix(od.y, od.x, u_sky_hazeBackground));
}

// Transmittance from a point at `height` above the surface toward a light: white at midday, warm at the horizon, ~0
// below it.
vec3 atmosTransmittanceToLight(float height, vec3 lightDir, vec3 up)
{
	const vec3 p = up * (ATMOS_R_PLANET + height);
	return exp(-(atmosTau(atmosLightOpticalDepth(p, lightDir)) + atmosOzoneTau(atmosOzonePath(p, lightDir))));
}

// (Rayleigh, Mie) optical depth between a planet-centred ro and ro + dir * tEnd, as a difference of two Chapman
// evaluations taken in the UPWARD direction of each half. A downward ray's "to space" depth goes through the planet
// (up to ~e^60), and a difference of two of those is float noise: a descending ray is measured backwards, and one that
// dips and rises again is split at its lowest point.
// AtmosRay is the per-ray part, for a march that asks many distances along one ray: the Chapman values at the origin
// and the lowest point are constant, so each distance costs ONE evaluation. TWO constants only (a march holds them
// across its loop: registers): the rising and the past-the-lowest-point cases share odFwd.
struct AtmosRay
{
	vec3 ro;
	vec3 dir;
	float tLow;    // the ray's lowest point (<= 0: rising all the way)
	vec2 odFwd;    // the rising part: origin -> space along dir; for a ray that descends first, origin -> lowest point -> space
	vec2 odRoBack; // the descending part: origin -> space along -dir
};

AtmosRay atmosRayBegin(vec3 ro, vec3 dir)
{
	AtmosRay r;
	r.ro = ro;
	r.dir = dir;
	r.tLow = -dot(ro, dir);
	r.odRoBack = vec2(0.0);

	if (r.tLow <= 0.0)
		r.odFwd = atmosLightOpticalDepth(ro, dir);
	else
	{
		r.odRoBack = atmosLightOpticalDepth(ro, -dir);
		const vec3 m = ro + dir * r.tLow;
		r.odFwd = max(atmosLightOpticalDepth(m, -dir) - r.odRoBack, vec2(0.0)) + atmosLightOpticalDepth(m, dir);
	}
	return r;
}

// A ray that only RISES (dir above the horizon at ro): one Chapman evaluation here in place of three (the general
// form's cases are flattened: all worked out), and atmosRayOD's descending case folds away.
AtmosRay atmosRayBeginRising(vec3 ro, vec3 dir)
{
	AtmosRay r;
	r.ro = ro;
	r.dir = dir;
	r.tLow = -1.0;
	r.odFwd = atmosLightOpticalDepth(ro, dir);
	r.odRoBack = vec2(0.0);
	return r;
}

// (Rayleigh, Mie) optical depth from the ray's origin to distance t (atmosSegmentOD's cases).
vec2 atmosRayOD(AtmosRay r, float t)
{
	// ONE Chapman evaluation, its direction and sign selected: two calls under an if are both worked out (flattened).
	const bool descending = t < r.tLow; // still descending: measured from p back up to the origin
	const vec2 od = atmosLightOpticalDepth(r.ro + r.dir * t, descending ? -r.dir : r.dir);
	return max(descending ? od - r.odRoBack : r.odFwd - od, vec2(0.0));
}

vec2 atmosSegmentOD(vec3 ro, vec3 dir, float tEnd)
{
	return atmosRayOD(atmosRayBegin(ro, dir), tEnd);
}

// The scattering (1/m) at a point of (Rayleigh, Mie) densities; atmosTau of the same densities is its extinction.
vec3 atmosScattering(vec2 dens)
{
	return u_sky_betaRayleigh * dens.x + vec3(u_sky_betaMie) * dens.y;
}

// THE MULTIPLE-SCATTERING LIGHT at height h (m): the radiance the air and the haze get from the rest of the sky and from
// the lit ground - every scattering order past the first. A march adds it x the scattering at each sample, with no phase
// (isotropic) and no light transmittance (it holds them). The CPU computes it per frame at two altitudes for the sun AND
// the sky-radiance light together, x "Scatter Boost" x "Multiple scattering" (RendererUbo.cpp buildUboSky, which mirrors
// the two heights): add it ONCE per ray, with no light colour. It is what keeps a thick haze bright: the sun's own light
// alone leaves the horizon away from the sun DARKER the more haze there is.
const float ATMOS_MS_LOW = 500.0;
const float ATMOS_MS_HIGH = 8000.0;
vec3 atmosMultiScatter(float h)
{
	return mix(u_sky_multiScatterLow, u_sky_multiScatterHigh, clamp((h - ATMOS_MS_LOW) * (1.0 / (ATMOS_MS_HIGH - ATMOS_MS_LOW)), 0.0, 1.0));
}

// THE MARCH to the atmosphere's edge (or the ground) for one directional light: returns the RADIANCE it scatters toward
// the viewer - the light scattered once, plus (multiScatter, a constant) the multiple-scattering light. A ray marches
// ONE of its two lights with it, so it is added once: the sky-radiance light where the sun's march is saturated (the
// sky, the mirror layer), else the sun.
// * lightColor = the light's radiance (the eclipse in it); hazePhase = the haze's phase toward THIS light, an
//   ATMOS_HAZE_* constant (atmosHazePhase). saturation = the "Saturation" curve on the once-scattered light (1 = none);
//   the multiple-scattering light does not take it (the haze's white).
// * THE OZONE is in the exponent of each sample's light only (the light's way in + the view's way back to the origin,
//   as lengths through the layer), and in the returned transmittance - never in `through`, which the steps take from.
//   REGISTERS: that light's transmittance is worked out FIRST in the step, and the view part takes the origin from
//   ray.tLow. The ozone length as a float held across the step was 64 in the sky FS, the whole ray's ozone held across
//   the loop 64 in gi_sky_map.
// * REGISTERS: ONE accumulator. The light colour and the saturation (linear, so per step = on the sum) are applied in
//   the step; summed per species and coloured after the loop, the march held three sums (sky FS 56 -> 72 registers).
//   The multiple-scattering light on a SATURATED march is a second vector across the step's widest point (gain apart
//   from grey: 64); with saturation 1 it adds into gain (56).
// * The step ENDS are quadratic, tFar * (i / steps)^2: the haze is a layer ~1 km thick, and a ray to the edge is 80 km
//   (zenith) to 1000 km (horizon) long. Equal steps put the first sample kilometres up or tens of kilometres out.
// * Each step's light = (source / extinction) at its sample x the transmittance the step TAKES AWAY, the transmittances
//   in closed form (atmosRayOD, exact for an exponential atmosphere, so no hue drift at grazing angles). A step of any
//   optical thickness then scatters the right amount: 12 steps are within 1 % of the integral in every direction, 4
//   within ~3 %, a thick haze included. The midpoint rule over equal steps lost 20 % at the horizon with 12 steps and
//   60 % with 4, and more the thicker the haze.
vec3 atmosMarch(AtmosRay ray, float tFar, vec3 lightDir, int hazePhase, vec3 lightColor, float saturation, bool multiScatter, int steps,
                out vec3 transmittance)
{
	// u_sky_scatterBoost scales the in-scatter without the transmittance; here, so every consumer scales the same.
	const float mu = dot(ray.dir, lightDir);
	const float pR = phaseRayleigh(mu) * u_sky_scatterBoost;
	const float pM = atmosHazePhase(mu, hazePhase) * u_sky_scatterBoost;
	const float invSteps = 1.0 / float(steps);

	vec3 sum = vec3(0.0);
	vec3 through = vec3(1.0); // the view transmittance to the step's start (the air and the haze)
	for (int i = 0; i < steps; ++i)
	{
		const float sEnd = float(i + 1) * invSteps;
		const float sMid = (float(i) + 0.5) * invSteps;
		const vec3 p = ray.ro + ray.dir * (tFar * sMid * sMid);
		// The light that reaches the sample, FIRST in the step (nothing of the sample is live across its Chapman
		// evaluation): the air and the haze, then the ozone - the light's way in + the view's way back to the origin
		// (-tLow = the origin along the ray; a rising ray's -1 is under the layer like its origin).
		vec3 lightTau = atmosTau(atmosLightOpticalDepth(p, lightDir));
		lightTau += atmosOzoneTau(atmosOzonePath(p, lightDir) + atmosOzoneSegment(p, ray.dir, -ray.tLow));
		const vec3 reaches = exp(-lightTau);
		const float h = max(length(p) - ATMOS_R_PLANET, 0.0);
		const vec2 dens = atmosDensity(h);
		// The step's light per unit of transmittance it takes: (source / extinction) at the sample. gain = its
		// per-colour part, grey = the luminance part of the saturation: saturate(taken * once) = taken * (once *
		// saturation) + dot(taken, once * luminosity * (1 - saturation)).
		const vec3 invExt = 1.0 / max(atmosTau(dens), vec3(1e-30));
		const vec3 once = reaches * (u_sky_betaRayleigh * (dens.x * pR) + vec3(u_sky_betaMie * dens.y * pM)) * lightColor * invExt;
		vec3 gain = once * saturation;
		vec3 grey = vec3(0.0);
		if (saturation != 1.0)
			grey = once * (vec3(0.2126, 0.7152, 0.0722) * (1.0 - saturation));
		if (multiScatter)
			gain += atmosScattering(dens) * atmosMultiScatter(h) * invExt;
		// x the transmittance taken, (through - next), as its two halves: `through` is dead before the next one's
		// optical depth is worked out (the widest point of the step), and only gain and grey cross it.
		sum += through * gain + dot(through, grey);
		through = exp(-atmosTau(atmosRayOD(ray, tFar * sEnd * sEnd)));
		sum -= through * gain + dot(through, grey);
	}

	transmittance = through * exp(-atmosOzoneTau(atmosOzoneSegment(ray.ro + ray.dir * tFar, ray.dir, -ray.tLow)));
	return sum;
}

// The sky along dir for an observer `observerHeight` metres above the ground (the visible sky passes the camera altitude:
// the sky darkens and the horizon dips), plus the view ray's total transmittance (for the sun disc / stars). The ray
// ends at the atmosphere edge or the ground. The sky uses 12 steps.
vec3 atmosphereScatter(vec3 dir, vec3 lightDir, int hazePhase, vec3 lightColor, float saturation, bool multiScatter, vec3 up, int steps,
                       float observerHeight, out vec3 transmittance)
{
	const vec3 ro = up * (ATMOS_R_PLANET + observerHeight);
	float tFar = max(atmosRaySphere(ro, dir, ATMOS_R_ATMOS).y, 0.0);
	const vec2 tGround = atmosRaySphere(ro, dir, ATMOS_R_PLANET);
	if (tGround.x > 0.0 && tGround.x < tGround.y)
		tFar = min(tFar, tGround.x);
	return atmosMarch(atmosRayBegin(ro, dir), tFar, lightDir, hazePhase, lightColor, saturation, multiScatter, steps, transmittance);
}

// The same for the indirect paths: from the ground observer (ATMOS_OBSERVE_HEIGHT), the ray to the atmosphere edge
// only (a ray under the horizon goes through the ground's air: the horizon's colour).
vec3 atmosphereScatterCheap(vec3 dir, vec3 lightDir, int hazePhase, vec3 lightColor, float saturation, bool multiScatter, vec3 up, int steps)
{
	const vec3 ro = up * (ATMOS_R_PLANET + ATMOS_OBSERVE_HEIGHT);
	vec3 transmittance;
	return atmosMarch(atmosRayBegin(ro, dir), max(atmosRaySphere(ro, dir, ATMOS_R_ATMOS).y, 0.0), lightDir, hazePhase, lightColor, saturation,
	                  multiScatter, steps, transmittance);
}

// ... and for a dir that RISES (above the horizon), with no saturation: the least work per ray.
vec3 atmosphereScatterRising(vec3 dir, vec3 lightDir, int hazePhase, vec3 lightColor, bool multiScatter, vec3 up, int steps)
{
	const vec3 ro = up * (ATMOS_R_PLANET + ATMOS_OBSERVE_HEIGHT);
	vec3 transmittance;
	return atmosMarch(atmosRayBeginRising(ro, dir), max(atmosRaySphere(ro, dir, ATMOS_R_ATMOS).y, 0.0), lightDir, hazePhase, lightColor, 1.0,
	                  multiScatter, steps, transmittance);
}

// The below-horizon ground's direct sun, albedo / PI * E_sun (constant over directions). The GI trace counts it as SUN
// light on its downward misses, so the lookup's cloud dimming reaches it (GI_SUN_V4 in gi_probe.inc.glsl). up is always
// u_skyUp, so the whole term is the CPU's per-frame u_sky_groundSun; up is unused (kept for the callers).
vec3 skyGroundSun(vec3 up)
{
	return u_sky_groundSun;
}

// Sky radiance for indirect lighting (GI miss rays, fog ambient, surface fallback): a low-step version of the sky's
// integral, sourced by the sun plus the sky-radiance light (moonlight / space light, along u_skyUp) and the
// multiple-scattering light. Below the horizon
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

	// The wide lobe alone: this is the low-res GI sky and the ambients (atmosHazePhase).
	vec3 radiance = atmosphereScatterRising(dir, u_sunDirection.xyz, ATMOS_HAZE_SUN_WIDE, u_sunColor.rgb * u_sunVisible, true, up, 4);
	if (dot(u_sky_radiance, u_sky_radiance) > 0.0)
		radiance += atmosphereScatterRising(dir, up, ATMOS_HAZE_DIFFUSE, u_sky_radiance, false, up, 2);
	return radiance * groundAtten + groundSun;
}

// Sky for a MIRROR ray (ocean, terrain wet film): matched to sky.fs.glsl (12 view steps + the eclipse saturation
// curve, which the multiple-scattering light does not take). No sun disc (the GGX glint is its reflection), no ground
// term.
vec3 mirrorSkyRadiance(vec3 dir)
{
	const vec3 up = u_skyUp;
	const float eclipse = u_sunVisible;
	// The multiple-scattering light rides the sky-light march, as in sky.fs.glsl.
	return atmosphereScatterCheap(dir, u_sunDirection.xyz, ATMOS_HAZE_SUN, u_sunColor.rgb * eclipse, u_sky_saturation * (2.0 - eclipse), false, up, 12)
	     + atmosphereScatterCheap(dir, up, ATMOS_HAZE_DIFFUSE, u_sky_radiance, 1.0, true, up, 4);
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
