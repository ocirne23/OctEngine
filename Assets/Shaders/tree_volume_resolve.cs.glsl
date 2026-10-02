#version 460

// FAR-TREE VOLUME BAKE, step 2 of 2 (TreeVolumePipeline): the splat's fixed-point sums -> the filterable R16F
// extinction volume the march samples.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r32ui) uniform readonly uimage3D u_accum;
layout (binding = 1, r16f) uniform writeonly image3D u_density;

const float ACCUM_SCALE = 1024.0; // tree_volume_splat.cs.glsl

void main()
{
    const ivec3 p = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(p, imageSize(u_density))))
        return;
    imageStore(u_density, p, vec4(float(imageLoad(u_accum, p).r) / ACCUM_SCALE));
}
