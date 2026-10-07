#version 460

// FAR-TREE VOLUME, after the hand-over (TreeVolumePipeline): the new bake's extinction (two half floats per texel in the
// accumulation, tree_volume_resolve.cs) -> the R16F density the march samples, a range of texel layers (two slices each)
// per dispatch - spread over frames: meanwhile every ray shows the new bake through the accumulation, so nothing reads
// the density.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r32ui) uniform readonly uimage3D u_accum;
layout (binding = 1, r16f) uniform writeonly image3D u_density;

layout (push_constant) uniform Push
{
    uint sliceOffset; // this dispatch's first texel layer of the accumulation
} pc;

void main()
{
    const ivec3 p = ivec3(gl_GlobalInvocationID.xy, gl_GlobalInvocationID.z + pc.sliceOffset);
    if (any(greaterThanEqual(p, imageSize(u_accum))))
        return;
    const vec2 slices = unpackHalf2x16(imageLoad(u_accum, p).r); // x = the even slice, y = the odd one
    const int s = p.z * 2;
    imageStore(u_density, ivec3(p.xy, s), vec4(slices.x));
    if (s + 1 < imageSize(u_density).z) // an odd slice count: the last layer's high half is unused
        imageStore(u_density, ivec3(p.xy, s + 1), vec4(slices.y));
}
