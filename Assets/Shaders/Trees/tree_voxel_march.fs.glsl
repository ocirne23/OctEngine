#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable

// THE VOXEL TREES' MARCH (the voxel march pass: the scene depth + the voxel hit target, after every other opaque draw;
// both tiers - the near one's VS is instanced_indirect.vs.glsl TREE_VOXEL, the far trees' tree_voxel_far.vs.glsl).
// What is drawn is ONE SCREEN RECTANGLE around the tree's grid box. This shader intersects the pixel's ray with the box
// (tree_voxel_ray.inc.glsl) and marches the tree's PREFILTERED voxel grid (sparse bricks: tree_voxel.inc.glsl) from the
// entry along the view ray at the level whose voxel is about a pixel, until the ray's coverage passes this pixel's
// threshold (stochastic coverage: TAA / DLSS resolve it). On a hit it writes the hit's depth and, into the hit target,
// the HIT WORD (the tree, the level, the hit voxel's cover) + the hit's distance from the camera.
// NO LIGHTING HERE: the lit core's registers were what limited this tier's warps, a ray that hits nothing paid for them
// through its whole march, and a tree behind another one was lit and then covered. The lighting is one full-screen draw
// over the hit target (instanced_indirect_tree_voxel.fs.glsl), once per final pixel.

#include "shared.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; }; // selects the per-eye view (1=left, 2=right) in VR
#endif

// THE WHOLE INTERFACE: the tree (its index + VOXEL_HIT_FAR). One flat component over 2 triangles; the ray comes from the
// pixel, the tree's values from its record.
layout (location = 0) in flat uint in_voxelTree;

struct MaterialInfo
{
	uint flags;
	float opacity;
	uint diffuseNormalTexIdx;
	uint metalRoughnessTexIdxAlphaMode;
};
layout (binding = 2, std430) readonly buffer InMaterialInfos { MaterialInfo in_materialInfos[]; };
layout (binding = 27) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count

layout (location = 0) out uvec2 out_hit; // x = the hit word (tree_voxel_box.inc.glsl), y = the hit's distance from the camera (a float's bits)
// Reversed-Z: the hit lies at or behind the rectangle (the box's nearest depth), so the written depth never exceeds the
// rasterized one - early depth still rejects a tree the scene hides.
layout (depth_less) out float gl_FragDepth;

#include "tree_voxel_ray.inc.glsl"
#include "tree_fade.inc.glsl"
#include "tree_voxel.inc.glsl"

