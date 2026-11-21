#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// EPipelineIndex::LitTreeVoxel: THE VOXEL TIER of the procedural trees (Procedural TreeSystem, "Trees/Voxel impostors").
// The main pass draws it past the tree's band, in its meshes' place (tree_cull.inc.glsl treeCullMainPiece); the shadows
// and the rays take the tree's shadow billboard. What is drawn is ONE SCREEN RECTANGLE around the variant's grid box (both tiers);
// this shader intersects the pixel's ray with the box (voxelTreeRay) and marches the tree's PREFILTERED voxel grid
// (sparse bricks: tree_voxel.inc.glsl, Procedural TreeImpostor.ixx "The VOXEL tier") from the
// entry along the view ray at the level whose voxel is about a pixel, stops where the ray's coverage passes this pixel's
// threshold (stochastic coverage: TAA / DLSS resolve it), writes that point's depth and lights it with the foliage
// terms - the leaf transmission, the N.V bend, the sky wrap - and a real self-shadow: the sun
// marched through the tree's own voxels. The cascades' shadow is looked up where that sun path LEAVES the box, so the
// tree's own shadow caster (its billboard) does not shadow it twice.
// Drawn from its own command list (StaticMeshGraphicsPipeline::record): it writes FragDepth, which no pipeline of the
// DGC execution set does. The VS is instanced_indirect.vs.glsl with TREE_VOXEL.

#include "shared.inc.glsl"
#ifdef TREE_VOXEL_RECORD
#include "tree_wind.inc.glsl"      // the near tier: the wind's motion at the hit
#include "tree_voxel_box.inc.glsl" // the near tier: the trunk's bend as a shear of the ray
#endif

layout (location = 3) in flat uint in_meshIdxMaterialIdx; // the tree set TYPE | the material << 16 (both tiers' culls write the type)
#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

// WHAT IS THE SAME FOR A WHOLE TREE IS NEVER A PLANE EQUATION PER TRIANGLE (flat, it was 13 of this shader's 27
// interpolated components, with the TRAM allocation the tier's launch limit). The tree's BOX comes from the tree set's
// type table (tree_type.inc.glsl), its rotation, scale and distance through the voxelTree* functions of the tier, and
// the rest is built from those where it is used. Every pixel of a tree shares the loads.
#define TREE_TYPES_BINDING 3
#include "tree_type.inc.glsl"

#ifdef TREE_VOXEL_RECORD
// THE NEAR TIER (the pipeline's define; its VS is instanced_indirect.vs.glsl TREE_VOXEL): NO BOX IS DRAWN - a screen
// rectangle around it, 2 triangles a tree, and this shader finds the ray's entry itself (voxelTreeRay). The tree's
// instance record is read at the draw slot (the main cull's OutMeshInstance, 64 B; `wind` / `rest` = tree_wind.inc.glsl
// treeWindPack). The wind's motion is not interpolated either: it follows from the hit's height.
layout (location = 0) in vec3 in_posU;     // a world point on this pixel's view ray, about as far as the tree
layout (location = 7) in flat uint in_treeInstance;
struct TreeDrawInstance
{
	vec4 posScale;
	vec4 quat;
	vec4 wind;
	uvec4 rest;
};
layout (binding = 1, std430) readonly buffer InDrawInstances { TreeDrawInstance in_drawInstances[]; };

