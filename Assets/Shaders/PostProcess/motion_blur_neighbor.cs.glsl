#version 460

#extension GL_GOOGLE_include_directive : enable

// Motion blur (MotionBlurPipeline): per MOTION_BLUR_TILE tile, the longest velocity of its 3 x 3 tile
// neighbourhood, straight from the 8 x 8 sub-tile maxima (TAA or motion_blur_tiles wrote them): a 3 x 3 tile
// block is (3 x TILE / SUBTILE)^2 sub-tiles. A blur can reach at most the max radius, which is at most one
// tile (MotionBlurPipeline clamps it), so a pixel whose own tile is still sees a moving object in the next
// tile - that is how the blur crosses a silhouette. The composite reads this to pick gather or copy.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0) uniform sampler2D u_subTiles;
layout (binding = 1, rg16f) uniform restrict writeonly image2D u_neighborMaxOut;

layout (push_constant) uniform PC
{
    uint tilesX;
    uint tilesY;
    uint subTilesX;
    uint subTilesY;
} pc;

void main()
{
    const ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    if (t.x >= int(pc.tilesX) || t.y >= int(pc.tilesY))
        return;
    const int per = MOTION_BLUR_TILE / MOTION_BLUR_SUBTILE;
    const ivec2 lo = max((t - 1) * per, ivec2(0));
    const ivec2 hi = min((t + 2) * per, ivec2(pc.subTilesX, pc.subTilesY)) - 1;
    vec2 best = vec2(0.0);
    float bestLen2 = 0.0;
    for (int y = lo.y; y <= hi.y; ++y)
    for (int x = lo.x; x <= hi.x; ++x)
    {
        const vec2 v = texelFetch(u_subTiles, ivec2(x, y), 0).xy;
        const float len2 = dot(v, v);
        if (len2 > bestLen2)
        {
            bestLen2 = len2;
            best = v;
        }
    }
    imageStore(u_neighborMaxOut, t, vec4(best, 0.0, 0.0));
}
