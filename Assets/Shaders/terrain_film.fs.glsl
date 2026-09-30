#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// The terrain overlay (EPipelineIndex::TerrainOverlay): the surface-water film over the lit ground drawn by
// instanced_indirect_terrain.fs.glsl. NEVER tessellated: the terrain VS compiled with TERRAIN_OVERLAY_PASS
// feeds it, lifted to the water level near the camera. A DUAL-SOURCE composite, out = out_color + ground *
// out_factor per channel. Depth write off, so the discard of an uncovered pixel keeps early depth testing.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal; // geometric (interpolated vertex) normal
layout (location = 2) in vec4 in_terrainFields; // VS-evaluated baked fields: x = macro altitude, y = temperature C, z = humidity, w = water level
// in_pos is LIFTED to the water level (instanced_indirect_terrain.vs.glsl) by this much along the normal. The mesh
// point under it - what the shadow map and the TLAS hold, and what the relief is measured from - is rebuilt at
// each use from the interpolants (a macro: nothing holds it live).
layout (location = 3) in float in_meshLift;
#define TERRAIN_LIT_POS (in_pos - normalize(in_normal) * in_meshLift)
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (early_fragment_tests) in;
layout (location = 0, index = 0) out vec4 out_color;  // added colour
layout (location = 0, index = 1) out vec4 out_factor; // the ground's multiplier