vec4 voxelTreeQuat() { return in_drawInstances[in_treeInstance].quat; } // grid (tree) axes -> world
float voxelTreeScale() { return in_drawInstances[in_treeInstance].posScale.w; }
float voxelTreeDistance() { return distance(u_viewPos, in_drawInstances[in_treeInstance].posScale.xyz); }
vec3 voxelTreeViewDir() { return normalize(in_posU - u_viewPos); } // the camera -> this pixel, world, unit
// THE PIXEL'S RAY THROUGH THE TREE'S GRID: camG = the camera in grid space, dirG = the ray's direction in grid units per
// metre (no zero component: the empty bricks' jump divides by it), tNear / tFar = the distances from the camera (metres)
// at which it enters and leaves the box (tNear 0: the camera stands inside). False = the ray misses the box: a pixel of
// the rectangle beside it, gone before any fetch. THE MARCH RUNS FROM THE CAMERA along it (its t starts at tNear): no
// entry point and no entry distance are held beside the ray.
// THE TRUNK'S BEND is a shear of the ray about the height it enters at (tree_voxel_box.inc.glsl treeVoxelBoxShear), so
// there are THREE slab tests: the ray as it is against the box widened by its top's bend (a first entry height; a ray
// that misses that misses the bent tree), then twice the ray sheared about the last entry's height against the box.
// Against the box's own bent faces (a numeric model, a top bend of some metres): the ray lies within 6 cm for 99 % of
// the pixels. Its distances are the sheared ray's: the camera and the entry shear by different amounts, so the hit's
// world position lies up to that difference along the view ray from the true one (centimetres; decimetres in a storm).
bool voxelTreeRay(out vec3 camG, out vec3 dirG, out float tNear, out float tFar)
{
	const uint typeIdx = in_meshIdxMaterialIdx & 0xFFFFu;
	const float scale = in_drawInstances[in_treeInstance].posScale.w;
	// INTO THE TREE'S AXES ONCE (metres): the camera, the pixel's direction and the trunk vector. A tree turns about up
	// only, so the shear (sideways by the height) is the same shear there - no rotation per test.
	const vec4 quat = in_drawInstances[in_treeInstance].quat;
	const vec4 toLocal = vec4(-quat.xyz, quat.w);
	const vec3 camL = quat_transform(u_viewPos - in_drawInstances[in_treeInstance].posScale.xyz, toLocal);
	const vec3 dirL = quat_transform(normalize(in_posU - u_viewPos), toLocal);
	const vec2 trunkL = quat_transform(vec3(in_drawInstances[in_treeInstance].wind.x, 0.0, in_drawInstances[in_treeInstance].wind.y), toLocal).xz;
	const vec3 perMetre = in_treeTypes[typeIdx].gridInvSize / scale;                                // grid units per metre
	const vec3 originG = -in_treeTypes[typeIdx].gridMin * in_treeTypes[typeIdx].gridInvSize;       // the tree's origin in grid space
	const float boxBase = in_treeTypes[typeIdx].gridMin.y;
	const float boxHeight = in_treeTypes[typeIdx].gridSize.y;
	// The top's bend in grid units, to either side (its x and z share the larger scale).
	const float topHeight = max(boxBase + boxHeight, 0.0) * scale;
	float widen = length(trunkL) * treeVoxelBoxShear(topHeight, topHeight).x * max(perMetre.x, perMetre.z);
	vec2 shear = vec2(0.0);
	camG = vec3(0.0);
	dirG = vec3(1e-6);
	tNear = 0.0;
	tFar = 0.0;
	for (int pass = 0; pass < 3; ++pass)
	{
		// The ray sheared about the last entry's height (the first test: as it is), then in grid units.
		const vec3 raw = normalize(vec3(dirL.xz - trunkL * (shear.y * dirL.y), dirL.y).xzy) * perMetre;
		camG = vec3(camL.xz - trunkL * shear.x, camL.y).xzy * perMetre + originG;
		dirG = mix(vec3(1e-6), raw, greaterThan(abs(raw), vec3(1e-6)));
		const vec3 t0 = (vec3(-widen, 0.0, -widen) - camG) / dirG;
		const vec3 t1 = (vec3(1.0 + widen, 1.0, 1.0 + widen) - camG) / dirG;
		const vec3 tLow = min(t0, t1);
		const vec3 tHigh = max(t0, t1);
		tNear = max(max(max(tLow.x, tLow.y), tLow.z), 0.0);
		tFar = min(min(tHigh.x, tHigh.y), tHigh.z);
		if (tNear >= tFar)
			return false;
		if (pass < 2)
		{
			shear = treeVoxelBoxShear(max(boxBase + (camG.y + dirG.y * tNear) * boxHeight, 0.0) * scale, camL.y);
			widen = 0.0;
		}
	}
	return true;
}
// THE WIND'S MOTION at the hit (the trunk's bend last frame minus now), from the HIT's height over the tree's origin: the
// record is read here, at the shader's end, so nothing of it is held across the lit core.
vec3 voxelTreePrevDelta(vec3 hitPos)
{
	vec3 prevDelta;
	treeWindTrunkVertex(in_drawInstances[in_treeInstance].wind.xy, unpackHalf2x16(in_drawInstances[in_treeInstance].rest.y),
		hitPos.y - in_drawInstances[in_treeInstance].posScale.y, 1.0, prevDelta);
	return prevDelta;
}
#else
// THE FAR VOXEL TREES (tree_voxel_far.vs.glsl: an instance-rate record, no draw slot): the same screen rectangle, with the
// tree as TWO flat values. NO WIND: the box stands still (their sway is a few hundredths of a pixel a frame), so the ray
// takes ONE slab test and there is no motion.
layout (location = 0) in vec3 in_posU;           // a world point on this pixel's view ray, about as far as the tree
layout (location = 5) in flat vec2 in_voxelYaw;  // sin, cos of half the yaw (the tree turns about up)
layout (location = 7) in flat vec4 in_voxelTree; // xyz = the tree's origin (world), w = its scale
vec4 voxelTreeQuat() { return vec4(0.0, in_voxelYaw.x, 0.0, in_voxelYaw.y); }
float voxelTreeScale() { return in_voxelTree.w; }
float voxelTreeDistance() { return distance(u_viewPos, in_voxelTree.xyz); }
vec3 voxelTreeViewDir() { return normalize(in_posU - u_viewPos); }
vec3 voxelTreePrevDelta(vec3 hitPos) { return vec3(0.0); }
// The pixel's ray through the tree's grid (the near tier's voxelTreeRay, above, without the trunk's shear).
bool voxelTreeRay(out vec3 camG, out vec3 dirG, out float tNear, out float tFar)
{
	const uint typeIdx = in_meshIdxMaterialIdx & 0xFFFFu;
	const vec4 toLocal = vec4(0.0, -in_voxelYaw.x, 0.0, in_voxelYaw.y);
	const vec3 perMetre = in_treeTypes[typeIdx].gridInvSize / in_voxelTree.w; // grid units per metre
	camG = quat_transform(u_viewPos - in_voxelTree.xyz, toLocal) * perMetre - in_treeTypes[typeIdx].gridMin * in_treeTypes[typeIdx].gridInvSize;
	const vec3 raw = quat_transform(normalize(in_posU - u_viewPos), toLocal) * perMetre;
	dirG = mix(vec3(1e-6), raw, greaterThan(abs(raw), vec3(1e-6)));
	const vec3 t0 = -camG / dirG;
	const vec3 t1 = (1.0 - camG) / dirG;
	const vec3 tLow = min(t0, t1);
	const vec3 tHigh = max(t0, t1);
	tNear = max(max(max(tLow.x, tLow.y), tLow.z), 0.0);
	tFar = min(min(tHigh.x, tHigh.y), tHigh.z);
	return tNear < tFar;
}
#endif

