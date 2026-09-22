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

#ifdef TERRAIN_OVERLAY_PASS
// The terrain overlay (EPipelineIndex::TerrainOverlay, see main): a DUAL-SOURCE composite over the lit ground,
// out = out_color + ground * out_factor per channel. Depth write off, so the discard of an uncovered pixel
// keeps early depth testing.
layout (early_fragment_tests) in;
layout (location = 0, index = 0) out vec4 out_color;  // added colour
layout (location = 0, index = 1) out vec4 out_factor; // the ground's multiplier
#else
layout (location = 0) out vec4 out_color;
#endif

#include "instanced_indirect_lit.inc.glsl"

// Terrain wetness clipmap (binding 18, written by terrain_wetness.cs.glsl): the decaying memory of
// where water touched the ground - the swash tongue, rain. Applied on top of the splat in main().
#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl"
#include "reflection_fog.inc.glsl"

// Sky for the film's reflection - the same per-frame bake of mirrorSkyRadiance the ocean's
// reflectedSkyRadiance fetches, so the film reflects the same sky the water next to it does. No sun
// disc: the GGX glint is its reflection.
vec3 terrainReflectedSkyRadiance(vec3 dir)
{
	return textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR), 0.0).rgb;
}

// TERRAIN_FILM_RT_MIRROR - DISABLED (StaticMeshGraphicsPipeline's TERRAIN_FILM_RT_MIRROR switch is false):
// the film reflects only the sky. Its scene mirror ray set the terrain's register allocation for EVERY
// pixel, filmed or not (terrain 80/32 regs/local with it, 72/16 without; Nsight: pixel warps launch-stalled
// on register allocation 72% of the Static meshes range).
#ifdef TERRAIN_FILM_RT_MIRROR
// The film's mirror ray: the ocean's (ocean.fs.glsl traceScene + shadeHit + applyReflectionFog) - the
// scene TLAS, sun + GI probes, no shadow ray. A hit takes its material's diffuse texture, terrain
// included: a splat at the hit needs a ray-cone LOD this shader's screen-derivative splat cannot give.
// hitTerrain lets the caller fade terrain hits on sloped film.
bool terrainFilmMirror(vec3 origin, vec3 dir, float tMax, vec3 sunRadiance, vec3 L, vec3 ambientSky, out vec3 radiance, out bool hitTerrain)
{
	hitTerrain = false;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsNoneEXT, 0xFFu, origin, 0.05, dir, tMax);
	while (rayQueryProceedEXT(rq))
	{
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT
			&& rtsCandidateBlocks(rq))
			rayQueryConfirmIntersectionEXT(rq);
	}
	if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT)
		return false;

	const float t = rayQueryGetIntersectionTEXT(rq, true);
	const vec3 hitPos = origin + dir * t;
	vec3 hitN = -dir;
	vec3 albedo = vec3(0.3);
	// Interpolated normal/uv + material albedo, bounds-checked like rt_shadow.inc.glsl.
	const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
	const uint meshIdx = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rq, true);
	if (uint(instanceIdx) < in_instances.length() && meshIdx < in_meshInfos.length())
	{
		const InMeshInfo mi = in_meshInfos[meshIdx];
		const uint triBase = mi.firstIndex + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)) * 3u;
		if (triBase + 2u < in_indices.length())
		{
			const uint v0 = uint(mi.vertexOffset) + in_indices[triBase + 0u];
			const uint v1 = uint(mi.vertexOffset) + in_indices[triBase + 1u];
			const uint v2 = uint(mi.vertexOffset) + in_indices[triBase + 2u];
			if ((max(max(v0, v1), v2) * 12u + 11u) < in_vertices.length())
			{
				const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
				const vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
				#define FILM_RT_V(vi, o) vec3(in_vertices[(vi) * 12u + (o)], in_vertices[(vi) * 12u + (o) + 1u], in_vertices[(vi) * 12u + (o) + 2u])
				const vec3 objN = FILM_RT_V(v0, 3u) * w.x + FILM_RT_V(v1, 3u) * w.y + FILM_RT_V(v2, 3u) * w.z;
				#undef FILM_RT_V
				const vec2 uv = w.x * rtsVertexUV(v0) + w.y * rtsVertexUV(v1) + w.z * rtsVertexUV(v2);
				hitN = normalize(mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, true)) * objN);
				if (dot(hitN, dir) > 0.0)
					hitN = -hitN;
				const uint materialIdx = in_instances[instanceIdx].meshIdxMaterialIdx >> 16;
				if (materialIdx < in_materialInfos.length())
				{
					hitTerrain = (in_materialInfos[materialIdx].flags & MATERIAL_FLAG_TERRAIN) != 0u;
					const uint diffuseTexIdx = in_materialInfos[materialIdx].diffuseNormalTexIdx & 0x0000FFFFu;
					const float lod = clamp(log2(max(t, 1.0)) + 1.0, 0.0, 7.0); // ray-cone-ish LOD by distance
					albedo = textureLod(u_textures[nonuniformEXT(diffuseTexIdx)], uv, lod).rgb;
				}
			}
		}
	}

	// Half shading of the hit (the GI read is gi_probe.inc.glsl's fp16 read side), widened once.
	const f16vec3 hitNh = f16vec3(hitN);
	const f16vec3 indirect = giIndirectOverPiH(hitPos, hitNh) * float16_t(u_aoParams.y);
	const float16_t NoL = max(dot(hitNh, f16vec3(L)), float16_t(0.0));
	radiance = vec3(f16vec3(albedo) * (f16vec3(sunRadiance) * (NoL * float16_t(INV_PI)) + indirect + f16vec3(u_ambientColor)));
	radiance = applyReflectionFog(radiance, origin, dir, t, sunRadiance, L, ambientSky);
	return true;
}
#endif

