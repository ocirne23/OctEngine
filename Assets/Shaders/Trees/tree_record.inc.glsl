// WORLD TREE RECORDS (Procedural TreeWorld, Docs/TreeRenderingPlan.md 3.5): 4 bytes per tree - x, z (12 bits each,
// chunk-local on a TREE_RECORD_STEPS lattice, the cell centre) and the species type (8 bits). Variant, scale and yaw
// come from treeRecordSeed. MIRRORS Code/Procedural/Private/TreeWorld.ixx and treeHash (TreeGenerator.ixx) - keep
// them in step. Read by tree_volume_records.cs (the far volume's records, W4).

#ifndef TREE_RECORD_INC_GLSL
#define TREE_RECORD_INC_GLSL

#define TREE_RECORD_STEPS 4096u

// PCG hash (TreeGenerator.ixx treeHash).
uint treeHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint treeHash(uint a, uint b) { return treeHash(a ^ treeHash(b)); }
float treeHash01(uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }

uint treeRecordType(uint record) { return record >> 24u; }

// The chunk-local position (m) in a chunk of `chunkSize` metres.
vec2 treeRecordLocal(uint record, float chunkSize)
{
    const float s = chunkSize / float(TREE_RECORD_STEPS);
    return (vec2(float(record & 0xFFFu), float((record >> 12u) & 0xFFFu)) + 0.5) * s;
}

// The record's random stream: keyed by the chunk and the QUANTIZED position.
uint treeRecordSeed(uint worldSeed, ivec2 chunk, uint record)
{
    return treeHash(treeHash(worldSeed, uint(chunk.x)), treeHash(uint(chunk.y), record & 0xFFFFFFu));
}

#ifdef TREE_RECORD_GROUND
// THE GROUND the records' trees stand on (they carry no height): each chunk's TREE_RECORD_HEIGHT_RES^2 heights, corners
// included, right before its records in the pool, found through the CHUNK MAP. MIRRORS RendererVK TreeRecordPool.ixx
// (TreeRecordMapGpu, the height encoding). The includer enables GL_EXT_buffer_reference and GL_EXT_scalar_block_layout.
#define TREE_RECORD_HEIGHT_RES 17u

struct TreeRecordMapEntry
{
    ivec2 coord;
    uint ground; // the pool word of the chunk's ground
};
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TreeRecordWords { uint w[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TreeRecordMap { TreeRecordMapEntry e[]; };

float treeRecordGridHeight(TreeRecordWords words, uint base, uvec2 g, float minH, float step)
{
    const uint k = g.x + g.y * TREE_RECORD_HEIGHT_RES;
    const uint word = words.w[base + 2u + k / 2u];
    return minH + float((k & 1u) != 0u ? word >> 16u : word & 0xFFFFu) * step;
}

// The ground (world Y) at xz: bilinear in its chunk's grid. False when that chunk holds no ground (not resident): the
// caller falls back to the terrain map.
bool treeRecordGround(TreeRecordWords words, TreeRecordMap map, uint mapSize, float chunkSize, vec2 xz, out float height)
{
    height = 0.0;
    if (mapSize == 0u)
        return false;
    const ivec2 chunk = ivec2(floor(xz / chunkSize));
    const TreeRecordMapEntry entry = map.e[(uint(chunk.x) & (mapSize - 1u)) + (uint(chunk.y) & (mapSize - 1u)) * mapSize];
    if (entry.coord != chunk)
        return false;
    const float last = float(TREE_RECORD_HEIGHT_RES - 1u);
    const vec2 g = clamp((xz - vec2(chunk) * chunkSize) / chunkSize * last, vec2(0.0), vec2(last - 1e-3));
    const uvec2 i0 = uvec2(g);
    const vec2 f = g - vec2(i0);
    const float minH = uintBitsToFloat(words.w[entry.ground]);
    const float step = uintBitsToFloat(words.w[entry.ground + 1u]);
    const float h00 = treeRecordGridHeight(words, entry.ground, i0, minH, step);
    const float h10 = treeRecordGridHeight(words, entry.ground, i0 + uvec2(1u, 0u), minH, step);
    const float h01 = treeRecordGridHeight(words, entry.ground, i0 + uvec2(0u, 1u), minH, step);
    const float h11 = treeRecordGridHeight(words, entry.ground, i0 + uvec2(1u, 1u), minH, step);
    height = mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
    return true;
}
#endif

#endif
