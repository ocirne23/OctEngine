// THE TREES' VOXEL GRIDS (Procedural TreeImpostor.ixx "The VOXEL tier" - keep in step): a variant's prefiltered grid,
// stored SPARSE in BRICKS over three textures, one level each (no mip chain):
// - THE OPACITY ATLAS (one channel): the stored bricks of every level. A level of n voxels is cut into bricks of
//   VOXEL_BRICK voxels a side, centre to centre, so a brick holds VOXEL_BRICK + 1 voxels a side and a filtered sample
//   never leaves it. Slot s sits at column s % cols, row s / cols, as its 8 z slices side by side (64 x 8 texels). A
//   voxel's value = the OPACITY over one voxel crossing. Slot 0 is the HEADER: its first two 4 x 4 blocks hold the index
//   texture's bindless id (low byte, high byte), so a pass that gets this atlas alone finds the rest.
// - THE INDEX (RGBA8, exact): texel (0, 0) = R log2(res), G the levels, B log2(cols), the rest of that row unused; below
//   it level by level its D^3 bricks - brick (x, y, z) at (x + z D, y) of the level's D rows: RGB = its slot, 0 = EMPTY
//   (nothing stored: air). The grid's BOX is not in the textures: the box mesh carries it for the voxel tier's draw, the
//   tree set's type table for the mesh leaves (tree_type.inc.glsl).
// - THE SURFACE ATLAS (half the opacity atlas each way): per stored brick 4^3 samples over the same span, as two tile
//   sets side by side (2 x 16 x 4 texels): RGB the mean colour (sRGB-encoded) + A the interior, then RGB the mean normal
//   + A the leaf share (0 = bark only).
// Shared by the main pass (instanced_indirect_tree_voxel.fs.glsl) and the voxel shadow casters (shadow_depth.fs.glsl).
// The includer declares u_textures[] and enables GL_EXT_nonuniform_qualifier.
#ifndef TREE_VOXEL_INC_GLSL
#define TREE_VOXEL_INC_GLSL

const float VOXEL_BRICK = 7.0;          // voxels a brick spans per axis (TREE_VOXEL_BRICK)
const float VOXEL_BRICK_TEXELS = 8.0;   // ... and stores (TREE_VOXEL_BRICK_TEXELS)
const float VOXEL_SURFACE_TEXELS = 4.0; // surface samples per axis over the same span (TREE_VOXEL_SURFACE_TEXELS)

struct VoxelGrid
{
	uint opacityTex;
	uint indexTex;
	float res;      // level 0's voxels per axis
	float levels;
	uint colsShift; // log2 of the bricks per atlas row (a power of two: a slot's column and row are a mask and a shift)
	vec2 invSize;   // 1 / the opacity atlas's size
};

// The last brick a march looked up on ONE level: the next sample is usually in the same brick (a brick is 7 steps wide),
// and then the index is not fetched again. Start it with voxelBrickNone().
// VOXEL_BRICK_ORIGIN (the includer's define): the brick holds its ORIGIN in the opacity atlas (two values), worked out
// once per brick; without it its SLOT (one value), and every sample works the origin out (a mask, a shift, two
// conversions, two multiply-adds). For a shader whose register count the march does not set (the main pass: its light
// loop does); in the shadow caster, where the march IS the count, the origin cost 4 registers.
struct VoxelBrick
{
	vec3 coord;
#ifdef VOXEL_BRICK_ORIGIN
	vec2 origin; // the centre of its first texel in the opacity atlas (texels); x < 0 = EMPTY
#else
	uint slot;   // 0 = EMPTY
#endif
};

// A slot's first texel centre in the opacity atlas: slot s sits at column s % cols, row s / cols (cols a power of two).
vec2 voxelSlotOrigin(VoxelGrid grid, uint slot)
{
	return vec2(float(slot & ((1u << grid.colsShift) - 1u)), float(slot >> grid.colsShift)) * vec2(VOXEL_BRICK_TEXELS * VOXEL_BRICK_TEXELS, VOXEL_BRICK_TEXELS) + 0.5;
}

#ifdef VOXEL_BRICK_ORIGIN
VoxelBrick voxelBrickNone() { return VoxelBrick(vec3(-1.0), vec2(-1.0)); }
VoxelBrick voxelBrickAt(VoxelGrid grid, vec3 coord, uint slot) { return VoxelBrick(coord, slot == 0u ? vec2(-1.0) : voxelSlotOrigin(grid, slot)); }
bool voxelBrickEmpty(VoxelBrick brick) { return brick.origin.x < 0.0; }
vec2 voxelBrickOrigin(VoxelGrid grid, VoxelBrick brick) { return brick.origin; }
#else
VoxelBrick voxelBrickNone() { return VoxelBrick(vec3(-1.0), 0u); }
VoxelBrick voxelBrickAt(VoxelGrid grid, vec3 coord, uint slot) { return VoxelBrick(coord, slot); }
bool voxelBrickEmpty(VoxelBrick brick) { return brick.slot == 0u; }
vec2 voxelBrickOrigin(VoxelGrid grid, VoxelBrick brick) { return voxelSlotOrigin(grid, brick.slot); }
#endif

