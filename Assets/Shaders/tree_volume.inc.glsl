// The FAR-TREE VOLUME (TreeVolumePipeline): one camera-centred 3D volume of leaf/wood EXTINCTION (1/m) that the
// far trees are marched through, from "Far start" (beyond the billboards, kilometres out) to "Far end".
//
// POLAR + LOG: image x = the angle around the bake centre (atan2(dz, dx), -pi..pi -> 0..1, it WRAPS - sample
// with a repeating sampler), image y = log(r / rMin) / log(rMax / rMin), so the volume starts AT the start distance
// and its cells grow linearly with the distance (a constant angular size from the camera); image z = the height
// above the column's FLOOR, [0, height] in `slices` slices. Nothing is stored inside rMin.
// THE FLOOR is the lowest base of the trees whose footprint reaches the column (R32UI, tree_volume_splat.cs's
// TREE_FLOOR_PASS, before the splat) - NOT the terrain height map: its far cascade's ~132 m texels put the ground
// tens of metres off on mountains, the trees fell outside the thin layer and went missing. A column without a tree
// has no floor (0): the march falls back to the height map there (no density to place anyway). The WORLD TREE RECORDS'
// trees and columns take their chunk's GROUND grid instead (tree_record.inc.glsl treeRecordGround: 16 m heights from
// the generator's own field) - a record has no height, its tree stands on that ground.
// rMin is a FIXED horizontal radius: a high camera
// moves the hand-over out instead (TreeVolumePipeline, the scaled start) - a ring shrinking toward the camera spread
// the radial texels thin and smeared the crowns into stripes.
// Mirrors TreeVolumePipeline.cpp - keep them in step.

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
    float height;       // m above the terrain the volume covers
    float densityScale; // x the baked extinction
    float pad0, pad1, pad2;
};

const float TV_TWO_PI = 6.28318531;

// THE ACCUMULATION (the bake's R32UI 3D image) packs TWO SLICES per texel: slice s lives in texel layer s / 2, in its
// low (even s) or high (odd s) 16 bits - each a fixed-point extinction x TV_ACCUM_SCALE, summed by imageAtomicAdd. A
// sum must stay below 64 / m per slice and texel (it is a mean extinction over the texel: overlapping crowns and rocks
// add, trees beside each other in one cell share it by area); a carry would spill into the other slice. After the
// resolve each half holds its slice's extinction as a half float (packHalf2x16: x = the even slice).
const float TV_ACCUM_SCALE = 1024.0;
ivec3 tvAccumTexel(ivec2 col, uint s) { return ivec3(col, int(s >> 1)); }
// One add's amount (a single add clamped to its half), shifted into slice s's half.
uint tvAccumAmount(float extinction, uint s) { return min(uint(extinction * TV_ACCUM_SCALE + 0.5), 0xFFFFu) << ((s & 1u) * 16u); }
// The world tree records' mass per column (tree_volume_records.cs -> tree_volume_far.cs): fixed point x this.
const float TV_AMOUNT_SCALE = 4096.0;
// The max-floor grid (tree_volume_floor_max.cs.glsl): columns per block side (TreeVolumePipeline's FLOOR_MAX_BLOCK), and
// the value of a region without any floor.
const int TV_FLOOR_MAX_BLOCK = 16;
const float TV_FLOOR_MAX_NONE = -1e30;
const int TV_FLOOR_AHEAD_SECTORS = 4; // the grid's AHEAD value: blocks each way of the angle it covers

float tvLogSpan(TreeVolumeParams v) { return log(v.rMax / v.rMin); }

// Radius r -> the image's normalized y (outside [0, 1]: outside the volume).
float tvRadialUv(float r, TreeVolumeParams v) { return log(max(r, 1e-3) / v.rMin) / tvLogSpan(v); }

// Radial texel coordinate (texels from rMin, continuous) -> radius.
float tvRadius(float texel, TreeVolumeParams v) { return v.rMin * exp(texel / float(v.radialRes) * tvLogSpan(v)); }

// An angular texel index wrapped into [0, res), for any x >= -2 res. NOT `((x % res) + res) % res`: GLSL leaves `%`
// UNDEFINED for a negative operand, and the indices just below 0 (the tent's and the floor ring's left neighbours, the
// march's bilinear base) are exactly the ones at the seam - a line of broken columns along the bake centre's -X.
int tvWrapAngle(int x, int res) { return int(uint(x + 2 * res) % uint(res)); }

// A cell's radial / tangential size (m) at radius r.
float tvRadialCell(float r, TreeVolumeParams v) { return r * tvLogSpan(v) / float(v.radialRes); }
float tvTangentialCell(float r, TreeVolumeParams v) { return r * TV_TWO_PI / float(v.angularRes); }

// The larger of a cell's tangential and radial size (m) at radius r.
float tvCellSize(float r, TreeVolumeParams v) { return max(tvRadialCell(r, v), tvTangentialCell(r, v)); }

// The floor's encoding: atomicMax of the INVERTED order-preserving bits keeps the MINIMUM height, and 0 (a NaN's
// pattern, never a height) = no floor.
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

// World XZ -> the image's normalized (angle, radius).
vec2 tvUvXZ(vec2 worldXZ, TreeVolumeParams v)
{
    const vec2 rel = worldXZ - v.centre;
    return vec2(atan(rel.y, rel.x) / TV_TWO_PI + 0.5, tvRadialUv(length(rel), v));
}

// Texel (angle, radius) centre -> world XZ.
vec2 tvTexelWorldXZ(ivec2 texel, TreeVolumeParams v)
{
    const float theta = ((float(texel.x) + 0.5) / float(v.angularRes) - 0.5) * TV_TWO_PI;
    return v.centre + vec2(cos(theta), sin(theta)) * tvRadius(float(texel.y) + 0.5, v);
}

#endif
