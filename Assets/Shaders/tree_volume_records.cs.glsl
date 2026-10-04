#version 460

// FAR-TREE VOLUME BAKE from the WORLD TREE RECORDS, far out (TreeVolumePipeline, W4; Procedural TreeWorld): one
// workgroup per record chunk BEYOND the record detail distance of the bake centre (the ones within it expand and splat
// in detail - tree_volume_splat.cs's TREE_SPLAT_RECORDS, the same test). Every record adds its tree's MASS (its
// extinction integrated over its volume, m^2: its exact variant's at scale 1 x its exact scale^2 - both from its seed,
// as Procedural TreeSystem::expandChunk picks them) / the column's area into a 2D image per column - fixed point,
// summed with atomics - over a TENT as wide as its crown (at least a cell each way: far out a tree is smaller than a
// column; near the detail distance a column can be smaller than a crown, and a point splat made thin pillars). The
// weights are normalized per axis, so a tree adds its whole mass. Its nearest column also takes its type (whose height
// profile tree_volume_far.cs spreads the mass with) and its colour - last writer wins, as the splat's colour. Bushes
// are not in it (a few metres tall, kilometres out).

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

#include "tree_volume.inc.glsl"
#include "tree_record.inc.glsl"

layout (local_size_x = 64) in;

layout (binding = 2, r32ui) uniform uimage2D u_amount;
layout (binding = 3, r32ui) uniform writeonly uimage2D u_type;
layout (binding = 4, rgba8) uniform writeonly image2D u_colour;

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

layout (push_constant, scalar) uniform Push
{
    RecordList records;
    ChunkList chunks;
    TypeList types;
    uint numChunks;
    uint numTypes;
    float chunkSize;
    uint worldSeed;
    TreeVolumeParams vol;
    float recordDetail; // m: the chunks whose centre lies within this of the bake centre splat in detail instead
} pc;

const int MAX_TENT = 8; // texels each way: the crown's footprint is capped there (cost)

void main()
{
    const uint chunkIdx = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
    if (chunkIdx >= pc.numChunks)
        return;
    const RecordChunk chunk = pc.chunks.c[chunkIdx];
    const vec2 origin = vec2(chunk.coord) * pc.chunkSize;
    const float d = length(origin + 0.5 * pc.chunkSize - pc.vol.centre);
    if (d < pc.recordDetail)
        return; // splatted in detail (tree_volume_splat.cs)
    const float reach = 0.7072 * pc.chunkSize; // the chunk's half diagonal
    if (d + reach < pc.vol.rMin || d - reach > pc.vol.rMax)
        return;
    const int angularRes = int(pc.vol.angularRes), radialRes = int(pc.vol.radialRes);

    for (uint i = gl_LocalInvocationID.x; i < chunk.count; i += 64u)
    {
        const uint record = pc.records.r[chunk.first + i];
        const uint type = treeRecordType(record);
        if (type >= pc.numTypes)
            continue;
        const uint numVariants = pc.types.t[type].numVariants;
        if (numVariants == 0u)
            continue;
        // The record's own variant and scale (expandChunk's placeVariant - keep in step).
        const uint seed = treeRecordSeed(pc.worldSeed, chunk.coord, record);
        const uint variant = treeHash(seed, 102u) % numVariants;
        const vec2 range = pc.types.t[type].scale;
        const float scale = mix(range.x, range.y, treeHash01(treeHash(seed, 103u)))
            * exp2(pc.types.t[type].sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0 - 1.0));
        const float mass = pc.types.t[type].variantMass[variant] * scale * scale;
        if (mass <= 0.0)
            continue;
        const vec2 rel = origin + treeRecordLocal(record, pc.chunkSize) - pc.vol.centre;
        const float r = length(rel);
        if (r < pc.vol.rMin || r > pc.vol.rMax)
            continue;
        // Continuous texel coordinates (texel centres at integers) and the tent's half-width per axis, in texels.
        const vec2 tc = vec2((atan(rel.y, rel.x) / TV_TWO_PI + 0.5) * float(angularRes), tvRadialUv(r, pc.vol) * float(radialRes)) - 0.5;
        const float radius = pc.types.t[type].radius * scale;
        const vec2 h = clamp(vec2(radius / tvTangentialCell(r, pc.vol), radius / tvRadialCell(r, pc.vol)), vec2(1.0), vec2(float(MAX_TENT)));
        const ivec2 lo = ivec2(ceil(tc - h)), hi = ivec2(floor(tc + h));
        // The per-axis sums (each tent's samples normalized to 1).
        float sumA = 0.0, sumR = 0.0;
        for (int x = lo.x; x <= hi.x; ++x)
            sumA += max(1.0 - abs(float(x) - tc.x) / h.x, 0.0);
        for (int y = lo.y; y <= hi.y; ++y)
            sumR += max(1.0 - abs(float(y) - tc.y) / h.y, 0.0);
        if (sumA <= 0.0 || sumR <= 0.0)
            continue;
        for (int y = max(lo.y, 0); y <= min(hi.y, radialRes - 1); ++y)
        {
            const float wR = max(1.0 - abs(float(y) - tc.y) / h.y, 0.0) / sumR;
            if (wR <= 0.0)
                continue;
            // The column's area at its centre radius.
            const float rc = tvRadius(float(y) + 0.5, pc.vol);
            const float perArea = mass / (tvRadialCell(rc, pc.vol) * tvTangentialCell(rc, pc.vol));
            for (int x = lo.x; x <= hi.x; ++x)
            {
                const float w = wR * max(1.0 - abs(float(x) - tc.x) / h.x, 0.0) / sumA;
                const uint amount = uint(w * perArea * TV_AMOUNT_SCALE + 0.5);
                if (amount != 0u)
                    imageAtomicAdd(u_amount, ivec2(tvWrapAngle(x, angularRes), y), amount);
            }
        }
        const ivec2 nearest = ivec2(tvWrapAngle(int(round(tc.x)), angularRes), clamp(int(round(tc.y)), 0, radialRes - 1));
        imageStore(u_type, nearest, uvec4(type));
        imageStore(u_colour, nearest, vec4(pc.types.t[type].albedo.rgb, 1.0));
    }
}
