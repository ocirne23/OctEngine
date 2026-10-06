#version 460

// FAR-TREE VOLUME, after the hand-over (TreeVolumePipeline): the new bake's extinction (float bits in the accumulation,
// tree_volume_resolve.cs) -> the R16F density the march samples, a range of slices per dispatch - spread over frames:
// meanwhile every chunk shows the new bake through the accumulation, so nothing reads the density.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r32ui) uniform readonly uimage3D u_accum;
layout (binding = 1, r16f) uniform writeonly image3D u_density;

layout (push_constant) uniform Push
{
    uint sliceOffset; // this dispatch's first slice
} pc;

void main()
{
    const ivec3 p = ivec3(gl_GlobalInvocationID.xy, gl_GlobalInvocationID.z + pc.sliceOffset);
    if (any(greaterThanEqual(p, imageSize(u_accum))))
        return;
    imageStore(u_density, p, vec4(uintBitsToFloat(imageLoad(u_accum, p).r)));
}