// Standing water on the ground, shaded the way the ocean shader shades its surface so the two meet
// seamlessly at the depth-buffer intersection: the lit ground is the refracted BODY (a film has no
// depth to absorb through), a Fresnel-weighted sky reflection sits on it, and the sun glint is the
// ocean's dielectric GGX at the ocean's roughness. The film normal is the LEVEL water plane tilted
// toward the LIVE FFT wave slopes ("Surface water waviness"), so ripples run across the pools in step
// with the water beside them.
// Two stages AFTER computeLitColor (main): terrainFilmSurface (the waves, the normal, the foam, the
// roughness), then terrainFilmShade on the lit ground as the body, with the film's own light walk. (The
// film's lights riding the lit core's loop - one shadow ray per light for both - measured worse: the film
// surface then had to be resolved before that loop and stayed live across its shadow ray query, 80/64
// regs/local against 80/32.) V and geoN are re-derived from the position and the interpolant in each
// stage, and the scalar inputs arrive half: none of them is live in 32-bit across the lit core's peaks.
struct TerrainFilm
{
	f16vec3 N;        // the film normal (level plane, rippled)
	float16_t alpha;  // the water's GGX alpha
	float16_t foam;   // crest foam + shoreline lace coverage
	float16_t milk;   // entrained-bubble turbidity
};

