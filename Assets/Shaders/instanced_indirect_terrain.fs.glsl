#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// Procedural terrain variant of instanced_indirect.fs.glsl (EPipelineIndex::TerrainLit): same lighting
// core (instanced_indirect_lit.inc.glsl), but the material albedo is replaced by climate-picked texture
// splatting - procedural terrain chunks carry no textures of their own.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal; // geometric (interpolated vertex) normal; terrain builds its own tangent bases
layout (location = 2) in vec4 in_terrainFields; // VS-evaluated baked fields: x = macro altitude, y = temperature C, z = humidity, w = water level
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (location = 0) out vec4 out_color;

#include "instanced_indirect_lit.inc.glsl"

// Terrain wetness clipmap (binding 18, written by terrain_wetness.cs.glsl): the decaying memory of
// where water touched the ground - the swash tongue, rain. Applied on top of the splat in main().
#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl"

// Sky for the film's mirror ray - the same per-frame bake of mirrorSkyRadiance the ocean's
// reflectedSkyRadiance fetches, so the film reflects the same sky the water next to it does. No sun
// disc: the GGX glint is its reflection.
vec3 terrainReflectedSkyRadiance(vec3 dir)
{
	return textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR), 0.0).rgb;
}

// Standing water on the ground, shaded the way the ocean shader shades its surface so the two meet
// seamlessly at the depth-buffer intersection: the lit ground is the refracted BODY (a film has no
// depth to absorb through), a Fresnel-weighted sky/scene reflection sits on it, and the sun glint is
// the ocean's dielectric GGX at the ocean's roughness. The film normal is the ground normal tilted
// toward the LIVE FFT wave slopes ("Surface water waviness"), so ripples run across the pools in step
// with the water beside them. body = the ground's lit colour; mask = how much of the pixel is film.
// depth = calm water level above the ground here (negative on dry land), waterLevel = that calm level,
// wet = the raw wetness (the virtual water depth follows it, not the mask, so the tint keeps thinning
// above the mask's ramp too).
vec3 terrainWaterFilm(vec3 body, vec3 worldPos, vec3 V, vec3 geoN, float footprint, float mask, float depth, float waterLevel, float wet)
{
	// Wave slopes, LEAN variance and fold Jacobian of the RAW cascade sum. The lace keys on the raw
	// Jacobian like the ocean's surf band; the normal's terms take the ocean's one depth weight below.
	vec2 slopeSum = vec2(0.0), varSum = vec2(0.0);
	float rxx = 0.0, rzz = 0.0, rxz = 0.0;
	float turbulence = 0.0;
	for (int c = 0; c < OCEAN_CASCADES; ++c)
	{
		const float L = u_oceanParams2[c];
		const float lod = max(log2(max(footprint, 1e-3) * float(OCEAN_FFT_SIZE) / L), 0.0);
		const vec2 uvc = worldPos.xz / L;
		const vec4 g = textureLod(u_uwOceanMaps, vec3(uvc, float(OCEAN_CASCADES + c)), lod); // (dh/dx, dh/dz, dDx/dx, dDz/dz)
		const vec4 m = textureLod(u_uwOceanMaps, vec3(uvc, float(2 * OCEAN_CASCADES + c)), lod); // LEAN moments, accel, turbulence (c0)
		const float dxz = textureLod(u_uwOceanMaps, vec3(uvc, float(c)), lod).w;                 // displacement layer w = dDx/dz
		slopeSum += g.xy;
		varSum += max(m.xy - g.xy * g.xy, vec2(0.0)); // slope variance lost to mip filtering (Bruneton 2010)
		rxx += g.z; rzz += g.w; rxz += dxz;
		if (c == 0)
			turbulence = m.w; // the accumulated breaking memory rides cascade 0's moments
	}
	// The ocean's ONE depth weight (oceanShoreWeights, underwater_light.inc.glsl): 1 in open water,
	// easing to the swash base (amplitude x sea x land fades) across the approach band. On dry sand the
	// weight IS the swash base, which is the water's surface shape at the waterline, so the film
	// continues the water.
	const float reach = max(u_oceanParams7.w, 0.01);
	float swashW, w;
	oceanShoreWeights(depth, waterLevel, swashW, w);
	slopeSum *= w;
	varSum *= w * w;
	const float sxx = rxx * w, szz = rzz * w, sxz = rxz * w; // the normal's Jacobian follows the weighted chop
	const float chop = u_oceanParams0.w;
	const float jxx = 1.0 + chop * sxx, jzz = 1.0 + chop * szz;
	// Floor + rational soft limit, as the ocean: near folds the raw division explodes the slope.
	vec2 slope = slopeSum / max(vec2(jxx, jzz), vec2(0.6));
	slope /= 1.0 + 0.2 * length(slope);
	const vec2 slopeVar = varSum;

	const float ns = u_oceanParams1.w;
	const vec3 waveN = normalize(vec3(-slope.x * ns, 1.0, -slope.y * ns));
	// Waviness scales with the wetness (mask): a fresh film ripples with the water, a fading one lies
	// flat on the sand.
	vec3 N = normalize(mix(geoN, waveN, u_terrainWetParams6.z * mask));
	if (dot(N, V) < 0.0)
		N = geoN; // a ripple tilted away from the viewer: fall back to the ground's own normal
	const float NoV = clamp(dot(N, V), 1e-3, 1.0);
	const float x = 1.0 - NoV;
	const float x2 = x * x;
	const float F = 0.02 + 0.98 * (x2 * x2 * x); // Schlick, water F0

	const vec3 up = normalize(u_skyUp);
	const vec3 L = u_sunDirection.xyz;
	const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * u_eclipseParams.x; // = the ocean's sunTint
	const vec3 ambientSky = textureLod(u_skyMap, vec3(skyMapUV(up), SKY_MAP_LAYER_GI), 0.0).rgb; // skyRadiance(up): constant per frame, one fetch

	// Body: the ground seen through the film - Beer-Lambert absorbed along the refracted path through a
	// virtual "Surface water depth" of water (a real film is too thin to tint; the ocean beside it has a
	// shallow column, and this is what keeps the two the same colour at the waterline), with the
	// ocean's in-scatter filling in what was absorbed. Exactly the ocean shader's body mix.
	vec3 tinted = body;
	if (u_terrainWetParams6.w > 0.0)
	{
		const vec3 refrDir = refract(-V, N, 1.0 / 1.33);
		const float path = u_terrainWetParams6.w * wet / max(-refrDir.y, 0.2);
		const vec3 T = exp(-u_oceanAbsorption.rgb * path);
		const vec3 inscatter = u_oceanScatter.rgb * u_oceanScatter.w * (ambientSky + sunTint * max(L.y, 0.0) / PI);
		tinted = body * T + inscatter * (1.0 - T);
	}

	// Shoreline lace coverage, up front (it is cheap) so the whitewater below is lit only where the milk
	// or the lace will show it: the ocean's surf-band foam at its waterline target (column ~ 0 -> target
	// 1), realized through the raw fold Jacobian with the same bias/threshold, eased toward "Shore foam
	// max" - so the lace the water carries onto the sand is the same lace it wears at the edge. Only at
	// and below the calm water level (eased out over half a swash reach above it): lace is what the surf
	// leaves at the waterline, not something the wet sand above it keeps.
	float foam = 0.0;
	if (u_oceanParams5.z > 0.0)
	{
		const float Jraw = (1.0 + chop * rxx) * (1.0 + chop * rzz) - chop * rxz * chop * rxz;
		const float target = mask * (1.0 - smoothstep(0.0, 0.5 * reach, -depth));
		const float b = mix(0.75, 1.45, target) + u_oceanParams8.y;
		const float foamMax = max(u_oceanParams7.y, 1e-3);
		foam = foamMax * (1.0 - exp(-target * (1.0 - smoothstep(b - 0.4, b + 0.4, Jraw)) / foamMax));
	}
	// Entrained bubbles: the ocean's accumulated turbulence (the decaying memory of breaking, strongest
	// exactly at the shore) turns the water milky ("Turbidity") and rougher. The surf beside the film
	// carries it, so the film carries it too.
	const float milk = clamp(turbulence * u_oceanParams5.y, 0.0, 1.0);
	// Whitewater: lambertian foam lit by the pixel's ALREADY-RESOLVED sun radiance AT THE SURFACE
	// (g_sunRadianceSurface - the lit core's shadow visibility, no second shadow evaluation, and NOT the
	// underwater caustic/absorption factor: the foam floats on the film, it is not the seabed under it)
	// plus sky and ambient. The ocean's whitewater; skipped where neither the milk nor the lace would
	// show it.
	vec3 whitewater = vec3(0.0);
	if (milk > 0.003 || foam > 0.003)
		whitewater = doLight(g_sunRadianceSurface, L, V, N, vec3(0.0), u_oceanFoam.rgb * INV_PI, 0.0, 0.85, 0.7225)
			+ u_oceanFoam.rgb * (ambientSky + u_ambientColor);
	tinted = mix(tinted, whitewater * 0.55, milk);

	// The ocean's microfacet alpha: perceptual roughness^2, plus the LEAN slope variance (scaled by
	// "Glint filtering") that stretches the glitter toward the horizon, plus the turbulence
	// micro-roughness. No spec-AA term: that is a screen derivative, undefined in this branch.
	// NOTE: the lit core's `roughness` parameter IS the GGX alpha (DistributionGGX takes its square as
	// a2), so alphaF goes in directly - passing a perceptual value there gave the film a wider glint
	// than the water beside it.
	const float baseRough = clamp(u_oceanAbsorption.w, 0.02, 1.0);
	const float slopeVariance = 0.5 * (slopeVar.x + slopeVar.y) * (ns * ns);
	const float alphaSq = baseRough * baseRough * baseRough * baseRough
		+ 2.0 * slopeVariance * u_oceanParams6.y + turbulence * u_oceanParams5.y * 0.35;
	const float alphaF = clamp(sqrt(alphaSq), 0.02, 1.0);

	// Reflection: sky mirror, roughness-blurred toward the average sky like the ocean does.
	vec3 R = reflect(-V, N);
	R.y = max(R.y, 0.02);
	R = normalize(R);
	const float reflBlur = clamp(alphaF * 2.0 - 0.05, 0.0, 0.6);
	const vec3 reflection = mix(terrainReflectedSkyRadiance(R), ambientSky, reflBlur);

	vec3 color = mix(tinted, reflection, F);
	// Sun glint: the ocean's dielectric GGX (F0 0.02, alphaF) on the pixel's resolved sun radiance AT
	// THE SURFACE - shadow-gated exactly as the ground under it was, without a second shadow evaluation,
	// but NOT underwater-gated: the glint is the sun mirrored off the film, before the water, so the
	// seabed's caustic focus has no business in it (with g_sunRadiance it warped into the lobe as
	// distorted caustics on every filmed pixel under a wave). Specular only (no diffuse term: the body
	// already carries the ground's diffuse light, caustics included).
	color += doLight(g_sunRadianceSurface, L, V, N, vec3(0.02), vec3(0.0), 0.0, alphaF, alphaF * alphaF);
	if (foam > 0.003)
		color = mix(color, whitewater, foam);
	return mix(body, color, mask);
}

