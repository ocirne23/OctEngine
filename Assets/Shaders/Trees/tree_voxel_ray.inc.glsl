// THE VOXEL TREES' RAY, shared by their TWO fragment shaders (Docs/Context/RendererVK/Scene.md "THE VOXEL TIER"):
// - THE MARCH (tree_voxel_march.fs.glsl; the voxel march pass, after every other opaque draw): the pixel's ray against the
//   tree's box, the march through its grid, and on a hit the depth + the HIT WORD and the hit's distance into the voxel
//   hit target. No lighting: it runs at the march's register count, and a ray that hits nothing pays for nothing else.
// - THE LIGHTING (instanced_indirect_tree_voxel.fs.glsl; one full-screen draw): per pixel that holds a hit, the same ray
//   again from the hit word, the hit's place on it from the stored distance, then the surface and the lit core - once
//   per FINAL pixel, however many trees overlap there.
// BOTH BUILD THE RAY FROM THE SAME INPUTS WITH THE SAME CODE (this file): the pixel (gl_FragCoord), and the tree - the
// near tier's instance record at its draw slot (the main cull's OutMeshInstance), a far voxel tree's record in its sorted
// list (TreeVoxelFarPipeline). No interpolated value is involved. Every value on the way is `precise` (no contraction
// that one shader's compile could take and the other's not), so the lighting finds the hit in the same voxel of the same
// brick the march stopped in; where it still does not (the hit's brick reads EMPTY), it takes a neutral surface.
// Requires shared.inc.glsl; declares bindings 1, 3 and 26 of the static mesh set.
#ifndef TREE_VOXEL_RAY_INC_GLSL
#define TREE_VOXEL_RAY_INC_GLSL

#include "tree_voxel_box.inc.glsl"
#ifndef TREE_TYPES_BINDING
#define TREE_TYPES_BINDING 3
#endif
#include "tree_type.inc.glsl"

// The near tier's tree: the main cull's OutMeshInstance of a tree record (64 B; `wind` / `rest` = tree_wind.inc.glsl
// treeWindPack: wind.xy = the trunk vector, rest.x = the set TYPE | the material << 16, rest.y = the trunk's delta).
struct VoxelNearTree
{
	vec4 posScale;
	vec4 quat;
	vec4 wind;
	uvec4 rest;
};
layout (binding = 1, std430) readonly buffer InVoxelNearTrees { VoxelNearTree in_voxelNearTrees[]; };
// A far voxel tree: TreeVoxelFarPipeline::InstanceGpu (32 B), in the view's sorted list.
struct VoxelFarTree
{
	vec4 posScale;
	float yaw;         // the tree turns about up
	uint typeMaterial; // the set TYPE | the material << 16
	uint wind;         // 0: a far voxel tree stands still
	uint bucketLocal;
};
layout (binding = 26, std430) readonly buffer InVoxelFarTrees { VoxelFarTree in_voxelFarTrees[]; };

// WHAT IS ONE VALUE PER TREE, read where it is used (`tree` = the hit word's low 24 bits: the index + VOXEL_HIT_FAR).
// Every pixel of a tree shares the loads; nothing is held from one use to the next.
bool voxelTreeIsFar(uint tree) { return (tree & VOXEL_HIT_FAR) != 0u; }
vec4 voxelTreePosScale(uint tree)
{
	const uint i = tree & VOXEL_HIT_INDEX_MASK;
	if (voxelTreeIsFar(tree))
		return in_voxelFarTrees[i].posScale;
	return in_voxelNearTrees[i].posScale;
}
vec4 voxelTreeQuat(uint tree) // grid (tree) axes -> world
{
	const uint i = tree & VOXEL_HIT_INDEX_MASK;
	if (voxelTreeIsFar(tree))
	{
		const float halfYaw = 0.5 * in_voxelFarTrees[i].yaw;
		return vec4(0.0, sin(halfYaw), 0.0, cos(halfYaw));
	}
	return in_voxelNearTrees[i].quat;
}
uint voxelTreeTypeMaterial(uint tree)
{
	const uint i = tree & VOXEL_HIT_INDEX_MASK;
	if (voxelTreeIsFar(tree))
		return in_voxelFarTrees[i].typeMaterial;
	return in_voxelNearTrees[i].rest.x;
}
vec2 voxelTreeTrunk(uint tree) // the trunk vector (world XZ; tree_wind.inc.glsl TreeWind::trunk)
{
	if (voxelTreeIsFar(tree))
		return vec2(0.0);
	return in_voxelNearTrees[tree & VOXEL_HIT_INDEX_MASK].wind.xy;
}

// mathutils.inc.glsl quat_transform, every product in its stated order.
vec3 voxelRotate(vec3 v, vec4 q)
{
	precise vec3 a = vec3(q.y * v.z - q.z * v.y, q.z * v.x - q.x * v.z, q.x * v.y - q.y * v.x) + q.w * v;
	precise vec3 b = vec3(q.y * a.z - q.z * a.y, q.z * a.x - q.x * a.z, q.x * a.y - q.y * a.x);
	precise vec3 r = v + 2.0 * b;
	return r;
}

