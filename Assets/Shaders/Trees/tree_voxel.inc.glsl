// THE TREES' VOXEL GRIDS (Procedural TreeImpostor.ixx "The VOXEL tier"): a variant's prefiltered grid, every level a 2D
// ATLAS of its z slices - slice z is tile (z % tilesX, z / tilesX) of tilesX x TREE_VOXEL_TILES_Y tiles, n x n texels each
// at a level of n voxels, the same uv rect at every level. Albedo atlas: RGB colour (sRGB), A = the OPACITY over one voxel
// crossing. Shared by the main pass (instanced_indirect_tree_voxel.fs.glsl) and the voxel shadow casters
// (shadow_depth.fs.glsl). The includer declares u_textures[].
#ifndef TREE_VOXEL_INC_GLSL
#define TREE_VOXEL_INC_GLSL

const float TREE_VOXEL_TILES_Y = 8.0; // Procedural TreeImpostor.ixx - keep in step

// The grid's resolution from its atlas width (res^2 / TREE_VOXEL_TILES_Y).
float voxelGridRes(uint tex)
{
	return floor(sqrt(float(textureSize(u_textures[tex], 0).x) * TREE_VOXEL_TILES_Y) + 0.5);
}

// Slice `slice`'s tile, at the point xy (0..1) inside it.
vec2 voxelTileUv(float slice, vec2 xy, float tilesX)
{
	return (vec2(mod(slice, tilesX), floor(slice / tilesX)) + xy) * vec2(1.0 / tilesX, 1.0 / TREE_VOXEL_TILES_Y);
}

// The grid at g (0..1 per axis) on a level of n voxels per axis: bilinear in the slice, linear between two slices. The xy
// is clamped half a texel inside the tile, so the filter never reads the neighbouring slice. `textureLevel` = the level
// to pass to textureLod (the caller takes its sampler's mip bias off): ONE level, never a blend of two - each has its
// own slice layout.
vec4 voxelTap(uint tex, vec3 g, float n, float tilesX, float textureLevel)
{
	const vec2 xy = clamp(g.xy * n, vec2(0.5), vec2(n - 0.5)) / n;
	const float z = clamp(g.z * n - 0.5, 0.0, n - 1.0);
	const float s0 = floor(z);
	const float s1 = min(s0 + 1.0, n - 1.0);
	const vec4 a = textureLod(u_textures[tex], voxelTileUv(s0, xy, tilesX), textureLevel);
	const vec4 b = textureLod(u_textures[tex], voxelTileUv(s1, xy, tilesX), textureLevel);
	return mix(a, b, z - s0);
}

// voxelTap's opacity with ONE fetch: the nearest slice, bilinear inside it (the shadow casters: a binary threshold, where
// the blend between two slices changed little and cost a second fetch per step).
float voxelOpacityNearest(uint tex, vec3 g, float n, float tilesX, float textureLevel)
{
	const vec2 xy = clamp(g.xy * n, vec2(0.5), vec2(n - 0.5)) / n;
	const float slice = clamp(floor(g.z * n), 0.0, n - 1.0);
	return textureLod(u_textures[tex], voxelTileUv(slice, xy, tilesX), textureLevel).a;
}

// EMPTY-SPACE SKIPPING: a grid's box is much larger than its crown, and a ray through its empty corners marched every
// step to the far side. The coarse level `skipLod`'s cell around g: when its opacity is about 0, the distance along the ray
// (dirG = grid units per metre, no zero component) to that cell's far side, else 0. A coarse voxel's opacity is
// prefiltered from all its children, and the filter's 8 taps always include the cell's own (weight >= 1/8), so a cell with
// any real cover reads above the threshold. `levelBias` = the sampler's mip bias, taken off.
const float VOXEL_SKIP_EMPTY = 0.5 / 255.0;
float voxelSkip(uint tex, vec3 g, vec3 dirG, float res, float tilesX, float skipLod, float levelBias)
{
	const float n = res * exp2(-skipLod);
	if (voxelTap(tex, g, n, tilesX, skipLod - levelBias).a > VOXEL_SKIP_EMPTY)
		return 0.0;
	const vec3 bound = (floor(g * n) + step(vec3(0.0), dirG)) / n;
	const vec3 toBound = (bound - g) / dirG;
	return max(min(min(toBound.x, toBound.y), toBound.z), 0.0) + 1e-3 / max(max(abs(dirG.x), abs(dirG.y)), abs(dirG.z));
}

#endif
