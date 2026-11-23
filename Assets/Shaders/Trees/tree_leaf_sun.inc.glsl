// A MESH LEAF'S SUN SELF-SHADOW: the sun through the tree's OWN voxel grid, from the leaf to where the path leaves the
// grid's box - the voxel tier's march (instanced_indirect_tree_voxel.fs.glsl), on the same grid, through the same
// tweaks ("Trees/Voxel self shadow", "... coarser levels"), so a tree shades itself alike on both sides of the mesh <->
// voxels crossfade. The cascades cannot do it: the tree's caster there is three flat cards.
//
// WHERE THE GRID COMES FROM: THE TREE SET'S TYPE TABLE (tree_type.inc.glsl: per type the grid's box in the tree's own
// space, its textures and its header - written by the CPU, RenderTreeSet::Type's voxel grid). The main cull puts a tree's
// TYPE into its record where a plain instance has its mesh index (instanced_indirect.cs.glsl), so a vertex and a fragment
// find the grid with no texture fetch. What differs per TREE (the sun in its grid's space, its scale) the fragment works
// out from the tree's instance record, at the draw slot the vertex shader hands on as ONE flat value. Only the leaf's grid
// coordinate is interpolated.
//
// The includer defines TREE_TYPES_BINDING (tree_type.inc.glsl: the table). The fragment side (TREE_LEAF_SUN_MARCH, the
// includer's define) requires tree_voxel.inc.glsl, u_textures[], the frame UBO, mathutils.inc.glsl and the instance
// records as in_drawInstances[] (posScale, quat).
#ifndef TREE_LEAF_SUN_INC_GLSL
#define TREE_LEAF_SUN_INC_GLSL

#include "tree_type.inc.glsl"

// THE VERTEX SIDE: the vertex `local` (the tree's own space) in its type's grid space, 0..1 per axis. A type without a
// grid has the inverse size 0: its coordinate is 0.
vec3 treeLeafGridCoord(uint typeIdx, vec3 local)
{
	return (local - in_treeTypes[typeIdx].gridMin) * in_treeTypes[typeIdx].gridInvSize;
}

#ifdef TREE_LEAF_SUN_MARCH
// THE FRAGMENT SIDE.

// The march's level has at most this many voxels a side: near the camera the voxel tier's own rule would take a level
// of 64 or more, a long march for every leaf pixel of a close tree.
const float LEAF_SUN_MAX_VOXELS = 32.0;
const int LEAF_SUN_MAX_STEPS = 24;

// THE SUN IN THE GRID of the tree at draw slot `instance`: xyz = grid units per metre, no zero component; w = the cube
// root of the box's volume in world metres (x 1 / a level's voxels = that level's step), 0 = NO GRID. Loaded and worked
// out where it is used (the exit, then the march): a caller holds nothing of it across the shadow search between them.
vec4 treeLeafSun(uint instance, uint typeIdx)
{
	const float cbrtVolume = in_treeTypes[typeIdx].gridCbrtVolume;
	if (cbrtVolume <= 0.0)
		return vec4(0.0);
	const vec4 quat = in_drawInstances[instance].quat;
	const float scale = in_drawInstances[instance].posScale.w;
	const vec3 sunRaw = quat_transform(u_sunDirection.xyz, vec4(-quat.xyz, quat.w)) * (in_treeTypes[typeIdx].gridInvSize / scale);
	return vec4(mix(vec3(1e-6), sunRaw, greaterThan(abs(sunRaw), vec3(1e-6))), cbrtVolume * scale);
}

// The grid's textures and its header: x = the opacity atlas | the index texture << 16 (u_textures), y = log2(res) |
// levels << 8 | log2(cols) << 16.
uvec2 treeLeafGridVoxel(uint typeIdx)
{
	return uvec2(in_treeTypes[typeIdx].gridTextures, in_treeTypes[typeIdx].gridHeader);
}

// The sun path's length out of the tree's box from the leaf (m), 0 without a grid, else > 0: the cascades' lookup moves
// there, so the tree's own caster does not shade it a second time (as the voxel tier's). No texture.
float treeLeafSunExit(vec3 leafG, vec4 sun)
{
	if (sun.w <= 0.0)
		return 0.0;
	const vec3 toFace = (mix(vec3(0.0), vec3(1.0), greaterThan(sun.xyz, vec3(0.0))) - clamp(leafG, vec3(0.0), vec3(1.0))) / sun.xyz;
	return max(min(min(toFace.x, toFace.y), toFace.z), 1e-3);
}

