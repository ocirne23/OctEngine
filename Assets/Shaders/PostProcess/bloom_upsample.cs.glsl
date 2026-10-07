#version 460

// Bloom (BloomPipeline): one UPSAMPLE step, level k + 1 -> k, from the smallest level up. Level k becomes
// its own weight x itself + a 3 x 3 tent filter of the (already accumulated) level k + 1, in place (each texel
// reads and writes only itself in level k). The first step also weighs the smallest level, which is never a
// destination. After the last step level 0 holds the WEIGHTED sum of every level's blur (the "Radius" weights);
// the composite divides by the weights' sum (BloomPipeline::getNormalize). Regions and clamping as in
// bloom_downsample.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0) uniform sampler2D u_src; // level k + 1 (linear, clamp)
layout (binding = 1, r11f_g11f_b10f) uniform restrict image2D u_dst; // level k, read + written

layout (push_constant) uniform PC
{
    ivec2 srcRegion;    // texels of level k + 1 in use
    ivec2 dstRegion;    // texels of level k in use
    vec2  srcInvSize;   // 1 / level k + 1's full size
    float dstWeight;    // level k's own weight
    float srcWeight;    // the smallest level's weight on the first step, 1 after (the source is then a sum)
} pc;

vec3 tap(vec2 texel)
{
    return textureLod(u_src, clamp(texel, vec2(0.5), vec2(pc.srcRegion) - 0.5) * pc.srcInvSize, 0.0).rgb;
}

void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= pc.dstRegion.x || p.y >= pc.dstRegion.y)
        return;
    const vec2 c = (vec2(p) + 0.5) * 0.5; // this texel's centre in level k + 1 texels
    vec3 sum = tap(c) * 4.0;
    sum += (tap(c + vec2(-1.0, 0.0)) + tap(c + vec2(1.0, 0.0)) + tap(c + vec2(0.0, -1.0)) + tap(c + vec2(0.0, 1.0))) * 2.0;
    sum += tap(c + vec2(-1.0, -1.0)) + tap(c + vec2(1.0, -1.0)) + tap(c + vec2(-1.0, 1.0)) + tap(c + vec2(1.0, 1.0));
    imageStore(u_dst, p, vec4(imageLoad(u_dst, p).rgb * pc.dstWeight + sum * (pc.srcWeight / 16.0), 0.0));
}
