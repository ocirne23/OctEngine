#version 460

// THE FAR VOXEL TREES (TreeVoxelFarPipeline, "Trees/Voxel far trees"): the world tree records of the chunks OUTSIDE the
// near set (the CPU lists them: resident, within the ring, not expanded by Procedural TreeSystem) expanded on the GPU as
// the far volume's splat does (variant, scale and yaw from the record's seed, the chunk's ground grid - Procedural
// TreeSystem::expandChunk's placeVariant, keep in step), each kept tree drawn as its variant's VOXEL box
// (instanced_indirect_tree_voxel.fs.glsl through tree_voxel_far.vs.glsl). No bushes (a few metres tall, far out).
// Three passes (FAR_PASS):
//   0 CULL    - one workgroup per listed chunk: per record its tree, distance (to "far end") + frustum test, appended to a
//               flat array with its bucket (its voxel mesh) and its index in the bucket.
//   1 PREFIX  - one workgroup: the buckets' first instances and one VkDrawIndexedIndirectCommand per bucket.
//   2 SCATTER - one thread per appended tree: into its bucket's range (the draws' instance-rate records).
// Every buffer comes in as a device address; the UBO (camera, frustum, wind) at binding 0.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

#include "shared.inc.glsl"
#define TREE_RECORD_GROUND
#include "tree_record.inc.glsl"

layout (local_size_x = 64) in;

const uint FAR_MAX_BUCKETS = 64u; // TreeVoxelFarPipeline::MAX_BUCKETS - keep in step

// Mirrors TreeRecordTypeGpu (RendererVK TreeRecords.ixx).
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
// Mirrors TreeVoxelFarPipeline's ChunkGpu / FarTypeGpu / BucketGpu / InstanceGpu.
struct FarChunk { ivec2 coord; uint first; uint count; };
struct FarType
{
    uint bucket;       // its voxel mesh's bucket (0xFFFFFFFF: the set type has no voxel tier)
    uint materialMesh; // meshIdx | materialIdx << 16
    float wind;        // x the trunk bend
    float centreY;     // the box's centre height at scale 1 (m)
    float radius;      // the box's half diagonal at scale 1 (m)
    uint shadowBucket; // FAR_SHADOW: its billboard mesh's bucket (0xFFFFFFFF: casts nothing)
    uint shadowTex;    // FAR_SHADOW: the billboard's alpha-mask texture
    float pad0;
};
struct FarBucket { uint indexCount; uint firstIndex; int vertexOffset; uint pad; };
// bucketLocal = bucket << 26 | its index. FAR_SHADOW: materialMesh = the alpha-mask texture | the cascade mask << 16.
struct FarInstance { vec4 posScale; float yaw; uint materialMesh; float wind; uint bucketLocal; };

layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TypeList { RecordType t[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ChunkList { FarChunk c[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer FarTypeList { FarType t[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer BucketList { FarBucket b[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) buffer InstanceList { FarInstance i[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) buffer UintList { uint v[]; }; // [0] count, [1 + b] bucket counts, [1 + 64 + b] bases

layout (push_constant, scalar) uniform Push
{
    TreeRecordWords records; // the pool (its grounds too)
    TreeRecordMap map;
    TypeList types;
    ChunkList chunks;
    FarTypeList farTypes;
    BucketList buckets;
    InstanceList appended;   // the cull's output, unsorted
    UintList state;
    UintList commands;       // VkDrawIndexedIndirectCommand per bucket (5 uints)
    InstanceList sorted;
    uint numChunks;
    uint numTypes;           // the record types
    uint numFarTypes;        // the set types
    uint numBuckets;
    uint worldSeed;
    uint mapSize;
    float chunkSize;
    float endDistance;       // m: no tree past it (the far volume's hand-over, or "Voxel far end")
    uint maxInstances;
    uint pad;
} pc;

bool farInFrustum(vec3 centre, float radius)
{
    for (uint i = 0u; i < 6u; ++i)
        if (dot(vec4(centre, 1.0), u_frustumPlanes[i]) + radius < 0.0)
            return false;
    return true;
}

// FAR_SHADOW: the cascades the caster's sphere overlaps, kept only within each cascade's split + "Foliage shadow cascade
// margin" from the scene focus - instanced_indirect_shadow.cs.glsl's cascadeOverlapMask + its tree rule, keep in step.
uint farCascadeMask(vec3 centre, float radius)
{
    uint mask = 0u;
    const float reach = distance(centre, u_sceneFocus.xyz) - radius - u_present_treeShadowMargin;
    for (uint c = 0u; c < NUM_SHADOW_CASCADES; ++c)
    {
        if (reach > u_cascadeViewProj[c][0][3])
            continue;
        bool inside = true;
        for (uint p = 0u; p < 6u && inside; ++p)
        {
            const vec4 pl = u_cascadePlanes[c * 6u + p];
            inside = dot(pl.xyz, centre) + pl.w >= -radius;
        }
        if (inside)
            mask |= 1u << c;
    }
    return mask;
}

void main()
{
#if FAR_PASS == 0
    if (gl_WorkGroupID.x >= pc.numChunks)
        return;
    const FarChunk chunk = pc.chunks.c[gl_WorkGroupID.x];
    const vec3 eye = u_views_viewPos[VIEW_CENTER].xyz;
    const float windReach = u_foliage_windReach;
    for (uint i = gl_LocalInvocationID.x; i < chunk.count; i += 64u)
    {
        const uint record = pc.records.w[chunk.first + i];
        const uint type = treeRecordType(record);
        if (type >= pc.numTypes)
            continue;
        const uint numVariants = pc.types.t[type].numVariants;
        if (numVariants == 0u)
            continue;
        const uint seed = treeRecordSeed(pc.worldSeed, chunk.coord, record);
        const uint variant = treeHash(seed, 102u) % numVariants;
        const uint setType = pc.types.t[type].variantType[variant];
        if (setType >= pc.numFarTypes)
            continue;
        const FarType farType = pc.farTypes.t[setType];
#ifdef FAR_SHADOW
        const uint bucket = farType.shadowBucket;
#else
        const uint bucket = farType.bucket;
#endif
        if (bucket >= pc.numBuckets)
            continue; // no voxel tier / no billboard (a rock, a bush)

        const vec2 range = pc.types.t[type].scale;
        const float scale = mix(range.x, range.y, treeHash01(treeHash(seed, 103u)))
            * exp2(pc.types.t[type].sizeVariation * (treeHash01(treeHash(seed, 105u)) * 2.0 - 1.0));
        const float yaw = treeHash01(treeHash(seed, 104u)) * 6.2831853;
        const vec2 p = vec2(chunk.coord) * pc.chunkSize + treeRecordLocal(record, pc.chunkSize);
        float ground;
        if (!treeRecordGround(pc.records, pc.map, pc.mapSize, pc.chunkSize, p, ground))
            continue;
        const vec3 base = vec3(p.x, ground - 0.05, p.y);

        const vec3 centre = base + vec3(0.0, farType.centreY * scale, 0.0);
        const float radius = farType.radius * scale + windReach * max(farType.wind, 1.0);
#ifdef FAR_SHADOW
        // The billboard casts shifted toward the sun ("Billboard shadow shift"): its sphere grows by the shift.
        const uint cascades = farCascadeMask(centre, radius + abs(u_foliage_billboardShadowShift));
        if (cascades == 0u)
            continue;
        const uint data = farType.shadowTex | cascades << 16u;
#else
        if (distance(centre, eye) > pc.endDistance || !farInFrustum(centre, radius))
            continue;
        const uint data = farType.materialMesh;
#endif

        const uint idx = atomicAdd(pc.state.v[0], 1u);
        if (idx >= pc.maxInstances)
            continue;
        const uint local = atomicAdd(pc.state.v[1u + bucket], 1u);
        pc.appended.i[idx] = FarInstance(vec4(base, scale), yaw, data, farType.wind, bucket << 26u | local);
    }
#elif FAR_PASS == 1
    if (gl_LocalInvocationID.x != 0u)
        return;
    uint base = 0u;
    for (uint b = 0u; b < FAR_MAX_BUCKETS; ++b)
    {
        const uint count = b < pc.numBuckets ? pc.state.v[1u + b] : 0u;
        pc.state.v[1u + FAR_MAX_BUCKETS + b] = base;
        const FarBucket bucket = b < pc.numBuckets ? pc.buckets.b[b] : FarBucket(0u, 0u, 0, 0u);
        pc.commands.v[b * 5u + 0u] = bucket.indexCount;
        pc.commands.v[b * 5u + 1u] = count;
        pc.commands.v[b * 5u + 2u] = bucket.firstIndex;
        pc.commands.v[b * 5u + 3u] = uint(bucket.vertexOffset);
        pc.commands.v[b * 5u + 4u] = base;
        base += count;
    }
#else
    const uint idx = gl_GlobalInvocationID.x;
    if (idx >= min(pc.state.v[0], pc.maxInstances))
        return;
    const FarInstance instance = pc.appended.i[idx];
    pc.sorted.i[pc.state.v[1u + FAR_MAX_BUCKETS + (instance.bucketLocal >> 26u)] + (instance.bucketLocal & 0x3FFFFFFu)] = instance;
#endif
}