// xyz = the box in world metres, w = the cube root of its volume (x 1 / a level's voxels = that level's step).
vec4 voxelTreeWorldSize()
{
	const uint typeIdx = in_meshIdxMaterialIdx & 0xFFFFu;
	return vec4(in_treeTypes[typeIdx].gridSize, in_treeTypes[typeIdx].gridCbrtVolume) * voxelTreeScale();
}
// The sun in grid units per metre, no zero component (the self-shadow march divides by it).
vec3 voxelTreeSun()
{
	const vec4 quat = voxelTreeQuat();
	const vec3 sun = quat_transform(u_sunDirection.xyz, vec4(-quat.xyz, quat.w))
		* (in_treeTypes[in_meshIdxMaterialIdx & 0xFFFFu].gridInvSize / voxelTreeScale());
	return mix(vec3(1e-6), sun, greaterThan(abs(sun), vec3(1e-6)));
}
// log2(this x the distance x level 0's voxels per axis) = the level whose voxel is about a pixel, + "Trees/Voxel LOD bias".
// A pixel at 1 m spans 2 / u_mipPixelScale metres.
float voxelTreeLodScale()
{
	return 2.0 * exp2(u_foliage_voxelLodBias) / (u_mipPixelScale * in_treeTypes[in_meshIdxMaterialIdx & 0xFFFFu].gridCbrtVolume * voxelTreeScale());
}

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target
// Reversed-Z: the hit lies at or behind what is rasterized (the rectangle at the box's nearest depth), so the written
// depth never exceeds the rasterized one - early depth still rejects a box the scene hides.
layout (depth_less) out float gl_FragDepth;

