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
// has no floor (0): the march falls back to the height map there (no density to place anyway). rMin is a FIXED horizontal radius: a high camera
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

float tvLogSpan(TreeVolumeParams v) { return log(v.rMax / v.rMin); }

// Radius r -> the image's normalized y (outside [0, 1]: outside the volume).
float tvRadialUv(float r, TreeVolumeParams v) { return log(max(r, 1e-3) / v.rMin) / tvLogSpan(v); }

// Radial texel coordinate (texels from rMin, continuous) -> radius.
float tvRadius(float texel, TreeVolumeParams v) { return v.rMin * exp(texel / float(v.radialRes) * tvLogSpan(v)); }

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
