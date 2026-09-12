#version 460

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// Sky variant of the static-mesh pipeline (EPipelineIndex::Sky), shading the inside of the sky sphere:
//  - single-scattering Rayleigh + Mie atmosphere, raymarched per pixel (VIEW_STEPS x SUN_STEPS, ALU only)
//  - sun disc attenuated by the same atmospheric transmittance (reddens and flattens at the horizon),
//    plus a Henyey-Greenstein forward-scatter halo (u_sunGlow = strength)
//  - one FBM cloud layer: domain-warped quintic value noise, wind-animated, coverage-driven, with a
//    smooth (unthresholded) directional sun-shading term and a silver lining
//  - hash stars that fade in when the sun sets
// Everything is driven from the UBO sky params (Tweaks panel: Sky / Sky/Atmosphere / Sky/Sun /
// Sky/Clouds). The GI/fog/fallback paths use the low-step skyRadiance() (atmosphere.inc.glsl), the
// same scatter model with the same UBO coefficients, so indirect sky light matches the visible sky.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in mat3 in_tbn;
layout (location = 4) in vec2 in_uv;
layout (location = 5) in flat uint in_meshIdxMaterialIdx;
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

layout (location = 0) out vec4 out_color;

// ---------------------------------------------------------------------------------------------
// Atmosphere: single-scattering Rayleigh + Mie, shared with the indirect paths (atmosphere.inc.glsl,
// pulled in by shared.inc.glsl). Coefficients come from the UBO (u_betaRayleigh / u_betaMie).
// ---------------------------------------------------------------------------------------------
const int VIEW_STEPS = 12;

// ---------------------------------------------------------------------------------------------
// Clouds: a marched slab [height, height + thickness] of domain-warped FBM with a vertical density
// profile - a cheap "2.5D" volumetric: layers parallax against each other, bottoms sit lower than
// tops, edges dissolve in depth instead of looking like a painted plane.
// ---------------------------------------------------------------------------------------------

// Integer hashes (iq-style). float fract(p * K) hashes correlate at large coordinates and draw long
// straight diagonal seams through the noise field; integer mixing has no such structure.
float hash12(vec2 p)
{
	uvec2 q = uvec2(ivec2(floor(p))) * uvec2(1597334673u, 3812015801u);
	uint n = (q.x ^ q.y) * 1597334673u;
	return float(n) * (1.0 / 4294967295.0);
}
float hash13(vec3 p)
{
	uvec3 q = uvec3(ivec3(floor(p))) * uvec3(1597334673u, 3812015801u, 2798796415u);
	uint n = (q.x ^ q.y ^ q.z) * 1597334673u;
	return float(n) * (1.0 / 4294967295.0);
}
float vnoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // quintic fade: C2-continuous, no cell creases
	float a = hash12(i);
	float b = hash12(i + vec2(1.0, 0.0));
	float c = hash12(i + vec2(0.0, 1.0));
	float d = hash12(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// True 3D value noise: unlike stacked/offset 2D layers, consecutive march samples sit in one coherent
// 3D field, so cloud shapes genuinely overhang and interpenetrate instead of reading as flat sheets.
float vnoise3(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float n000 = hash13(i + vec3(0, 0, 0)), n100 = hash13(i + vec3(1, 0, 0));
	float n010 = hash13(i + vec3(0, 1, 0)), n110 = hash13(i + vec3(1, 1, 0));
	float n001 = hash13(i + vec3(0, 0, 1)), n101 = hash13(i + vec3(1, 0, 1));
	float n011 = hash13(i + vec3(0, 1, 1)), n111 = hash13(i + vec3(1, 1, 1));
	float nx00 = mix(n000, n100, f.x), nx10 = mix(n010, n110, f.x);
	float nx01 = mix(n001, n101, f.x), nx11 = mix(n011, n111, f.x);
	return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}
float fbm(vec2 p)
{
	const mat2 rot = mat2(0.8, 0.6, -0.6, 0.8) * 2.02;
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 4; ++i)
	{
		v += a * vnoise(p);
		p = rot * p;
		a *= 0.5;
	}
	return v * 1.0667;
}
// FBM with a conservative early-out: after each octave the maximum the remaining octaves could still
// add is known, so if even that cannot reach minNeeded the result is provably below the coverage
// threshold and the remaining (most expensive, high-frequency) octaves are skipped. In cloud gaps the
// first octave rejects most samples, which is the bulk of the march's noise cost.
float fbm3(vec3 p, int octaves, float minNeeded)
{
	float norm = 1.0 - exp2(-float(octaves)); // sum of 0.5 + 0.25 + ...
	float v = 0.0;
	float a = 0.5;
	float remaining = norm;
	for (int i = 0; i < octaves; ++i)
	{
		v += a * vnoise3(p);
		remaining -= a;
		if (v + remaining < minNeeded * norm)
			return 0.0;
		p = p * 2.13 + vec3(17.7, 9.2, 31.4); // offset octaves instead of rotating (cheap in 3D)
		a *= 0.5;
	}
	return v / norm;
}

