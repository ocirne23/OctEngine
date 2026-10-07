#version 460

// Bloom (BloomPipeline): one DOWNSAMPLE step, level k -> k + 1 (half the size). The 13-tap filter of Jimenez,
// "Next Generation Post Processing in Call of Duty: Advanced Warfare" (2014): five overlapping 2 x 2 boxes out of
// 13 bilinear taps, which stays stable under sub-pixel motion (a plain 2 x 2 box flickers). Level 0 is not made
// here: the eye-adaptation histogram writes it (eyeadapt_histogram.cs.glsl).
// Every level uses only its VIEWPORT REGION (the texels the viewport maps to, from the origin); the taps are
// clamped inside it, so nothing outside the viewport - stale texels - leaks in.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 0) uniform sampler2D u_src; // level k (linear, clamp)
layout (binding = 1, r11f_g11f_b10f) uniform restrict writeonly image2D u_dst; // level k + 1

layout (push_constant) uniform PC
{
    ivec2 srcRegion;    // texels of level k in use
    ivec2 dstRegion;    // texels of level k + 1 in use
    vec2  srcInvSize;   // 1 / level k's full size
} pc;

vec3 tap(vec2 texel) // a bilinear tap at a level-k texel coordinate, clamped into the region
{
    return textureLod(u_src, clamp(texel, vec2(0.5), vec2(pc.srcRegion) - 0.5) * pc.srcInvSize, 0.0).rgb;
}

void main()
{
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= pc.dstRegion.x || p.y >= pc.dstRegion.y)
        return;
    const vec2 c = vec2(p * 2) + 1.0; // the destination texel's centre, in source texels
    const vec3 a = tap(c + vec2(-2.0,  2.0));
    const vec3 b = tap(c + vec2( 0.0,  2.0));
    const vec3 d = tap(c + vec2( 2.0,  2.0));
    const vec3 e = tap(c + vec2(-2.0,  0.0));
    const vec3 f = tap(c);
    const vec3 g = tap(c + vec2( 2.0,  0.0));
    const vec3 h = tap(c + vec2(-2.0, -2.0));
    const vec3 i = tap(c + vec2( 0.0, -2.0));
    const vec3 j = tap(c + vec2( 2.0, -2.0));
    const vec3 k = tap(c + vec2(-1.0,  1.0));
    const vec3 l = tap(c + vec2( 1.0,  1.0));
    const vec3 m = tap(c + vec2(-1.0, -1.0));
    const vec3 n = tap(c + vec2( 1.0, -1.0));
    // The centre box 0.5, the four corner boxes 0.125 each (every tap weighed by the boxes it is in).
    const vec3 result = f * 0.125 + (a + d + h + j) * 0.03125 + (b + e + g + i) * 0.0625 + (k + l + m + n) * 0.125;
    imageStore(u_dst, p, vec4(result, 0.0));
}
