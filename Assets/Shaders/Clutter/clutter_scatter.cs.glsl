#version 460

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE CLUTTER RECORDS INTO THEIR BUCKETS (ClutterPipeline, after clutter_prefix): a thread per kept record, to its
// bucket's start + its bucket-local index (both from the cull's key). The draws read the sorted buffer.

#include "clutter.inc.glsl"

layout (local_size_x = CLUTTER_SCATTER_GROUP) in;

layout (binding = 0, std430) readonly buffer InCounts { uint in_counts[4]; };
layout (binding = 1, std430) readonly buffer InKeys { uint in_keys[]; };
layout (binding = 2, std430) readonly buffer InFlat { ClutterInstance in_flat[]; };
layout (binding = 3, std430) readonly buffer InBucketBase { uint in_bucketBase[]; };
layout (binding = 4, std430) writeonly buffer OutSorted { ClutterInstance out_sorted[]; };

void main()
{
    const uint i = gl_GlobalInvocationID.x;
    if (i >= min(in_counts[0], CLUTTER_MAX_INSTANCES))
        return;
    const uint key = in_keys[i];
    out_sorted[in_bucketBase[key >> CLUTTER_LOCAL_BITS] + (key & ((1u << CLUTTER_LOCAL_BITS) - 1u))] = in_flat[i];
}