#define SUN_SHADOW_FIRST    // instanced_indirect_lit.inc.glsl: the sun's shadow before the material
#define LIT_SUN_SHADOW_HARD // instanced_indirect_lit.inc.glsl: one shadow tap (a voxel pixel is noise that TAA / DLSS resolves)
#define SHADOW_FOLIAGE_BIAS // shadows.inc.glsl: the foliage's constant depth bias
#define LIT_NO_RTAO         // instanced_indirect_lit.inc.glsl: no RTAO read (the depth is a point inside a crown, not a surface)
#define LIT_NO_UNDERWATER   // instanced_indirect_lit.inc.glsl: a tree's crown is never under the water
#define LIT_DIELECTRIC      // instanced_indirect_lit.inc.glsl: a voxel is never metal
#define LIT_LIGHTS_DIFFUSE  // instanced_indirect_lit.inc.glsl: the local lights' diffuse only (a rough crown seen from afar)
#define LEAF_SKY_WRAP
#define LIT_EXTRA_LIGHT     // the leaves' transmitted sun, added inside computeLitColor
float g_canopyHeight = 0.0;
#define LIT_CANOPY_HEIGHT g_canopyHeight // the hit's height up the tree's box: the canopy map's occlusion above it
#include "instanced_indirect_lit.inc.glsl"
#include "tree_fade.inc.glsl"
#include "leaf_look.inc.glsl"
#define FACTION_MAP_BINDING 24
#include "terrain_faction.inc.glsl" // the game's faction layer: the far trees take the faction look of the land they stand on

#define VOXEL_BRICK_ORIGIN // tree_voxel.inc.glsl: a march's brick holds its atlas origin (the light loop sets this shader's registers, not the march)
#include "tree_voxel.inc.glsl"

