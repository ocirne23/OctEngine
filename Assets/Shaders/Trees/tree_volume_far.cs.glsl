#version 460

// FAR-TREE VOLUME BAKE, the WORLD TREE RECORDS' second step (TreeVolumePipeline, W4), one thread per column, after
// tree_volume_records.cs summed the records' mass per column:
// * THE FLOOR of a column with record mass, or next to one, that no tree set floored: the ground under its centre -
//   its chunk's height grid (the records carry no height - a record's tree stands on it), the terrain map where that
//   chunk holds none. (The map alone, ~100 m texels in its far cascade, buried the trees on every peak until the
//   camera came within its near cascade.) The march filters the density over
//   the 2x2 columns around a point but reads the nearest column's floor and skips a column without one: the ring
//   around the mass must carry a floor too, as the splat's floor ring (tree_volume_splat.cs).
// * THE SLICES: the column's mass x its type's height profile (the profile's mean over each slice), added to the
//   accumulation in the splat's fixed point. A column without a type (its trees' nearest columns lie next to it)
//   takes a neighbour's, else the first type with mass.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
#include "tree_volume.inc.glsl"
#define TREE_RECORD_GROUND
#include "tree_record.inc.glsl"

layout (local_size_x = 8, local_size_y = 8) in;

layout (binding = 2, r32ui) uniform readonly uimage2D u_amount;
layout (binding = 3, r32ui) uniform readonly uimage2D u_type;
layout (binding = 4, r32ui) uniform uimage2D u_floor;
layout (binding = 5, r32ui) uniform uimage3D u_accum; // two slices per texel (tree_volume.inc.glsl tvAccumTexel)

// Mirrors TreeRecordTypeGpu (RendererVK TreeRecordPool.ixx).
struct RecordType
{
    vec4 albedo;
    float mass;
    float height;
    float radius;
    float sizeVariation;
    vec2 scale;
    uint numVariants;
    uint numBushes;
    float bushesPerTree;
    float bushRadius;
    float pad0, pad1;
    uint variantType[8];
    float variantMass[8];
    uint bushTypes[8];
    float shape[32];
};
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TypeList { RecordType t[]; };

// The push block: TreeVolumePipeline's FarPush (pc_vol_*: the bake's lockable values).
#include "push.generated.glsl"

const uint PROFILE_BINS = 32u;      // TREE_RECORD_PROFILE_BINS

bool validType(uint type) { return type < pc_numTypes && pc_types.t[type].mass > 0.0; }

void main()
{
    const ivec2 col = ivec2(gl_GlobalInvocationID.x, gl_GlobalInvocationID.y + pc_rowOffset);
    const int angularRes = int(pc_vol_angularRes), radialRes = int(pc_vol_radialRes);
    if (col.x >= angularRes || col.y >= radialRes)
        return;
    const uint amountBits = imageLoad(u_amount, col).r;
    bool massNear = amountBits != 0u;
    for (int k = 0; k < 9 && !massNear; ++k)
    {
        const ivec2 n = ivec2(tvWrapAngle(col.x + k % 3 - 1, angularRes), col.y + k / 3 - 1);
        if (n.y >= 0 && n.y < radialRes)
            massNear = imageLoad(u_amount, n).r != 0u;
    }
    if (!massNear)
        return;
    uint floorBits = imageLoad(u_floor, col).r;
    if (floorBits == 0u)
    {
        const vec2 xz = tvTexelWorldXZ(col, TV_PUSH_VOL);
        float ground;
        if (!treeRecordGround(pc_records, pc_map, pc_mapSize, pc_chunkSize, xz, ground))
            ground = terrainHeightAt(xz);
        floorBits = tvFloorEncode(ground);
        imageStore(u_floor, col, uvec4(floorBits));
    }
    if (amountBits == 0u)
        return;

    uint type = imageLoad(u_type, col).r;
    for (int k = 0; k < 9 && !validType(type); ++k)
    {
        const ivec2 n = ivec2(tvWrapAngle(col.x + k % 3 - 1, angularRes), col.y + k / 3 - 1);
        if (n.y >= 0 && n.y < radialRes)
            type = imageLoad(u_type, n).r;
    }
    for (uint t = 0u; t < pc_numTypes && !validType(type); ++t)
        type = t;
    if (!validType(type))
        return;

    const float amount = float(amountBits) / TV_AMOUNT_SCALE;
    const float height = pc_types.t[type].height;
    const float binH = height / float(PROFILE_BINS);
    const float sliceH = pc_vol_height / float(pc_vol_slices);
    for (uint s = 0u; s < pc_vol_slices; ++s)
    {
        // The profile's mean over the slice [s0, s1].
        const float s0 = float(s) * sliceH, s1 = s0 + sliceH;
        if (s0 >= height)
            break;
        float sum = 0.0;
        for (uint b = uint(s0 / binH); b < PROFILE_BINS && float(b) * binH < s1; ++b)
        {
            const float o = min(s1, float(b + 1u) * binH) - max(s0, float(b) * binH);
            sum += pc_types.t[type].shape[b] * max(o, 0.0);
        }
        const float extinction = amount * sum / sliceH;
        if (extinction > 1e-4)
            imageAtomicAdd(u_accum, tvAccumTexel(col, s), tvAccumAmount(extinction, s));
    }
}