#include "instanced_indirect_lit.inc.glsl"
#include "terrain_common.inc.glsl"
#include "ocean_bubbles.inc.glsl"
#include "ocean_foam_field.inc.glsl"

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
			if (max(max(v0, v1), v2) < in_vertices.length())
			{
				const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
				const vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
				const vec3 objN = in_vertices[v0].normalV.xyz * w.x + in_vertices[v1].normalV.xyz * w.y + in_vertices[v2].normalV.xyz * w.z;
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
	float16_t milk;   // entrained-bubble cloud coverage
	float16_t foamNoL; // the foam's Lambert N.L on its own slope, the ocean's rule (one scalar, not the slope)
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
	// bubbles); its complement is where the wind ripples live.
	const float16_t shore = float16_t((1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_oceanParams2.w)))
		* (1.0 - smoothstep(0.6 * reach, reach, -depth)));
	// Wind ripples ("Wind ripple strength"): off the shore w is 0 and the film would lie dead flat, so
	// the FINEST cascade keeps a weight of its own there - the taps the loop makes anyway. Slope + LEAN
	// variance only (the Jacobian sums stay on w: no chop, no foam); amplitude and heading follow the
	// ocean's wind through the spectrum.
	const float16_t ripple = float16_t(u_terrainWetParams5.w) * (one - shore);
	const float16_t lodBase = log2(max(footprintH, float16_t(1e-3)) * float16_t(OCEAN_FFT_SIZE));
	// THE FLOW ("Film flow speed (m/s)" u_terrainWetParams6.w, "Film flow cycle (s)" u_terrainWetParams8.w):
	// water on a slope runs DOWNHILL, so the inland ripples - the finest cascade's slope tap and the detail
	// tap - travel that way. Direction: +N.xz of the smooth mesh normal - a normal leans toward the DOWNHILL
	// side (y = x has N = (-1, 1)/sqrt2: downhill is -x) - exact per pixel (the terrain-data
	// map's 8-bit flow angles are 8 m nearest texels). Speed: "Film flow speed" x sqrt(tan slope) - a thin
	// sheet speeds up with the slope - gated by the slope ("Film flow min slope (deg)", u_terrainWetParams10.xy
	// = tan of half of it and of it): still on gentle ground and flats, so pools lie still. (A tan^2 curve
	// instead stilled the flats but slowed the moderate slopes with them.)
	// Off the shore only (x (1 - shore)): the shore band carries the ocean's own waves. A two-phase flow map:
	// each tap twice, offset along the flow by a sawtooth half a cycle apart, crossfaded by a triangle so
	// neither phase's reset shows (the offsets stay within +-speed x cycle / 2).
	// Only the velocity x cycle (m) and the phase stay live through the taps; the offsets derive at use.
	// (When only the tessellated film had it, the second-phase taps cost the far, untessellated film 16
	// B/thread: 56/48 -> 56/64. The film is now one untessellated variant, so it carries the flow everywhere.)
	vec2 flowSpan = vec2(0.0); // velocity x cycle: the distance one phase travels
	{
		const vec3 n = normalize(in_normal);
		const float horiz = length(n.xz);
		const float tanSlope = horiz / max(n.y, 0.1);
		const float speed = u_terrainWetParams6.w * sqrt(tanSlope)
			* smoothstep(u_terrainWetParams10.x, u_terrainWetParams10.y, tanSlope) * float(one - shore);
		if (speed > 1e-3 && horiz > 1e-4)
			flowSpan = n.xz * (speed * max(u_terrainWetParams8.w, 0.05) / horiz);
	}
	const float flowPh0 = fract(u_timeSeconds / max(u_terrainWetParams8.w, 0.05));
	const vec2 flowOff0 = flowSpan * (flowPh0 - 0.5);
	const vec2 flowOff1 = flowSpan * (fract(flowPh0 + 0.5) - 0.5);
	const float16_t flowMix = dot(flowSpan, flowSpan) > 0.0 ? float16_t(abs(2.0 * flowPh0 - 1.0)) : float16_t(0.0); // phase 1's share

	// Wave slopes, LEAN variance, vertical acceleration and the RAW fold Jacobian sums of the cascades.
	f16vec2 slopeSum = f16vec2(0.0), varSum = f16vec2(0.0);
	float16_t rxx = float16_t(0.0), rzz = float16_t(0.0), rxz = float16_t(0.0);
	float16_t accel = float16_t(0.0);
	f16vec3 fineJ = f16vec3(0.0);
	f16vec2 lastSlope = f16vec2(0.0); // the finest cascade's phase-0 slope, for the phase-1 blend after the loop
	for (int c = 0; c < OCEAN_CASCADES; ++c)
	{
		const float16_t wc = c == OCEAN_CASCADES - 1 ? max(w, ripple) : w; // this cascade's slope weight
		const float L = u_oceanParams2[c];
		const float16_t lod = max(lodBase - float16_t(log2(L)), float16_t(0.0));
		const vec2 uvc = worldPos.xz / L;
		// The finest cascade's slopes FLOW (above; the offsets are 0 without it): the ripple normal travels,
		// the moments / Jacobian (shore-gated foam) stay put. Its phase 1 blends in AFTER the loop:
		// inside it, the second tap cost 16 B/thread.
		const bool flows = c == OCEAN_CASCADES - 1;
		const vec4 g32 = textureLod(u_uwOceanMaps, vec3(flows ? (worldPos.xz - flowOff0) / L : uvc, float(OCEAN_CASCADES + c)), lod); // (dh/dx, dh/dz, dDx/dx, dDz/dz)
		if (flows)
			lastSlope = f16vec2(g32.xy);
		const vec4 m32 = textureLod(u_uwOceanMaps, vec3(uvc, float(2 * OCEAN_CASCADES + c)), lod); // LEAN moments, accel
		const float16_t dxz = float16_t(textureLod(u_uwOceanMaps, vec3(uvc, float(c)), lod).w); // displacement layer w = dDx/dz
		// Slope variance lost to mip filtering (Bruneton 2010): the cancelling difference in 32-bit.
		varSum += f16vec2(max(m32.xy - g32.xy * g32.xy, vec2(0.0))) * (wc * wc);
		const f16vec4 g = f16vec4(g32);
		slopeSum += g.xy * wc;
		rxx += g.z; rzz += g.w; rxz += dxz;
		accel += float16_t(m32.z);
		if (c == OCEAN_CASCADES - 1)
			fineJ = f16vec3(g.z, g.w, dxz); // for the stuck foam's Jacobian ("Foam fine waves")
	}
	// The ocean's world-space foam field (ocean_foam_field.inc.glsl): the foam amount breaking left stuck to
	// the water - white foam above "Foam threshold", the bubble cloud below. At the film's own
	// position: near the shore the rest lattice the ocean samples it at lies within a displacement of it.
	float16_t foamAmount = float16_t(oceanSampleFoamField(u_uwOceanMaps, worldPos.xz, float(footprintH), 0.0)); // shore-gated below
	const float stuckAmount = float(foamAmount) * u_oceanFoamField1.x; // coverage below, on the weighted Jacobian
	// The finest cascade's flow phase 1, crossfaded in: slope only.
	if (flowMix > float16_t(0.0))
	{
		const int fc = OCEAN_CASCADES - 1;
		const float Lf = u_oceanParams2[fc];
		const float16_t lodF = max(lodBase - float16_t(log2(Lf)), float16_t(0.0));
		const f16vec2 s1 = f16vec2(textureLod(u_uwOceanMaps, vec3((worldPos.xz - flowOff1) / Lf, float(OCEAN_CASCADES + fc)), lodF).xy);
		slopeSum += (s1 - lastSlope) * (flowMix * max(w, ripple));
	}
	accel *= w;
	foamAmount *= shore; // the field is open-ocean math over land too; off the shore it would milk and roughen a puddle
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
	f16vec2 detailSlope = f16vec2(0.0); // kept apart for the foam's lighting slope
	{
		float16_t detailFade = max(w, ripple);
		if (u_oceanParams11.z > 0.0)
			detailFade *= float16_t(1.0 - smoothstep(0.5 * u_oceanParams11.z, u_oceanParams11.z, distance(worldPos.xz, u_viewPos.xz)));
		if (float16_t(u_oceanParams11.x) * detailFade > float16_t(0.0))
		{
			const int dc = OCEAN_CASCADES - 1;
			const float Ld = max(u_oceanParams2[dc] * u_oceanParams11.y, 1e-3);
			const vec2 rot = vec2(cos(u_oceanParams11.w), sin(u_oceanParams11.w));
			const float16_t lodD = max(lodBase - float16_t(log2(Ld)), float16_t(0.0));
			// Flowing too (the same two phases).
			const vec2 q0 = worldPos.xz - flowOff0;
			const vec2 p0 = vec2(rot.x * q0.x + rot.y * q0.y, -rot.y * q0.x + rot.x * q0.y);
			vec2 s32 = textureLod(u_uwOceanMaps, vec3(p0 / Ld, float(OCEAN_CASCADES + dc)), lodD).xy;
			if (flowMix > float16_t(0.0))
			{
				const vec2 q1 = worldPos.xz - flowOff1;
				const vec2 p1 = vec2(rot.x * q1.x + rot.y * q1.y, -rot.y * q1.x + rot.x * q1.y);
				s32 = mix(s32, textureLod(u_uwOceanMaps, vec3(p1 / Ld, float(OCEAN_CASCADES + dc)), lodD).xy, float(flowMix));
			}
			const f16vec2 s = f16vec2(s32) * (float16_t(u_oceanParams11.x) * detailFade);
			const f16vec2 rotH = f16vec2(rot);
			detailSlope = f16vec2(rotH.x * s.x - rotH.y * s.y, rotH.y * s.x + rotH.x * s.y); // back into world space
			slope += detailSlope;
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
	// ALL the film's foam reads the Jacobian sums with the finest cascade scaled by "Foam fine waves"
	// (u_oceanFoamField2.x, as the ocean's foamJacobian): the short, fast waves would reshape it every frame.
	const f16vec3 rFoam = f16vec3(rxx, rzz, rxz) + fineJ * float16_t(u_oceanFoamField2.x - 1.0);
	float16_t foam = float16_t(0.0);
	if (u_oceanParams5.z > 0.0)
	{
		const float16_t Jraw = (one + chop * rFoam.x) * (one + chop * rFoam.y) - chop * rFoam.z * chop * rFoam.z;
		// The ocean's tongue has column ~ 0, i.e. lace target 1, over its whole run-up - so does its film.
		const float16_t target = maskH * shore;
		// The target fades the lace in through its THRESHOLD, as the ocean's (ocean.fs.glsl) - not as a coverage
		// multiplier. `shore` alone still multiplies, as the LAND gate: the raw Jacobian is the full open-sea
		// fold field inland too, so a threshold alone would foam the puddles.
		const float16_t b = mix(float16_t(u_oceanFoam.w - 0.8), float16_t(1.45), target) + float16_t(u_oceanParams8.y);
		const float16_t foamMax = float16_t(max(u_oceanParams7.y, 1e-3));
		// The ocean's knee, normalised so full lace reaches the cap (ocean.fs.glsl: the bare knee stopped at 0.63).
		const float16_t kneeNorm = float16_t(1.0 / (1.0 - exp(-1.0 / max(u_oceanParams7.y, 1e-3))));
		foam = shore * foamMax * (one - exp(-(one - smoothstep(b - float16_t(0.4), b + float16_t(0.4), Jraw)) / foamMax)) * kneeNorm;
	}
	// The ocean's crest foam: oceanInstantFoam (ocean_wave.inc.glsl) inlined - fold of the WEIGHTED Jacobian
	// or a breaking downward acceleration. The water beside the film wears max(crest, lace), and the lace
	// alone is capped at "Shore foam max".
	{
		const f16vec3 wFoam = rFoam * (w * chop); // weighted, as the normal's Jacobian
		const float16_t jacobian = (one + wFoam.x) * (one + wFoam.y) - wFoam.z * wFoam.z;
		const float16_t softness = float16_t(max(u_oceanParams4.w, 0.02));
		const float16_t bias = float16_t(u_oceanFoam.w);
		const float16_t fold = one - smoothstep(bias - softness, bias, jacobian);
		const float16_t breakStart = float16_t(u_oceanParams5.w);
		const float16_t breaking = smoothstep(breakStart, breakStart + softness, -accel * float16_t(1.0 / 9.81));
		// The ocean's stuck foam: its density over this Jacobian, thresholded (no screen derivatives in this
		// branch: no AA widening - the Jacobian taps are already mip-filtered to the footprint).
		const float16_t stuckFoam = float16_t(oceanStuckFoamCoverage(oceanStuckFoamDensity(stuckAmount, float(jacobian)), 0.0));
		foam = clamp(max(foam, maskH * shore * max(max(fold, breaking), stuckFoam)), float16_t(0.0), one);
	}
	// Entrained bubbles: the ocean's foam amount (the decaying memory of breaking, strongest exactly at the
	// shore) is the bubble cloud and roughens the water. The surf beside the film carries it,
	// so the film carries it too - read blurred by "Bubble blur (m)", as the ocean's cloud.
	const float16_t milk = clamp(float16_t(oceanSampleFoamField(u_uwOceanMaps, worldPos.xz, float(footprintH), u_oceanFoamField2.y))
		* shore, float16_t(0.0), one);
	// The ocean's microfacet alpha on the film's own base ("Water roughness"): perceptual roughness^2, plus the LEAN slope variance (scaled by
	// "Glint filtering") that stretches the glitter toward the horizon, plus the churn's
	// micro-roughness. No spec-AA term: that is a screen derivative, undefined in this branch.
	// NOTE: the lit core's `roughness` parameter IS the GGX alpha, so it goes in directly - passing a
	// perceptual value there gave the film a wider glint than the water beside it.
	// alpha^2 in 32-bit (its perceptual^4 term underflows half); the half terms widen into it.
	const float baseRough = clamp(u_terrainWetParams2.z, 0.02, 1.0); // "Water roughness" (perceptual, as the ocean's)
	const float slopeVariance = float(float16_t(0.5) * (slopeVar.x + slopeVar.y) * (ns * ns));
	const float alphaSq = baseRough * baseRough * baseRough * baseRough
		+ 2.0 * slopeVariance * u_oceanParams6.y + float(foamAmount) * 0.35;

	// The foam's lighting slope, the ocean's rule (ocean.fs.glsl foamSlope): the large waves eased flat by
	// "Foam flatten", the sub-band detail x "Foam detail", at the OCEAN's normal strength - not the film's
	// normal scale or waviness, so the film's foam lights exactly like the ocean's beside it.
	// Resolved to its N.L here, so one half crosses into the shading stage, not the slope.
	const f16vec2 foamSlope = ((slope - detailSlope) * float16_t(1.0 - u_oceanParams12.z) + detailSlope * float16_t(u_oceanFoamField1.w))
		* float16_t(u_oceanParams1.w);
	const float16_t foamNoL = max(dot(normalize(f16vec3(-foamSlope.x, float16_t(1.0), -foamSlope.y)), f16vec3(u_sunDirection.xyz)), float16_t(0.0));

	return TerrainFilm(N, float16_t(clamp(sqrt(alphaSq), 0.02, 1.0)), foam, milk, foamNoL);
}

// The film over the lit ground (the "body"), as the overlay composites it: final = body * groundFactor +
// addColor, per channel (the dual-source blend supplies the body - the pass never reads the scene colour).
// waterDepth = the metres of water standing over this pixel (the film's own body thickness). Shaded in HALF math, as the ocean's top side: the vectors, the weights and the
// colours (the scene colour is RGBA16F). The sky ray's direction stays 32-bit (widened from the half N / V).
void terrainFilmShade(vec3 worldPos, TerrainFilm film, float16_t maskH, float16_t waterDepth, out f16vec3 addColor, out f16vec3 groundFactor)
{
	const f16vec3 Nh = film.N;
	// The GI's sky visibility on both sky reflections (the blurred share and the mirror): a puddle under a roof
	// or an overhang mirrors no sky. First: its lookup peaks before the body's and the foam's values are live.
	const float16_t skyVis = float16_t(giSkyVisibility(worldPos, vec3(Nh)));
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
	// = the ocean's sunTint, cloud shadow included: without it the film's in-scatter and reflection fog stayed
	// sunlit under a cloud, brighter than the ocean it meets.
	const vec3 sunTint = u_sunTransmittance * u_sunColor.rgb * (u_eclipseParams.x * cloudSunTransmittance(worldPos));
	// The sky light on the film (body in-scatter, whitewater): the HEMISPHERE average E(up) / pi of the GI sky
	// SH, constant per frame. Not the sky map's zenith texel: with clouds on that is the one cloud straight over
	// the camera, and every puddle took its colour (the ocean's rule). GI off (u_aoParams.y 0, stale SH): the
	// zenith texel.
	const f16vec3 ambientSky = u_aoParams.y > 0.0
		? max(giEvalSkySHH(f16vec3(up)) * float16_t(INV_PI), f16vec3(0.0))
		: f16vec3(textureLod(u_skyMap, vec3(skyMapUV(up), SKY_MAP_LAYER_GI), 0.0).rgb);

	// Body: the ground seen through the film - Beer-Lambert absorbed along the refracted path through the
	// water ACTUALLY standing here (the film's coverage depth: the pool the wetness fills the relief to, or
	// the live ocean over the ground, in metres), with the ocean's in-scatter filling in what was absorbed.
	// Exactly the ocean shader's body mix, tinted = body * T + tintAdd, so a puddle deepens toward its
	// middle and the two are the same colour where the film meets the ocean.
	f16vec3 T = f16vec3(1.0);
	f16vec3 tintAdd = f16vec3(0.0);
	const f16vec3 inscatter = f16vec3(u_oceanScatter.rgb * u_oceanScatter.w) * (ambientSky + f16vec3(sunTint * (max(L.y, 0.0) * INV_PI)));
	if (waterDepth > float16_t(0.0))
	{
		const vec3 refrDir = refract(-vec3(Vh), vec3(Nh), 1.0 / 1.33);
		const float path = float(waterDepth) / max(-refrDir.y, 0.2);
		T = f16vec3(exp(-u_oceanAbsorption.rgb * path));
		tintAdd = inscatter * (f16vec3(1.0) - T);
	}

	// The sun on the foam and the bubbles: sunSurfaceRadiance() - the pixel's ALREADY-RESOLVED visibility
	// (main: shadow x cloud shadow x eclipse, no second shadow evaluation). NOT sunTint x that: sunTint carries
	// the cloud and the eclipse too, and the product squared the cloud shadow (a darker foam than the ocean's).
	const f16vec3 sunLight = f16vec3(sunSurfaceRadiance());
	// Whitewater: EXACTLY the ocean's (ocean.fs.glsl) - Lambert on the foam's own slope (film.foamNoL, the
	// ocean's rule), plus sky and ambient - so the two foams meet in one tone. Skipped where the lace would
	// not show it.
	f16vec3 whitewater = f16vec3(0.0);
	if (foamH > float16_t(0.003))
		whitewater = f16vec3(u_oceanFoam.rgb) * (sunLight * (film.foamNoL * float16_t(INV_PI)) + ambientSky + f16vec3(u_ambientColor));
	// Bubbles: tinted = mix(body * T + tintAdd, bubbles, milk) - the ocean's bubble cloud, no deeper than
	// the water standing here.
	f16vec3 bubbles = f16vec3(0.0);
	if (milk > float16_t(0.003))
		bubbles = oceanBubbleRadiance(min(float16_t(u_oceanParams12.x), max(waterDepth, float16_t(0.0))), NoV, float16_t(max(L.y, 0.0)),
			sunLight, ambientSky + f16vec3(u_ambientColor), inscatter);

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
	vec3 R = reflect(-vec3(Vh), vec3(Nh));
	R.y = max(R.y, 0.02);
	R = normalize(R);
	// The blur's sky share: the sky SH's cosine lobe (E(n) / pi) centred between R and up, so a rough film sees
	// the sky on its own side without half the lobe below the horizon (the ocean's rule).
	const f16vec3 blurSky = u_aoParams.y > 0.0 ? max(giEvalSkySHH(f16vec3(normalize(R + up))) * float16_t(INV_PI), f16vec3(0.0)) : ambientSky;
	f16vec3 color = (maskH * foamH) * whitewater
		+ clearW * ((one - F) * (tintAdd * (one - milk) + bubbles * milk) + (F * reflBlur * skyVis) * blurSky + glint);

	// Reflection: the sky (the ray-traced scene mirror is disabled, TERRAIN_FILM_RT_MIRROR), roughness-
	// blurred toward the sky around R (the blur's sky share is in the fold above). Before the lights: with the
	// mirror, lights-first kept N / V and the mirror gate live across the lights' own shadow-ray peak, which
	// cost more (368 vs 352 B/thread).
	// The sky fallback, fogged too (the baked mirror sky carries none), resolved BEFORE the mirror trace and
	// folded in whole: a hit swaps its share for the hit below. So neither R's sky lookup nor the ground
	// normal is live across the trace - only the half sky colour and the hit's slope share. (A hit pixel
	// pays the one sky fetch it could have skipped; hits are the minority of film pixels.)
	const f16vec3 skyMirror = f16vec3(min(applyReflectionFogSky(terrainReflectedSkyRadiance(R), worldPos, R, sunTint, L, vec3(ambientSky)), vec3(65504.0))) * (mirrorWeight * skyVis);
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
	// THIS LOOP IS THE PASS'S REGISTER PEAK, and it is the shadow rays: doLight instead measured 56/0
	// against 56/48 (demand 56 vs 68). Lights-first, or no separate accumulator, measured the same.
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

// The relief depth (m) the film's own surface was displaced in (terrain_tess.tes.glsl).
float terrainReliefDepth(TerrainLayers L)
{
	return mix(mix(u_terrainTessParams1.z, u_terrainTessParams1.w, float(L.rockW)), u_terrainTessParams1.z, float(L.snowW));
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	const TerrainFields fields = terrainFields();
	// The smooth mesh normal: the layer coverages, the slope drain and the sun gate read it (the film shades
	// on its own normals).
	const vec3 coverN = normalize(in_normal);
	const vec3 geoN = coverN;
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
	// The film's sun visibility (glint, whitewater, bubbles): the ground pass's resolve is not available here,
	// so ONE hard tap (the moving water hides a penumbra), or one ray with the RT sun, as the ocean does. x the
	// cloud shadow, as the lit core's sunShadowVisibility: without it the film's foam and glint stayed sunlit
	// under a cloud, brighter than the ocean beside it.
	// AT THE WATER SURFACE (in_pos, the level water's up normal), where the foam and the glint are - the
	// ocean's rule. It was the GROUND point under the water (TERRAIN_LIT_POS, gated on the ground normal):
	// metres under the live ocean's edge, sunk into the relief, facing away on the far side of a ripple - a
	// sun the surface never loses.
	const vec3 L = u_sunDirection.xyz;
	float sunVis = 0.0;
	if (L.y > 0.0)
	{
#if LIT_RT_SUN_SHADOW
		sunVis = rtShadowVisibility(in_pos + vec3(0.0, 0.1, 0.0), L, 0.05, 10000.0);
#else
		sunVis = sampleSunShadowHard(in_pos, vec3(0.0, 1.0, 0.0));
#endif
		if (sunVis > 0.0)
			sunVis *= cloudSunTransmittance(in_pos);
	}
	g_sunVisSurface = float16_t(sunVis * u_eclipseParams.x);
	const TerrainFilm film = terrainFilmSurface(in_pos, wetFootprint, filmMask, fields.waterLevel - in_pos.y, fields.waterLevel);
	f16vec3 addColor, groundFactor;
	terrainFilmShade(in_pos, film, filmMask, waterDepth, addColor, groundFactor);
	out_color = vec4(addColor, 0.0);
	out_factor = vec4(groundFactor, 1.0);
}