// mask = how much of the pixel is film. depth = calm water level above the ground here (negative on dry
// land), waterLevel = that calm level.
TerrainFilm terrainFilmSurface(vec3 worldPos, float16_t footprintH, float16_t maskH, float depth, float waterLevel)
{
	// HALF math throughout (the wave maps are RGBA16F already, MAPS_FORMAT). 32-bit only where it is needed:
	//  - positions and heights: the view vector, the ground normal, the map UVs and the shore weights, whose
	//    inputs are absolute heights (a 0.05 - 1 m band at hundreds of metres: half steps 0.25 m there);
	//  - the LEAN variance's E[s^2] - E[s]^2 (catastrophic cancellation: the difference is far below the
	//    terms' own half rounding);
	//  - alpha^2, whose base term is perceptual^4 (0.02^4 = 1.6e-7, below half's normal range).
	const f16vec3 Vh = f16vec3(normalize(u_viewPos - worldPos));
	const f16vec3 geoN = f16vec3(normalize(in_normal));
	const float16_t one = float16_t(1.0);
	// The ocean's ONE depth weight (oceanShoreWeights, underwater_light.inc.glsl): 1 in open water,
	// easing to the swash base (amplitude x sea x land fades) across the approach band. On dry sand the
	// weight IS the swash base, which is the water's surface shape at the waterline, so the film
	// continues the water.
	const float reach = max(u_oceanParams7.w, 0.01);
	float swashW32, w32;
	oceanShoreWeights(depth, waterLevel, swashW32, w32);
	const float16_t w = float16_t(w32);
	// The SHORE, as the ocean defines it (oceanSwashBase): water connected to the sea (its baked level at
	// sea level - not a lake, a river, or ground the water-reach bake sank the level under) within ONE
	// swash reach above that level, as far as the tongue ever runs. Gates everything breaking makes (foam,
	// turbulence); its complement is where the wind ripples live.
	const float16_t shore = float16_t((1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_oceanParams2.w)))
		* (1.0 - smoothstep(0.6 * reach, reach, -depth)));
	// Wind ripples ("Wind ripple strength"): off the shore w is 0 and the film would lie dead flat, so
	// the FINEST cascade keeps a weight of its own there - the taps the loop makes anyway. Slope + LEAN
	// variance only (the Jacobian sums stay on w: no chop, no foam); amplitude and heading follow the
	// ocean's wind through the spectrum.
	const float16_t ripple = float16_t(u_terrainWetParams5.w) * (one - shore);
	const float16_t lodBase = log2(max(footprintH, float16_t(1e-3)) * float16_t(OCEAN_FFT_SIZE));

	// Wave slopes, LEAN variance, vertical acceleration and the RAW fold Jacobian sums of the cascades.
	f16vec2 slopeSum = f16vec2(0.0), varSum = f16vec2(0.0);
	float16_t rxx = float16_t(0.0), rzz = float16_t(0.0), rxz = float16_t(0.0);
	float16_t turbulence = float16_t(0.0);
	float16_t accel = float16_t(0.0);
	for (int c = 0; c < OCEAN_CASCADES; ++c)
	{
		const float16_t wc = c == OCEAN_CASCADES - 1 ? max(w, ripple) : w; // this cascade's slope weight
		const float L = u_oceanParams2[c];
		const float16_t lod = max(lodBase - float16_t(log2(L)), float16_t(0.0));
		const vec2 uvc = worldPos.xz / L;
		const vec4 g32 = textureLod(u_uwOceanMaps, vec3(uvc, float(OCEAN_CASCADES + c)), lod); // (dh/dx, dh/dz, dDx/dx, dDz/dz)
		const vec4 m32 = textureLod(u_uwOceanMaps, vec3(uvc, float(2 * OCEAN_CASCADES + c)), lod); // LEAN moments, accel, turbulence (c0)
		const float16_t dxz = float16_t(textureLod(u_uwOceanMaps, vec3(uvc, float(c)), lod).w); // displacement layer w = dDx/dz
		// Slope variance lost to mip filtering (Bruneton 2010): the cancelling difference in 32-bit.
		varSum += f16vec2(max(m32.xy - g32.xy * g32.xy, vec2(0.0))) * (wc * wc);
		const f16vec4 g = f16vec4(g32);
		slopeSum += g.xy * wc;
		rxx += g.z; rzz += g.w; rxz += dxz;
		accel += float16_t(m32.z);
		if (c == 0)
			turbulence = float16_t(m32.w); // the accumulated breaking memory rides cascade 0's moments
	}
	accel *= w;
	turbulence *= shore; // the map tiles over the whole world; off the shore it would milk and roughen a puddle
	const float16_t sxx = rxx * w, szz = rzz * w, sxz = rxz * w; // the normal's Jacobian follows the weighted chop
	const float16_t chop = float16_t(u_oceanParams0.w);
	const float16_t jxx = one + chop * sxx, jzz = one + chop * szz;
	// Floor + rational soft limit, as the ocean: near folds the raw division explodes the slope.
	f16vec2 slope = slopeSum / max(f16vec2(jxx, jzz), f16vec2(0.6));
	slope /= one + float16_t(u_oceanParams10.x) * length(slope); // "Crest slope limit"
	const f16vec2 slopeVar = varSum;
	// The ocean's sub-band detail: oceanDetailSlope (ocean_wave.inc.glsl) inlined - that include binds the
	// ocean's own sampler - with an explicit LOD (no derivatives in this branch). Weighted like the finest
	// cascade, so an inland puddle gets it too.
	{
		float16_t detailFade = max(w, ripple);
		if (u_oceanParams11.z > 0.0)
			detailFade *= float16_t(1.0 - smoothstep(0.5 * u_oceanParams11.z, u_oceanParams11.z, distance(worldPos.xz, u_viewPos.xz)));
		if (float16_t(u_oceanParams11.x) * detailFade > float16_t(0.0))
		{
			const int dc = OCEAN_CASCADES - 1;
			const float Ld = max(u_oceanParams2[dc] * u_oceanParams11.y, 1e-3);
			const vec2 rot = vec2(cos(u_oceanParams11.w), sin(u_oceanParams11.w));
			const vec2 p = vec2(rot.x * worldPos.x + rot.y * worldPos.z, -rot.y * worldPos.x + rot.x * worldPos.z);
			const float16_t lodD = max(lodBase - float16_t(log2(Ld)), float16_t(0.0));
			const f16vec2 s = f16vec2(textureLod(u_uwOceanMaps, vec3(p / Ld, float(OCEAN_CASCADES + dc)), lodD).xy) * (float16_t(u_oceanParams11.x) * detailFade);
			const f16vec2 rotH = f16vec2(rot);
			slope += f16vec2(rotH.x * s.x - rotH.y * s.y, rotH.y * s.x + rotH.x * s.y); // back into world space
		}
	}

	// The ocean's "Normal strength" x "Surface water normal scale" (1 = the ocean's).
	const float16_t ns = float16_t(u_oceanParams1.w * u_terrainWetParams5.z);
	const f16vec3 waveN = normalize(f16vec3(-slope.x * ns, one, -slope.y * ns));
	// The base is the LEVEL water plane, not the ground normal: water lies flat whatever the slope under
	// it, so the film takes the sun, the sky and the lights at the ocean's angles. A level plane is only
	// visible from above, and a slope rises past the camera's eye height (V.y -> 0: Fresnel a full mirror,
	// then negative), so the base eases to the ground normal as the view flattens onto the plane - a hard
	// switch draws a line across the slope at eye height.
	// Waviness scales with the wetness (mask): a fresh film ripples with the water, a fading one lies flat.
	const f16vec3 levelN = f16vec3(0.0, 1.0, 0.0);
	const f16vec3 baseN = normalize(mix(geoN, levelN, smoothstep(float16_t(0.05), float16_t(0.35), Vh.y)));
	const float16_t waviness = float16_t(u_terrainWetParams5.y) * maskH;
	f16vec3 N = normalize(mix(baseN, normalize(baseN + (waveN - levelN)), waviness)); // the wave tilt, carried onto the base
	if (dot(N, Vh) < float16_t(0.0))
		N = dot(baseN, Vh) > float16_t(0.0) ? baseN : geoN; // a ripple tilted away from the viewer: the unrippled base

	// Shoreline lace coverage: the ocean's surf-band foam at its waterline target (column ~ 0 -> target
	// 1), realized through the raw fold Jacobian with the same bias/threshold, eased toward "Shore foam
	// max" - so the lace the water carries onto the sand is the same lace it wears at the edge. Shore only.
	float16_t foam = float16_t(0.0);
	if (u_oceanParams5.z > 0.0)
	{
		const float16_t Jraw = (one + chop * rxx) * (one + chop * rzz) - chop * rxz * chop * rxz;
		// The ocean's tongue has column ~ 0, i.e. lace target 1, over its whole run-up - so does its film.
		const float16_t target = maskH * shore;
		const float16_t b = mix(float16_t(0.75), float16_t(1.45), target) + float16_t(u_oceanParams8.y);
		const float16_t foamMax = float16_t(max(u_oceanParams7.y, 1e-3));
		foam = foamMax * (one - exp(-target * (one - smoothstep(b - float16_t(0.4), b + float16_t(0.4), Jraw)) / foamMax));
	}
	// The ocean's crest foam: oceanInstantFoam (ocean_wave.inc.glsl) inlined - fold of the WEIGHTED Jacobian
	// or a breaking downward acceleration, the threshold relaxed by the turbulence ("Foam boost"). The
	// water beside the film wears max(crest, lace), and the lace alone is capped at "Shore foam max".
	{
		const float16_t jxz = chop * sxz;
		const float16_t jacobian = jxx * jzz - jxz * jxz;
		const float16_t softness = float16_t(max(u_oceanParams4.w, 0.02));
		const float16_t bias = float16_t(u_oceanFoam.w) + turbulence * float16_t(u_oceanParams5.x);
		const float16_t fold = one - smoothstep(bias - softness, bias, jacobian);
		const float16_t breakStart = float16_t(u_oceanParams5.w);
		const float16_t breaking = smoothstep(breakStart, breakStart + softness, -accel * float16_t(1.0 / 9.81));
		foam = clamp(max(foam, maskH * shore * max(fold, breaking)), float16_t(0.0), one);
	}
	// Entrained bubbles: the ocean's accumulated turbulence (the decaying memory of breaking, strongest
	// exactly at the shore) turns the water milky ("Turbidity") and rougher. The surf beside the film
	// carries it, so the film carries it too.
	const float16_t milk = clamp(turbulence * float16_t(u_oceanParams5.y), float16_t(0.0), one);
	// The ocean's microfacet alpha on the film's own base ("Water roughness"): perceptual roughness^2, plus the LEAN slope variance (scaled by
	// "Glint filtering") that stretches the glitter toward the horizon, plus the turbulence
	// micro-roughness. No spec-AA term: that is a screen derivative, undefined in this branch.
	// NOTE: the lit core's `roughness` parameter IS the GGX alpha, so it goes in directly - passing a
	// perceptual value there gave the film a wider glint than the water beside it.
	// alpha^2 in 32-bit (its perceptual^4 term underflows half); the half terms widen into it.
	const float baseRough = clamp(u_terrainWetParams2.z, 0.02, 1.0); // "Water roughness" (perceptual, as the ocean's)
	const float slopeVariance = float(float16_t(0.5) * (slopeVar.x + slopeVar.y) * (ns * ns));
	const float alphaSq = baseRough * baseRough * baseRough * baseRough
		+ 2.0 * slopeVariance * u_oceanParams6.y + float(turbulence) * u_oceanParams5.y * 0.35;

	return TerrainFilm(N, float16_t(clamp(sqrt(alphaSq), 0.02, 1.0)), foam, milk);
}

