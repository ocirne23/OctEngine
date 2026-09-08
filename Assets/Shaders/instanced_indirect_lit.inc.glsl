// Shared lighting core for the lit instanced_indirect fragment variants (instanced_indirect.fs.glsl,
// instanced_indirect_terrain.fs.glsl): the material/light/GI/RT bindings and computeLitColor()
// (screen-space AO + GI probes + sun with underwater caustics + the clustered light grid).
// The includer provides #version, the extensions and shared.inc.glsl first.
#ifndef INSTANCED_INDIRECT_LIT_INC_GLSL
#define INSTANCED_INDIRECT_LIT_INC_GLSL

struct MaterialInfo
{
	uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
	uint metalRoughnessTexIdxAlphaMode;
};
struct LightInfo
{
	vec3 pos;
	float range;
	vec3 color;
	float width;     // 0 = point light, > 0 = rectangular area light
	vec3 direction;  // area light: up-axis, magnitude = height
	float rotation;  // area light: rotation of the quad around direction
};

layout (binding = 2, std430) readonly buffer InMaterialInfos
{
	MaterialInfo in_materialInfos[];
};
layout (binding = 4, std430) readonly buffer InLightInfos
{
	LightInfo in_lightInfos[];
};
layout (binding = 5, std430) readonly buffer InLightGrid
{
    uint in_gridData[];
};
layout (binding = 6, std430) readonly buffer InGridTable
{
	uint in_numGrids;
	uint in_gridDataCounter;
    uint in_tableSize;
    uint in_gridTable[];
};

layout (binding = 20) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
layout (binding = 8) uniform sampler2DArrayShadow u_shadowMap;      // comparison sampler (hardware PCF)
layout (binding = 9) uniform sampler2DArray u_shadowMapDepth;       // raw depth (PCSS blocker search)
layout (binding = 13) uniform sampler2D u_ao;                       // denoised half-res screen-space AO (bilateral upsample)
layout (binding = 12) uniform sampler2D u_gbufferDepth;             // full-res hardware depth (AO upsample edge weights)

layout (binding = 11) uniform accelerationStructureEXT u_tlas;       // ray-traced shadows for punctual/area lights

// Mesh/instance data for the shadow rays' alpha test (rt_shadow.inc.glsl). in_instances must be the same
// buffer the TLAS instance records were built from (custom index = index into it), not the culled list.
struct InMeshInfo
{
	vec3 center;
	float radius;
	uint indexCount;
	uint firstIndex;
	int  vertexOffset;
	uint _padding;
};
struct InMeshInstance
{
	uint renderNodeIdx;
	uint instanceOffsetIdx;
	uint meshIdxMaterialIdx;
	uint pipelineIdxAlphaMode;
};
layout (binding = 14, std430) readonly buffer InRTVertices  { float in_vertices[]; }; // MeshVertex as 12 floats
layout (binding = 15, std430) readonly buffer InRTIndices   { uint in_indices[]; };
layout (binding = 16, std430) readonly buffer InRTMeshInfos { InMeshInfo in_meshInfos[]; };
layout (binding = 17, std430) readonly buffer InRTInstances { InMeshInstance in_instances[]; };

// GI irradiance probes (diffuse indirect). Persistent cascaded clipmap SH volume, written by the probe
// trace pass; addressed toroidally relative to the camera (u_viewPos), so no hash table is needed.
layout (binding = 10, std430) readonly buffer GiGridData { float gi_gridData[]; };

#include "shadows.inc.glsl"

#define GRID_DATA_NAME         in_gridData
#define GRID_TABLE_NAME 	   in_gridTable
#define TABLE_SIZE_NAME        in_tableSize
#include "light_grid.inc.glsl"

#define GI_GRID_DATA_NAME      gi_gridData
#include "gi_probe.inc.glsl"

#include "rt_shadow.inc.glsl"

#include "punctual_lights.inc.glsl"

