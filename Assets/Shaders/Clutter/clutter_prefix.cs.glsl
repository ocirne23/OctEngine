#version 460

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE CLUTTER BUCKETS' LAYOUT (ClutterPipeline, after clutter_cull): ONE workgroup, a thread per bucket. An exclusive
// prefix sum of the bucket counts gives each bucket its range of the sorted record buffer; each bucket gets its indexed
// draw (instanceCount = its count, firstInstance = its range's start - the records are instance-rate attributes). The
// flowers' buckets come first (their draw is fixed: CLUTTER_FLOWER_LODS commands), then (mesh, LOD) - their draw COUNT
// is written here (io_counts[1]).

#define CLUTTER_FRAME_BINDING 0
#include "clutter.inc.glsl"

layout (local_size_x = CLUTTER_PREFIX_GROUP) in;

layout (binding = 1, std430) readonly buffer InMeshes { ClutterMesh in_meshes[]; };
layout (binding = 2, std430) buffer Counts { uint io_counts[4]; };
layout (binding = 3, std430) readonly buffer InBucketCounts { uint in_bucketCounts[]; };
layout (binding = 4, std430) writeonly buffer OutBucketBase { uint out_bucketBase[]; };
struct DrawIndexedIndirect // VkDrawIndexedIndirectCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout (binding = 5, std430) writeonly buffer OutCommands { DrawIndexedIndirect out_commands[]; };

shared uint s_scan[CLUTTER_PREFIX_GROUP];

void main()
{
    const uint b = gl_LocalInvocationIndex;
    const uint count = b < CLUTTER_MAX_BUCKETS ? in_bucketCounts[b] : 0u;
    s_scan[b] = count;
    barrier();
    for (uint offset = 1u; offset < CLUTTER_PREFIX_GROUP; offset <<= 1u)
    {
        const uint add = b >= offset ? s_scan[b - offset] : 0u;
        barrier();
        s_scan[b] += add;
        barrier();
    }
    if (b >= CLUTTER_MAX_BUCKETS)
        return;
    const uint first = s_scan[b] - count;
    out_bucketBase[b] = first;
    DrawIndexedIndirect draw = DrawIndexedIndirect(0u, 0u, 0u, 0, first);
    if (b < CLUTTER_FLOWER_LODS)
        draw = DrawIndexedIndirect(cf_flowerLods[b].y, count, cf_flowerLods[b].x, 0, first);
    else
    {
        const uint mesh = (b - CLUTTER_FLOWER_LODS) / CLUTTER_LODS;
        const uint lod = (b - CLUTTER_FLOWER_LODS) % CLUTTER_LODS;
        if (mesh < cf_numMeshes)
        {
            const uvec4 range = in_meshes[mesh].lods[lod];
            draw = DrawIndexedIndirect(range.y, range.y > 0u ? count : 0u, range.x, int(range.z), first);
        }
    }
    out_commands[b] = draw;
    if (b == 0u)
        io_counts[1] = cf_numMeshes * CLUTTER_LODS;
}
