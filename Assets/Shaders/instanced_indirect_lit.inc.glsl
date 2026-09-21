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

layout (binding = 20) uniform sampler2DArray u_skyMap;   // GI's per-frame sky bake (atmosphere.inc.glsl: skyMapUV / SKY_MAP_LAYER_*)
layout (binding = 22) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
#ifdef GI_VOLUME
// The baked irradiance volume (GIProbePipeline, gi_volume_bake.cs.glsl): 4 images per cascade, partially bound.
layout (binding = 21) uniform sampler3D u_giVolume[GI_VOLUME_MAX_IMAGES];
#define GI_VOLUME_TEXTURES_NAME u_giVolume
#endif
layout (binding = 8) uniform sampler2DArrayShadow u_shadowMap;      // comparison sampler (hardware PCF)
layout (binding = 9) uniform sampler2DArray u_shadowMapDepth;       // raw depth (PCSS blocker search)
layout (binding = 13) uniform sampler2D u_ao;                       // LAST frame's denoised half-res screen-space AO (reprojected bilateral upsample)
layout (binding = 12) uniform sampler2D u_prevDepth;                // LAST frame's full-res hardware depth (the AO upsample's edge weights)

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
layout (binding = 10, std430) readonly buffer GiGridData { vec4 gi_gridData[]; }; // vec4 layout: gi_probe.inc.glsl

#include "shadows.inc.glsl"

#define GRID_DATA_NAME         in_gridData
#define GRID_TABLE_NAME 	   in_gridTable
#define TABLE_SIZE_NAME        in_tableSize
#include "light_grid.inc.glsl"

#define GI_GRID_DATA_NAME      gi_gridData
#define GI_PROBE_HALF // the fp16 read side (computeLitColor's indirect term is half)
#include "gi_probe.inc.glsl"

#include "rt_shadow.inc.glsl"

#include "punctual_lights.inc.glsl"

// The RT shadow toggles are defines on every includer of this core (the lit, masked, transparent and
// terrain fragments); the uniforms stay for the ocean, the fog and GI, which read them at run time.
#ifndef LIT_RT_SUN_SHADOW
#error "LIT_RT_SUN_SHADOW must be defined by the pipeline"
#endif
#ifndef LIT_RT_LIGHT_SHADOWS
#error "LIT_RT_LIGHT_SHADOWS must be defined by the pipeline"
#endif

// Terrain data cascades (height/water level/climate/altitude) + FFT ocean maps: underwater sunlight
// (caustics) for EVERY lit material, and the terrain variant's procedural coloring. Both bindings exist
// set-wide (19 = terrain data, 7 = u_oceanMaps - see StaticMeshGraphicsPipeline::buildPipelineLayout).
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"
#define UNDERWATER_OCEAN_BINDING 7
#include "underwater_light.inc.glsl"

// Optional precomputed local water level, so callers that already sampled the terrain data cascade (the
// terrain variant does, for its splatting) don't pay for a second identical terrainDataAt fetch here.
// >= this sentinel = not provided -> fetch it. Default keeps every other lit variant unchanged.
const float WATER_LEVEL_UNSET = 1e30;
float g_waterLevelOverride = WATER_LEVEL_UNSET;
// The sun's shadow visibility x eclipse that doSunLight resolved for this pixel (PCSS / RT / terrain
// march), BEFORE the underwater factor (no caustic focus, no Beer-Lambert): the sun as it arrives at the
// water SURFACE above this pixel. A material that adds a lobe on that surface after computeLitColor (the
// terrain's water film glint and whitewater) lights it with sunSurfaceRadiance() instead of paying the
// shadow evaluation again - with the underwater factor the ground's caustic pattern rode into the film's
// specular and showed as warped caustics on every filmed pixel under a wave. One HALF scalar, not the
// radiance: it is live across computeLitColor's whole light loop. 0 = sun behind the surface.
float16_t g_sunVisSurface = float16_t(0.0);
vec3 sunSurfaceRadiance()
{
	return u_sunTransmittance * u_sunColor.rgb * float(g_sunVisSurface);
}
// Depth of this pixel below the LIVE water surface (m; negative = above it, -1e30 = no terrain data),
// resolved by doSunLight for every pixel - the same test that gates the caustics/absorption, published
// so the terrain's water film can gate on it too (ground under the live surface is the ocean's to draw).
float g_liveDepthBelow = -1e30;
float g_liveWaterLevel = 0.0;      // the calm local level the depth was measured from
bool  g_liveDepthResolved = false; // resolveLiveDepth ran for this pixel (a material may call it EARLY -
                                   // the terrain gates its wetness gloss on it before computeLitColor)

