#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// The procedural rocks (EPipelineIndex::LitRock; Procedural RockSystem, Docs/RockRenderingPlan.md 5): the lighting
// core of instanced_indirect.fs.glsl with a surface that comes from the CLIMATE, never from the rock type or a
// material. Bottom-up, the terrain splat's own materials (terrain_splat.inc.glsl) and rules:
//   1. BEDROCK - the splat's rock entries, picked by the climate at the rock exactly as a cliff face there picks
//                them, in world-space biplanar projection (a rock is static: world space does not swim, and every
//                instance of a shape gets its own piece of the texture).
//   2. GROUND  - the climate's ground material (moss, sand, leaf litter): a patchy COVER on the up-facing faces,
//                and the CONTACT BAND at the foot (it hides the line where the rock enters the ground).
//   3. SNOW    - the terrain's snow rule with this face's own slope: on the top, sliding off the sides.
// Then the terrain's wetness (darker, glossier). The material the instance carries is not read.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal; // geometric (interpolated vertex) normal
layout (location = 2) in vec4 in_rockFields; // VS-evaluated: x = ground height under the vertex (world Y), y = temperature C, z = humidity, w = water level
layout (location = 3) in float in_cavity;    // the vertex's baked cavity: 1 = open, 0 = deep in a crevice
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (location = 0) out vec4 out_color;
// The motion target: write-masked (a rock is static), but the opaque family's DGC set needs one fragment output
// interface (RendererVKLayout::PIPELINE_TRANSPARENT_MASK).
layout (location = 1) out vec4 out_motion;
// The sun shadow at the top of main, from the geometric normal (see instanced_indirect_lit.inc.glsl).
#define SUN_SHADOW_FIRST