// The film over the lit ground (the "body"), as the overlay composites it: final = body * groundFactor +
// addColor, per channel (the dual-source blend supplies the body - the pass never reads the scene colour).
// waterDepth = the metres of water standing over this pixel (the film's own body thickness). Shaded in HALF math, as the ocean's top side: the vectors, the weights and the
// colours (the scene colour is RGBA16F). The sky ray's direction stays 32-bit (widened from the half N / V).
void terrainFilmShade(vec3 worldPos, TerrainFilm film, float16_t maskH, float16_t waterDepth, out f16vec3 addColor, out f16vec3 groundFactor)
{
	const f16vec3 Nh = film.N;
	const f16vec3 Vh = f16vec3(normalize(u_viewPos - worldPos));
	const float16_t alphaH = film.alpha;
	const float16_t foamH = film.foam;
	const float16_t milk = film.milk;
	const float16_t one = float16_t(1.0);
	const float16_t NoV = clamp(dot(Nh, Vh), float16_t(1e-3), float16_t(1.0));
	const float16_t x = float16_t(1.0) - NoV;
	const float16_t x2 = x * x;
	const float16_t F = float16_t(0.02) + float16_t(0.98) * (x2 * x2 * x); // Schlick, water F0

	const vec3 up = normalize(u_skyUp);
	const vec3 L = u_sunDirection.xyz;
	const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * u_eclipseParams.x; // = the ocean's sunTint
	const f16vec3 ambientSky = f16vec3(textureLod(u_skyMap, vec3(skyMapUV(up), SKY_MAP_LAYER_GI), 0.0).rgb); // skyRadiance(up): constant per frame, one fetch

	// Body: the ground seen through the film - Beer-Lambert absorbed along the refracted path through the
	// water ACTUALLY standing here (the film's coverage depth: the pool the wetness fills the relief to, or
	// the live ocean over the ground, in metres), with the ocean's in-scatter filling in what was absorbed.
	// Exactly the ocean shader's body mix, tinted = body * T + tintAdd, so a puddle deepens toward its
	// middle and the two are the same colour where the film meets the ocean.
	f16vec3 T = f16vec3(1.0);
	f16vec3 tintAdd = f16vec3(0.0);
	if (waterDepth > float16_t(0.0))
	{
		const vec3 refrDir = refract(-vec3(Vh), vec3(Nh), 1.0 / 1.33);
		const float path = float(waterDepth) / max(-refrDir.y, 0.2);
		T = f16vec3(exp(-u_oceanAbsorption.rgb * path));
		const f16vec3 inscatter = f16vec3(u_oceanScatter.rgb * u_oceanScatter.w) * (ambientSky + f16vec3(sunTint * (max(L.y, 0.0) * INV_PI)));
		tintAdd = inscatter * (f16vec3(1.0) - T);
	}

	// Whitewater: lambertian foam lit by the pixel's ALREADY-RESOLVED sun radiance AT THE SURFACE
	// (sunSurfaceRadiance() - the lit core's shadow visibility, no second shadow evaluation, and NOT the
	// underwater caustic/absorption factor: the foam floats on the film, it is not the seabed under it)
	// plus sky and ambient. The ocean's whitewater; skipped where neither the milk nor the lace would
	// show it.
	f16vec3 whitewater = f16vec3(0.0);
	if (milk > float16_t(0.003) || foamH > float16_t(0.003))
		whitewater = f16vec3(doLightH(sunSurfaceRadiance(), f16vec3(L), Vh, Nh, f16vec3(0.0), f16vec3(u_oceanFoam.rgb * INV_PI), float16_t(0.0), float16_t(0.85)))
			+ f16vec3(u_oceanFoam.rgb) * (ambientSky + f16vec3(u_ambientColor));
	// Turbidity: tinted = mix(body * T + tintAdd, whitewater * 0.55, milk).

	// The film is LINEAR in the body and in its remaining unknowns, the mirror (the sky; the traced scene
	// mirror when TERRAIN_FILM_RT_MIRROR is on) and the scene lights:
	//   film  = mix(mix(tinted, mix(mirror, ambientSky, reflBlur), F) + glint + lights, whitewater, foam)
	//   final = mix(body, film, mask) = body * groundFactor + C + mirror * mirrorWeight + lights * clearW
	// so everything else - the tint, the milk, the blur's sky share, the glint, the foam - folds into C
	// (with TERRAIN_FILM_RT_MIRROR, C is live across the mirror trace, not the shading inputs). The foam
	// mix is not gated at 0.3% (it folds here).
	const float16_t reflBlur = clamp(alphaH * float16_t(2.0) - float16_t(0.05), float16_t(0.0), float16_t(0.6));
	const float16_t clearW = maskH * (float16_t(1.0) - foamH);
	const float16_t mirrorWeight = clearW * F * (float16_t(1.0) - reflBlur);
	// Sun glint: the ocean's dielectric GGX (F0 0.02, alphaF) on the pixel's resolved sun radiance AT
	// THE SURFACE - shadow-gated exactly as the ground under it was, without a second shadow evaluation,
	// but NOT underwater-gated: the glint is the sun mirrored off the film, before the water, so the
	// seabed's caustic focus has no business in it (with the underwater factor it warped into the lobe as
	// distorted caustics on every filmed pixel under a wave). Specular only (no diffuse term: the body
	// already carries the ground's diffuse light, caustics included).
	const f16vec3 glint = f16vec3(min(doLightH(sunSurfaceRadiance(), f16vec3(L), Vh, Nh, f16vec3(0.02), f16vec3(0.0), float16_t(0.0), alphaH), vec3(65504.0)));
	groundFactor = (one - maskH) + (clearW * (one - F) * (one - milk)) * T;
	f16vec3 color = (maskH * foamH) * whitewater
		+ clearW * ((one - F) * (tintAdd * (one - milk) + whitewater * (float16_t(0.55) * milk)) + (F * reflBlur) * ambientSky + glint);

	// Reflection: the sky (the ray-traced scene mirror is disabled, TERRAIN_FILM_RT_MIRROR), roughness-
	// blurred toward the average sky (the blur's sky share is in the fold above). Before the lights: with the
	// mirror, lights-first kept N / V and the mirror gate live across the lights' own shadow-ray peak, which
	// cost more (368 vs 352 B/thread).
	vec3 R = reflect(-vec3(Vh), vec3(Nh));
	R.y = max(R.y, 0.02);
	R = normalize(R);
	// The sky fallback, fogged too (the baked mirror sky carries none), resolved BEFORE the mirror trace and
	// folded in whole: a hit swaps its share for the hit below. So neither R's sky lookup nor the ground
	// normal is live across the trace - only the half sky colour and the hit's slope share. (A hit pixel
	// pays the one sky fetch it could have skipped; hits are the minority of film pixels.)
	const f16vec3 skyMirror = f16vec3(min(applyReflectionFogSky(terrainReflectedSkyRadiance(R), worldPos, R, sunTint, L, vec3(ambientSky)), vec3(65504.0))) * mirrorWeight;
	color += skyMirror;
#ifdef TERRAIN_FILM_RT_MIRROR // DISABLED - see terrainFilmMirror; the film reflects the sky only
	// The ocean's gates: the mirror's weight in the pixel (under 2% = skipped), "Reflection max rough"
	// (this alpha has no micro-roughness term, so it is the ocean's gate value), "Ray cutoff dist".
	if (mirrorWeight > float16_t(0.02) && float(alphaH) < u_oceanParams9.w
		&& (u_oceanParams8.w <= 0.0 || distance(u_viewPos, worldPos) < u_oceanParams8.w))
	{
		// A level mirror lying on a slope is not a real surface: its ray runs straight into the hill it
		// lies on (the whole film ground-coloured, and sky again past "Ray cutoff dist" - a seam). So
		// TERRAIN hits fade out with the local slope; flat ground keeps them (a pool at the foot of a
		// hill does mirror the hill), and scene objects always stay.
		const vec3 geoN = normalize(in_normal);
		const float16_t terrainSkyShare = float16_t(smoothstep(0.03, 0.15, 1.0 - clamp(geoN.y, 0.0, 1.0)));
		vec3 hitRadiance;
		bool hitTerrain;
		// Origin lifted along the GROUND normal: the level film normal would start the ray inside a slope.
		if (terrainFilmMirror(worldPos + geoN * 0.05, R, u_oceanParams9.z, sunTint, L, vec3(ambientSky), hitRadiance, hitTerrain)) // "Reflection range"
		{
			const float16_t hitShare = hitTerrain ? float16_t(1.0) - terrainSkyShare : float16_t(1.0);
			color += (f16vec3(min(hitRadiance, vec3(65504.0))) * mirrorWeight - skyMirror) * hitShare;
		}
	}
#endif

	// Scene lights: the ocean's grid walk, SPECULAR ONLY on the film normal - the body already carries the
	// ground's diffuse light, and the ground's wet gloss is off under this pass, so this is the one
	// highlight, on the water's angle. Each light may cost a shadow ray: skipped under 2% visible.
	if (clearW > float16_t(0.02))
	{
		f16vec3 lights = f16vec3(0.0);
		const f16vec3 waterSpec = f16vec3(0.02);
		const ivec3 gridPos = getGridPos(worldPos);
		uint tableIdx = getTableIdx(gridPos);
		while (true)
		{
			const uint gridIdx = getGridIdx(tableIdx);
			if (gridIdx == EMPTY_ENTRY)
				break;
			const ivec3 gridMin = getGridMin(gridIdx);
			if (gridMin == gridPos)
			{
				// ONE loop over large + cell lights (as the lit core): each loop inlines doLightShadowed whole.
				const uint numLargeLights = min(getLargeLightCount(gridIdx), MAX_LARGE_LIGHTS_PER_GRID);
				const uint cellOffset     = calcCellOffset(gridIdx, gridMin, worldPos);
				const uint numLights      = numLargeLights + min(getNumLightsForCell(cellOffset), MAX_LIGHTCELL_LIGHTS);
				for (uint i = 0; i < numLights; ++i)
				{
					const uint lightId = i < numLargeLights ? getLargeLightId(gridIdx, i) : getLightId(cellOffset, i - numLargeLights);
					lights = min(lights + f16vec3(min(doLightShadowed(in_lightInfos[lightId], worldPos, Vh, Nh, waterSpec, f16vec3(0.0), 0.0, alphaH), vec3(MEDIUMP_FLT_MAX))), f16vec3(MEDIUMP_FLT_MAX));
				}
				break;
			}
			tableIdx = getNextTableIdx(tableIdx);
		}
		color += lights * clearW;
	}
	addColor = min(color, f16vec3(MEDIUMP_FLT_MAX));
}

