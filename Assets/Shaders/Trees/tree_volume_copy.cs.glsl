#version 460

// FAR-TREE VOLUME, after the hand-over (TreeVolumePipeline): the new bake's extinction (four sRGB codes per texel in the
// accumulation, tree_volume_resolve.cs) -> the density the march samples (R8: the codes as they are; R16F: decoded -
// tree_volume.inc.glsl), and the bake's R32UI floor -> the shown R16UI one. A range of texel layers per dispatch, spread
// over frames: meanwhile every ray reads the new bake from the accumulation and the bake floor, so nothing reads either.

#include "tree_volume.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r32ui) uniform readonly uimage3D u_accum;
#ifdef TV_DENSITY_R8
layout (binding = 1, r8) uniform writeonly image3D u_density; // the R8_UNORM view (the march samples the R8_SRGB one)
#else
layout (binding = 1, r16f) uniform writeonly image3D u_density;
#endif
layout (binding = 2, r32ui) uniform readonly uimage2D u_floorBake;  // the bake's floor (tree_volume.inc.glsl's encoding)
layout (binding = 3, r16ui) uniform writeonly uimage2D u_floorShown; // -> the march's (tvFloorFrontEncode), with layer 0

layout (push_constant) uniform Push
{
    uint sliceOffset; // this dispatch's first texel layer of the accumulation
} pc;

void main()
{
    const ivec3 p = ivec3(gl_GlobalInvocationID.xy, gl_GlobalInvocationID.z + pc.sliceOffset);
    if (any(greaterThanEqual(p, imageSize(u_accum))))
        return;
    if (p.z == 0) // the 2D floor once, with the first layer (the dispatches walk the layers in order)
        imageStore(u_floorShown, p.xy, uvec4(tvFloorFrontEncode(imageLoad(u_floorBake, p.xy).r)));
    const vec4 codes = unpackUnorm4x8(imageLoad(u_accum, p).r); // channel i = slice i x layers + z (interleaved)
    const int layers = imageSize(u_accum).z;
    const int slices = imageSize(u_density).z; // the unused bytes (a slice count not divisible by 4) are skipped
    for (int i = 0; i < 4 && i * layers + p.z < slices; ++i)
#ifdef TV_DENSITY_R8
        imageStore(u_density, ivec3(p.xy, i * layers + p.z), vec4(codes[i])); // the same code: R8_UNORM stores it back exactly
#else
        imageStore(u_density, ivec3(p.xy, i * layers + p.z), vec4(tvAccumDecode(codes[i])));
#endif
}