// The splat itself (TerrainFields / TerrainSample / terrainSplat) is terrain_splat.inc.glsl - shared with
// the ocean shader, which evaluates it at refraction-ray hits so the seabed IS this terrain.
#include "terrain_splat.inc.glsl"

// Baked terrain fields at one point (terrain-data cascades), mild-climate fallbacks without a map.
// altitude is the MACRO band: height far above it = mountain crag; height ~ altitude = flatland.
// Evaluated PER VERTEX in instanced_indirect_terrain.vs.glsl (every field is band-limited far below any
// LOD's vertex lattice, and temperature is linear in height, so interpolation is exact - see the comment
// there) and read from the in_terrainFields interpolant. This dropped ~6-10 texel fetches per pixel.
TerrainFields terrainFields()
{
	TerrainFields f;
	f.altitude = in_terrainFields.x;
	f.temperature = in_terrainFields.y;
	f.humidity = in_terrainFields.z;
	f.waterLevel = in_terrainFields.w;
	return f;
}

// --- Terrain field debug view: set a mode, F5. Draws the baked data map instead of shading it. ---
//   1 = temperature  heat ramp -25..+50 C, 5 C contours; MAGENTA = freezing line, CYAN + darkened
//                    fill = the live snow line. No contours at all = the field is stuck.
//   2 = humidity     black -> white, 0.1 contours
//   3 = crag relief  |height - altitude|: black 0 -> white 200 m (drives the rock layer)
//   4 = altitude     black sea level -> white 5 km
//   5 = cascade      green = near, red = far, yellow = crossfade band
#define TERRAIN_DEBUG_MODE 0