// Resolves g_liveDepthBelow / g_liveWaterLevel for worldPos, once per pixel. One water-level fetch (or
// the material's override); only the swash band pays for the wave taps.
void resolveLiveDepth(vec3 worldPos)
{
	if (g_liveDepthResolved)
		return;
	g_liveDepthResolved = true;
	if (!terrainHeightMapPresent())
		return;
	const float localWaterLevel = g_waterLevelOverride < WATER_LEVEL_UNSET ? g_waterLevelOverride : terrainDataAt(worldPos.xz).y;
	float depthBelow = localWaterLevel - worldPos.y;
	// Gate against the LIVE displaced surface, not the calm level - a receded wave leaves sand below
	// the calm line dry (no caustics/absorption tint on exposed bottom), and the run-up tongue is lit
	// as underwater while it covers the beach. The wave taps are paid only near the surface: within
	// the surface's max EXCURSION - the larger of the swash reach (u_oceanParams7.w) and the open-water
	// trough (u_fogParams7.y = 2 x trough + 0.5) - the wave decides which side of the surface the point
	// is on, so it is added in full; beyond it only the caustic/absorption PATH LENGTH would change, so
	// the contribution fades out over a half-excursion band instead of cutting. A hard cut at the swash
	// reach drew a line across the seabed that moved with "Swash amplitude": the depth stepped by the
	// wave height across it. Both terms are 0 with the ocean off, so this is free without water.
	const float excursion = max(u_oceanParams7.w, 0.5 * (u_fogParams7.y - 0.5));
	if (excursion > 0.0)
	{
		const float fadeEnd = excursion + max(0.5 * excursion, 0.5);
		const float dist = abs(depthBelow);
		if (dist < fadeEnd)
			depthBelow += underwaterLiveWaveY(worldPos.xz, depthBelow, localWaterLevel) * (1.0 - smoothstep(excursion, fadeEnd, dist));
	}
	g_liveDepthBelow = depthBelow;
	g_liveWaterLevel = localWaterLevel;
}