// A level of the grid: its voxels per axis, its bricks per axis and its first row in the index.
struct VoxelLevel
{
	float n;
	float bricks;
	float indexY;
};

// The opacity atlas names its index texture in its header: the CPU guarantees the atlas's bindless slot holds it before
// any draw reads it (TextureStreamer's descriptor writes are never lost). NON-UNIFORM: one warp holds pixels of several trees.
VoxelGrid voxelGrid(uint opacityTex)
{
	VoxelGrid grid;
	grid.opacityTex = opacityTex;
	const float lo = texelFetch(u_textures[nonuniformEXT(opacityTex)], ivec2(1, 1), 0).r;
	const float hi = texelFetch(u_textures[nonuniformEXT(opacityTex)], ivec2(5, 1), 0).r;
	grid.indexTex = uint(lo * 255.0 + 0.5) | (uint(hi * 255.0 + 0.5) << 8);
	const vec3 header = floor(texelFetch(u_textures[nonuniformEXT(grid.indexTex)], ivec2(0), 0).rgb * 255.0 + 0.5);
	grid.res = exp2(header.r);
	grid.levels = header.g;
	grid.colsShift = uint(header.b);
	grid.invSize = 1.0 / vec2(textureSize(u_textures[nonuniformEXT(opacityTex)], 0));
	return grid;
}

float voxelLevelBricks(float n)
{
	return max(ceil((n - 1.0) / VOXEL_BRICK), 1.0);
}

VoxelLevel voxelLevel(VoxelGrid grid, float lod)
{
	VoxelLevel level;
	level.indexY = 1.0; // below the header row
	float n = grid.res;
	for (float k = 0.0; k < lod; k += 1.0)
	{
		level.indexY += voxelLevelBricks(n);
		n *= 0.5;
	}
	level.n = n;
	level.bricks = voxelLevelBricks(n);
	return level;
}

// The level one coarser than `level` (the caller keeps it inside the grid's levels): no second walk down the index.
VoxelLevel voxelLevelCoarser(VoxelLevel level)
{
	const float n = level.n * 0.5;
	return VoxelLevel(n, voxelLevelBricks(n), level.indexY + level.bricks);
}

// THE MARCHES RUN IN A LEVEL'S VOXEL COORDINATES: c = the grid coordinate (0..1 per axis) x the level's n, voxel i's
// centre at i + 0.5. `u` below = c - 0.5 held between the first and the last voxel centre (just under the last: the brick
// and the voxel below a sample are then plain floors, with no clamp after them).
vec3 voxelCentreCoord(VoxelLevel level, vec3 c)
{
	return clamp(c - 0.5, vec3(0.0), vec3(level.n - 1.001));
}

// voxelCentreCoord for a march that holds its start as c - 0.5 already (one subtraction a ray, not three a sample).
vec3 voxelCentreClamp(VoxelLevel level, vec3 centre)
{
	return clamp(centre, vec3(0.0), vec3(level.n - 1.001));
}

// A brick from the index: its slot in the atlases, 0 = EMPTY (nothing stored).
VoxelBrick voxelBrickFetch(VoxelGrid grid, VoxelLevel level, vec3 brick)
{
	const uvec3 e = uvec3(texelFetch(u_textures[nonuniformEXT(grid.indexTex)], ivec2(brick.x + brick.z * level.bricks, level.indexY + brick.y), 0).rgb * 255.0 + 0.5);
	return voxelBrickAt(grid, brick, e.x | (e.y << 8) | (e.z << 16));
}

// The distance along the ray from c to the far side of the EMPTY brick `brick`: the march jumps it. dirC = the ray's
// direction (voxels per metre, no zero component; divided by HERE, once per jumped brick - its reciprocal held across
// the march was three more registers). The first / last brick of an axis reaches past the box's side (the march's own
// exit distance ends it there).
float voxelBrickSkip(VoxelLevel level, vec3 c, vec3 dirC, vec3 brick)
{
	const vec3 lo = mix(brick * VOXEL_BRICK + 0.5, vec3(-1.0), lessThan(brick, vec3(0.5)));
	const vec3 hi = mix(brick * VOXEL_BRICK + (VOXEL_BRICK + 0.5), vec3(level.n + 1.0), greaterThan(brick, vec3(level.bricks - 1.5)));
	const vec3 toBound = (mix(lo, hi, greaterThan(dirC, vec3(0.0))) - c) / dirC;
	return max(min(min(toBound.x, toBound.y), toBound.z), 0.0);
}

