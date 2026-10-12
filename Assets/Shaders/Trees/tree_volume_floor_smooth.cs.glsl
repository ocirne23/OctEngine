#version 460

// FAR-TREE VOLUME BAKE, between the floor passes and the splat (TreeVolumePipeline): the tree FLOOR blurred, one AXIS per
// dispatch (angular, then radial: a separable tent of "Far floor smoothing" columns each way). The floor is the base of each
// column's dominant tree, so on a slope neighbouring columns differ by whole floor steps, and everything is stored and read
// relative to it: the steps showed as faint vertical lines and a moire on slopes.
// Masked: a column WITHOUT a floor (0) stays without one and adds nothing to its neighbours' averages. A tree that no longer
// fits the layer is moved down into it by the splat (its `shift`).

#include "tree_volume.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r32ui) uniform readonly uimage2D u_src;
layout (binding = 1, r32ui) uniform writeonly uimage2D u_dst;

// The push block: TreeVolumePipeline's FloorSmoothPush (pc_vol_angularRes / radialRes, pc_floorSmoothing: the bake's
// lockable values).
#include "push.generated.glsl"

void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const int angularRes = int(pc_vol_angularRes), radialRes = int(pc_vol_radialRes);
    if (p.x >= angularRes || p.y >= radialRes)
        return;

    const uint centre = imageLoad(u_src, p).r;
    if (centre == 0u)
    {
        imageStore(u_dst, p, uvec4(0u));
        return;
    }

    // Relative to the centre column's floor: the sum stays small whatever the absolute height.
    const float base = tvFloorDecode(centre);
    float sum = 0.0, weight = 0.0;
    for (int k = -pc_floorSmoothing; k <= pc_floorSmoothing; ++k)
    {
        const ivec2 q = pc_radialAxis != 0u ? ivec2(p.x, p.y + k) : ivec2(tvWrapAngle(p.x + k, angularRes), p.y);
        if (q.y < 0 || q.y >= radialRes)
            continue;
        const uint bits = imageLoad(u_src, q).r;
        if (bits == 0u)
            continue;
        const float w = float(pc_floorSmoothing + 1 - abs(k));
        sum += w * (tvFloorDecode(bits) - base);
        weight += w;
    }

    imageStore(u_dst, p, uvec4(tvFloorEncode(base + sum / weight)));
}
