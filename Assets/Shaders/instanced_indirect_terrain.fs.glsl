#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// Procedural terrain variant of instanced_indirect.fs.glsl (EPipelineIndex::TerrainLit): same lighting
// core (instanced_indirect_lit.inc.glsl), but the material albedo is replaced by climate-picked texture
// splatting - procedural terrain chunks carry no textures of their own.
// The surface-water film over this ground is terrain_film.fs.glsl (EPipelineIndex::TerrainOverlay); the
// shared part of the two is terrain_common.inc.glsl.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal; // geometric (interpolated vertex) normal; terrain builds its own tangent bases
layout (location = 2) in vec4 in_terrainFields; // VS-evaluated baked fields: x = macro altitude, y = temperature C, z = humidity, w = water level
#ifdef TERRAIN_TESS
// The tessellated terrain: in_pos is DISPLACED, this is the flat mesh under it - what the shadow map and the TLAS
// hold. Every shadow / light evaluation uses it (terrain_tess.tes.glsl has why); the splat samples in_pos.
layout (location = 3) in vec3 in_meshPos;
#define TERRAIN_LIT_POS in_meshPos
#else
#define TERRAIN_LIT_POS in_pos
#endif
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (location = 0) out vec4 out_color;
// The motion target: write-masked (the terrain is static), but the opaque family's DGC set needs one fragment
// output interface (RendererVKLayout::PIPELINE_TRANSPARENT_MASK). Written once at the top of main.
layout (location = 1) out vec4 out_motion;
// The ground's sun shadow at the top of main, from the geometric normal (see instanced_indirect_lit.inc.glsl).
#define SUN_SHADOW_FIRST

#include "instanced_indirect_lit.inc.glsl"
#include "terrain_common.inc.glsl"
#include "grass.inc.glsl" // the canopy shading of the ground under the grass (grassGroundCanopy)

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
	const float snowLineC = u_terrainTex_snowTempNone; // the LIVE snow tweak, not a mirrored constant
	if (f.temperature <= snowLineC)
		col = mix(col, vec3(0.05), 0.75);
	col = mix(col, vec3(0.0, 1.0, 1.0), debugIsoline(f.temperature, snowLineC, 1.5));
	col = mix(col, vec3(1.0, 0.0, 1.0), debugIsoline(f.temperature, 0.0, 2.0));
	return col;
#elif TERRAIN_DEBUG_MODE == 2
	return vec3(clamp(f.humidity, 0.0, 1.0)) * debugContour(f.humidity, 0.1);
#elif TERRAIN_DEBUG_MODE == 3
	const float relief = abs((worldPos.y - u_terrainLive_seaLevel) - f.altitude);
	return debugHeatRamp(relief / 200.0) * debugContour(relief, 25.0);
#elif TERRAIN_DEBUG_MODE == 4
	return vec3(clamp(f.altitude / 5000.0, 0.0, 1.0)) * debugContour(f.altitude, 250.0);
#else // 5 = which cascade fed this pixel
	const vec2 uv0 = (worldPos.xz - u_terrainLive_mapCentre) * u_terrainLive_mapInvNearSize + 0.5;
	const float edge = max(abs(uv0.x - 0.5), abs(uv0.y - 0.5));
	const float nearW = u_terrainLive_mapInvFarSize > 0.0 ? 1.0 - smoothstep(0.42, 0.48, edge) : 1.0;
	return mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), nearW);
#endif
}
#endif