#if TERRAIN_DEBUG_MODE != 0
vec3 debugHeatRamp(float t)
{
	t = clamp(t, 0.0, 1.0);
	const float s = t * 4.0;
	if (s < 1.0) return mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), s);
	if (s < 2.0) return mix(vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0), s - 1.0);
	if (s < 3.0) return mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), s - 2.0);
	return mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), s - 3.0);
}

// Dark line at every multiple of `spacing`.
float debugContour(float v, float spacing)
{
	const float f = abs(fract(v / spacing - 0.5) - 0.5) / fwidth(v / spacing);
	return 1.0 - clamp(1.0 - f, 0.0, 1.0) * 0.7;
}

// Single isoline at `v == level`; /fwidth keeps it a constant width ON SCREEN regardless of gradient.
float debugIsoline(float v, float level, float widthPx)
{
	const float d = abs(v - level) / max(fwidth(v), 1e-6);
	return 1.0 - smoothstep(0.0, widthPx, d);
}

vec3 terrainDebugColor(TerrainFields f, vec3 worldPos)
{
#if TERRAIN_DEBUG_MODE == 1
	vec3 col = debugHeatRamp((f.temperature + 25.0) / 75.0) * debugContour(f.temperature, 5.0);
	const float snowLineC = u_terrainTexParams3.w; // the LIVE snow tweak, not a mirrored constant
	if (f.temperature <= snowLineC)
		col = mix(col, vec3(0.05), 0.75);
	col = mix(col, vec3(0.0, 1.0, 1.0), debugIsoline(f.temperature, snowLineC, 1.5));
	col = mix(col, vec3(1.0, 0.0, 1.0), debugIsoline(f.temperature, 0.0, 2.0));
	return col;
#elif TERRAIN_DEBUG_MODE == 2
	return vec3(clamp(f.humidity, 0.0, 1.0)) * debugContour(f.humidity, 0.1);
#elif TERRAIN_DEBUG_MODE == 3
	const float relief = abs((worldPos.y - u_terrainParams.z) - f.altitude);
	return debugHeatRamp(relief / 200.0) * debugContour(relief, 25.0);
#elif TERRAIN_DEBUG_MODE == 4
	return vec3(clamp(f.altitude / 5000.0, 0.0, 1.0)) * debugContour(f.altitude, 250.0);
#else // 5 = which cascade fed this pixel
	const vec2 uv0 = (worldPos.xz - u_fogParams5.xy) * u_fogParams3.y + 0.5;
	const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
	const float nearW = u_fogParams5.z > 0.0 ? 1.0 - smoothstep(0.42, 0.48, edge) : 1.0;
	return mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), nearW);