vec3 doSunLight(vec3 worldPos, f16vec3 V, f16vec3 Nh, f16vec3 specularCol, f16vec3 matColOverPi, float metalness, float16_t roughness)
{
	const vec3 N = vec3(Nh); // the facing test and the shadow's normal offset
	const vec3 L = u_sunDirection.xyz; // normalized on the CPU (SkyParams / setSunLight)
	if (dot(N, L) <= 0.0)
		return vec3(0.0);
	// After the facing early-out: a material that needs the live depth on EVERY pixel (the terrain's
	// film gate) resolves it itself before computeLitColor; here it is a no-op then, and other lit
	// materials keep paying the water-level fetch only on sun-facing pixels.
	resolveLiveDepth(worldPos);
	const float depthBelow = g_liveDepthBelow;
	const float localWaterLevel = g_liveWaterLevel;
	// BAKED (LIT_RT_SUN_SHADOW, StaticMeshGraphicsPipeline): only the active path is compiled, so the
	// registers are allocated for one of the PCSS search and the ray-query loop, not for the larger one.
#if LIT_RT_SUN_SHADOW
	float visibility = traceSunVisibility(worldPos, N);
#else
	float visibility = sampleSunShadow(worldPos, N);
#endif
	// Long-range terrain shadows. BOTH sources above run out of data well inside the streamed mesh ring -
	// the cascades end at Shadows/Max distance (3 km default), the RT path's instances at RT/TLAS Range
	// (4 km) - while terrain meshes run to ~33 km, so distant ground otherwise sits uniformly lit behind a
	// hard terminator. The baked height cascades still cover all of it, so march them there and take the
	// DARKER of the two: inside the fade band both terms see the same ridge (min is a no-op, no double
	// darkening), past it only the march survives. Skipped up close, where the map's texels are coarser
	// than the geometry they would be shadowing and could only produce acne.
	// Reach is bias * 2^10 - ~77 km at the default 150 m, comfortably past the far cascade's own range.
	if (u_terrainShadowParams.x > 0.0 && terrainHeightMapPresent())
	{
		const float fadeIn = smoothstep(u_terrainShadowParams.x, u_terrainShadowParams.x * 1.25, distance(worldPos, u_viewPos));
		if (fadeIn > 0.0)
		{
			const float terrainVis = terrainSunVisibility(worldPos, L, u_terrainShadowParams.y, 10, u_terrainShadowParams.z, 4.0);
			visibility = min(visibility, mix(1.0, terrainVis, fadeIn));
		}
	}
	// u_sunTransmittance = atmosTransmittanceToLight(0.0, L, u_skyUp), evaluated once per frame on the CPU.
	visibility *= u_eclipseParams.x;
	g_sunVisSurface = float16_t(visibility);
	vec3 lightRadiance = u_sunTransmittance * u_sunColor.rgb * visibility;
	// Underwater: the sun crossed the wavy surface - caustic focus + Beer-Lambert absorption
	// (underwater_light.inc.glsl), so seabed/submerged objects get the dancing light patterns. Keyed on
	// the live depth resolved above.
	if (depthBelow > 0.0)
		lightRadiance *= underwaterSunTransmittance(worldPos.xz, depthBelow, 0.0, 1.0, localWaterLevel - worldPos.y, localWaterLevel); // surfaces: physical reach
	return doLightH(lightRadiance, f16vec3(L), V, Nh, specularCol, matColOverPi, float16_t(metalness), roughness);
}
// Depth-aware 2x2 upsample of LAST FRAME's half-res AO/bent-normal image, reprojected. The AO is traced
// from the scene depth, which this pass is still writing - so this pass reads the previous frame's AO
// against the previous frame's depth: no input of this frame, no ordering constraint on the trace.
// Static geometry is exact under camera motion (the tap test is in world space); a moving object
// trails by one frame, inside the temporal accumulation's own lag. Plain bilinear bleeds across depth
// discontinuities (a far wall's AO/bent normal mixing into a near silhouette shows as a bright GI rim),
// so each tap's bilinear weight is scaled by its world-space distance to the shaded point; the same
// test rejects disoccluded taps. Returns (0, 0, 0, 1) - no bent normal, no occlusion - without history.
vec4 sampleAOBilateral(vec2 fullUv, vec3 pos, float viewDist)
{
	// Clip-space reprojection of this fragment (see prevScreenUVClip). Both images are jittered: the
	// fragment's surface sits at uv - jitter, and last frame's image holds a surface at uv + ITS jitter.
	float clipW;
	const vec2 prevJitter = taaJitterUv(u_taaJitter.zw);
	const vec2 prevUv = prevScreenUVClip(fullUv - taaJitterUv(u_taaJitter.xy), gl_FragCoord.z, clipW) + prevJitter;
	if (clipW <= 0.0 || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0))))
		return vec4(0.0, 0.0, 0.0, 1.0);

	const vec2 aoRes   = ceil(u_screenSize.xy * 0.5);
	const vec2 aoTexel = 1.0 / aoRes;
	const vec2 st   = prevUv * aoRes - 0.5;
	const vec2 base = (floor(st) + 0.5) * aoTexel;
	const vec2 f    = fract(st);
	const float bw[4] = float[]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	const vec2 offs[4] = vec2[](vec2(0.0), vec2(aoTexel.x, 0.0), vec2(0.0, aoTexel.y), aoTexel);

	const float sigmaZ = max(0.05 * viewDist, 0.02);
	const float gaussK = -1.4426950409 / (2.0 * sigmaZ * sigmaZ); // exp(-x/(2s^2)) == exp2(x * gaussK): one exp2, no divide per tap
	vec4 sum = vec4(0.0);
	float wsum = 0.0;
	for (int i = 0; i < 4; ++i)
	{
		const vec2 uv = base + offs[i];
		const float d = texture(u_prevDepth, uv).r;
		if (d <= 0.0) // background (reversed-Z far = 0)
			continue;
		const vec3 tapPos = worldPosFromDepthMat(uv - prevJitter, d, u_prevInvMvp);
		const vec3 dp = tapPos - pos;
		const float w  = bw[i] * exp2(dot(dp, dp) * gaussK); // squared distance straight from the dot: no sqrt
		sum  += texture(u_ao, uv) * w;
		wsum += w;
	}
	// All taps rejected: disoccluded this frame, or thin geometry the half-res image never saw.
	return wsum > 1e-4 ? sum / wsum : vec4(0.0, 0.0, 0.0, 1.0);
}

