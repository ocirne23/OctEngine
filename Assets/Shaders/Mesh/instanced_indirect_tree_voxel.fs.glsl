#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// EPipelineIndex::LitTreeVoxel: THE LIGHTING of the procedural trees' VOXEL tier (Procedural TreeSystem, "Trees/Voxel
// impostors"; both tiers - the one that crossfades with the meshes and the far voxel trees). ONE FULL-SCREEN DRAW
// (sky.vs.glsl's triangle, no depth test) over the VOXEL HIT TARGET, which the voxel march pass filled
// (Trees/tree_voxel_march.fs.glsl: per pixel the nearest voxel hit's tree, level and distance, and its depth in the
// scene depth). Per pixel that holds a hit this shader builds the same ray again (tree_voxel_ray.inc.glsl), finds the
// hit on it, and lights it with the foliage terms - the leaf transmission, the N.V bend, the sky wrap - and a real
// self-shadow: the sun marched through the tree's own voxels. The cascades' shadow is looked up where that sun path
// LEAVES the box, so the tree's own shadow caster does not shadow it twice.
// The lit core runs ONCE PER FINAL PIXEL: not for a ray that hits nothing, and not for a tree that another one covers.
// Drawn by StaticMeshGraphicsPipeline::record's late phase, before the sky and the transparent set.

#include "shared.inc.glsl"
#include "tree_wind.inc.glsl" // the near tier: the wind's motion at the hit

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

// THE VOXEL HIT TARGET (SceneColor; RG32UI, nearest): x = the hit word (tree_voxel_box.inc.glsl; 0 = no voxel tree
// here), y = the hit's distance from the camera (a float's bits).
layout (binding = 25) uniform usampler2D u_voxelHits;

// WHAT IS ONE VALUE PER TREE comes from the tree's record and its type through the voxelTree* functions
// (tree_voxel_ray.inc.glsl), each where it is used. Every pixel of a tree shares the loads.
#include "tree_voxel_ray.inc.glsl"

// xyz = the box in world metres, w = the cube root of its volume (x 1 / a level's voxels = that level's step).
vec4 voxelTreeWorldSize(uint tree)
{
	const uint typeIdx = voxelTreeTypeMaterial(tree) & 0xFFFFu;
	return vec4(in_treeTypes[typeIdx].gridSize, in_treeTypes[typeIdx].gridCbrtVolume) * voxelTreePosScale(tree).w;
}
// The sun in grid units per metre, no zero component (the self-shadow march divides by it).
vec3 voxelTreeSun(uint tree)
{
	const vec4 quat = voxelTreeQuat(tree);
	const vec3 sun = quat_transform(u_sunDirection.xyz, vec4(-quat.xyz, quat.w))
		* (in_treeTypes[voxelTreeTypeMaterial(tree) & 0xFFFFu].gridInvSize / voxelTreePosScale(tree).w);
	return mix(vec3(1e-6), sun, greaterThan(abs(sun), vec3(1e-6)));
}
// THE WIND'S MOTION at the hit (the trunk's bend last frame minus now), from the HIT's height over the tree's origin: the
// record is read here, at the shader's end, so nothing of it is held across the lit core. A far voxel tree stands still.
vec3 voxelTreePrevDelta(uint tree, vec3 hitPos)
{
	if (voxelTreeIsFar(tree))
		return vec3(0.0);
	const uint i = tree & VOXEL_HIT_INDEX_MASK;
	vec3 prevDelta;
	treeWindTrunkVertex(in_voxelNearTrees[i].wind.xy, unpackHalf2x16(in_voxelNearTrees[i].rest.y),
		hitPos.y - in_voxelNearTrees[i].posScale.y, 1.0, prevDelta);
	return prevDelta;
}

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target

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
#include "leaf_look.inc.glsl"
#define FACTION_MAP_BINDING 24
#include "terrain_faction.inc.glsl" // the game's faction layer: the far trees take the faction look of the land they stand on

#define VOXEL_BRICK_ORIGIN // tree_voxel.inc.glsl: a march's brick holds its atlas origin (the light loop sets this shader's registers, not the march)
#include "tree_voxel.inc.glsl"

