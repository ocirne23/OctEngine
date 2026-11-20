// A MESH LEAF'S SUN SELF-SHADOW: the sun through the tree's OWN voxel grid, from the leaf to where the path leaves the
// grid's box - the voxel tier's march (instanced_indirect_tree_voxel.fs.glsl), on the same grid, through the same
// tweaks ("Trees/Voxel self shadow", "... coarser levels"), so a tree shades itself alike on both sides of the mesh <->
// voxels crossfade. The cascades cannot do it: the tree's caster there is three flat cards.
//
// WHERE THE GRID COMES FROM: a variant's LEAF MATERIAL carries its voxel opacity atlas's u_textures index in its
// metal-roughness slot (RenderMaterial::derive's linkedTexture; a tree material samples no metal-roughness), and the grid
// holds its own box (tree_voxel.inc.glsl voxelGridBox). THE VERTEX SHADER reads both, once per vertex
// (treeLeafGridVertex), and hands on the leaf's grid coordinate and, flat, the tree's part: no fetch of it per pixel.
//
// Requires tree_voxel.inc.glsl and the frame UBO.
#ifndef TREE_LEAF_SUN_INC_GLSL
#define TREE_LEAF_SUN_INC_GLSL

// The march's level has at most this many voxels a side: near the camera the voxel tier's own rule would take a level
// of 64 or more, a long march for every leaf pixel of a close tree.
const float LEAF_SUN_MAX_VOXELS = 32.0;
const int LEAF_SUN_MAX_STEPS = 24;

// THE VERTEX SIDE. linkedTexture = the material's metal-roughness slot (0xFFFF = no grid), local = the vertex in the
// tree's own space, quat / scale = the tree's.
//   grid  = the vertex in grid space (0..1 per axis; interpolated)
//   sun   = xyz the sun in grid units per metre, no zero component; w = the cube root of the box's volume in world
//           metres (x 1 / a level's voxels = that level's step), 0 = NO GRID
//   voxel = x the opacity atlas | the index texture << 16 (u_textures), y log2(res) | levels << 8 | log2(cols) << 16
void treeLeafGridVertex(uint linkedTexture, vec3 local, vec4 quat, float scale, out vec3 grid, out vec4 sun, out uvec2 voxel)
{
	grid = vec3(0.0);
	sun = vec4(0.0);
	voxel = uvec2(0u);
	if (linkedTexture == 0xFFFFu)
		return;
	const VoxelGrid g = voxelGrid(linkedTexture);
	vec3 boxMin, boxSize;
	voxelGridBox(g, boxMin, boxSize);
	if (boxSize.x <= 0.0 || boxSize.y <= 0.0 || boxSize.z <= 0.0)
		return;
	grid = (local - boxMin) / boxSize;
	const vec3 size = boxSize * scale;
	const vec3 sunRaw = quat_transform(u_sunDirection.xyz, vec4(-quat.xyz, quat.w)) / size;
	sun = vec4(mix(vec3(1e-6), sunRaw, greaterThan(abs(sunRaw), vec3(1e-6))), pow(size.x * size.y * size.z, 1.0 / 3.0));
	voxel = uvec2(g.opacityTex | (g.indexTex << 16), uint(log2(g.res) + 0.5) | (uint(g.levels) << 8) | (g.colsShift << 16));
}

// THE FRAGMENT SIDE.
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

#endif