// Terrain data cascades (height/water level/climate/altitude) + FFT ocean maps: underwater sunlight
// (caustics) for EVERY lit material, and the terrain variant's procedural coloring. Both bindings exist
// set-wide (19 = terrain data, 7 = u_oceanMaps — see StaticMeshGraphicsPipeline::buildPipelineLayout).
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"
#define UNDERWATER_OCEAN_BINDING 7
#include "underwater_light.inc.glsl"

// Optional precomputed local water level, so callers that already sampled the terrain data cascade (the
// terrain variant does, for its splatting) don't pay for a second identical terrainDataAt fetch here.
// >= this sentinel = not provided -> fetch it. Default keeps every other lit variant unchanged.
const float WATER_LEVEL_UNSET = 1e30;
float g_waterLevelOverride = WATER_LEVEL_UNSET;

vec3 doSunLight(vec3 worldPos, vec3 V, vec3 N, vec3 specularCol, vec3 matColOverPi, float metalness, float roughness, float roughnessSq)
{
	vec3 L = normalize(u_sunDirection.xyz);
	if (max(dot(N, L), 0.0) <= 0.0)
		return vec3(0.0);
	float visibility = u_rtSunShadow > 0.5 ? traceSunVisibility(worldPos, N) : sampleSunShadow(worldPos, N);
	// Long-range terrain shadows. BOTH sources above run out of data well inside the streamed mesh ring —
	// the cascades end at Shadows/Max distance (3 km default), the RT path's instances at RT/TLAS Range
	// (4 km) — while terrain meshes run to ~33 km, so distant ground otherwise sits uniformly lit behind a
	// hard terminator. The baked height cascades still cover all of it, so march them there and take the
	// DARKER of the two: inside the fade band both terms see the same ridge (min is a no-op, no double
	// darkening), past it only the march survives. Skipped up close, where the map's texels are coarser
	// than the geometry they would be shadowing and could only produce acne.
	// Reach is bias * 2^10 — ~77 km at the default 150 m, comfortably past the far cascade's own range.
	if (u_terrainShadowParams.x > 0.0 && terrainHeightMapPresent())
	{
		const float fadeIn = smoothstep(u_terrainShadowParams.x, u_terrainShadowParams.x * 1.25, distance(worldPos, u_viewPos));
		if (fadeIn > 0.0)
		{
			const float terrainVis = terrainSunVisibility(worldPos, L, u_terrainShadowParams.y, 10, u_terrainShadowParams.z, 4.0);
			visibility = min(visibility, mix(1.0, terrainVis, fadeIn));
		}
	}
	vec3 lightRadiance = atmosTransmittanceToLight(0.0, L, u_skyUp) * u_sunColor.rgb * visibility * u_eclipseParams.x;
	// Underwater: the sun crossed the wavy surface — caustic focus + Beer-Lambert absorption
	// (underwater_light.inc.glsl), so seabed/submerged objects get the dancing light patterns. One
	// water-level fetch + a branch above water; only underwater pixels pay for the wave taps.
	if (terrainHeightMapPresent())
	{
		const float localWaterLevel = g_waterLevelOverride < WATER_LEVEL_UNSET ? g_waterLevelOverride : terrainDataAt(worldPos.xz).y;
		float depthBelow = localWaterLevel - worldPos.y;
		// Swash band: gate against the LIVE displaced surface, not the calm level — a receded wave
		// leaves sand below the calm line dry (no caustics/absorption tint on exposed bottom), and the
		// run-up tongue is lit as underwater while it covers the beach. Points deeper than the swash
		// reach are underwater at any wave phase and skip the wave taps (u_oceanParams7.w is 0 with
		// swash off, so this is free for non-swash setups).
		const float swashReach = u_oceanParams7.w;
		if (swashReach > 0.0 && abs(depthBelow) < swashReach)
			depthBelow += underwaterLiveWaveY(worldPos.xz, depthBelow, localWaterLevel);
		if (depthBelow > 0.0)
			lightRadiance *= underwaterSunTransmittance(worldPos.xz, depthBelow, 0.0, 1.0); // surfaces: physical reach
	}
	return doLight(lightRadiance, L, V, N, specularCol, matColOverPi, metalness, roughness, roughnessSq);
}
// Depth-aware 2x2 upsample of the half-res AO/bent-normal image. Plain bilinear bleeds across depth
// discontinuities (a far wall's AO/bent normal mixing into a near silhouette shows as a bright GI rim),
// so each tap's bilinear weight is scaled by its world-space distance to the shaded point. Tap depths
// come from the full-res depth at the tap's UV (the half-res texel center), which is close enough to
// the depth the trace actually used.
vec4 sampleAOBilateral(vec2 fullUv, vec3 pos, float viewDist)
{
	const vec2 aoRes   = ceil(u_screenSize.xy * 0.5);
	const vec2 aoTexel = 1.0 / aoRes;
	const vec2 st   = fullUv * aoRes - 0.5;
	const vec2 base = (floor(st) + 0.5) * aoTexel;
	const vec2 f    = fract(st);
	const float bw[4] = float[]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	const vec2 offs[4] = vec2[](vec2(0.0), vec2(aoTexel.x, 0.0), vec2(0.0, aoTexel.y), aoTexel);

	const float sigmaZ = max(0.05 * viewDist, 0.02);
	vec4 sum = vec4(0.0);
	float wsum = 0.0;
	for (int i = 0; i < 4; ++i)
	{
		const vec2 uv = base + offs[i];
		const float d = texture(u_gbufferDepth, uv).r;
		if (d <= 0.0) // background (reversed-Z far = 0)
			continue;
		const vec3 tapPos = worldPosFromDepth(uv, d);
		const float dz = length(tapPos - pos);
		const float w  = bw[i] * exp(-(dz * dz) / (2.0 * sigmaZ * sigmaZ));
		sum  += texture(u_ao, uv) * w;
		wsum += w;
	}
	// All taps rejected (thin geometry the half-res image never saw): fall back to plain bilinear.
	return wsum > 1e-4 ? sum / wsum : texture(u_ao, fullUv);
}