#endif
}
#endif

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	const vec3 toView = u_viewPos - in_pos;
	const float viewDist = length(toView);
	const vec3 V = toView / viewDist;
	const vec3 geoN = normalize(in_normal);
	const TerrainFields fields = terrainFields();
	// Pixel footprint for the surface-water wave taps; a derivative, so taken here in uniform flow.
	const float wetFootprint = length(fwidth(in_pos.xz));

#if TERRAIN_DEBUG_MODE != 0
	out_color = vec4(terrainDebugColor(fields, in_pos), 1.0); // unlit: the field itself, not its shading
	return;
#endif

	TerrainSample surf = terrainSplat(in_pos, geoN, fields);
	// Wetness: darker, glossier ground where water touched it recently (the clipmap holds the memory).
	// Deliberately the MAP ALONE - no instantaneous "under the live surface" override here: the map
	// accumulates at the wet-in rate (slower on slopes), so ground under a wave soaks up visibly
	// rather than snapping to wet, and a cliff face a wave splashes only ever gets damp. The lit core
	// still lights the covered pixels as underwater from the live surface, so the water itself reads.
	// Wetness AFTER the slope drain, for the surface-water pass below. Deliberately the raw field and
	// NOT the pooled film: the field is spatially smooth (bilinear + diffusion) and highest where the
	// water left most recently, so the water look covers the whole tongue behind a wave and fades out
	// smoothly with it - the pools are a noise pattern, and keying on them drew random water blobs.
	float wetSurface = 0.0;
	// We already sampled the terrain data cascade for fields.waterLevel; hand it to the lit core so its
	// underwater test reuses it instead of re-fetching the same cascade - then resolve that test NOW
	// (doSunLight would, but the gloss below needs it first; it runs once per pixel either way).
	// aboveLive: 0 on ground under the LIVE water surface right now - the same test that switches the
	// caustics on - so neither the wet gloss nor the surface film shows through the ocean's own water
	// (a second sky reflection under the first). The estimate sits UNDER the drawn ocean edge (no choppy
	// XZ offset, no tongue thickness), so the gate is pushed "Live surface margin" (u_terrainWetParams7.y)
	// below it - without that a bare band of ground showed between the waterline and the film - and
	// eases in over the 10 cm above that.
	g_waterLevelOverride = fields.waterLevel;
	resolveLiveDepth(in_pos);
	const float liveMargin = u_terrainWetParams7.y;
	const float aboveLive = 1.0 - smoothstep(liveMargin - 0.1, liveMargin, g_liveDepthBelow);
	if (terrainWetPresent())
	{
		float wet = terrainWetnessAt(in_pos.xz);
		// Slope drain: water runs off a face instead of soaking in, so steep ground dries faster. The
		// stored wetness decays as exp(-t / tau), so wet^k IS a k-times faster decay - evaluated here per
		// pixel against the exact geometric normal (the map's 8 m texels cannot see a cliff face), with no
		// extra state. k = 1 + slope * drain: at drain 4 a 45-degree face dries ~2.2x faster, a wall 5x.
		// Ground under water is 1 either way; the film only leaves faster once the wave has gone.
		const float slope = 1.0 - clamp(geoN.y, 0.0, 1.0);
		wet = pow(wet, 1.0 + slope * u_terrainWetParams5.x);
		wetSurface = wet;
		// Pooling: draining water retreats into the crevices. A world-anchored value fBm stands in for the
		// micro-relief; a point is POOLED where the noise sits below the wetness, so at full wetness the
		// whole surface is filmed, and as it dries only the low spots (low noise) keep their film - the
		// blobby, breaking-up gloss of a beach draining. The ground between the pools is merely DAMP:
		// darkened by the wetness itself, with only a fraction of the roughness drop.
		float pool = wet;
		if (u_terrainWetParams4.x > 0.0 && wet > 0.0 && wet < 1.0)
		{
			const float n = terrainFbm(in_pos.xz * u_terrainWetParams4.x) * 0.5 + 0.5;
			const float soft = max(u_terrainWetParams4.y, 1e-3);
			// Pool hold: the crevices keep their water long after the surface between them has drained,
			// so the pool threshold lags the wetness - wet^(1/hold): at hold 2 the pools are still half
			// there when the wetness itself is down to a quarter. The threshold is scaled so that AT the
			// surface water threshold (where the ground is drawn as water) it already clears the noise's
			// whole range plus the soft edge: every pixel is pooled, one uniform film. The noise only
			// starts to break through below that wetness, and the two looks hand over without a seam.
			const float hold = max(u_terrainWetParams4.w, 1.0);
			const float full = pow(max(u_terrainWetParams6.x, 1e-3), 1.0 / hold); // held wetness at the surface water threshold
			const float level = pow(wet, 1.0 / hold) * ((1.0 + soft) / full);
			pool = 1.0 - smoothstep(level - soft, level + soft, n);
		}
		// Two darkening layers. DAMP is the soaked ground everywhere, pools and the spaces between them
		// alike: a plateau that holds while the wetness is above the damp knee (0.25 = ~1.4 dry times),
		// then fades smoothly to dry. FILM is standing water on top of it - the whole surface just after a
		// wave (the spike, above the spike-start wetness) and the pools once it drains. Fully wet ground
		// carries both, so its albedo is the product of the two scales.
		const float damp = smoothstep(0.0, max(u_terrainWetParams5.z, 1e-3), wet);
		const float spike = smoothstep(min(u_terrainWetParams5.w, 0.99), 1.0, wet);
		const float film = max(spike, pool);
		// Under the live water surface (aboveLive) the ground is seabed: it keeps the darkening - the
		// ocean's traced seabed carries the same - but no gloss (a sky reflection has no business under
		// the ocean's own).
		const float gloss = max(film, damp * u_terrainWetParams4.z) * aboveLive;
		surf.albedo *= mix(1.0, u_terrainWetParams5.y, damp) * mix(1.0, u_terrainWetParams2.y, film);
		surf.rough = mix(surf.rough, u_terrainWetParams2.z, gloss); // a water film flattens the microfacets
	}
	// surf.ao = baked texture AO on top of the screen-space term (ambient/indirect only).
	vec3 color = computeLitColor(in_pos, V, surf.normal, surf.albedo, surf.rough, surf.metal, surf.ao);
	// Surface water: where the wetness is still near full the ground is drawn AS WATER - the ocean
	// shader's own surface terms over the lit ground - so the ocean's depth-buffer intersection with
	// the sand lands on ground that already looks like the water leaving it, instead of a hard line.
	// The mask ramps up to the surface water threshold (fully water) over the smooth wetness field,
	// starting at the HIGHER of the wet spike start (where the standing-film darkening begins, so the
	// water look never starts before the film does) and threshold - softness (so softness narrows the
	// ramp). It blends out over the tongue as the field decays, never at an edge.
	if (wetSurface > 0.0)
	{
		const float th = u_terrainWetParams6.x;
		const float start = min(max(u_terrainWetParams5.w, th - u_terrainWetParams6.y), th - 1e-3);
		// Faded out on ground under the LIVE water surface (aboveLive, resolved above with the gloss: the
		// ocean draws the water there, a film would only double it).
		const float waterMask = smoothstep(start, th, wetSurface) * aboveLive;
		if (waterMask > 0.0)
			color = terrainWaterFilm(color, in_pos, V, geoN, wetFootprint, waterMask, fields.waterLevel - in_pos.y, fields.waterLevel, wetSurface);
	}
	out_color = vec4(color, 1.0); // opaque terrain
}