// Interleaved gradient noise, shifted per frame (TAA averages it); `offset` decorrelates the uses.
float voxelNoise(vec2 offset)
{
	const vec2 pixel = gl_FragCoord.xy + offset + 5.588238 * float(u_frameIndex & 7u);
	return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	// THE RAY through the tree's grid; its parameter is the distance from the CAMERA (metres). First of all: a pixel of the
	// rectangle that misses the box leaves here, before the material and every fetch.
	const uint tree = in_voxelTree;
	const vec3 dirW = voxelPixelDir();
	vec3 camG, dirG;
	float tNear, tFar;
	if (!voxelTreeRay(tree, dirW, camG, dirG, tNear, tFar))
		discard;

	const uint typeMaterial = voxelTreeTypeMaterial(tree);
	const MaterialInfo material = in_materialInfos[typeMaterial >> 16];
	const vec4 posScale = voxelTreePosScale(tree);
	// The crossfade with the meshes (the type's band), per tree: before the march. The distance only on a material that fades.
	if ((material.flags & MATERIAL_FLAG_DISTANCE_FADE) != 0u && treeDistanceFaded(material.flags, distance(u_viewPos, posScale.xyz)))
		discard;

	// The material's albedo slot = the grid's opacity atlas (its header names the index).
	const VoxelGrid grid = voxelGrid(material.diffuseNormalTexIdx & 0xFFFFu);
	if (!voxelGridValid(grid))
		discard; // the atlas is not in its slot yet (voxelGrid)
	const float maxLod = grid.levels - 1.0;

	// THE LEVEL: a voxel about a pixel at the entry's distance ("Trees/Voxel LOD bias"), dithered between the two nearest
	// per pixel. A pixel at 1 m spans 2 / u_mipPixelScale metres; the box's cube root x 1 / a level's voxels = that
	// level's step.
	const float cubeRoot = in_treeTypes[typeMaterial & 0xFFFFu].gridCbrtVolume * posScale.w;
	const float lodF = log2(grid.res * 2.0 * exp2(u_foliage_voxelLodBias) / (u_mipPixelScale * cubeRoot) * max(tNear, 1e-3));
	const float lod = clamp(floor(lodF + voxelNoise(vec2(29.0, 61.0))), 0.0, maxLod);
	const VoxelLevel level = voxelLevel(grid, lod);
	const float n = level.n;
	const float stepWorld = cubeRoot / n; // one crossing: the stored opacity's length

	// THE MARCH, in the level's voxel coordinates (tree_voxel.inc.glsl): until the ray's coverage passes this pixel's
	// threshold, or it leaves the box (then nothing). Not the distance fade's noise (tree_fade.inc.glsl, offset 0):
	// inside its band only the high thresholds would be kept. An EMPTY brick (nothing stored: the index says so) is
	// jumped to its far side.
	// TWO STEPS A WAIT: where the next step's sample lies in the same brick, both are fetched together (voxelOpacityPair) and
	// taken in turn - the same samples as one a step. `t` stops at the hit's sample: the lighting finds the hit from it
	// (voxelRayCentre, the function the samples here come from).
	const float threshold = max(voxelNoise(vec2(47.0, 113.0)), 0.004);
	const vec3 dirC = dirG * n;
	float t = tNear + stepWorld * voxelNoise(vec2(83.0, 7.0));
	// THE LOOP SETS THIS SHADER'S REGISTER COUNT, so little is held across it:
	// - no step counter: t alone ends the march (every step and every jump moves it on, by 3 % of a step at least);
	// - the transmittance is held OVER ITS END VALUE (1 - the threshold), so the end is a compare with 1;
	// - a hit is the hit sample's opacity being set (> 0: it took the coverage past the threshold);
	// - a sample's place in the atlas is formed before the next sample is worked out (voxelOpacityPairAt);
	// - the pixel's direction is built again after the loop, for the depth.
	float transmit = 1.0 / (1.0 - threshold); // the threshold is a fract(): < 1
	float hitOpacity = 0.0;
	VoxelBrick marchBrick = voxelBrickNone();
// One step's sample: its opacity onto the ray, and the march ends where the coverage passes the pixel's threshold.
#define VOXEL_VIEW_STEP(opacity) \
		transmit *= 1.0 - voxelThin(opacity); \
		if (transmit <= 1.0) \
		{ \
			hitOpacity = max(voxelThin(opacity), 1e-6); \
			break; \
		}
	while (t <= tFar)
	{
		const vec3 u = voxelCentreClamp(level, voxelRayCentre(camG, dirG, n, t));
		if (!voxelSampleBrick(grid, level, u, marchBrick))
		{
			t += voxelBrickSkip(level, voxelRayCentre(camG, dirG, n, t) + 0.5, dirC, marchBrick.coord) + 0.03 * stepWorld;
			continue;
		}
		const vec3 place = voxelSliceUv(grid, marchBrick, u);
		const float tNext = t + stepWorld;
		const vec3 uNext = voxelCentreClamp(level, voxelRayCentre(camG, dirG, n, tNext));
		const bool pair = tNext <= tFar && voxelInBrick(uNext, marchBrick);
		vec3 placeNext = place;
		if (pair)
			placeNext = voxelSliceUv(grid, marchBrick, uNext);
		const vec2 a = voxelOpacityPairAt(grid, place, placeNext);
		VOXEL_VIEW_STEP(a.x)
		t = tNext;
		if (!pair)
			continue;
		VOXEL_VIEW_STEP(a.y)
		t += stepWorld;
	}
#undef VOXEL_VIEW_STEP
	if (hitOpacity <= 0.0)
		discard;

	// THE DEPTH at the hit: on the pixel's own view ray, t from the camera. Never nearer than the rectangle (depth_less).
	const vec4 clip = u_mvp * vec4(u_viewPos + voxelPixelDir() * t, 1.0);
	gl_FragDepth = min(clip.z / clip.w, gl_FragCoord.z);
	// The hit voxel's COVER, as the lighting uses it: how far its mean normal is trusted (a sparse voxel - a thin tip - holds
	// a leaf or two, whose mean is long though the tip around it faces every way).
	const uint agree = uint(smoothstep(0.05, 0.5, hitOpacity) * 7.0 + 0.5);
	out_hit = uvec2(VOXEL_HIT_VALID | tree | (uint(lod) << VOXEL_HIT_LOD_SHIFT) | (agree << VOXEL_HIT_AGREE_SHIFT), floatBitsToUint(t));
}