#include "instanced_indirect_lit.inc.glsl"
#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl"
// No TERRAIN_SPLAT_RELIEF: linear layer blends, no parallax (R3's tessellation displaces by the height maps instead).
#include "terrain_splat.inc.glsl"

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	out_motion = vec4(0.0); // write-masked: static
	const vec3 geoN = normalize(in_normal);
	// THE SHADOW FIRST - the register peak - while only the position and the normal are live.
	g_sunShadowFirst = dot(geoN, u_sunDirection.xyz) > 0.0 ? sunShadowVisibility(in_pos, geoN) : 0.0;
	g_waterLevelOverride = in_rockFields.w; // the lit core's underwater test: no second terrain-data fetch

	const float16_t one = float16_t(1.0);
	const f16vec3 geoNh = f16vec3(geoN);
	const float cavity = clamp(in_cavity, 0.0, 1.0);
	// Before a splat texture set is registered (the terrain off, or its bake still running): plain grey stone.
	TerrainSample surf = TerrainSample(f16vec3(0.42, 0.40, 0.38), geoNh, float16_t(0.85), float16_t(0.0), one, float16_t(0.5));
	float16_t contact = float16_t(0.0);
	if (u_terrainTexParams0.x >= 0.0 && u_terrainTexParams0.z >= 1.0)
	{
		const int baseMat = int(u_terrainTexParams0.x);
		const int numGround = int(u_terrainTexParams0.y);
		const int numRock = int(u_terrainTexParams0.z);
		const float16_t opaque = float16_t(TERRAIN_LAYER_OPAQUE);
		const float16_t blendEps = float16_t(TERRAIN_BLEND_EPS);
		// The splat's climate space (terrainLayers): the temperature already carries the lapse to this height.
		const vec2 climate = vec2(clamp((in_rockFields.y + 25.0) / 75.0, 0.0, 1.0), in_rockFields.z);
		const float invS2 = 1.0 / (2.0 * u_terrainTexParams0.w * u_terrainTexParams0.w);

		// --- The coverages first (cheap), so a buried layer never samples ---
		// SNOW: the terrain's rule (cold x holds x humid) with this face's own slope - an underside holds none.
		float16_t snowW = float16_t(0.0);
		if (u_terrainTexParams3.y > 0.5)
		{
			const float cold = 1.0 - smoothstep(u_terrainTexParams3.z, u_terrainTexParams3.w, in_rockFields.y);
			const float holds = 1.0 - smoothstep(u_terrainTexParams4.x, u_terrainTexParams4.y, 1.0 - clamp(geoN.y, 0.0, 1.0));
			const float humid = smoothstep(0.0, max(u_terrainTexParams4.z, 1e-3), in_rockFields.z);
			snowW = float16_t(cold * holds * humid);
		}
		// THE CONTACT BAND: 1 at the ground, 0 "Contact height" above it. The ground height is the terrain-data
		// map's (per vertex): metres-wide texels near the camera, far coarser beyond - so the band fades out by
		// "Contact fade distance".
		const float bandFade = 1.0 - smoothstep(0.5 * u_rockParams2.x, u_rockParams2.x, distance(in_pos, u_viewPos));
		contact = float16_t((1.0 - smoothstep(0.0, max(u_rockParams1.x, 1e-3), in_pos.y - in_rockFields.x)) * bandFade);
		// THE GROUND COVER: "Ground cover" of the up-facing faces ("Cover start / full"), broken into patches by a
		// world noise ("Cover patch size") - and of the CREVICES ("Cavity cover" x the baked cavity: moss and dust
		// gather where the rock is occluded), on any face that does not look down.
		float cover = smoothstep(u_rockParams0.y, u_rockParams0.z, geoN.y);
		if (cover > 0.0)
			cover *= smoothstep(-0.25, 0.25, terrainFbm(in_pos.xz * u_rockParams0.w));
		cover = u_rockParams0.x * max(cover, u_rockParams2.z * (1.0 - cavity) * smoothstep(-0.2, 0.3, geoN.y));
		const float16_t groundW = numGround > 0 ? max(float16_t(cover), contact * float16_t(u_rockParams1.y)) : float16_t(0.0);

		// 1. The bedrock - buried under a full ground band or full snow: the placeholder stays, replaced below.
		if (snowW < opaque && groundW < opaque)
		{
			const ClimatePick r = pickClimate(climate, numGround, numRock, invS2);
			const float uvScale = u_terrainTexParams1.y * u_rockParams1.w;
			surf = sampleTerrainTriplanar(uint(baseMat) + climatePickIdx(r, 0), in_pos, geoNh, uvScale);
			if (r.n1 > blendEps)
				terrainMixInto(surf, sampleTerrainTriplanar(uint(baseMat) + climatePickIdx(r, 1), in_pos, geoNh, uvScale), r.n1 / max(one - r.n2, float16_t(1e-4)));
			if (r.n2 > blendEps)
				terrainMixInto(surf, sampleTerrainTriplanar(uint(baseMat) + climatePickIdx(r, 2), in_pos, geoNh, uvScale), r.n2);
		}

		// 2. The ground material (the cover and the contact band share it): the climate's top two ground entries,
		// biplanar too - the band runs up steep faces, where a world-XZ projection smears.
		if (groundW > blendEps && snowW < opaque)
		{
			const ClimatePick g = pickClimate(climate, 0, numGround, invS2);
			TerrainSample ground = sampleTerrainTriplanar(uint(baseMat) + climatePickIdx(g, 0), in_pos, geoNh, u_terrainTexParams1.x);
			if (g.n1 > blendEps)
				terrainMixInto(ground, sampleTerrainTriplanar(uint(baseMat) + climatePickIdx(g, 1), in_pos, geoNh, u_terrainTexParams1.x), g.n1 / max(one - g.n2, float16_t(1e-4)));
			if (groundW >= opaque)
				surf = ground;
			else
				terrainMixInto(surf, ground, groundW);
		}

		// 3. Snow (it lies on up-facing faces only: the world-XZ projection is enough).
		if (snowW > blendEps)
		{
			const uint snowMat = uint(baseMat + numGround + numRock) + (u_terrainTexParams3.x > 0.5 ? 1u : 0u);
			const TerrainSample snow = sampleTerrainXZ(snowMat, in_pos.xz * u_terrainTexParams2.w, geoNh);
			if (snowW >= opaque)
				surf = snow;
			else
				terrainMixInto(surf, snow, snowW);
		}
		surf.normal = normalize(surf.normal);
	}
	// The foot of the rock sits in the corner it makes with the ground: "Contact darkening" on the ambient. And the
	// shape's own crevices: the baked cavity x "Cavity AO".
	surf.ao *= (one - contact * float16_t(u_rockParams1.z)) * float16_t(mix(1.0, cavity, u_rockParams2.y));

	// THE WET LOOK, from the terrain's wetness field (rain, the swash): the ground's darkening and gloss with its
	// thresholds and its slope drain (a steep face dries faster), without the ground's drying pattern, glints and
	// sky reflection.
	if (terrainWetPresent())
	{
		const float16_t slope = one - float16_t(clamp(geoN.y, 0.0, 1.0));
		const float16_t wet = pow(float16_t(terrainWetnessAt(in_pos.xz)), one + slope * float16_t(u_terrainWetParams3.z));
		surf.albedo *= mix(one, float16_t(u_terrainWetParams2.y), smoothstep(float16_t(0.0), float16_t(max(u_terrainWetParams8.x, 1e-3)), wet));
		surf.rough = mix(surf.rough, float16_t(u_terrainWetParams7.x), smoothstep(float16_t(0.0), float16_t(max(u_terrainWetParams8.y, 1e-3)), wet));
	}

	// V HERE, not at the top: its registers are then not live across the shadow and the splat.
	const vec3 V = normalize(u_viewPos - in_pos);
	out_color = vec4(computeLitColor(in_pos, V, surf.normal, surf.albedo, surf.rough, surf.metal, surf.ao), 1.0);
}