// FBM with a per-octave domain ROTATION (orthonormal, no axis preserved). fbm3 only scales+offsets
// between octaves, so every octave shares one axis-aligned cube lattice and the sum shows repeating
// grid/cube shapes at low frequencies; rotating decorrelates the lattices. Used by the nebula (the
// clouds keep the cheaper fbm3 - their density threshold + profile already hide the lattice).
float fbmR(vec3 p, int octaves)
{
	const mat3 rot = mat3( 0.00,  0.80,  0.60,
	                      -0.80,  0.36, -0.48,
	                      -0.60, -0.48,  0.64);
	float norm = 1.0 - exp2(-float(octaves));
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < octaves; ++i)
	{
		v += a * vnoise3(p);
		p = rot * p * 2.13 + vec3(17.7, 9.2, 31.4);
		a *= 0.5;
	}
	return v / norm;
}

float ign(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

const int CLOUD_STEPS = 6;

// Cloud density at a world-offset position inside the slab (x = normalized height in [0,1]).
// bottom/top shift the vertical profile per column so the deck has uneven bases and tower tops instead
// of a flat slab; sharpEx remaps the density curve (exponent < 1 = hard, filled shapes; > 1 = wispy).
float cloudDensity(vec3 q, float x, float threshold, float softness, int octaves, float bottom, float top, float sharpEx)
{
	// Profile first: outside the (column-varying) vertical extent the density is zero regardless of the
	// noise, so skip the FBM entirely (the sun-shading sample lands out of slab often).
	float profile = smoothstep(bottom, bottom + 0.18, x) * (1.0 - smoothstep(top - 0.35, top, x));
	if (profile <= 0.0)
		return 0.0;
	float field = fbm3(q, octaves, threshold);
	if (field <= threshold)
		return 0.0;
	float d = smoothstep(threshold, threshold + softness, field) * profile;
	return pow(d, sharpEx);
}

// rgb = cloud color, a = opacity.
vec4 clouds(vec3 dir, vec3 up, vec3 sunDir, vec3 sunTint, vec3 skyAmbient)
{
	float cosUp = dot(dir, up);
	if (cosUp <= 0.02 || u_cloudCoverage <= 0.0)
		return vec4(0.0);

	// Stable tangent basis around the sky-up axis; the slab is camera-anchored (travels with the viewer).
	vec3 refAxis = abs(up.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 e0 = normalize(cross(up, refAxis));
	vec3 e1 = cross(up, e0);
	vec2 windOfs = vec2(cos(u_cloudParams0.w), sin(u_cloudParams0.w)) * (u_cloudParams0.z * u_timeSeconds);

	float softness  = u_cloudParams1.x;
	float threshold = mix(0.74, 0.30, clamp(u_cloudCoverage, 0.0, 1.0));
	float baseH     = u_cloudParams0.x;
	float thickness = max(u_cloudThickness, 1.0);
	float nScale    = u_cloudParams0.y;
	float vScale    = nScale * 2.2; // vertical noise frequency relative to horizontal

	// Noise-space position for a world point: planar coords + wind drift, height as the third axis.
	// (One shared 2D domain warp at the slab entry keeps the billowy large shapes from last round.)
	vec3 entry = dir * (baseH / cosUp);
	vec2 pEntry = vec2(dot(entry, e0), dot(entry, e1)) * nScale + windOfs;
	vec2 warp = (vec2(fbm(pEntry * 1.6 + vec2(11.3, 27.8)), fbm(pEntry * 1.6 + vec2(96.4, 4.7))) - 0.5) * 1.1;

	// Per-column height variation from a low-frequency field: raises the cloud base and grows the tops
	// where the column is "strong", so the underside undulates instead of sitting on a flat plane.
	float heightVar = clamp(u_cloudParams2.z, 0.0, 1.0);
	float colNoise = fbm(pEntry * 0.35 + vec2(7.7, 13.9));
	float colBottom = colNoise * heightVar * 0.45;
	float colTop    = 1.0 - (1.0 - colNoise) * heightVar * 0.4;
	float sharpEx   = mix(2.2, 0.55, clamp(u_cloudParams2.y, 0.0, 1.0));
	#define CLOUD_Q(pos, h) vec3(vec2(dot(pos, e0), dot(pos, e1)) * nScale + windOfs + warp, (h) * vScale)

	// Noise-space step toward the sun for the Beer-Lambert light sample, ~40% of the slab.
	float sunStepWorld = thickness * 0.4;
	vec3 sunWorldStep = sunDir * sunStepWorld;
	vec3 qSunStep = vec3(vec2(dot(sunWorldStep, e0), dot(sunWorldStep, e1)) * nScale, dot(sunWorldStep, up) * vScale);

	// Front-to-back march, jittered per pixel/frame (IGN; TAA integrates it) so the step boundaries
	// dissolve into grain instead of reading as discrete stacked layers.
	float jitter = ign(gl_FragCoord.xy + float(u_frameIndex % 64u) * 5.588238);
	float T = 1.0;
	vec3 acc = vec3(0.0);
	for (int i = 0; i < CLOUD_STEPS; ++i)
	{
		float x = (float(i) + jitter) / float(CLOUD_STEPS); // 0 = slab bottom, 1 = top
		float hi = baseH + thickness * x;
		vec3 posI = dir * (hi / cosUp);
		vec3 q = CLOUD_Q(posI, thickness * x);

		float dens = cloudDensity(q, x, threshold, softness, 4, colBottom, colTop, sharpEx);
		if (dens <= 0.004)
			continue;

		// Beer-Lambert sun attenuation: one density sample toward the sun (2 cheap octaves; the low
		// frequencies dominate shadowing). exp() gives the smooth bright-top/dark-belly falloff.
		float densSun = cloudDensity(q + qSunStep, x + sunStepWorld * dot(sunDir, up) / thickness, threshold, softness, 2, colBottom, colTop, sharpEx);
		float lit = exp(-densSun * u_cloudParams1.y);
		// Powder term: thin edges in-scatter less, which reads as the puffy "dark rim" of cumulus.
		float powder = 1.0 - exp(-dens * 4.0);

		vec3 c = sunTint * lit * mix(0.7, 1.3, powder) * mix(0.55, 1.0, x);

		// Exponential (Beer-Lambert) step opacity: u_cloudParams2.x is the extinction strength, so dense
		// cores saturate to fully opaque instead of staying translucent ("gassy") like the old linear term.
		float a = 1.0 - exp(-dens * u_cloudParams2.x);
		acc += c * a * T;
		T *= 1.0 - a;
		if (T < 0.03)
			break;
	}

	float alpha = 1.0 - T;
	if (alpha <= 0.001)
		return vec4(0.0);

	// Silver lining: forward-scatter highlight through the thin parts, on the accumulated result.
	float mu = max(dot(dir, sunDir), 0.0);
	vec3 col = acc / alpha + sunTint * pow(mu, 12.0) * (1.0 - alpha) * skyAmbient;

	// Horizon fade doubles as anti-aliasing: distant FBM has no mips, so melt it into the haze.
	return vec4(col, alpha * smoothstep(0.02, 0.18, cosUp));
}

vec3 adjustSaturation(vec3 color, float saturation) 
{ // Standard luminosity weights for human perception
    const vec3 luminosity = vec3(0.2126, 0.7152, 0.0722);
    vec3 grayscale = vec3(dot(color, luminosity));
    return mix(grayscale, color, saturation);
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex); // per-eye ray reconstruction below
#endif
	// View ray from the screen position, not from the interpolated world position (in_pos - u_viewPos
	// cancels two large float32 values per pixel and jitters away from the origin). Derived from u_mvp's
	// x/y/w ROWS only: for a world direction d, ndc.xy = (r0.d, r1.d) / (rw.d), so solving the 3x3 system
	// {r0.d = ndc.x, r1.d = ndc.y, rw.d = 1} gives the exact ray from O(1)-magnitude rotation/projection
	// terms - no camera translation and no ill-conditioned 4x4 inverse (u_invMvp is a float32 CPU inverse
	// whose error grows with the camera's distance from the origin and re-rolls every frame = jitter).
	// The raster ran TAA-jittered while u_mvp is unjittered, so back the jitter out of the NDC first.
	vec2 vpUv = (gl_FragCoord.xy * u_screenSize.zw - u_viewportRect.xy) / u_viewportRect.zw;
	vec2 ndc = vec2(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0) - u_taaJitter.xy;
	const vec3 rayR0 = vec3(u_mvp[0][0], u_mvp[1][0], u_mvp[2][0]);
	const vec3 rayR1 = vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]);
	const vec3 rayRw = vec3(u_mvp[0][3], u_mvp[1][3], u_mvp[2][3]);
	const vec3 dir = normalize(vec3(ndc, 1.0) * inverse(mat3(rayR0, rayR1, rayRw)));
	// Direction-space size of one screen pixel, computed here in uniform control flow (derivatives are
	// undefined inside non-uniform branches). Scaled by each star grid's frequency to clamp star
	// footprints to >= a pixel so the TAA jitter samples them consistently.
	const float dirPx = length(fwidth(dir));
	const vec3 up = normalize(u_skyUp);
	const vec3 L = normalize(u_sunDirection.xyz);

	// With the sun fully off (color/intensity zero) the entire scattering integral is a multiply by
	// zero: skip the raymarch and keep only the cheap ground test for the branches below.
	vec3 sunSurfaceColor = u_sunColor.rgb;
	const float eclipseFactor = u_eclipseParams.x;
	const float sunMagnitude = (u_sunColor.r + u_sunColor.g + u_sunColor.b);
	const bool sunLit = sunMagnitude * eclipseFactor > 1e-5;
	vec3 transmittance = vec3(1.0);
	vec3 color = vec3(0.0);
	const float sunElev = dot(L, up);
	
	{
		vec3 moonDir = u_moonParams.xyz;
		float cosM = dot(dir, moonDir);
		const float cosAngle = dot(dir, L);
		const float sunMoonAngle = dot(L, moonDir);
		const bool moonCovered = cosM < u_moonParams.w * 2.00 - cosM;

		color = atmosphereScatter(dir, L, up, VIEW_STEPS, transmittance) * sunSurfaceColor.rgb;
		color = adjustSaturation(color, 2.0 * (2.0-eclipseFactor)) * eclipseFactor; // Saturate the sky as the sun goes into eclipse, to keep it from looking like a flat gray haze
		sunSurfaceColor *= eclipseFactor;

		// Sky radiance (moonlight / space light): a second in-scatter pass from the sky up axis, so it
		// gives the night sky a faint glow consistent with what GI/fog receive from skyRadiance().
		if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
		{
			vec3 skyLTrans;
			color += atmosphereScatter(dir, up, up, 4, skyLTrans) * u_skyRadianceColor;
		}

		// Sun halo: a tight Henyey-Greenstein forward lobe (smooth peak, long graceful tail) instead of a
		// pow() spike. u_sunGlow is the strength; the transmittance keeps it warm/dim near the horizon.
		if (sunLit && u_sunGlow > 0.0 && moonCovered)
			color += sunSurfaceColor * phaseHG(cosAngle, 0.985) * 0.015 * u_sunGlow * transmittance;

		if (sunLit && u_sunAngularCos < 1.0 && moonCovered) // 1.0 disables the disc
		{
			// Sun disc: analytically anti-aliased rim (pixel-footprint smoothstep, TAA-stable) and a mild
			// limb darkening. The gradient through the overexposed sun region is recovered by the highlight
			// roll-off at the end of main() (u_skySunParams.z), not by darkening the disc itself.
			vec3 sT = normalize(cross(L, abs(L.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
			vec3 sB = cross(L, sT);
			float discSin = sqrt(max(1.0 - u_sunAngularCos * u_sunAngularCos, 1e-12));
			vec2 suv = vec2(dot(dir, sT), dot(dir, sB)) / discSin; // disc-local, rim at |suv| = 1
			float r2 = dot(suv, suv);
			if (r2 < 1.1 && cosAngle > 0.0)
			{
				float pr = max(dirPx / discSin, 0.004); // pixel footprint in disc radii
				float edge = 1.0 - smoothstep(1.0 - pr * 1.5, 1.0 + pr * 0.5, sqrt(r2));
				float limb = 1.0 - 0.35 * (1.0 - sqrt(max(1.0 - r2, 0.0)));
				color += sunSurfaceColor * (edge * limb) * transmittance;
			}
		}

		// Moon: a sun-lit sphere shaded into the disc around u_moonParams.xyz (independent of the sun's
		// position; the phases still fall out of lighting the reconstructed sphere normals with the
		// actual sun direction, so they react to where the sun is). Disc size comes from u_moonParams.w.
		// Drawn before the stars so its disc coverage can occlude them (even the unlit, new-moon part of
		// the disc blocks the stars behind it). Clouds blend after this, so they occlude it.
		if (u_cloudParams2.w > 0.0 && !moonCovered)
		{
			vec3 mT = normalize(cross(moonDir, abs(moonDir.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
			vec3 mB = cross(moonDir, mT);
			float discSin = sqrt(max(1.0 - u_moonParams.w * u_moonParams.w, 1e-8));
			vec2 duv = vec2(dot(dir, mT), dot(dir, mB)) / discSin;
			float r2 = dot(duv, duv);

			// Visible-hemisphere normal at this disc point; Lambert against the sun => phase.
			vec3 n = normalize(mT * duv.x + mB * duv.y - moonDir * sqrt(1.0 - r2));
			float surfAngle = dot(n, -dir);
			float lambert = clamp(dot(n, L), 0.02, 1.0);
			float planetlit = mix(0.0, 0.07, surfAngle * surfAngle * surfAngle);
			float rimlight = clamp(1.0 - distance(moonDir, L) * discSin, 0.0, 1.0);
			lambert += planetlit * smoothstep(0.0, 0.2, distance(L, moonDir) * u_moonParams.w) * length(color);
			lambert += smoothstep(0.15, 0.0, dot(n, -L)) * rimlight * clamp(0.1 * sunMagnitude * u_moonParams.w - distance(L, moonDir), 0.0, 1.0);
			float albedo = 0.7 + 0.6 * (fbm3(n * 7.0, 3, 0.0) - 0.5); // maria/crater mottling
			color += vec3(0.93, 0.95, 1.0) * ((lambert * albedo * u_cloudParams2.w) * transmittance);
		}

		// Night sky (stars + nebula). Visibility comes from the local sky luminance - daylight in-scatter
		// washes them out, so they fade in automatically at dusk and in dark sky regions. The moon disc
		// masks both out (it is a solid body, not additive light).
		if ((u_skySunParams.w > 0.0 || u_nebulaParams.x > 0.0) && moonCovered)
		{
			float skyLum = dot(color, vec3(0.2126, 0.7152, 0.0722));
			float nightVis = clamp(1.0 - skyLum * 25.0, 0.0, 1.0);
			if (nightVis > 0.0)
			{
				// Milky-way band: a gaussian falloff around the great circle perpendicular to u_nebulaAxis.
				// Raw value-noise FBM reads as soft low-res blobs, so the field is domain-warped by a vector
				// FBM (turns the blobs into wisps and filaments) and squashed across the band so structure
				// stretches lengthwise, like a galaxy seen edge-on. Composed from four layers: broad tinted
				// gas, ridged bright filaments, a narrow hot core line, and dark dust lanes cutting through.
				if (u_nebulaParams.x > 0.0)
				{
					vec3 bandPole = normalize(u_nebulaAxis.xyz);
					float hgt = dot(dir, bandPole);
					float bw = max(u_nebulaParams.z, 0.01);
					float bandFade = exp(-(hgt * hgt) / (bw * bw));
					if (bandFade > 0.004)
					{
						vec3 p = (dir - bandPole * hgt * 0.25) * u_nebulaParams.y;
						vec3 warp = vec3(fbmR(p * 0.8 + vec3(17.1, 3.7, 9.2), 3),
						                 fbmR(p * 0.8 + vec3(27.3, 21.9, 5.8), 3),
						                 fbmR(p * 0.8 + vec3(91.7, 63.2, 33.4), 3)) - 0.5;
						vec3 q = p + warp * (1.7 + 2.6 * vnoise3(p * 3.1 + vec3(77.0, 1.0, 36.0)));
						float gas  = fbmR(q, 5);
						float fil  = 1.0 - abs(2.0 * fbmR(q * 2.2 + vec3(5.0, 27.0, 11.0), 4) - 1.0); // ridged
						float dust = fbmR(q * 1.6 - warp * 1.2 + vec3(31.0, 71.0, 13.0), 4);
						float gasD = smoothstep(0.10, 0.78, gas);
						float filD = pow(fil, 3.0) * smoothstep(0.14, 0.70, gas);
						float coreLine = exp(-(hgt * hgt) / (bw * bw * 0.52)); // narrow hot line along the band center
						float dustCut = 1.0 - u_nebulaParams.w * smoothstep(0.36, 0.72, dust) * mix(0.5, 1.0, coreLine) * bandFade;
						float vary = smoothstep(0.88, 0.72, fbm3(q * 0.35 + vec3(1.2, 1.3, 2.9), 5, 0.0));
						float dens = (gasD * 0.25 + filD * 0.4 + coreLine * gasD * 0.4) * dustCut * bandFade * (0.5 + 5.4 * vary);
						float hueT = fbm3(q * 0.5 + vec3(3.0, 29.0, 3.0), 3, 0.0);
						float t = hueT * 2.2 + dust * 0.6;
						vec3 hue = vec3(0.5) + vec3(0.5) * cos(6.2831853 * (t + vec3(0.00, 0.30, 0.60)));
						hue = mix(vec3(dot(hue, vec3(0.333))), hue, 0.65); // saturation push for vibrancy
						hue = clamp(mix(hue, vec3(0.95), clamp(dens * 0.6, 0.0, 0.55)), 0.0, 1.0);
						float grain = vnoise3(dir * u_nebulaParams.y * 1.0);
						vec3 neb = hue * dens * (0.08 + 0.14 * grain * grain);

						// Micro-star grid scale: ~1.5 px per cell at 1080p, so each speck is resolvable and
						// the footprint clamp below keeps it TAA-stable. (The old 155.753 * 100.0 scale put
						// ~15 cells inside one pixel - sub-pixel stars that only showed up as aliasing noise.)
						const float MICRO_STAR_SCALE = 650.0;
						vec3 sd2 = dir * MICRO_STAR_SCALE + warp * 31.0;
						float h2 = hash13(floor(sd2));
						if (h2 > 1.0 - dens * 0.45)
						{
							// Footprint clamped to >= ~1.2 pixels with a pixel-wide edge, energy conserved by
							// the area ratio: sub-pixel points land on a different TAA jitter sample every
							// frame and get eaten by the variance clipping.
							float px2 = dirPx * MICRO_STAR_SCALE;
							float r2 = max(0.14, px2 * 1.2);
							vec3 ofs2 = fract(h2 * vec3(113.1, 42.7, 743.3)) * 5.8 + 0.25;
							float pt = smoothstep(r2, max(r2 - px2 * 1.5, 0.0), length(fract(sd2) - ofs2)) * (0.44 * 0.44) / (r2 * r2);
							neb += mix(vec3(1.0), hue, 0.35) * (pt * (140.3 + 1.2 * fract(h2 * 27.3)));
						}

						color += neb * (u_nebulaParams.x * 0.9) * nightVis * transmittance.b;
					}
				}

				// Random stars
				if (u_skySunParams.w > 0.0)
				{
					vec3 sd = dir * 220.0;
					// Cell-space size of one screen pixel (derivatives are only defined in uniform control
					// flow, so this sits before the per-cell branch). Keeps every star >= ~a pixel wide.
					float pxr = length(fwidth(sd));
					vec3 cell = floor(sd);
					float h = hash13(cell);
					float gate = step(mix(0.9995, 0.995, u_skySunParams.w), h); // density: fraction of lit cells
					if (gate > 0.0)
					{
						// Round point at a hashed position inside the cell; size/brightness vary per star,
						// with a slow twinkle. Size variation skews small (most stars tiny, a few big), and
						// color variation tints each star along a cool/warm "temperature" axis.
						vec3 ofs = fract(h * vec3(113.1, 412.7, 743.3)) * 0.5 + 0.25;
						float radius = 0.52 * u_starParams.x * mix(1.0, 0.3 + 1.3 * fract(h * 57.31), u_starParams.y);
						// Clamp the rendered footprint to >= ~1.2 pixels with a pixel-wide smooth edge and
						// conserve the original energy via the area ratio. A sub-pixel point lands on a
						// different jitter sample every frame (it pops in and out), so the TAA variance
						// clipping eats it - dim smeared stars. A pixel-sized, analytically anti-aliased
						// disc shades identically every frame and survives the accumulation intact.
						float r = max(radius, pxr * 1.2);
						float comp = (radius * radius) / (r * r);
						float core = smoothstep(r, max(r - pxr * 1.5, 0.0), length(fract(sd) - ofs)) * comp;
						// Twinkle = two octaves of value noise sampled along the time axis (per-star rate and
						// row, so every star walks its own random curve instead of a periodic wave), then
						// squared so it reads as mostly-steady with occasional brighter glints.
						float tt = u_timeSeconds * mix(0.5, 1.4, fract(h * 37.7));
						float n = 0.75 * vnoise(vec2(tt, h * 797.0)) + 0.25 * vnoise(vec2(tt * 2.3 + 13.1, h * 311.0));
						float twinkle = 0.65 + 0.7 * n * n;
						vec3 tint = mix(vec3(1), 
										mix(vec3(0.1, 0.52, 0.20), 
											vec3(0.95, 0.40, 0.7), 
											fract(h * 636)
										), 
									u_starParams.w);
						color += tint * (core * twinkle * (0.5 + h) * u_starParams.z) * nightVis * transmittance.b;
					}
				}
			}
		}
	}

	// Ground plane (below the horizon): the through-planet march is meaningless there, so blend to a
	// ground albedo (u_groundParams, Sky > Ground Albedo) lit like a diffuse surface - direct sun
	// (horizon-tinted by the same transmittance the clouds use) plus the grazing sky as ambient, the
	// same fallback skyRadiance() gives downward GI/fog rays. A narrow blend band keeps the horizon
	// scatter colors (the red sunset band) intact right at the horizon line.
	const float cosUpDir = dot(dir, up);
	if (cosUpDir < 0.0)
	{
		// The marched color is meaningless below the horizon (it collapses to black within a degree or
		// two), so it is replaced entirely: the blend runs from the grazing-horizon sky color (continuous
		// with the sky right at the horizon line) into the lit ground, never through black.
		vec3 grazeDir = normalize(dir - up * (cosUpDir - 0.02));
		vec3 skyAmb = atmosphereScatterCheap(grazeDir, L, up, 4) * u_sunColor.rgb * eclipseFactor;
		if (dot(u_skyRadianceColor, u_skyRadianceColor) > 0.0)
			skyAmb += atmosphereScatterCheap(grazeDir, up, up, 2) * u_skyRadianceColor;
		vec3 groundLit = u_groundParams.rgb *
			(sunSurfaceColor * atmosTransmittanceToLight(0.0, L, up) * (max(sunElev, 0.0) / PI) + skyAmb);
		const float groundBlend = smoothstep(0.0, 0.08, -cosUpDir);
		color = mix(skyAmb, groundLit + u_ambientColor, groundBlend);
	}

	// Clouds last, over everything (they also dim the sun disc behind them). Direct cloud light = sun
	const vec3 sunTint = (sunSurfaceColor.rgb * atmosTransmittanceToLight(u_cloudParams0.x, L, up) + u_skyRadianceColor) / PI;
	vec4 cl = clouds(dir, up, L, sunTint, color);
	color = mix(color, cl.rgb, cl.a);
	/*
	// Highlight roll-off (u_skySunParams.z): there is no tonemapper, so everything over 1.0 hard-clips
	// to flat white - the sun disc, its halo and the Mie forward peak all merge into one featureless
	// circle. Soft-clip the max channel above a knee with an exponential shoulder that asymptotes at 1:
	// the overexposed region keeps a smooth gradient (disc > halo core > halo tail) instead of a hard
	// silhouette. Max-channel (not per-channel) so saturated sunset hues roll off without shifting hue.
	// 0 = off (raw clip); higher = lower knee + longer shoulder = more of the brightness range mapped
	// into the visible gradient.
	const float rolloff = clamp(u_skySunParams.z, 0.0, 2.0);
	if (rolloff > 0.0)
	{
		// Knee saturates at rolloff = 1; past that only the headroom keeps growing (gentler shoulder).
		const float knee = mix(1.0, min(u_rolloffKnee, 0.99), min(rolloff, 1.0)); // where compression starts
		const float headroom = mix(0.35, u_eclipseParams.y, rolloff);             // brightness range the shoulder absorbs
		float lum = max(color.r, max(color.g, color.b));
		if (lum > knee)
		{
			float compressed = knee + (1.0 - knee) * (1.0 - exp(-(lum - knee) / ((1.0 - knee) * headroom)));
			color *= compressed / lum;
		}
	}
	*/
	out_color = vec4(color, 1.0);
}
