#version 460

// THE MAX-FLOOR GRID (TreeVolumePipeline, the bake's last step): the highest tree floor per BLOCK of
// TV_FLOOR_MAX_BLOCK x TV_FLOOR_MAX_BLOCK columns, then
//   x = DILATED by one block each way (the angle wraps): a point higher than it + the volume's height is above every
//       tree within one block in every direction, so the march takes a long step there;
//   y = AHEAD: the highest over this block's row and every row FARTHER OUT, within TV_FLOOR_AHEAD_SECTORS blocks each
//       way - a RISING ray above it + the height is done (tree_volume_march.cs.glsl bounds its angular drift).
// Only floored columns count - a column without a floor holds no density. No floor in the region = TV_FLOOR_MAX_NONE.
// Pass 1 (default): floor -> block max (R32F). Pass 2 (TREE_FLOOR_MAX_DILATE): block max -> the grid (RG32F).

#include "tree_volume.inc.glsl"

layout (local_size_x = 8, local_size_y = 8) in;

#ifdef TREE_FLOOR_MAX_DILATE
layout (binding = 0, r32f) uniform readonly image2D u_src;   // the block max
layout (binding = 1, rg32f) uniform writeonly image2D u_dst;
#else
layout (binding = 0, r32ui) uniform readonly uimage2D u_src; // the floor (tree_volume.inc.glsl's encoding)
layout (binding = 1, r32f) uniform writeonly image2D u_dst;
#endif

layout (push_constant) uniform Push
{
    uvec2 srcSize;
    uvec2 dstSize;
} pc;

void main()
{
    const ivec2 b = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(uvec2(b), pc.dstSize)))
        return;
    float m = TV_FLOOR_MAX_NONE;
#ifdef TREE_FLOOR_MAX_DILATE
    for (int dy = -1; dy <= 1; ++dy)
    {
        const int row = b.y + dy;
        if (row < 0 || row >= int(pc.srcSize.y))
            continue; // past the ring's ends: nothing there
        for (int dx = -1; dx <= 1; ++dx)
            m = max(m, imageLoad(u_src, ivec2(tvWrapAngle(b.x + dx, int(pc.srcSize.x)), row)).r);
    }
    float ahead = m; // this row's neighbours (and the one before) are in already
    for (int row = b.y; row < int(pc.srcSize.y); ++row)
        for (int dx = -TV_FLOOR_AHEAD_SECTORS; dx <= TV_FLOOR_AHEAD_SECTORS; ++dx)
            ahead = max(ahead, imageLoad(u_src, ivec2(tvWrapAngle(b.x + dx, int(pc.srcSize.x)), row)).r);
    imageStore(u_dst, b, vec4(m, ahead, 0.0, 0.0));
    return;
#else
    const ivec2 base = b * TV_FLOOR_MAX_BLOCK;
    for (int y = 0; y < TV_FLOOR_MAX_BLOCK; ++y)
        for (int x = 0; x < TV_FLOOR_MAX_BLOCK; ++x)
        {
            const ivec2 texel = base + ivec2(x, y);
            if (any(greaterThanEqual(uvec2(texel), pc.srcSize)))
                continue;
            const uint bits = imageLoad(u_src, texel).r;
            if (bits != 0u)
                m = max(m, tvFloorDecode(bits));
        }
#endif
    imageStore(u_dst, b, vec4(m));
}