// The distance along the ray from c (inside the box, or on its face) to where it leaves the level's box: a march runs
// while its distance is below this one - one compare a step, no box test on the sample.
float voxelBoxExit(VoxelLevel level, vec3 c, vec3 dirC)
{
	const vec3 toFace = (mix(vec3(0.0), vec3(level.n), greaterThan(dirC, vec3(0.0))) - c) / dirC;
	return max(min(min(toFace.x, toFace.y), toFace.z), 0.0);
}

// SHARP CELLS ("Trees/Voxel sharp cells", u_foliage_voxelSharp): each voxel covers its OWN cell - a sample reads the
// nearest voxel's texel, ONE fetch, no blend. Off = the trilinear filter: it spreads each voxel's opacity a whole voxel
// into its neighbours (the bake writes every surface point into its one voxel), which swells every clump and the
// silhouette, and it fetches a second slice. THE SWITCH IS A UBO VALUE: a constant while its tweak row is locked, so one
// of the two paths compiles; unlocked, a branch on a uniform.

// THE OPACITY, IN STEPS A MARCH PUTS TOGETHER ITSELF (the main pass's view and sun marches): a march's cost is its chain of
// fetches, each one waited for before the next sample is known - so a march fetches TWO samples of a brick before it waits
// for either (voxelOpacityPair). u = a sample in voxelCentreCoord's form.
// voxelSampleBrick: the sample's brick into `last` (the march's brick on this level: the index is fetched only when the
// sample enters another brick). False = that brick is EMPTY: the march jumps it (voxelBrickSkip, + 3 % of its step so the
// next sample lies inside the next brick).
bool voxelSampleBrick(VoxelGrid grid, VoxelLevel level, vec3 u, inout VoxelBrick last)
{
	const vec3 brick = floor(u * (1.0 / VOXEL_BRICK));
	if (any(notEqual(brick, last.coord)))
		last = voxelBrickFetch(grid, level, brick);
	return !voxelBrickEmpty(last);
}

bool voxelInBrick(vec3 u, VoxelBrick brick)
{
	return all(equal(floor(u * (1.0 / VOXEL_BRICK)), brick.coord));
}

// A sample's place in its STORED brick: xy = its uv in the opacity atlas, z = the blend to the next slice.
// Sharp cells: the nearest voxel's texel centre (a brick stores both its ends, so that voxel is in it), z 0.
// The filter: on its lower z slice (bilinear inside the slice).
vec3 voxelSliceUv(VoxelGrid grid, VoxelBrick brick, vec3 u)
{
	// Inside the brick: 0 .. VOXEL_BRICK between its first and last stored voxel centres.
	if (u_foliage_voxelSharp > 0.5)
	{
		const vec3 cell = floor(u + 0.5) - brick.coord * VOXEL_BRICK;
		return vec3((voxelBrickOrigin(grid, brick) + vec2(cell.z * VOXEL_BRICK_TEXELS + cell.x, cell.y)) * grid.invSize, 0.0);
	}
	const vec3 local = u - brick.coord * VOXEL_BRICK;
	const float s0 = min(floor(local.z), VOXEL_BRICK - 1.0);
	return vec3((voxelBrickOrigin(grid, brick) + vec2(s0 * VOXEL_BRICK_TEXELS + local.x, local.y)) * grid.invSize, local.z - s0);
}

// The opacities at u0 and u1, both in the stored brick `brick`. The two first taps go out TOGETHER - one wait for two
// steps of the march. A march with one sample left in the brick passes it twice (the same texel: no second wait).
// The filter only: linear between two slices, the second slice fetched where it weighs in.
// voxelOpacityPairAt: the same from the two samples' places (voxelSliceUv) - for a march that forms the first place
// before it works out the second sample (the voxel march pass: fewer values live at once).
vec2 voxelOpacityPairAt(VoxelGrid grid, vec3 s0, vec3 s1)
{
	vec2 a = vec2(textureLod(u_textures[grid.opacityTex], s0.xy, 0.0).r, textureLod(u_textures[grid.opacityTex], s1.xy, 0.0).r);
	if (u_foliage_voxelSharp > 0.5)
		return a;
	const vec2 nextSlice = vec2(VOXEL_BRICK_TEXELS * grid.invSize.x, 0.0);
	if (s0.z > 0.0)
		a.x = mix(a.x, textureLod(u_textures[grid.opacityTex], s0.xy + nextSlice, 0.0).r, s0.z);
	if (s1.z > 0.0)
		a.y = mix(a.y, textureLod(u_textures[grid.opacityTex], s1.xy + nextSlice, 0.0).r, s1.z);
	return a;
}
vec2 voxelOpacityPair(VoxelGrid grid, VoxelBrick brick, vec3 u0, vec3 u1)
{
	return voxelOpacityPairAt(grid, voxelSliceUv(grid, brick, u0), voxelSliceUv(grid, brick, u1));
}
// The opacity with ONE fetch at any setting: the nearest slice; inside it the nearest voxel (sharp cells) or bilinear
// (the shadow casters: a binary threshold, where the blend between two slices changed little and cost a second fetch
// per step).
float voxelOpacityNearest(VoxelGrid grid, VoxelLevel level, vec3 c, vec3 dirC, float stepWorld, inout VoxelBrick last, out float skip)
{
	const vec3 u = voxelCentreCoord(level, c);
	const vec3 brick = floor(u * (1.0 / VOXEL_BRICK));
	if (any(notEqual(brick, last.coord)))
		last = voxelBrickFetch(grid, level, brick);
	if (voxelBrickEmpty(last))
	{
		skip = voxelBrickSkip(level, c, dirC, brick) + 0.03 * stepWorld;
		return 0.0;
	}
	skip = 0.0;
	const vec2 local = (u_foliage_voxelSharp > 0.5 ? floor(u.xy + 0.5) : u.xy) - brick.xy * VOXEL_BRICK;
	const float slice = floor(u.z - brick.z * VOXEL_BRICK + 0.5);
	return textureLod(u_textures[grid.opacityTex], (voxelBrickOrigin(grid, last) + vec2(slice * VOXEL_BRICK_TEXELS + local.x, local.y)) * grid.invSize, 0.0).r;
}