// The splat itself (TerrainFields / TerrainSample / terrainSplat) is terrain_splat.inc.glsl - shared with
// the ocean shader, which evaluates it at refraction-ray hits so the seabed IS this terrain.
// RELIEF: the height blend + the parallax march, here only (it needs screen derivatives). All of it runs
// before computeLitColor and nothing of it but the shifted texture position outlives the splat.
#define TERRAIN_SPLAT_RELIEF
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

// THE WETNESS AT THIS PIXEL, after the slope drain, plus the gate that keeps water off ground the OCEAN
// itself is drawing.
// wet: the smooth field (bilinear + diffusion), highest where water stood most recently. Everything else -
// the ground's darkening and gloss, the water LEVEL in the relief, the film's surface - derives from it.
// aboveLive: fades from 1 to 0 over the first "Ocean blend (m)" of LIVE water over the GROUND (the same
// live surface that switches the caustics on). The wet gloss and the film then never show deep in the
// ocean's own water (a second sky reflection under the first). The film lies over the ocean's shallow edge
// and fades into it gradually, and the fade is complete before its surface sinks under the ocean's. There,
// the depth test cut it with a hard edge. groundBelow = how far the tested point (in_pos) sits above the
// ground under it: 0 for the ground itself, the film's height over the relief for the film.
// Also 0 for the whole frame while the CAMERA is under water: a submerged viewer never sees water mirrored
// off standing water (the ocean's own underside draws what it mirrors). The camera's side is the particle
// draw's gate: the live wave height under the camera (the CPU mirror, u_weatherWind2.z), else the calm
// level here, else sea level.
void terrainWetness(vec3 coverN, TerrainFields fields, float groundBelow, out float16_t wet, out float16_t aboveLive)
{
	// We already sampled the terrain data cascade for fields.waterLevel; hand it to the lit core so its
	// underwater test reuses it instead of re-fetching the same cascade - then resolve that test NOW
	// (doSunLight would, but the gloss below needs it first; it runs once per pixel either way).
	g_waterLevelOverride = fields.waterLevel;
	resolveLiveDepth(in_pos);
	const float16_t one = float16_t(1.0);
	const float waterAtCamera = u_weatherWind2.w > 0.5 ? u_weatherWind2.z : fields.waterLevel;
	aboveLive = float16_t(u_viewPos.y < waterAtCamera ? 0.0
		: 1.0 - smoothstep(0.0, u_terrainWetParams5.x, g_liveDepthBelow + groundBelow));
	wet = float16_t(0.0);
	if (!terrainWetPresent())
		return;
	wet = float16_t(terrainWetnessAt(in_pos.xz));
	// Slope drain: water runs off a face instead of soaking in, so steep ground dries faster. The stored
	// wetness decays as exp(-t / tau), so wet^k IS a k-times faster decay - evaluated here per pixel against
	// the exact mesh normal (the map's 8 m texels cannot see a cliff face), with no extra state.
	// k = 1 + slope * drain: at drain 4 a 45-degree face dries ~2.2x faster, a wall 5x.
	const float16_t slope = one - float16_t(clamp(coverN.y, 0.0, 1.0));
	wet = pow(wet, one + slope * float16_t(u_terrainWetParams3.z));
}

