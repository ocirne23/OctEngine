#version 460

// FAR-TREE VOLUME BAKE, between the floor passes and the splat (TreeVolumePipeline): the tree FLOOR blurred, one
// AXIS per dispatch (angular, then radial - a separable tent of "Far floor smoothing" columns each way).
//
// The floor is the base of each column's dominant tree, so on a slope neighbouring columns differ by whole floor
// steps. Everything is stored and read relative to it: the march's bilinear primary sample blends 4 columns at 4
// floors and its lighting taps a smooth blend of them, and either way the steps showed as faint vertical lines and a
// moire on slopes (2026-10-03). A smooth floor keeps the splat and the march in one continuous frame.
//
// Masked: a column WITHOUT a floor (0) stays without one (the march skips it, the splat writes nothing there) and
// adds nothing to its neighbours' averages. The splat moves a tree that no longer fits the layer down into it (its
// `shift`), as for any floor below the tree.

#include "tree_volume.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0, r32ui) uniform readonly uimage2D u_src;
layout (binding = 1, r32ui) uniform writeonly uimage2D u_dst;

layout (push_constant) uniform Push
{
    uint angularRes;
    uint radialRes;
    int radius;     // columns each way (> 0)
    uint radialAxis; // 0 = along the angle (wraps), 1 = along the radius (clamps)
} pc;

void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const int angularRes = int(pc.angularRes), radialRes = int(pc.radialRes);
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
    for (int k = -pc.radius; k <= pc.radius; ++k)
    {
        const ivec2 q = pc.radialAxis != 0u ? ivec2(p.x, p.y + k) : ivec2(tvWrapAngle(p.x + k, angularRes), p.y);
        if (q.y < 0 || q.y >= radialRes)
            continue;
        const uint bits = imageLoad(u_src, q).r;
        if (bits == 0u)
            continue;
        const float w = float(pc.radius + 1 - abs(k));
        sum += w * (tvFloorDecode(bits) - base);
        weight += w;
    }
    imageStore(u_dst, p, uvec4(tvFloorEncode(base + sum / weight)));
}