// Light debug overlay ("Graphics/LOD/Light grid/Debug Mode"): 0 off, 1 grid cells, 2 per-cell light
// count heat, 3 light ranges. BAKED: StaticMeshGraphicsPipeline defines LIGHT_GRID_DEBUG and the tweak
// reloads the pipeline, so at 0 every debug branch and variable below folds away (the sun cascade
// view is the SHADOW_DEBUG overlay at the end).
#ifndef LIGHT_GRID_DEBUG
#define LIGHT_GRID_DEBUG 0
#endif

// Full surface lighting for one shaded point: screen-space AO + bent-normal GI probe irradiance, sun
// (with underwater caustics) and the clustered light grid. texAO multiplies only the ambient/indirect
// term (baked texture AO on top of the screen-space term - pass 1.0 when the material carries none).
// The surface arrives HALF (N, colour, roughness = GGX alpha >= 0.01, metalness, AO): the direct-light BRDF
// is half math end to end (punctual_lights.inc.glsl, the fp16 BRDF), and so is the ambient/indirect term.
// Only V converts here; no 32-bit copy of the surface exists to stay live across the sun's shadow search.
// (A second, specular-only lobe riding this light loop - the terrain's water film, one shadow ray per light
// for both - was tried and dropped: the film's surface then had to be resolved before the loop and its
// values stayed live across the shadow ray query, 80/64 regs/local against 80/32 with the film's own loop.)
vec3 computeLitColor(vec3 worldPos, vec3 Vf, f16vec3 N, f16vec3 materialColor, float16_t roughnessH, float16_t metalness, float16_t texAO)
{
	const f16vec3 V = f16vec3(Vf);
	const f16vec3 specularColor = mix(f16vec3(0.04), materialColor, metalness);
	// (1 - metalness) folded in ONCE: every direct-light call below passes metalness 0, so the BRDF's
	// kD = (1 - F) * (1 - 0) and metalness is not live across the light loop (one register fewer).
	const f16vec3 diffuseColOverPi = materialColor * (float16_t(INV_PI) * (float16_t(1.0) - metalness));

	// The sun FIRST: its shadow search (PCSS taps or the ray-query loop) is the likely register peak, and
	// here no AO / GI result is live across it yet.
	// The accumulator is HALF (the scene colour target is RGBA16F): live from here across the GI read and the
	// whole light loop. Each light's radiance is unbounded, so it is clamped to the half range as it lands.
	f16vec3 colorH = f16vec3(min(doSunLight(worldPos, V, N, specularColor, diffuseColOverPi, 0.0, roughnessH), vec3(MEDIUMP_FLT_MAX)));

	// The AO, the bent normal and the indirect term are half too (gi_probe.inc.glsl's fp16 read side).
	float16_t ao = float16_t(1.0);
	f16vec3 bentN = N;
	// Past the RTAO max distance (u_aoParams.z) the trace writes exactly (0, 1.0) - no occlusion, no
	// bent normal (rtao.cs.glsl early-out) - so the depth-aware upsample (up to 8 taps + 4
	// world-pos reconstructions) would only re-fetch those constants. Skip it and use them directly;
	// z = 0 (falloff disabled) keeps the upsample everywhere. The gate measures from the SCENE FOCUS, the
	// same origin as rtao.cs.glsl's early-out; the camera distance still drives the upsample's depth weights.
	const float aoFocusDist = length(worldPos - u_sceneFocus.xyz);
	if (u_aoParams.x > 0.5 && (u_aoParams.z <= 0.0 || aoFocusDist < u_aoParams.z))
	{
		const float aoViewDist = length(worldPos - u_viewPos);
		const vec4 aoSample = sampleAOBilateral(gl_FragCoord.xy * u_screenSize.zw, worldPos, aoViewDist);
		ao = float16_t(aoSample.w);
		// Evaluate the indirect irradiance along the bent normal rather than the surface normal: in concave
		// areas it points toward the open hemisphere, so the low-frequency probe SH stops leaking light from
		// occluded directions. Mix partway toward N so flat, unoccluded surfaces are left untouched.
		// A zero bent normal = none (past the trace's range, fully occluded, or no history): keep N.
		const float bentLen2 = dot(aoSample.xyz, aoSample.xyz);
		if (bentLen2 > 1e-6)
			bentN = normalize(mix(bentN, f16vec3(aoSample.xyz * inversesqrt(bentLen2)), float16_t(0.75)));
	}
	// Blend to the virtual sky probe over the probe field's outer band (coverage) instead of stepping
	// at the outermost cascade's window face; the fallback is only evaluated where it contributes.
	// Volume mode: ~8 filtered fetches, not ~100 probe loads.
	const f16vec3 indirect = giIndirectOverPiH(worldPos, bentN);
	// indirect * strength + ambient, times AO: two scalar folds fewer than the previous grouping.
	colorH += materialColor * ((indirect * float16_t(u_aoParams.y) + f16vec3(u_ambientColor)) * (ao * texAO));

	const ivec3 gridPos = getGridPos(worldPos);
    uint tableIdx = getTableIdx(gridPos);

#if LIGHT_GRID_DEBUG == 2
	bool  debugHit = false;          // this point's grid was found in the hash table
	uint  debugLightCount = 0u;      // large + cell lights the point evaluated
#elif LIGHT_GRID_DEBUG == 3
	vec3  debugRangeTint = vec3(0.0);
#endif

	while (true)
	{
		const uint gridIdx = getGridIdx(tableIdx);
		if (gridIdx == EMPTY_ENTRY)
			break;
		const ivec3 gridMin = getGridMin(gridIdx);
		if (gridMin == gridPos)
		{
#if LIGHT_GRID_DEBUG == 2
			debugHit = true;
#endif
			// ONE loop over the grid's large lights, then the cell's lights: the loop body inlines every
			// light type plus the shadow ray queries, and two loops carried two copies of all of it.
			const uint numLargeLights = min(getLargeLightCount(gridIdx), MAX_LARGE_LIGHTS_PER_GRID);
			const uint cellOffset     = calcCellOffset(gridIdx, gridMin, worldPos);
			const uint numLights      = numLargeLights + min(getNumLightsForCell(cellOffset), MAX_LIGHTCELL_LIGHTS);
			for (uint i = 0; i < numLights; ++i)
			{
				const uint lightId    = i < numLargeLights ? getLargeLightId(gridIdx, i) : getLightId(cellOffset, i - numLargeLights);
				const LightInfo light = in_lightInfos[lightId];
				// The analytic term goes HALF before the shadow ray: only the half result (not its 32-bit value,
				// not the light record) is live across the query, the loop's peak.
				const vec3 lit = doLight(light, worldPos, V, N, specularColor, diffuseColOverPi, 0.0, roughnessH);
				const f16vec3 litH = f16vec3(min(lit, vec3(MEDIUMP_FLT_MAX)));
				float16_t visibility = float16_t(1.0);
#if PL_RT_LIGHTS_COMPILED
				if (PL_RT_LIGHTS_ON && dot(lit, lit) > 1e-7) // toggle off, or black analytic term: skip the trace
					visibility = float16_t(lightShadowVisibility(light, worldPos, vec3(N)));
#endif
				colorH = min(colorH + litH * visibility, f16vec3(MEDIUMP_FLT_MAX));
#if LIGHT_GRID_DEBUG == 3
				if (distance(worldPos, light.pos) < abs(light.range))
					debugRangeTint += vec3(0.0, 0.0, 0.08);
#endif
			}
#if LIGHT_GRID_DEBUG == 2
			debugLightCount = numLights;
#endif
			break;
		}
		tableIdx = getNextTableIdx(tableIdx);
	}

	vec3 color = vec3(colorH);
#if LIGHT_GRID_DEBUG == 1 // one random colour per grid (the coarse hash-table entry)
	color = mix(color, randomColor(gridPos), 0.4);
#elif LIGHT_GRID_DEBUG == 2 // light count heat: green (1) -> red (the cell cap); magenta = no grid entry
	if (!debugHit)
		color = mix(color, vec3(1.0, 0.0, 1.0), 0.5);
	else if (debugLightCount > 0u)
	{
		const float heat = clamp(float(debugLightCount) / float(MAX_LIGHTCELL_LIGHTS), 0.0, 1.0);
		color = mix(color, mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), heat), 0.5);
	}
#elif LIGHT_GRID_DEBUG == 3 // every light whose range covers the point adds a step of blue
	color += debugRangeTint + vec3(0.0, 0.0, 0.02);
#endif
#if defined(SHADOW_DEBUG) && SHADOW_DEBUG != 0
	color = shadowDebugOverlay(color, worldPos, vec3(N)); // "Shadows/Debug mode": baked, see shadows.inc.glsl
#endif
	return color;
}

#endif // INSTANCED_INDIRECT_LIT_INC_GLSL