// Full surface lighting for one shaded point: screen-space AO + bent-normal GI probe irradiance, sun
// (with underwater caustics) and the clustered light grid. texAO multiplies only the ambient/indirect
// term (baked texture AO on top of the screen-space term — pass 1.0 when the material carries none).
vec3 computeLitColor(vec3 worldPos, vec3 V, vec3 N, vec3 materialColor, float roughness, float metalness, float texAO)
{
	const vec3 specularColor  = mix(vec3(0.04), materialColor, metalness);
	const float roughnessSq = roughness * roughness;
	const vec3 matColOverPi = materialColor / PI;

	float ao = 1.0;
	vec3 bentN = N;
	// Past the RTAO max distance (u_aoParams.z) the trace writes exactly (N, 1.0) — no occlusion, bent
	// normal = surface normal (rtao.cs.glsl early-out) — so the depth-aware upsample (up to 8 taps + 4
	// world-pos reconstructions) would only re-fetch those constants. Skip it and use them directly;
	// z = 0 (falloff disabled) keeps the upsample everywhere.
	const float aoViewDist = length(worldPos - u_viewPos);
	if (u_aoParams.x > 0.5 && (u_aoParams.z <= 0.0 || aoViewDist < u_aoParams.z))
	{
		const vec4 aoSample = sampleAOBilateral(gl_FragCoord.xy * u_screenSize.zw, worldPos, aoViewDist);
		ao = aoSample.w;
		// Evaluate the indirect irradiance along the bent normal rather than the surface normal: in concave
		// areas it points toward the open hemisphere, so the low-frequency probe SH stops leaking light from
		// occluded directions. Mix partway toward N so flat, unoccluded surfaces are left untouched.
		bentN = normalize(mix(N, normalize(aoSample.xyz), 0.75));
	}
	// Blend to the virtual sky probe over the probe field's outer band (coverage) instead of stepping
	// at the outermost cascade's window face; the fallback is only evaluated where it contributes.
	float giCoverage;
	const vec3 indirectE = evalProbeSHCoverage(worldPos, bentN, giCoverage);
	vec3 indirect = (indirectE.x >= 0.0) ? (indirectE / PI) : vec3(0.0);
	if (giCoverage < 1.0)
		indirect = mix(giEvalSkySH(bentN) / PI, indirect, giCoverage);
	indirect *= u_aoParams.y;
	ao *= texAO;
	vec3 color = materialColor * (indirect + u_ambientColor) * ao;

	color += doSunLight(worldPos, V, N, specularColor, matColOverPi, metalness, roughness, roughnessSq);
	const ivec3 gridPos = getGridPos(worldPos);
    uint tableIdx = getTableIdx(gridPos);

	// Light debug overlay ("Graphics/LOD/Light grid/Debug Mode", u_aoParams.w): 0 off, 1 grid cells,
	// 2 per-cell light count heat, 3 light ranges, 4 sun cascades. Uniform-driven, so no reload.
	const int debugMode = int(u_aoParams.w + 0.5);
	bool  debugHit = false;          // this point's grid was found in the hash table
	uint  debugLightCount = 0u;      // large + cell lights the point evaluated
	vec3  debugRangeTint = vec3(0.0);

	while (true)
	{
		const uint gridIdx = getGridIdx(tableIdx);
		if (gridIdx == EMPTY_ENTRY)
			break;
		const ivec3 gridMin = getGridMin(gridIdx);
		if (gridMin == gridPos)
		{
			debugHit = true;
			const uint numLargeLights = getLargeLightCount(gridIdx);
			for (uint i = 0; i < min(numLargeLights, MAX_LARGE_LIGHTS_PER_GRID); ++i)
			{
				const uint lightId    = getLargeLightId(gridIdx, i);
				const LightInfo light = in_lightInfos[lightId];
				color += doLightShadowed(light, worldPos, V, N, specularColor, matColOverPi, metalness, roughness, roughnessSq);
				if (debugMode == 3 && distance(worldPos, light.pos) < abs(light.range))
					debugRangeTint += vec3(0.0, 0.0, 0.08);
			}

			const uint cellOffset = calcCellOffset(gridIdx, gridMin, worldPos);
			const uint numLights  = getNumLightsForCell(cellOffset);
			for (uint i = 0; i < min(numLights, MAX_LIGHTCELL_LIGHTS); ++i)
			{
				const uint lightId    = getLightId(cellOffset, i);
				const LightInfo light = in_lightInfos[lightId];
				color += doLightShadowed(light, worldPos, V, N, specularColor, matColOverPi, metalness, roughness, roughnessSq);
				if (debugMode == 3 && distance(worldPos, light.pos) < abs(light.range))
					debugRangeTint += vec3(0.0, 0.0, 0.08);
			}
			debugLightCount = min(numLargeLights, MAX_LARGE_LIGHTS_PER_GRID) + min(numLights, MAX_LIGHTCELL_LIGHTS);
			break;
		}
		tableIdx = getNextTableIdx(tableIdx);
	}

	if (debugMode != 0)
	{
		if (debugMode == 1)      // one random colour per grid (the coarse hash-table entry)
			color = mix(color, randomColor(gridPos), 0.4);
		else if (debugMode == 2) // light count heat: green (1) -> red (the cell cap); magenta = no grid entry
		{
			if (!debugHit)
				color = mix(color, vec3(1.0, 0.0, 1.0), 0.5);
			else if (debugLightCount > 0u)
			{
				const float heat = clamp(float(debugLightCount) / float(MAX_LIGHTCELL_LIGHTS), 0.0, 1.0);
				color = mix(color, mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), heat), 0.5);
			}
		}
		else if (debugMode == 3) // every light whose range covers the point adds a step of blue
			color += debugRangeTint + vec3(0.0, 0.0, 0.02);
		else if (debugMode == 4) // sun shadow cascades
			color = mix(color, cascadeDebugColor(getSunCascade(worldPos)), 0.35);
	}
	return color;
}

//	#define GI_DEBUG_VIZ 0
//	#if GI_DEBUG_VIZ == 1
//		color = giDebugColor(worldPos, N);
//	#elif GI_DEBUG_VIZ == 2
//		color = (indirectE.x >= 0.0) ? indirectE / PI : vec3(0.0);
//	#endif

#endif // INSTANCED_INDIRECT_LIT_INC_GLSL
