#version 460

// FAR-TREE VOLUME BAKE, the first step (TreeVolumePipeline): zero the accumulation, a range of slices per dispatch - the
// bake spreads it over frames (vkCmdClearColorImage clears a 3D image whole, in one frame).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r32ui) uniform writeonly uimage3D u_accum;

layout (push_constant) uniform Push
{
    uint sliceOffset; // this dispatch's first slice
} pc;

void main()
{
    const ivec3 p = ivec3(gl_GlobalInvocationID.xy, gl_GlobalInvocationID.z + pc.sliceOffset);
    if (any(greaterThanEqual(p, imageSize(u_accum))))
        return;
    imageStore(u_accum, p, uvec4(0u));
}