// THE PIXEL'S VIEW DIRECTION (world, unit), from the pixel alone: camera-relative and linear in the NDC position
// (shared.inc.glsl viewRelFromDepth's numerator: the eye cancels, so it keeps its precision far from the world origin).
// The image at a pixel is the surface at the pixel less the TAA jitter.
vec3 voxelPixelDir()
{
	const vec2 uv = gl_FragCoord.xy * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
	const vec2 vpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
	const mat4 invM = u_invMvp;
	precise vec3 ray = invM[0].xyz * (vpUv.x * 2.0 - 1.0) + invM[1].xyz * (1.0 - vpUv.y * 2.0) + (invM[3].xyz - u_viewPos * invM[3].w);
	precise vec3 dir = ray * inversesqrt(dot(ray, ray));
	return dir;
}

// THE PIXEL'S RAY THROUGH THE TREE'S GRID: camG = the camera in grid space, dirG = the ray's direction in grid units per
// metre (no zero component: the empty bricks' jump divides by it), tNear / tFar = the distances from the camera (metres)
// at which it enters and leaves the box (tNear 0: the camera stands inside). False = the ray misses the box: a pixel of
// the rectangle beside it. dirW = voxelPixelDir(). The march runs from the CAMERA along it (its t starts at tNear).
// THE TRUNK'S BEND (the near tier) is a shear of the ray (tree_voxel_box.inc.glsl treeVoxelBoxShear) about THE HEIGHT AT
// WHICH THE RAY PASSES THE TREE'S AXIS - where the crown it will hit stands - found from the ray alone (two dot
// products), so there is ONE slab test, of the sheared ray against the box. The bend is exact at that height and off by
// trunk x (the height from it / ref)² elsewhere along the ray. A far voxel tree's trunk vector is 0: the same code, no
// shear. Its distances are the sheared ray's: the camera and the hit shear by different amounts, so the hit's world
// position lies up to that difference along the view ray from the true one (centimetres; decimetres in a storm).
// Tried, dropped: the pivot at the ray's ENTRY height, which needs the entry first - three slab tests (the plain ray
// against the box widened by its top's bend, then twice the sheared ray). Every test after the first held the ray's
// inputs across the one before: 48 registers where one test takes 40, in the shader whose warps the registers limit.
bool voxelTreeRay(uint tree, vec3 dirW, out vec3 camG, out vec3 dirG, out float tNear, out float tFar)
{
	const uint typeIdx = voxelTreeTypeMaterial(tree) & 0xFFFFu;
	const vec4 posScale = voxelTreePosScale(tree);
	// INTO THE TREE'S AXES ONCE (metres): the camera, the pixel's direction and the trunk vector. A tree turns about up
	// only, so the shear (sideways by the height) is the same shear there - no rotation per test.
	const vec4 quat = voxelTreeQuat(tree);
	const vec4 toLocal = vec4(-quat.xyz, quat.w);
	const vec2 trunkW = voxelTreeTrunk(tree);
	precise vec3 camL = voxelRotate(u_viewPos - posScale.xyz, toLocal);
	precise vec3 dirL = voxelRotate(dirW, toLocal);
	precise vec2 trunkL = voxelRotate(vec3(trunkW.x, 0.0, trunkW.y), toLocal).xz;
	precise vec3 perMetre = in_treeTypes[typeIdx].gridInvSize / posScale.w;                         // grid units per metre
	// THE PIVOT: the ray's height (over the tree's base, metres) where it is nearest to the tree's axis, inside the tree.
	// Ahead of the camera only; a ray straight down the axis takes the camera's height.
	const float topHeight = max(in_treeTypes[typeIdx].gridMin.y + in_treeTypes[typeIdx].gridSize.y, 0.0) * posScale.w;
	precise float toAxis = max(-(camL.x * dirL.x + camL.z * dirL.z) / max(dirL.x * dirL.x + dirL.z * dirL.z, 1e-6), 0.0);
	precise float pivot = clamp(camL.y + dirL.y * toAxis, 0.0, topHeight);
	precise vec2 shear = treeVoxelBoxShear(pivot, camL.y);
	// The ray sheared about it, then in grid units.
	precise float lean = shear.y * dirL.y;
	precise vec3 sheared = vec3(dirL.x - trunkL.x * lean, dirL.y, dirL.z - trunkL.y * lean);
	precise vec3 raw = sheared * inversesqrt(dot(sheared, sheared)) * perMetre;
	precise vec3 cam = vec3(camL.x - trunkL.x * shear.x, camL.y, camL.z - trunkL.y * shear.x) * perMetre
		- in_treeTypes[typeIdx].gridMin * in_treeTypes[typeIdx].gridInvSize; // the tree's origin in grid space
	precise vec3 dir = mix(vec3(1e-6), raw, greaterThan(abs(raw), vec3(1e-6)));
	precise vec3 t0 = -cam / dir;
	precise vec3 t1 = (1.0 - cam) / dir;
	const vec3 tLow = min(t0, t1);
	const vec3 tHigh = max(t0, t1);
	camG = cam;
	dirG = dir;
	tNear = max(max(max(tLow.x, tLow.y), tLow.z), 0.0);
	tFar = min(min(tHigh.x, tHigh.y), tHigh.z);
	return tNear < tFar;
}

// A SAMPLE OF THE RAY in a level's voxel CENTRE coordinates (c - 0.5; tree_voxel.inc.glsl): the camera's + the direction
// x the distance. n = the level's voxels per axis. The march steps with it and the lighting finds the hit with it - the
// same three operations, in this order.
vec3 voxelRayCentre(vec3 camG, vec3 dirG, float n, float t)
{
	precise vec3 start = camG * n - 0.5;
	precise vec3 perMetre = dirG * n;
	precise vec3 centre = start + perMetre * t;
	return centre;
}

#endif