// The relief depth (m) the film's own surface was displaced in (terrain_tess.tes.glsl).
float terrainReliefDepth(TerrainLayers L)
{
	return mix(mix(u_terrainTessParams1.z, u_terrainTessParams1.w, float(L.rockW)), u_terrainTessParams1.z, float(L.snowW));
}

#if defined(TERRAIN_TESS) && !defined(TERRAIN_OVERLAY_PASS)
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
	const float dist = distance(in_meshPos, u_viewPos);
	const float fadeStart = u_terrainTessParams1.x, fadeEnd = u_terrainTessParams1.y;
	if (dist >= fadeEnd || u_terrainTexParams0.x < 0.0 || u_terrainTexParams0.y < 1.0)
		return N;
	const float t = clamp((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3), 0.0, 1.0);
	const float strength = (1.0 - pow(t, u_terrainTessParams0.w)) * smoothstep(0.35, 0.6, N.y);
	const float depth = mix(mix(u_terrainTessParams1.z, u_terrainTessParams1.w, float(L.rockW)), u_terrainTessParams1.z, float(L.snowW)) * strength;
	if (depth <= 1e-4)
		return N;
	strengthOut = strength;
	// The TES's footprint (terrain_tess.tes.glsl): the projection's y scale is row 1 of the mvp's 3x3.
	const float projY = length(vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]));
	const float e = max(max(dist, u_terrainTessParams2.x) * 2.0 * u_terrainTessParams0.z / max(projY * u_screenSize.y * u_viewportRect.w, 1.0), 1e-3);
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
	const vec3 toView = u_viewPos - in_pos;
	const float viewDist = length(toView);
	const vec3 V = toView / viewDist;
	const TerrainFields fields = terrainFields();
	// coverN: the smooth mesh normal - the layer coverages and the slope drain read it (the relief's bumps must
	// not scatter rock / snow). geoN: the shading base - the same, except on the tessellated terrain.
	const vec3 coverN = normalize(in_normal);