// THE SURFACE at the march's HIT c (voxel coordinates) on a level, from the surface atlas `surfaceTex`: the mean colour
// (LINEAR), the mean normal x 0.5 + 0.5 and the interior. `hitBrick` = the march's brick at the hit (voxelOpacity's
// `last`: the hit sample lies in it, and it is a stored one) - no second index fetch.
// The surface atlas is half the opacity atlas each way, so a surface texel is two of `grid.invSize`: every offset below
// is in OPACITY texels (the brick's origin as the march holds it, a slice 8 on, the normal set 32 on).
// `leafShare` (the normal set's A): the share of LEAVES in the surface there, 0 = bark only (the trunk).
void voxelSurface(VoxelGrid grid, VoxelLevel level, uint surfaceTex, vec3 c, VoxelBrick hitBrick, out vec3 colour, out vec3 normal, out float interior, out float leafShare)
{
	// 0 .. 2 (VOXEL_SURFACE_TEXELS - 1), in opacity texels, between the brick's first and last surface sample.
	const vec3 q = (voxelCentreCoord(level, c) - hitBrick.coord * VOXEL_BRICK) * (2.0 * (VOXEL_SURFACE_TEXELS - 1.0) / VOXEL_BRICK);
	const float s0 = floor(q.z * 0.5);
	const float f = q.z * 0.5 - s0;
	const vec2 uv = (voxelBrickOrigin(grid, hitBrick) + 0.5 + vec2(s0 * (2.0 * VOXEL_SURFACE_TEXELS) + q.x, q.y)) * grid.invSize;
	const vec2 nextSlice = vec2(2.0 * VOXEL_SURFACE_TEXELS * grid.invSize.x, 0.0);
	const vec2 normalSet = vec2(2.0 * VOXEL_SURFACE_TEXELS * VOXEL_SURFACE_TEXELS * grid.invSize.x, 0.0); // one tile set: its slices side by side
	const vec4 tap = mix(textureLod(u_textures[surfaceTex], uv, 0.0), textureLod(u_textures[surfaceTex], uv + nextSlice, 0.0), f);
	const vec4 normalTap = mix(textureLod(u_textures[surfaceTex], uv + normalSet, 0.0), textureLod(u_textures[surfaceTex], uv + normalSet + nextSlice, 0.0), f);
	normal = normalTap.rgb;
	leafShare = normalTap.a;
	// The colour is stored sRGB-encoded in a linear texture (it shares it with the normals). Decoded with a cubic fit of
	// the sRGB curve (at most 0.002 off): the exact curve is a pow per channel and a select, on a colour that is a mean.
	colour = tap.rgb * (tap.rgb * (tap.rgb * 0.305306011 + 0.682171111) + 0.012522878);
	interior = tap.a;
}

// THINNING ("Trees/Voxel thinning"): a sampled opacity over one crossing with its extinction x u_foliage_voxelDensity
// (1 - the thinning) - the crown thinned evenly, its dense core and its sparse edge alike. Every march applies it (the view,
// the sun self-shadow, the shadow casters), so the tree and its shadow thin together.
float voxelThin(float a)
{
	// No thinning (a constant while its row is locked): the opacity as stored - no pow per step.
	if (u_foliage_voxelDensity == 1.0)
		return a;
	return 1.0 - pow(max(1.0 - a, 1e-4), u_foliage_voxelDensity);
}

#endif