// The sun's transmittance through the tree's own voxels at the leaf (1 = open, and without a grid). viewDist = the
// camera's distance to the fragment (the level, as the voxel tier picks it).
float treeLeafSunTransmit(vec3 leafG, vec4 sun, uvec2 voxel, float viewDist)
{
	if (sun.w <= 0.0)
		return 1.0;
	VoxelGrid grid;
	grid.opacityTex = voxel.x & 0xFFFFu;
	grid.indexTex = voxel.x >> 16;
	grid.res = exp2(float(voxel.y & 0xFFu));
	grid.levels = float((voxel.y >> 8) & 0xFFu);
	grid.colsShift = (voxel.y >> 16) & 0xFFu;
	grid.invSize = 1.0 / vec2(textureSize(u_textures[nonuniformEXT(grid.opacityTex)], 0));

	// THE LEVEL: the voxel tier's - the level whose voxel is about a pixel, + "Voxel self shadow coarser levels" - but
	// never finer than LEAF_SUN_MAX_VOXELS a side.
	const float lodScale = 2.0 * exp2(u_foliage_voxelLodBias) / (u_mipPixelScale * sun.w);
	const float viewLod = max(floor(log2(max(grid.res * lodScale * viewDist, 1e-6))), 0.0);
	const float lod = clamp(max(viewLod + u_foliage_voxelSelfShadowLevels, log2(grid.res / LEAF_SUN_MAX_VOXELS)), 0.0, grid.levels - 1.0);
	const VoxelLevel level = voxelLevel(grid, lod);
	const float n = level.n;
	const float stepWorld = sun.w / n; // one crossing: the stored opacity's length

	// The march, in the level's voxel coordinates (tree_voxel.inc.glsl), two steps a wait. It starts ONE step out: the
	// leaf's own voxel holds the leaf.
	const vec3 leaf = clamp(leafG, vec3(0.0), vec3(1.0));
	const vec3 toFace = (mix(vec3(0.0), vec3(1.0), greaterThan(sun.xyz, vec3(0.0))) - leaf) / sun.xyz;
	const float exitDist = max(min(min(toFace.x, toFace.y), toFace.z), 0.0);
	const vec3 leafU = leaf * n - 0.5;
	const vec3 dirC = sun.xyz * n;
	const int steps = min(int(exitDist / stepWorld) + 1, LEAF_SUN_MAX_STEPS);
	float d = stepWorld;
	float transmit = 1.0;
	VoxelBrick brick = voxelBrickNone();
	for (int i = 0; i < steps && transmit > 0.01 && d <= exitDist; ++i)
	{
		const vec3 centre = leafU + dirC * d;
		const vec3 u = voxelCentreClamp(level, centre);
		if (!voxelSampleBrick(grid, level, u, brick))
		{
			d += voxelBrickSkip(level, centre + 0.5, dirC, brick.coord) + 0.03 * stepWorld;
			continue;
		}
		const float dNext = d + stepWorld;
		const vec3 uNext = voxelCentreClamp(level, leafU + dirC * dNext);
		const bool pair = dNext <= exitDist && i + 1 < steps && voxelInBrick(uNext, brick);
		const vec2 a = voxelOpacityPair(grid, brick, u, pair ? uNext : u);
		transmit *= 1.0 - voxelThin(a.x);
		d = dNext;
		if (pair && transmit > 0.01)
		{
			transmit *= 1.0 - voxelThin(a.y);
			d += stepWorld;
			++i;
		}
	}
	return transmit;
}

// THE CROWN'S INTERIOR at the leaf (0 = the crown's surface .. 1 = its core): the voxel tier's baked value - the surface
// atlas's alpha (voxelSurface's `interior`) of the type's VOXEL material, on a level of about 16 voxels a side, at the
// leaf's grid point. The snow on the mesh leaves takes the voxel crowns' outer-shell rule by it. 0 without a grid, without
// a voxel tier, and in an EMPTY brick (a leaf outside the crown's stored voxels lies on its surface). Three fetches.
float treeLeafCrownInterior(vec3 leafG, uint typeIdx)
{
	const uint voxelMaterial = in_treeTypes[typeIdx].voxel.meshMaterial;
	if (in_treeTypes[typeIdx].gridCbrtVolume <= 0.0 || voxelMaterial == 0xFFFFFFFFu)
		return 0.0;
	const uvec2 voxel = treeLeafGridVoxel(typeIdx);
	VoxelGrid grid;
	grid.opacityTex = voxel.x & 0xFFFFu;
	grid.indexTex = voxel.x >> 16;
	grid.res = exp2(float(voxel.y & 0xFFu));
	grid.levels = float((voxel.y >> 8) & 0xFFu);
	grid.colsShift = (voxel.y >> 16) & 0xFFu;
	grid.invSize = 1.0 / vec2(textureSize(u_textures[nonuniformEXT(grid.opacityTex)], 0));
	const VoxelLevel level = voxelLevel(grid, clamp(log2(grid.res / 16.0), 0.0, grid.levels - 1.0));
	const vec3 c = clamp(leafG, vec3(0.0), vec3(1.0)) * level.n;
	const vec3 u = voxelCentreCoord(level, c);
	const VoxelBrick brick = voxelBrickFetch(grid, level, floor(u * (1.0 / VOXEL_BRICK)));
	if (voxelBrickEmpty(brick))
		return 0.0;
	// voxelSurface's place in the surface atlas (half the opacity atlas each way), the colour set's alpha only.
	const uint surfaceTex = in_materialInfos[voxelMaterial >> 16].diffuseNormalTexIdx >> 16;
	const vec3 q = (u - brick.coord * VOXEL_BRICK) * (2.0 * (VOXEL_SURFACE_TEXELS - 1.0) / VOXEL_BRICK);
	const float s0 = floor(q.z * 0.5);
	const vec2 uv = (voxelBrickOrigin(grid, brick) + 0.5 + vec2(s0 * (2.0 * VOXEL_SURFACE_TEXELS) + q.x, q.y)) * grid.invSize;
	const vec2 nextSlice = vec2(2.0 * VOXEL_SURFACE_TEXELS * grid.invSize.x, 0.0);
	return mix(textureLod(u_textures[nonuniformEXT(surfaceTex)], uv, 0.0).a, textureLod(u_textures[nonuniformEXT(surfaceTex)], uv + nextSlice, 0.0).a,
		q.z * 0.5 - s0);
}
#endif

#endif