// motion_vector.inc.glsl's motionVector at the HIT's depth (this draw's own depth is the far plane).
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
	// THE HIT at this pixel, or none: most of the screen leaves here, after one fetch.
	const uvec2 hitWord = texelFetch(u_voxelHits, ivec2(gl_FragCoord.xy), 0).xy;
	if (hitWord.x == 0u)
		discard;
	const uint tree = hitWord.x & (VOXEL_HIT_INDEX_MASK | VOXEL_HIT_FAR);
	const float t = uintBitsToFloat(hitWord.y); // the hit's distance from the camera (m)
	const float lod = float((hitWord.x >> VOXEL_HIT_LOD_SHIFT) & 15u);
	// 0..1: how far the hit voxel's cover lets its mean normal be trusted (the march's smoothstep of its opacity).
	const float coverTrust = float((hitWord.x >> VOXEL_HIT_AGREE_SHIFT) & 7u) * (1.0 / 7.0);

	// THE MARCH'S RAY AGAIN (the same inputs, the same code), and the hit on it in the level's voxel coordinates.
	const vec3 dirW = voxelPixelDir();
	vec3 camG, dirG;
	float tNear, tFar;
	voxelTreeRay(tree, dirW, camG, dirG, tNear, tFar);

	const MaterialInfo material = in_materialInfos[voxelTreeTypeMaterial(tree) >> 16];
	// The material's albedo slot = the grid's opacity atlas (its header names the index), its normal slot = the surface atlas.
	const VoxelGrid grid = voxelGrid(material.diffuseNormalTexIdx & 0xFFFFu);
	const uint surfaceTex = material.diffuseNormalTexIdx >> 16;
	const float maxLod = grid.levels - 1.0;
	const VoxelLevel level = voxelLevel(grid, lod);
	const float n = level.n;
	const vec3 hitCentre = voxelRayCentre(camG, dirG, n, t);
	const vec3 hitC = hitCentre + 0.5;

	// ONE view vector for the position and the lighting: the hit lies on the pixel's own view ray, t from the camera.
	const vec3 V = -dirW;
	const vec3 pos = u_viewPos + dirW * t;

	// THE SURFACE at the hit: its colour, mean normal and interior (the surface atlas: 4 samples per brick side). The
	// hit's brick is a stored one (the march stopped on a sample of it). Read EMPTY here - the two shaders' rays differ
	// in a last bit at a brick's side - it is a neutral leaf, not a fetch through the empty slot.
	vec3 albedo = vec3(0.04, 0.06, 0.025);
	vec3 normalTap = vec3(0.5);
	float interiorTap = 0.5;
	float leafShare = 1.0;
	VoxelBrick hitBrick = voxelBrickNone();
	if (voxelSampleBrick(grid, level, voxelCentreClamp(level, hitCentre), hitBrick))
		voxelSurface(grid, level, surfaceTex, hitC, hitBrick, albedo, normalTap, interiorTap, leafShare);
	const vec3 hitG = hitC / n;

	// THE NORMAL: the voxel's mean leaf normal, stored in the tree's axes.
	const vec3 meanN = normalTap * 2.0 - 1.0;
	const float meanLength = min(length(meanN), 1.0);
	// 0 = the voxel's leaves face every way, 1 = all one way. Trusted by the voxel's cover: a sparse voxel (a thin tip) holds
	// a leaf or two, whose mean is long though the tip around it faces every way.
	const float leafAgree = meanLength * coverTrust;
	// A zero vector (a voxel whose leaves cancel) stays zero: no NaN, no select.
	const vec3 localN = meanN * inversesqrt(max(dot(meanN, meanN), 1e-8));
	f16vec3 N = f16vec3(quat_transform(localN, voxelTreeQuat(tree)));

	// THE SUN through the tree's own voxels (one level coarser), to where its path leaves the box. Its direction in grid
	// space is one per tree (voxelTreeSun).
	const vec3 L = u_sunDirection.xyz;
	const vec3 sunG = voxelTreeSun(tree);
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
		const float sunStep = voxelTreeWorldSize(tree).w / sunN;
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
			// TWO STEPS A WAIT: where the next step's sample lies in the same brick, both are fetched together.
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

	g_canopyHeight = hitG.y * voxelTreeWorldSize(tree).y; // the box stands on the ground: the canopy map's occlusion above the hit

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

	// THE MOTION VECTOR at the hit, formed AFTER the lighting from `pos` (which the light loop keeps live anyway) and the
	// hit word. The hit's depth is in the scene depth already (the march wrote it); this draw writes none.
	// Flagged NO-AO (shared.inc.glsl motionNoAo, as motionVectorNoAo): this shader reads no RTAO, so the trace spends no
	// ray on its pixels and leaves a hole.
	const vec4 clip = u_mvp * vec4(pos, 1.0);
	vec4 motion = voxelMotionVector(voxelTreePrevDelta(tree, pos), clip.z / clip.w, 1.0 / clip.w);
	motion.w = motion.w > 0.5 ? 2.0 : -1.0;
	out_motion = motion;
}