#if defined(TERRAIN_TESS) && !defined(TERRAIN_OVERLAY_PASS)
	// The shading base: the displaced surface's own normal (terrainTessPixelNormal). reliefStrength fades the
	// normal maps down below (they carry the same relief again).
	// The coverages ONCE, at the undisplaced point the TES displaced from: the pixel normal and the splat share
	// them (terrainLayers is the costly part: the climate walk, the crag fBm).
	const TerrainLayers tessLayers = terrainLayers(in_meshPos, coverN, fields);
	float reliefStrength;
	const vec3 geoN = terrainTessPixelNormal(coverN, tessLayers, reliefStrength);
#else
	// (The tessellated overlay too: the film shades on its own normals.)
	const vec3 geoN = coverN;
#endif
	const float16_t one = float16_t(1.0);

#ifdef TERRAIN_OVERLAY_PASS
	// THE TERRAIN OVERLAY (EPipelineIndex::TerrainOverlay): the surface-water film, composited over the lit
	// ground (out = K + ground * factor, dual-source). Every other pixel discards.
	// Pixel footprint for the film's wave taps; a derivative, so taken here in uniform flow.
	const float16_t wetFootprint = float16_t(length(fwidth(in_pos.xz)));
	// The film coverage's relief taps (below): derivatives, so taken before any discard.
	const vec2 filmDx = dFdx(TERRAIN_LIT_POS.xz), filmDy = dFdy(TERRAIN_LIT_POS.xz);
	// The chunk's SKIRT (TerrainGenerator: vertical walls dropped from the border, hiding LOD cracks): the
	// ground needs it, the film does not - drawn with it, the film showed as a translucent wall at chunk
	// edges. The skirt's faces are exactly vertical in the undisplaced mesh; the terrain surface's never are.
	// After the derivatives above (a discard ends the quad's helpers).
	const vec3 meshFaceN = cross(dFdx(TERRAIN_LIT_POS), dFdy(TERRAIN_LIT_POS));
	if (abs(meshFaceN.y) < 0.05 * length(meshFaceN))
		discard;
	// The ground under this pixel: the relief height here, centred on the mesh (0.5 = the mesh).
	const TerrainLayers filmLayers = terrainLayers(TERRAIN_LIT_POS, coverN, fields);
	const float reliefDepth = terrainReliefDepth(filmLayers);
	const float16_t reliefH = terrainReliefAt(filmLayers, TERRAIN_LIT_POS.xz, filmDx, filmDy);
	const float groundBelow = in_pos.y - (TERRAIN_LIT_POS.y + (float(reliefH) - 0.5) * reliefDepth);
	float16_t wet, aboveLive;
	terrainWetness(coverN, fields, groundBelow, wet, aboveLive);
	// THE FILM'S COVERAGE: how much water stands over this pixel (metres), faded over the last "Edge fade (m)"
	// of it, so the film always dies exactly where its surface meets the terrain. Per pixel and shaped by the
	// texture relief: the film's own outline is the tessellated surface crossing the ground (a per-triangle,
	// stepped edge) over the wetness clipmap's 0.5 m texel contour, which popped in blocky on its own.
	// The water is the pool the wetness fills the relief to, or the LIVE ocean over the ground where that is
	// deeper: the film then always covers the ocean's thin edge, where the ocean dithers out and hands over
	// to it ("Ocean edge fade (m)", ocean.fs.glsl), also where the wetness has not caught up yet.
	// aboveLive fades it out into the ocean's deeper water ("Ocean blend (m)").
	const float16_t poolDepth = max(float16_t(terrainPoolLevel(float(wet), coverN.y)) - reliefH, float16_t(0.0)) * float16_t(reliefDepth);
	const float16_t waterDepth = max(poolDepth, float16_t(clamp(g_liveDepthBelow + groundBelow, 0.0, 16.0)));
	const float16_t filmMask = smoothstep(float16_t(0.0), float16_t(max(u_terrainWetParams4.w, 1e-4)), waterDepth) * aboveLive;
	if (filmMask <= float16_t(0.0))
		discard;
	// The film's sun visibility (glint, whitewater): the ground pass's resolve is not available here, so ONE
	// hard tap (the moving water hides a penumbra), or one ray with the RT sun, as the ocean does. The
	// ground's gate: no sun on a surface facing away from it.
	const vec3 L = u_sunDirection.xyz;
	float sunVis = 0.0;
	if (dot(geoN, L) > 0.0)
	{
#if LIT_RT_SUN_SHADOW
		sunVis = rtShadowVisibility(TERRAIN_LIT_POS + geoN * 0.1, L, 0.05, 10000.0);
#else
		sunVis = sampleSunShadowHard(TERRAIN_LIT_POS, geoN);
#endif
	}
	g_sunVisSurface = float16_t(sunVis * u_eclipseParams.x);
	const TerrainFilm film = terrainFilmSurface(in_pos, wetFootprint, filmMask, fields.waterLevel - in_pos.y, fields.waterLevel);
	f16vec3 addColor, groundFactor;
	terrainFilmShade(in_pos, film, filmMask, waterDepth, addColor, groundFactor);
	out_color = vec4(addColor, 0.0);
	out_factor = vec4(groundFactor, 1.0);
#else

#if TERRAIN_DEBUG_MODE != 0
	out_color = vec4(terrainDebugColor(fields, in_pos), 1.0); // unlit: the field itself, not its shading
	return;
#endif

#ifdef TERRAIN_TESS
	TerrainSample surf = terrainSplatLayers(in_pos, geoN, tessLayers);