#ifdef TERRAIN_TESS
// The normal of the CONTINUOUS displaced surface at this pixel - terrain_tess.tes.glsl's displacement function
// (the same layers, height composite, depth and falloff, evaluated at the undisplaced point) differentiated by
// forward differences along world X and Z: the shading follows the geometry 1:1 at any distance (a facet-normal
// tilt showed the tessellated triangles as facets close up). L = the coverages at in_meshPos, shared with the
// splat; the relief taps take explicit gradients (non-uniform flow).
// The step AND the mip are the TES's own footprint (the target edge at max(distance, freeze distance)), not the
// pixel's: at a pixel step close up, mip 0 magnified the height map's TEXEL GRID (bilinear = a constant
// gradient per texel, jumping at every texel edge) and its BC4 steps into a pixelated contour pattern. Finer
// detail than the geometry's is the normal maps' (kept at half strength).
// strength = how much of the relief is displaced here (0..1), for the normal map's fade-down.
vec3 terrainTessPixelNormal(vec3 N, TerrainLayers L, out float strengthOut)
{
	strengthOut = 0.0;
	// The CENTRE view, as the TES (both VR eyes see the same displaced surface, so the same normal).
	const float dist = distance(in_meshPos, u_views_viewPos[VIEW_CENTER].xyz);
	const float fadeStart = u_terrainTess_fadeStart, fadeEnd = u_terrainTess_fadeEnd;
	if (dist >= fadeEnd || u_terrainLive_splatBase < 0.0 || u_terrainLive_numGround < 1.0)
		return N;
	const float t = clamp((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3), 0.0, 1.0);
	const float strength = (1.0 - pow(t, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, N.y); // the HEIGHT falloff, as the TES
	const float depth = mix(mix(u_terrainTess_depthGround, u_terrainTess_depthRock, float(L.rockW)), u_terrainTess_depthGround, float(L.snowW)) * strength;
	if (depth <= 1e-4)
		return N;
	strengthOut = strength;
	// The TES's footprint (terrain_tess.tes.glsl): the projection's y scale is row 1 of the centre mvp's 3x3.
	const mat4 centreMvp = u_views_mvp[VIEW_CENTER];
	const float projY = length(vec3(centreMvp[0][1], centreMvp[1][1], centreMvp[2][1]));
	const float e = max(max(dist, u_terrainTess_freezeDistance) * 2.0 * u_terrainTess_targetEdgePx / max(projY * u_screenSize.y * u_viewportRect.w, 1.0), 1e-3);
	const vec3 h3 = vec3(terrainReliefAt3(L, in_meshPos.xz, e, vec2(e, 0.0), vec2(0.0, e))); // at xz, xz + (e, 0), xz + (0, e)
	const float invE = 1.0 / e;
	const float hx = (h3.y - h3.x) * invE;
	const float hz = (h3.z - h3.x) * invE;
	// On the local base plane a step of 1 m in world X / Z is Tx / Tz (N.y >= ~0.35 wherever depth > 0).
	const vec3 Tx = vec3(1.0, -N.x / N.y, 0.0) + N * (depth * hx);
	const vec3 Tz = vec3(0.0, -N.z / N.y, 1.0) + N * (depth * hz);
	return normalize(cross(Tz, Tx));
}
#endif

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	out_motion = vec4(0.0); // write-masked: static
	// coverN: the smooth mesh normal - the layer coverages and the slope drain read it (the relief's bumps must
	// not scatter rock / snow). geoN: the shading base - the same, except on the tessellated terrain.
	const vec3 coverN = normalize(in_normal);
	// THE SHADOW FIRST, before the layers, the splat and the wetness - only the position and coverN live. From the
	// smooth MESH normal, also on the tessellated terrain: the shadow is evaluated at the flat mesh
	// (TERRAIN_LIT_POS), which is what the shadow map and the TLAS hold, so its own normal carries the offset. The
	// displaced faces turned away from the sun are gated below (reliefSunGate), and doSunLight's facing test on
	// the shading normal still applies. Never the splat's normal maps: they must not bend the shadow's offset.
	g_sunShadowFirst = dot(coverN, u_sunDirection.xyz) > 0.0 ? sunShadowVisibility(TERRAIN_LIT_POS, coverN) : 0.0;
	const TerrainFields fields = terrainFields();
#ifdef TERRAIN_TESS
	// The shading base: the displaced surface's own normal (terrainTessPixelNormal). reliefStrength fades the
	// normal maps down below (they carry the same relief again).
	// The coverages ONCE, at the undisplaced point the TES displaced from: the pixel normal and the splat share
	// them (terrainLayers is the costly part: the climate walk, the crag fBm).
	const TerrainLayers tessLayers = terrainLayers(in_meshPos, coverN, fields);
	float reliefStrength;
	const vec3 geoN = terrainTessPixelNormal(coverN, tessLayers, reliefStrength);
	// THE GRASS CANOPY over this ground (grass.inc.glsl): its sun and ambient, from the same layer coverages as the
	// grass placement. Formed here so only these two halves stay live across the splat, not the layers' picks.
	const f16vec2 grassCanopy = f16vec2(grassGroundCanopy(tessLayers, in_meshPos, coverN));
#else
	const vec3 geoN = coverN;
	const TerrainLayers layers = terrainLayers(in_pos, coverN, fields);
	const f16vec2 grassCanopy = f16vec2(grassGroundCanopy(layers, in_pos, coverN)); // see the tessellated path
#endif
	const float16_t one = float16_t(1.0);

#if TERRAIN_DEBUG_MODE != 0
	out_color = vec4(terrainDebugColor(fields, in_pos), 1.0); // unlit: the field itself, not its shading
	return;
#endif

#ifdef TERRAIN_TESS
	TerrainSample surf = terrainSplatLayers(in_pos, geoN, tessLayers);
#else
	TerrainSample surf = terrainSplatLayers(in_pos, geoN, layers);
#endif
	// THE GROUND'S WET LOOK: one darkening and one roughness drop, both on the wetness field alone (the
	// clipmap's memory of where water stood, slope-drained per pixel). No instantaneous "under the live
	// surface" override: the field accumulates at the wet-in rate, so ground under a wave soaks up visibly
	// instead of snapping to wet, and a cliff a wave splashes only ever gets damp.
	float16_t wet, aboveLive;
	terrainWetness(coverN, fields, 0.0, wet, aboveLive);
	float16_t skyReflW = float16_t(0.0); // the wet ground's sky reflection weight (after the lighting, below)
	if (terrainWetPresent())
	{
		// Damp ground: darker and glossier with the wetness itself. The STANDING water's own highlights are
		// the film's (the overlay draws exactly where water stands), so nothing here tries to double them.
		// Under the live ocean the ground is seabed: it keeps the darkening - the ocean's traced seabed
		// carries the same - and takes "Underwater roughness" instead of the wet gloss (a sky reflection has
		// no business under the ocean's own; the ocean's edge fade shows this ground).
		// The darkening and the gloss each hold full above their own wetness THRESHOLD ("Darkening threshold",
		// "Roughness threshold": u_terrainWater_darkeningThreshold / roughnessThreshold) - a plateau while the
		// ground is soaked - and fade smoothly to dry below it.
		const float16_t darkAmount = smoothstep(float16_t(0.0), float16_t(u_terrainWater_darkeningThreshold), wet);
		const float16_t glossAmount = smoothstep(float16_t(0.0), float16_t(u_terrainWater_roughnessThreshold), wet);
		// THE DRYING PATTERN: ground does not dry uniformly - a beach breaks into metre-scale blotches
		// (porosity, micro-drainage) that dry first while the rest stays dark. P (0..1, low = holds its water
		// longest) is a world-anchored value fBm of "Drying pattern size (m)" (u_terrainWater_invDryingPatternSize
		// = 1 / size), with "Drying pattern relief" (dryingPatternRelief) of the splat's height composite mixed in
		// for the fine breakup at the island edges. Each amount becomes a LEVEL through P: below it wet, above it
		// a dry ISLAND, and the islands grow as the level sinks. The darkening and the gloss share P, so an island
		// loses its gloss first (a higher roughness threshold), then its darkness. The soft band around each level
		// is its own: "Darkening edge" (darkeningEdge) - wide, the darkening fades over a larger range - and
		// "Roughness edge" (roughnessEdge) - crisp gloss islands. "Drying pattern" (dryingPattern, 0..1) mixes
		// from the uniform amount (0) to the patterned
		// one, and fades out where the blotches shrink to a few pixels (the noise would shimmer).
		// (The relief ALONE, tried before, tiles at the splat texture's scale: speckle, not drying patches.)
		const float16_t darkBand = float16_t(max(u_terrainWater_darkeningEdge, 1e-3));
		const float16_t band = float16_t(max(u_terrainWater_roughnessEdge, 1e-3));
		const float pixelWidth = length(fwidth(TERRAIN_LIT_POS.xz)); // m (the drying pattern's and the glints' fades)
		const float footprint = pixelWidth * u_terrainWater_invDryingPatternSize; // pattern cells per pixel
		const float16_t patternW = float16_t(u_terrainWater_dryingPattern * (1.0 - smoothstep(0.15, 0.4, footprint)));
		float16_t damp = darkAmount, glossW = glossAmount;
		if (patternW > float16_t(0.0))
		{
			// The fBm stays 32-bit (its hash is fract() of large products); stretched from its central
			// bunching toward the full 0..1 by "Drying pattern contrast" (dryingContrastHalf, pre-halved): higher =
			// more of the ground at the extremes, so the islands separate more strongly (clamped: fully dry / fully wet).
			const float n = clamp(terrainFbm(TERRAIN_LIT_POS.xz * u_terrainWater_invDryingPatternSize) * u_terrainWater_dryingContrastHalf + 0.5, 0.0, 1.0);
			const float16_t P = mix(float16_t(n), surf.height, float16_t(u_terrainWater_dryingPatternRelief));
			damp = mix(damp, smoothstep(P - darkBand, P + darkBand, darkAmount * (one + darkBand + darkBand) - darkBand), patternW);
			glossW = mix(glossW, smoothstep(P - band, P + band, glossAmount * (one + band + band) - band), patternW);
		}
		surf.albedo *= mix(one, float16_t(u_terrainWater_darkening), damp);
		// Where the FILM stands over this ground: the same pool level through the relief the film's coverage
		// uses, faded over its "Edge fade (m)" (the ground/beach relief depth as the metres). The ground there is
		// UNDER water - its wet gloss would sit beneath the film's own surface - so it takes the underwater
		// roughness, as under the live ocean (not a wetness gate: this follows the film's actual outline).
		const float16_t poolOver = (float16_t(terrainPoolLevel(float(wet), coverN.y)) - surf.height)
			* float16_t(u_terrainTess_depthGround / max(u_terrainWater_edgeFade, 1e-4));
		const float16_t underFilm = clamp(poolOver, float16_t(0.0), one);
		// "Wet roughness" with the gloss, "Underwater roughness" under the film and the live ocean.
		const float16_t submerged = max(one - aboveLive, underFilm);
		surf.rough = mix(mix(surf.rough, float16_t(u_terrainWater_wetRoughness), glossW), float16_t(u_terrainWater_underwaterRoughness), submerged);
		// GLINTS ("Glint size (m)" u_terrainWater_invGlintSize = 1 / size, "Glint coverage" glintCoverage, "Glint
		// roughness" glintRoughness): wet sand is not uniformly glossy - beaded water and flat wet grains catch the sun in small sharp
		// points. Sparse world-anchored patches - a single-octave value noise over glint-size cells, its peaks
		// (the high corner hashes) thresholded so roughly "Glint coverage" of the ground qualifies - drop the
		// roughness to a near-mirror alpha, on the wet gloss only (not under the film or the ocean). Small and
		// sparse, so the overall specular barely changes. Faded out where the patches shrink toward a pixel:
		// there they would only shimmer.
		if (u_terrainWater_glintCoverage > 0.0)
		{
			const float glintFade = 1.0 - smoothstep(0.3, 0.7, pixelWidth * u_terrainWater_invGlintSize);
			if (glintFade > 0.0)
			{
				const float n = terrainValueNoise(TERRAIN_LIT_POS.xz * u_terrainWater_invGlintSize);
				const float glint = smoothstep(1.0 - u_terrainWater_glintCoverage, 1.0 - 0.5 * u_terrainWater_glintCoverage, n) * glintFade;
				surf.rough = mix(surf.rough, float16_t(u_terrainWater_glintRoughness), float16_t(glint) * glossW * (one - submerged));
			}
		}
		// "Wet normal scale" (u_terrainWater_wetNormalScale): the normal map's tilt off the shading base, scaled with the
		// gloss - below 1 the water fills the micro relief (a sharper highlight: at full, the bumps scattered
		// it over the whole wet area), above 1 it is exaggerated. Kept on the base's side of the horizon (an
		// extrapolated tilt can pass it).
		{
			const f16vec3 baseN = f16vec3(geoN);
			f16vec3 n = baseN + (surf.normal - baseN) * mix(one, float16_t(u_terrainWater_wetNormalScale), glossW);
			n += baseN * max(float16_t(0.05) - dot(n, baseN), float16_t(0.0));
			surf.normal = normalize(n);
		}
		// The sky reflection's weight: the gloss above the live ocean, minus where the film stands (it mirrors
		// the sky itself there).
		// The GI's sky visibility (no sky mirrored under a roof or an overhang), folded in HERE: skyReflW is live
		// across the lit core anyway, and the lookup runs before its peak, not after it with the colour live.
		skyReflW = glossW * aboveLive * (one - underFilm);
		if (skyReflW > float16_t(0.0))
			skyReflW *= float16_t(giSkyVisibility(TERRAIN_LIT_POS, geoN));
	}
#ifdef TERRAIN_TESS
	// Where the relief is displaced, the normal maps carry the same relief a second time: their tilt re-lit the
	// faces the displacement turned away from the sun. Half of it is kept at full displacement (the fine detail
	// finer than the height composite). And the relief surface itself gates the sun: a face turned away from it
	// gets none, whatever the normal map says (through g_sunVisMaterial - the ground's sun only).
	surf.normal = normalize(mix(surf.normal, f16vec3(geoN), float16_t(0.5 * reliefStrength)));
	const float reliefSunGate = mix(1.0, smoothstep(0.0, 0.1, dot(geoN, u_sunDirection.xyz)), reliefStrength);
	g_sunVisMaterial = float16_t(float(g_sunVisMaterial) * reliefSunGate);
#endif
	// THE WET SKY REFLECTION: the lit core has no environment
	// specular (diffuse GI + the lights' GGX lobes only), so wet ground showed one sun highlight and read as
	// merely darker. The film's sky: the baked mirror sky along R, water Fresnel (F0 0.02), the film's
	// roughness-to-blur rule (rough wet ground mirrors nothing), the texture AO as the specular occlusion and
	// the mirror fog rule (the bake carries no fog). After the lighting. It costs the TESSELLATED ground 16 B
	// (72/16 -> 72/32; untessellated unchanged): the normal, roughness, AO and weight it reads stay live across
	// the lit core's peak. Measured alternatives, none better (tessellated / untessellated): the weight before
	// and the normal across 72/32 / 72/16; R.xz + the weight across 72/32 / 72/32; the whole reflection
	// before, its half result across 72/32 / 72/32.
	// surf.ao = baked texture AO on top of the screen-space term (ambient/indirect only).
	// V HERE, not at the top: its 3 registers are then not live across the shadow, the layers and the splat.
	// The grass canopy: the ground's sun only (the water film on top keeps its own) and its ambient.
	g_sunVisMaterial *= grassCanopy.x;
	surf.ao *= grassCanopy.y;
	const vec3 V = normalize(u_viewPos - in_pos);
	vec3 color = computeLitColor(TERRAIN_LIT_POS, V, surf.normal, surf.albedo, surf.rough, surf.metal, surf.ao);
	if (skyReflW > float16_t(0.0))
	{
		const f16vec3 Vh = f16vec3(V);
		const float16_t x = one - clamp(dot(surf.normal, Vh), float16_t(0.0), one);
		const float16_t x2 = x * x;
		const float16_t F = float16_t(0.02) + float16_t(0.98) * (x2 * x2 * x);
		const float16_t blur = clamp(surf.rough * float16_t(2.0) - float16_t(0.05), float16_t(0.0), one);
		const float16_t w = skyReflW * F * (one - blur) * surf.ao;
		if (w > float16_t(0.002))
		{
			vec3 R = reflect(-V, vec3(surf.normal));
			R.y = max(R.y, 0.02);
			R = normalize(R);
			const vec3 ambientSky = texelFetch(u_skyMap, SKY_MAP_GI_ZENITH_TEXEL, 0).rgb;
			const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * u_sunVisible;
			color += applyReflectionFogSky(terrainReflectedSkyRadiance(R), TERRAIN_LIT_POS, R, sunTint, u_sunDirection.xyz, ambientSky) * float(w);
		}
	}
	out_color = vec4(color, 1.0); // opaque terrain
}
