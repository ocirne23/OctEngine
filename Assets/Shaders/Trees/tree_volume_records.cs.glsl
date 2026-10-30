#version 460

// FAR-TREE VOLUME BAKE from the WORLD TREE RECORDS, far out (TreeVolumePipeline): one workgroup per record chunk BEYOND the
// record detail distance of the bake centre (the ones within it splat in detail: tree_volume_splat.cs's TREE_SPLAT_RECORDS,
// the same test). Every record adds its tree's MASS (its extinction integrated over its volume, m^2: its variant's at scale
// 1 x its scale^2, both from its seed as Procedural TreeSystem::expandChunk picks them) / the column's area into a 2D image,
// fixed point, over a TENT as wide as its crown (at least a cell each way: a point splat made thin pillars where a column is
// smaller than a crown). The weights are normalized per axis, so a tree adds its whole mass. Its nearest column also takes
// its TYPE and COLOUR in one atomic min on the colour map's R32UI view (tree_volume.inc.glsl tvColourType: the type in the
// alpha byte): the lowest type wins - a tree's before a rock's - with its own colour, the same every bake. tree_volume_far.cs
// spreads the mass with that type's height profile; the resolve sets the alpha back. No bushes (a few metres tall, kilometres out).
// albedo.w < 0.75 = a SOLID: its mass is its occupied volume x scale^3 x "Far rock extinction". albedo.w < 0.25 = a ROCK: no
// colour, its share goes into the column's ROCK SUM in the accumulation's units (mass / area / slice height: what
// tree_volume_far.cs adds, summed over the slices). 0.5 = dead wood: a solid's mass, a tree's colour.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

#include "tree_volume.inc.glsl"
#include "tree_record.inc.glsl"

layout (local_size_x = 64) in;

layout (binding = 2, r32ui) uniform uimage2D u_amount;
layout (binding = 3, r32ui) uniform uimage2D u_colourType; // the back colour map's R32UI view (the type in the alpha byte)
layout (binding = 5, r32ui) uniform uimage2D u_rockSum;

// Mirrors TreeRecordChunkGpu / TreeRecordTypeGpu (RendererVK TreeRecordPool.ixx).
struct RecordChunk
{
    ivec2 coord;
    uint first;
    uint count;
};

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

layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer RecordList { uint r[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ChunkList { RecordChunk c[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TypeList { RecordType t[]; };

// The push block: TreeVolumePipeline's RecordsPush (pc_vol_*, pc_recordDetail, pc_rockExtinction: the bake's lockable values).
#include "push.generated.glsl"

const int MAX_TENT = 8; // texels each way: the crown's footprint is capped there (cost)

void main()
{
    const uint local = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
    if (local >= pc_numChunks)
        return;
    const RecordChunk chunk = pc_chunks.c[local + pc_chunkOffset];
    const vec2 origin = vec2(chunk.coord) * pc_chunkSize;
    const float d = length(origin + 0.5 * pc_chunkSize - pc_vol_centre);
    if (d < pc_recordDetail)
        return; // splatted in detail (tree_volume_splat.cs)
    const float reach = 0.7072 * pc_chunkSize; // the chunk's half diagonal
    if (d + reach < pc_vol_rMin || d - reach > pc_vol_rMax)
        return;
    const int angularRes = int(pc_vol_angularRes), radialRes = int(pc_vol_radialRes);
    const float invSliceH = float(pc_vol_slices) / pc_vol_height;

    for (uint i = gl_LocalInvocationID.x; i < chunk.count; i += 64u)
    {
        const uint record = pc_records.r[chunk.first + i];
        const uint type = treeRecordType(record);
        if (type >= pc_numTypes)
            continue;
        const uint numVariants = pc_types.t[type].numVariants;
        if (numVariants == 0u)
            continue;

        // The record's own variant and scale (expandChunk's placeVariant - keep in step).
        const uint seed = treeRecordSeed(pc_worldSeed, chunk.coord, record);
        const uint variant = treeHash(seed, 102u) % numVariants;
        const vec2 range = pc_types.t[type].scale;
        const float scale = mix(range.x, range.y, treeHash01(treeHash(seed, 103u)))
            * exp2(pc_types.t[type].sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0 - 1.0));
        const bool solid = pc_types.t[type].albedo.w < 0.75;
        const bool bedrock = pc_types.t[type].albedo.w < 0.25; // a rock: the resolve's climate colour
        const float mass = pc_types.t[type].variantMass[variant] * scale * scale * (solid ? scale * pc_rockExtinction : 1.0);
        if (mass <= 0.0)
            continue;

        const vec2 rel = origin + treeRecordLocal(record, pc_chunkSize) - pc_vol_centre;
        const float r = length(rel);
        if (r < pc_vol_rMin || r > pc_vol_rMax)
            continue;

        // Continuous texel coordinates (texel centres at integers) and the tent's half-width per axis, in texels.
        const vec2 tc = vec2((atan(rel.y, rel.x) * (1.0 / TV_TWO_PI) + 0.5) * float(angularRes), tvRadialUv(r, TV_PUSH_VOL) * float(radialRes)) - 0.5;
        const float radius = pc_types.t[type].radius * scale;
        const vec2 h = clamp(vec2(radius / tvTangentialCell(r, TV_PUSH_VOL), radius / tvRadialCell(r, TV_PUSH_VOL)), vec2(1.0), vec2(float(MAX_TENT)));
        const vec2 invH = 1.0 / h;
        const ivec2 lo = ivec2(ceil(tc - h)), hi = ivec2(floor(tc + h));

        // The per-axis sums (each tent's samples normalized to 1).
        float sumA = 0.0, sumR = 0.0;
        for (int x = lo.x; x <= hi.x; ++x)
            sumA += max(1.0 - abs(float(x) - tc.x) * invH.x, 0.0);
        for (int y = lo.y; y <= hi.y; ++y)
            sumR += max(1.0 - abs(float(y) - tc.y) * invH.y, 0.0);
        if (sumA <= 0.0 || sumR <= 0.0)
            continue;

        const float massPerSum = mass / (sumA * sumR);
        for (int y = max(lo.y, 0); y <= min(hi.y, radialRes - 1); ++y)
        {
            const float wR = max(1.0 - abs(float(y) - tc.y) * invH.y, 0.0); // unnormalized: massPerSum holds the 1 / sums
            if (wR <= 0.0)
                continue;
            // The column's area at its centre radius.
            const float rc = tvRadius(float(y) + 0.5, TV_PUSH_VOL);
            const float rowAmount = wR * massPerSum / (tvRadialCell(rc, TV_PUSH_VOL) * tvTangentialCell(rc, TV_PUSH_VOL));
            for (int x = lo.x; x <= hi.x; ++x)
            {
                const float a = max(1.0 - abs(float(x) - tc.x) * invH.x, 0.0) * rowAmount; // mass / area
                const uint amount = uint(a * TV_AMOUNT_SCALE + 0.5);
                if (amount == 0u)
                    continue;
                const ivec2 col = ivec2(tvWrapAngle(x, angularRes), y);
                imageAtomicAdd(u_amount, col, amount);
                if (bedrock)
                    imageAtomicAdd(u_rockSum, col, uint(a * invSliceH * TV_ROCK_SCALE + 0.5));
            }
        }

        const ivec2 nearest = ivec2(tvWrapAngle(int(round(tc.x)), angularRes), clamp(int(round(tc.y)), 0, radialRes - 1));
        // A rock writes no colour of its own: the clear colour's (the resolve mixes the climate's bedrock in).
        imageAtomicMin(u_colourType, nearest, (type << 24) | (bedrock ? TV_CLEAR_RGB : packUnorm4x8(vec4(pc_types.t[type].albedo.rgb, 0.0))));
    }
}