// Interleaved gradient noise, shifted per frame (TAA averages it); `offset` decorrelates the uses.
float voxelNoise(vec2 offset)
{
	const vec2 pixel = gl_FragCoord.xy + offset + 5.588238 * float(u_frameIndex & 7u);
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// motion_vector.inc.glsl's motionVector at the HIT's depth, not the box face's.
vec4 voxelMotionVector(vec3 prevWorldDelta, float depth, float invW)
{
	if (all(equal(prevWorldDelta, vec3(0.0))))
		return vec4(0.0); // still: the readers reproject the written depth with the camera
	const vec2 uv = gl_FragCoord.xy * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
	const vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
	const vec4 prevClip = u_reprojClip * vec4(vpUv.x * 2.0 - 1.0, 1.0 - vpUv.y * 2.0, depth, 1.0)
		+ (u_prevMvp * vec4(prevWorldDelta, 0.0)) * invW;
	if (prevClip.w <= 0.0)
		return vec4(0.0, 0.0, -1.0, 1.0);
	return vec4(uv - prevClipToScreenUV(prevClip), prevClip.z / prevClip.w, 1.0);
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	// THE RAY through the tree's grid; its parameter is the distance from the CAMERA (metres). First of all: a pixel of the
	// rectangle that misses the box leaves here, before the material and every fetch.
	vec3 camG, dirG;
	float tNear, tFar;
	if (!voxelTreeRay(camG, dirG, tNear, tFar))
		discard;

	const uint materialIdx = in_meshIdxMaterialIdx >> 16;
	const MaterialInfo material = in_materialInfos[materialIdx];
	// The crossfade with the meshes (the type's band), per tree: before the march. The distance only on a material that fades.
	if ((material.flags & MATERIAL_FLAG_DISTANCE_FADE) != 0u && treeDistanceFaded(material.flags, voxelTreeDistance()))
		discard;

	// The material's albedo slot = the grid's opacity atlas (its header names the index), its normal slot = the surface atlas.
	const VoxelGrid grid = voxelGrid(material.diffuseNormalTexIdx & 0xFFFFu);
	const uint surfaceTex = material.diffuseNormalTexIdx >> 16;
	const float maxLod = grid.levels - 1.0;

	// THE LEVEL: a voxel about a pixel at the entry's distance ("Trees/Voxel LOD bias", inside the LOD scale), dithered
	// between the two nearest per pixel.
	const float lodF = log2(grid.res * voxelTreeLodScale() * max(tNear, 1e-3));
	const float lod = clamp(floor(lodF + voxelNoise(vec2(29.0, 61.0))), 0.0, maxLod);
	const VoxelLevel level = voxelLevel(grid, lod);
	const float n = level.n;
	const float stepWorld = voxelTreeWorldSize().w / n; // one crossing: the stored opacity's length

	// THE MARCH, in the level's voxel coordinates (tree_voxel.inc.glsl): until the ray's coverage passes this pixel's
	// threshold, or it leaves the box (then nothing). Not the distance fade's noise (tree_fade.inc.glsl, offset 0):
	// inside its band only the high thresholds would be kept. An EMPTY brick (nothing stored: the index says so) is
	// jumped to its far side.
	const float threshold = max(voxelNoise(vec2(47.0, 113.0)), 0.004);
	const vec3 dirC = dirG * n;
	const float exitT = tFar; // the march ends where the ray leaves the box
	// TWO STEPS A WAIT: where the next step's sample lies in the same brick, both are fetched together (voxelOpacityPair) and
	// taken in turn - the same samples as one a step. `t` stops at the hit: the hit's place is formed from it after the loop.
	// The loop's math is the tier's other cost (a fifth less of it was a tenth less time): the ray is held as the voxel
	// CENTRE coordinate of the CAMERA (c - 0.5, once) + the direction x t, t from the entry on, and the end is one compare
	// of the transmittance against 1 - the threshold.
	float t = tNear + stepWorld * voxelNoise(vec2(83.0, 7.0));
	float transmit = 1.0;
	bool hit = false;
	float hitOpacity = 0.0;
	const int maxSteps = int(n * 1.75) + 2;
	const vec3 faceU = camG * n - 0.5;
	const float transmitEnd = 1.0 - threshold;
	VoxelBrick marchBrick = voxelBrickNone();
// One step's sample: its opacity onto the ray, and the march ends where the coverage passes the pixel's threshold.
#define VOXEL_VIEW_STEP(opacity) \
		transmit *= 1.0 - voxelThin(opacity); \
		if (transmit <= transmitEnd) \
		{ \
			hit = true; \
			hitOpacity = voxelThin(opacity); \
			break; \
		}
	for (int i = 0; i < maxSteps && t <= exitT; ++i)
	{
		const vec3 centre = faceU + dirC * t;
		const vec3 u = voxelCentreClamp(level, centre);
		if (!voxelSampleBrick(grid, level, u, marchBrick))
		{
			t += voxelBrickSkip(level, centre + 0.5, dirC, marchBrick.coord) + 0.03 * stepWorld;
			continue;
		}
		const float tNext = t + stepWorld;
		const vec3 uNext = voxelCentreClamp(level, faceU + dirC * tNext);
		const bool pair = tNext <= exitT && i + 1 < maxSteps && voxelInBrick(uNext, marchBrick);
		const vec2 a = voxelOpacityPair(grid, marchBrick, u, pair ? uNext : u);
		VOXEL_VIEW_STEP(a.x)
		t = tNext;
		if (!pair)
			continue;
		VOXEL_VIEW_STEP(a.y)
		t += stepWorld;
		++i;
	}
#undef VOXEL_VIEW_STEP
	if (!hit)
		discard;
	const vec3 hitC = faceU + 0.5 + dirC * t;

	// The hit lies on the pixel's own view ray, t from the camera: ONE view vector for the position and the lighting,
	// formed after the march. Anchored on the interpolated point of that ray (in_posU - V x a distance).
	const vec3 V = -voxelTreeViewDir();
	const vec3 pos = in_posU - V * (t - distance(in_posU, u_viewPos));

	// THE SURFACE at the hit: its colour, mean normal and interior (the surface atlas: 4 samples per brick side).
	vec3 albedo, normalTap;
	float interiorTap, leafShare;
	voxelSurface(grid, level, surfaceTex, hitC, marchBrick, albedo, normalTap, interiorTap, leafShare);
	const vec3 hitG = hitC / n;

	// THE NORMAL: the voxel's mean leaf normal, stored in the tree's axes.
	const vec3 meanN = normalTap * 2.0 - 1.0;
	const float meanLength = min(length(meanN), 1.0);
	// 0 = the voxel's leaves face every way, 1 = all one way. Trusted by the voxel's cover: a sparse voxel (a thin tip) holds
	// a leaf or two, whose mean is long though the tip around it faces every way.
	const float leafAgree = meanLength * smoothstep(0.05, 0.5, hitOpacity);
	// A zero vector (a voxel whose leaves cancel) stays zero: no NaN, no select.
	const vec3 localN = meanN * inversesqrt(max(dot(meanN, meanN), 1e-8));
	f16vec3 N = f16vec3(quat_transform(localN, voxelTreeQuat()));

	// THE SUN through the tree's own voxels (one level coarser), to where its path leaves the box. Its direction in grid
	// space is one per tree (voxelTreeSun).
	const vec3 L = u_sunDirection.xyz;
	const vec3 sunG = voxelTreeSun();
	const float exitDist = voxelBoxExit(level, hitC, sunG * n);
	float sunTransmit = 1.0;
	if (u_foliage_voxelSelfShadow > 0.0 && u_sunLit > 0.5) // at night the march's result scales a light of 0
	{
		// "Trees/Voxel self shadow coarser levels" above the view march's level (a constant while its row is locked: the
		// loop unrolls), inside the grid's levels.
		VoxelLevel sunLevel = level;
		for (float k = 0.0; k < u_foliage_voxelSelfShadowLevels && lod + k < maxLod; k += 1.0)
			sunLevel = voxelLevelCoarser(sunLevel);
		const float sunN = sunLevel.n;
		const float sunStep = voxelTreeWorldSize().w / sunN;
		const vec3 sunU = hitG * sunN - 0.5; // the voxel centre coordinate, as the view march
		const vec3 sunDirC = sunG * sunN;
		// Empty bricks are jumped: the path out of the crown often crosses the box's empty corners.
		const int sunSteps = min(int(exitDist / sunStep) + 1, 64);
		float d = 0.5 * sunStep;
		VoxelBrick sunBrick = voxelBrickNone();
		for (int i = 0; i < sunSteps && sunTransmit > 0.01 && d <= exitDist; ++i)
		{
			const vec3 centre = sunU + sunDirC * d;
			const vec3 u = voxelCentreClamp(sunLevel, centre);
			if (!voxelSampleBrick(grid, sunLevel, u, sunBrick))
			{
				d += voxelBrickSkip(sunLevel, centre + 0.5, sunDirC, sunBrick.coord) + 0.03 * sunStep;
				continue;
			}
			// Two steps a wait, as the view march.
			const float dNext = d + sunStep;
			const vec3 uNext = voxelCentreClamp(sunLevel, sunU + sunDirC * dNext);
			const bool pair = dNext <= exitDist && i + 1 < sunSteps && voxelInBrick(uNext, sunBrick);
			const vec2 a = voxelOpacityPair(grid, sunBrick, u, pair ? uNext : u);
			sunTransmit *= 1.0 - voxelThin(a.x);
			d = dNext;
			if (pair && sunTransmit > 0.01)
			{
				sunTransmit *= 1.0 - voxelThin(a.y);
				d += sunStep;
				++i;
			}
		}
	}

	// EVERYTHING OF THE SURFACE IS FORMED HERE, before the cascades' shadow search (the register peak): across it stay the
	// position, the colour, the normal and two scalars (the transmission's back + glow, the sun march's result).
	// A voxel material is always a LEAF material (Procedural createVoxelMaterial): no leaf test.

	g_canopyHeight = hitG.y * voxelTreeWorldSize().y; // the box stands on the ground: the canopy map's occlusion above the hit

	// THE CROWN'S INSIDE DARKER ("Trees/Voxel interior darkening"): the colour x 2 ^ (-2 x that x the hit's baked interior,
	// 0 on the crown's surface .. 1 at its core). On the COLOUR, so every light takes it - the sun, the ambient, the
	// transmission: on the ambient alone it did not show in sunlight. An exponential, NEVER TO BLACK (a quarter at the
	// core at 1): the deepest voxels are the SOLID bark bodies (a branch inside the crown, opaque - every pixel of the
	// voxel stops there), and x (1 - the interior) drew them as black voxels among the leaves.
	albedo *= exp2(-2.0 * u_foliage_voxelInteriorDarkening * interiorTap);
	// The crown's colour patches on the LEAVES only: by the voxel's leaf share, so the trunk and the bare branches keep
	// their bark colour.
	const f16vec3 materialColor = terrainFactionAlbedo(f16vec3(mix(albedo, leafPatchTint(albedo, pos), leafShare)), pos.xz, 1.0);

	// LEAVES seen edge-on: N bent toward the viewer until N.V reaches "Foliage min N.V" (the foliage FS's). After it N
	// faces the viewer (N.V >= 0: the min is >= 0), so the transmission below needs no "seen side" flip.
	{
		const float noV = dot(vec3(N), V);
		if (noV < u_foliage_minNoV)
			N = f16vec3(normalize(vec3(N) + V * (u_foliage_minNoV - noV)));
	}

	// LEAF TRANSMISSION, as the foliage cards' and through the same "Foliage transmission" settings (strength, glow,
	// focus, shadow): a diffuse back term + the forward glow, x the sun's shadow and the self-shadow, each at "Foliage
	// transmission shadow"'s share.
	// THE BACK TERM by the voxel's leaves: its mean normal points out of the crown, so -N.L alone lit only the crown's
	// underside (under a high sun the top and the sides face it), while a mesh crown's leaves face every way and glow all
	// over. Leaves facing every way (two-sided, the face the viewer sees): on average 0.25 x (1 - V.L) - 0 with the sun
	// behind the viewer, 0.5 straight ahead; aligned leaves: -N.L, N turned to the seen side. Blended by the mean normal's
	// length. The back + glow sum needs no shadow: formed here, ONE scalar across the search.
	float throughNoShadow;
	{
		const float VoL = dot(V, L);
		const float back = mix(0.25 - 0.25 * VoL, max(-dot(vec3(N), L), 0.0), leafAgree);
		const float glow = pow(max(-VoL, 0.0), u_foliage_transmissionFocus) * u_foliage_transmissionGlow;
		// x the voxel's LEAF SHARE: no sun comes through bark (the trunk, the bare branches).
		throughNoShadow = (back + glow) * leafShare;
		// The sky wrap too is a leaf's (a downward leaf's ambient mirrored toward the sky): set here, so the share itself
		// is not held across the shadow search.
		g_leafSkyWrap = float16_t(u_foliage_leafSkyWrap * leafShare);
	}

	// The cascades' shadow (and the terrain's, the clouds'). With the SELF-SHADOW on ("Voxel self shadow" > 0): where the
	// sun path leaves the box - the march above already darkens by the tree's own leaves, and its own caster (the voxel
	// grid with "Trees/Voxel shadows", else the shadow billboard) lies inside the box: it would shadow it twice. With it
	// OFF: at the hit itself, so the tree takes its own caster's shadow from the cascades.
	const vec3 shadowPos = u_foliage_voxelSelfShadow > 0.0 ? pos + L * (exitDist + 0.5) : pos;
	g_shadowFoliage = true;
	g_sunShadowFirst = u_sunLit > 0.5 ? sunShadowVisibility(shadowPos, L) : 0.0; // no search at night
	g_shadowFoliage = false;
	// The self-shadow's two strengths are two powers of the sun march's result: ONE logarithm for both.
	const float sunLog = log2(max(sunTransmit, 1e-4)) * u_foliage_voxelSelfShadow;
	{
		// The transmission takes the cascades' shadow and the self-shadow by "Foliage transmission shadow": light scattered
		// forward through the leaves loses less than the direct sun.
		const float selfTransmit = exp2(sunLog * u_foliage_transmissionShadow);
		const float through = throughNoShadow * mix(1.0, g_sunShadowFirst, u_foliage_transmissionShadow) * selfTransmit;
		if (through > 0.0)
			g_litExtraLight = f16vec3(min(vec3(materialColor)
				* (through * u_foliage_transmission * INV_PI)
				* (u_sunTransmittance * u_sunColor.rgb), vec3(MEDIUMP_FLT_MAX)));
	}
	g_sunShadowFirst *= exp2(sunLog);

	const vec3 color = computeLitColor(pos, V, N, materialColor, float16_t(0.65), float16_t(0.0), float16_t(1.0));
	out_color = vec4(color, 1.0);

	// THE DEPTH and the motion vector at the hit, formed AFTER the lighting from `pos` (which the light loop keeps live
	// anyway): held from the march on, the depth and 1 / w were two more values across the lit core's register peak.
	const vec4 clip = u_mvp * vec4(pos, 1.0);
	const float depth = min(clip.z / clip.w, gl_FragCoord.z); // never nearer than the rectangle (depth_less)
	gl_FragDepth = depth;
	// Flagged NO-AO (shared.inc.glsl motionNoAo, as motionVectorNoAo): this shader reads no RTAO, so the trace spends no
	// ray on its pixels and leaves a hole.
	vec4 motion = voxelMotionVector(voxelTreePrevDelta(pos), depth, 1.0 / clip.w);
	motion.w = motion.w > 0.5 ? 2.0 : -1.0;
	out_motion = motion;
}
