// The FAR-TREE VOLUME (TreeVolumePipeline): one camera-centred volume of leaf / wood EXTINCTION (1/m) that the far
// trees are marched through, from "Far start" to "Far end". Mirrors TreeVolumePipeline.cpp - keep them in step.
//
// POLAR + LOG: image x = the angle around the bake centre (atan2(dz, dx), -pi..pi -> 0..1; it WRAPS, so sample with
// a repeating sampler), image y = log(r / rMin) / log(rMax / rMin) (the cells grow linearly with the distance: a
// constant angular size), image z = the height above the column's FLOOR, [0, height] in `slices` slices. Nothing is
// stored inside rMin, a FIXED horizontal radius (a high camera moves the hand-over out instead).
// THE FLOOR (R32UI, tvFloorEncode; 0 = no floor) is the base of the column's dominant tree (tree_volume_splat.cs's
// TREE_FLOOR_PASS), NOT the terrain height map: its far texels put the ground tens of metres off on mountains and the
// trees fell out of the thin layer. The world tree records' trees stand on their chunk's ground grid
// (tree_record.inc.glsl treeRecordGround). A column without a floor holds no density.

#ifndef TREE_VOLUME_INC_GLSL
#define TREE_VOLUME_INC_GLSL

struct TreeVolumeParams
{
    vec2 centre;        // world XZ of the bake centre
    float rMin;         // m: the volume's inner radius (horizontal)
    float rMax;         // m: its outer radius ("Far end")
    uint angularRes;    // texels around
    uint radialRes;     // texels from rMin to rMax
    uint slices;        // height slices
    float height;       // m above the floor the volume covers
    float densityScale; // x the baked extinction
    float pad0, pad1, pad2;
};

// The params in a pass's PUSH BLOCK (push.generated.glsl, TreeVolumePipeline's TvVolPush): flat pc_vol_* members, all
// but the centre lockable. TV_PUSH_VOL packs them for the helpers below, so the baked members fold.
#define TV_PUSH_VOL TreeVolumeParams(pc_vol_centre, pc_vol_rMin, pc_vol_rMax, pc_vol_angularRes, pc_vol_radialRes, pc_vol_slices, \
    pc_vol_height, pc_vol_densityScale, 0.0, 0.0, 0.0)

const float TV_TWO_PI = 6.28318531;

// THE ACCUMULATION (the bake's R32UI 3D image) packs TWO SLICES per texel: slice s in texel layer s / 2, its low (even
// s) or high (odd s) 16 bits, a fixed-point extinction x TV_ACCUM_SCALE summed by imageAtomicAdd. A sum must stay below
// 64 / m per slice and texel (a carry spills into the other slice). After the resolve each half holds its slice's
// extinction as a half float (packHalf2x16: x = the even slice).
const float TV_ACCUM_SCALE = 1024.0;
ivec3 tvAccumTexel(ivec2 col, uint s) { return ivec3(col, int(s >> 1)); }
// One add's amount, clamped to its half and shifted into slice s's half.
uint tvAccumAmount(float extinction, uint s) { return min(uint(extinction * TV_ACCUM_SCALE + 0.5), 0xFFFFu) << ((s & 1u) * 16u); }
// The world tree records' mass per column (tree_volume_records.cs -> tree_volume_far.cs), fixed point.
const float TV_AMOUNT_SCALE = 4096.0;
// The max-floor grid (tree_volume_floor_max.cs): columns per block side (TreeVolumePipeline's FLOOR_MAX_BLOCK), the
// value of a region without any floor, and the blocks each way of the angle its AHEAD value covers.
const int TV_FLOOR_MAX_BLOCK = 16;
const float TV_FLOOR_MAX_NONE = -1e30;
const int TV_FLOOR_AHEAD_SECTORS = 4;

float tvLogSpan(TreeVolumeParams v) { return log(v.rMax / v.rMin); }

// Radius r -> the image's normalized y (outside [0, 1]: outside the volume).
float tvRadialUv(float r, TreeVolumeParams v) { return log(max(r, 1e-3) / v.rMin) / tvLogSpan(v); }

// Radial texel coordinate (texels from rMin, continuous) -> radius.
float tvRadius(float texel, TreeVolumeParams v) { return v.rMin * exp(texel / float(v.radialRes) * tvLogSpan(v)); }

// An angular texel index wrapped into [0, res), for any x >= -2 res. NOT `((x % res) + res) % res`: GLSL leaves `%`
// UNDEFINED for a negative operand, and the indices just below 0 lie exactly on the seam.
int tvWrapAngle(int x, int res) { return int(uint(x + 2 * res) % uint(res)); }

// A cell's radial / tangential size (m) at radius r, and the larger of the two.
float tvRadialCell(float r, TreeVolumeParams v) { return r * tvLogSpan(v) / float(v.radialRes); }
float tvTangentialCell(float r, TreeVolumeParams v) { return r * TV_TWO_PI / float(v.angularRes); }
float tvCellSize(float r, TreeVolumeParams v) { return max(tvRadialCell(r, v), tvTangentialCell(r, v)); }

// The floor's encoding: atomicMax of the INVERTED order-preserving bits keeps the MINIMUM height; 0 (a NaN's pattern,
// never a height) = no floor.
uint tvFloorEncode(float y)
{
    const uint u = floatBitsToUint(y);
    return ~((u & 0x80000000u) != 0u ? ~u : u | 0x80000000u);
}
float tvFloorDecode(uint e)
{
    const uint o = ~e;
    return uintBitsToFloat((o & 0x80000000u) != 0u ? o & 0x7FFFFFFFu : ~o);
}

// Texel (angle, radius) centre -> world XZ.
vec2 tvTexelWorldXZ(ivec2 texel, TreeVolumeParams v)
{
    const float theta = ((float(texel.x) + 0.5) / float(v.angularRes) - 0.5) * TV_TWO_PI;
    return v.centre + vec2(cos(theta), sin(theta)) * tvRadius(float(texel.y) + 0.5, v);
}

#endif
