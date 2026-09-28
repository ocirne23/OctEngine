#version 460

#extension GL_GOOGLE_include_directive : enable

// Motion blur, pass 2 of 3 (MotionBlurPipeline): per tile, the longest velocity of its 3x3 tile neighbourhood.
// A blur can reach at most the max radius, which is at most one tile (MotionBlurPipeline clamps it), so a pixel
// whose own tile is still sees a moving object in the next tile - that is how the blur crosses a silhouette.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0) uniform sampler2D u_tileMax;
layout (binding = 1, rg16f) uniform restrict writeonly image2D u_neighborMaxOut;

layout (push_constant) uniform PC
{
    uint tilesX;
    uint tilesY;
} pc;

void main()
{
    const ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    if (t.x >= int(pc.tilesX) || t.y >= int(pc.tilesY))
        return;
    const ivec2 last = ivec2(pc.tilesX, pc.tilesY) - 1;
    vec2 best = vec2(0.0);
    float bestLen2 = 0.0;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        const vec2 v = texelFetch(u_tileMax, clamp(t + ivec2(x, y), ivec2(0), last), 0).xy;
        const float len2 = dot(v, v);
        if (len2 > bestLen2)
        {
            bestLen2 = len2;
            best = v;
        }
    }
    imageStore(u_neighborMaxOut, t, vec4(best, 0.0, 0.0));
}