#else
	TerrainSample surf = terrainSplat(in_pos, geoN, coverN, fields);
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
		// "Roughness threshold": u_terrainWetParams8.xy) - a plateau while the ground is soaked - and fade
		// smoothly to dry below it.
		const float16_t darkAmount = smoothstep(float16_t(0.0), float16_t(u_terrainWetParams8.x), wet);
		const float16_t glossAmount = smoothstep(float16_t(0.0), float16_t(u_terrainWetParams8.y), wet);
		// THE DRYING PATTERN: ground does not dry uniformly - a beach breaks into metre-scale blotches
		// (porosity, micro-drainage) that dry first while the rest stays dark. P (0..1, low = holds its water
		// longest) is a world-anchored value fBm of "Drying pattern size (m)" (u_terrainWetParams9.y = 1 / size),
		// with "Drying pattern relief" (9.z) of the splat's height composite mixed in for the fine breakup at
		// the island edges. Each amount becomes a LEVEL through P: below it wet, above it a dry ISLAND, and the
		// islands grow as the level sinks. The darkening and the gloss share P, so an island loses its gloss
		// first (a higher roughness threshold), then its darkness. The soft band around each level is its own:
		// "Darkening edge" (9.x) - wide, the darkening fades over a larger range - and "Roughness edge" (7.z) -
		// crisp gloss islands. "Drying pattern" (7.w, 0..1) mixes from the uniform amount (0) to the patterned
		// one, and fades out where the blotches shrink to a few pixels (the noise would shimmer).
		// (The relief ALONE, tried before, tiles at the splat texture's scale: speckle, not drying patches.)
		const float16_t darkBand = float16_t(max(u_terrainWetParams9.x, 1e-3));
		const float16_t band = float16_t(max(u_terrainWetParams7.z, 1e-3));
		const float footprint = length(fwidth(TERRAIN_LIT_POS.xz)) * u_terrainWetParams9.y; // pattern cells per pixel
		const float16_t patternW = float16_t(u_terrainWetParams7.w * (1.0 - smoothstep(0.15, 0.4, footprint)));
		float16_t damp = darkAmount, glossW = glossAmount;
		if (patternW > float16_t(0.0))
		{
			// The fBm stays 32-bit (its hash is fract() of large products); stretched from its central
			// bunching toward the full 0..1 by "Drying pattern contrast" (9.w, pre-halved): higher = more of the
			// ground at the extremes, so the islands separate more strongly (clamped: fully dry / fully wet).
			const float n = clamp(terrainFbm(TERRAIN_LIT_POS.xz * u_terrainWetParams9.y) * u_terrainWetParams9.w + 0.5, 0.0, 1.0);
			const float16_t P = mix(float16_t(n), surf.height, float16_t(u_terrainWetParams9.z));
			damp = mix(damp, smoothstep(P - darkBand, P + darkBand, darkAmount * (one + darkBand + darkBand) - darkBand), patternW);
			glossW = mix(glossW, smoothstep(P - band, P + band, glossAmount * (one + band + band) - band), patternW);
		}
		surf.albedo *= mix(one, float16_t(u_terrainWetParams2.y), damp);
		// Where the FILM stands over this ground: the same pool level through the relief the film's coverage
		// uses, faded over its "Edge fade (m)" (the ground/beach relief depth as the metres). The ground there is
		// UNDER water - its wet gloss would sit beneath the film's own surface - so it takes the underwater
		// roughness, as under the live ocean (not a wetness gate: this follows the film's actual outline).
		const float16_t poolOver = (float16_t(terrainPoolLevel(float(wet), coverN.y)) - surf.height)
			* float16_t(u_terrainTessParams1.z / max(u_terrainWetParams4.w, 1e-4));
		const float16_t underFilm = clamp(poolOver, float16_t(0.0), one);
		// "Wet roughness" with the gloss, "Underwater roughness" under the film and the live ocean.
		surf.rough = mix(mix(surf.rough, float16_t(u_terrainWetParams7.x), glossW), float16_t(u_terrainWetParams7.y),
			max(one - aboveLive, underFilm));
		// "Wet normal scale" (u_terrainWetParams8.z): the normal map's tilt off the shading base, scaled with the
		// gloss - below 1 the water fills the micro relief (a sharper highlight: at full, the bumps scattered
		// it over the whole wet area), above 1 it is exaggerated. Kept on the base's side of the horizon (an
		// extrapolated tilt can pass it).
		{
			const f16vec3 baseN = f16vec3(geoN);
			f16vec3 n = baseN + (surf.normal - baseN) * mix(one, float16_t(u_terrainWetParams8.z), glossW);
			n += baseN * max(float16_t(0.05) - dot(n, baseN), float16_t(0.0));
			surf.normal = normalize(n);
		}
		// The sky reflection's weight: the gloss above the live ocean, minus where the film stands (it mirrors
		// the sky itself there).
		skyReflW = glossW * aboveLive * (one - underFilm);
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
	// surf.ao = baked texture AO on top of the screen-space term (ambient/indirect only).
	vec3 color = computeLitColor(TERRAIN_LIT_POS, V, surf.normal, surf.albedo, surf.rough, surf.metal, surf.ao);
	// THE WET SKY REFLECTION: the lit core has no environment
	// specular (diffuse GI + the lights' GGX lobes only), so wet ground showed one sun highlight and read as
	// merely darker. The film's sky: the baked mirror sky along R, water Fresnel (F0 0.02), the film's
	// roughness-to-blur rule (rough wet ground mirrors nothing), the texture AO as the specular occlusion and
	// the mirror fog rule (the bake carries no fog). After the lighting, past the light loop's register peak.
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
			const vec3 ambientSky = textureLod(u_skyMap, vec3(skyMapUV(normalize(u_skyUp)), SKY_MAP_LAYER_GI), 0.0).rgb;
			const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * u_eclipseParams.x;
			color += applyReflectionFogSky(terrainReflectedSkyRadiance(R), TERRAIN_LIT_POS, R, sunTint, u_sunDirection.xyz, ambientSky) * float(w);
		}
	}
	out_color = vec4(color, 1.0); // opaque terrain
#endif
}
