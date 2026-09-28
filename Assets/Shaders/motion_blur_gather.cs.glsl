#version 460

#extension GL_GOOGLE_include_directive : enable

// Motion blur, pass 3 of 3 (MotionBlurPipeline): the reconstruction filter of McGuire et al., "A Reconstruction
// Filter for Plausible Motion Blur" (2012). Samples along the tile neighbourhood's longest velocity; each sample
// Y weighs by whether ITS blur reaches this pixel X (a moving foreground object smears over the background
// behind it) or X's own blur reaches Y (X smears over what is behind it), with soft depth tests deciding which
// of the two is in front. The velocities are displacements over the exposure; a blur spans half of one each way.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_color;       // TAA's resolved colour (or the scene colour with TAA off)
layout (binding = 2) uniform sampler2D u_velocity;    // pass 1: xy = velocity (px), z = view distance (m)
layout (binding = 3) uniform sampler2D u_neighborMax; // pass 2: per tile
layout (binding = 4, rgba16f) uniform restrict writeonly image2D u_out;

layout (push_constant) uniform PC
{
    uint  width;
    uint  height;
    float shutter;
    float maxRadius;
    float cameraScale;
    uint  samples;
} pc;

// 1 where za is in front of zb, fading to 0 over a few percent of the distance behind.
float softDepthCompare(float za, float zb)
{
    return clamp(1.0 - (za - zb) / (0.02 * min(za, zb) + 0.05), 0.0, 1.0);
}
// A blur of radius r covers a point d away: linear falloff (the cone), and a hard edge (the cylinder).
float cone(float d, float r) { return clamp(1.0 - d / max(r, 1e-3), 0.0, 1.0); }
float cylinder(float d, float r) { return 1.0 - smoothstep(0.95 * r, 1.05 * r, d); }

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.width) || px.y >= int(pc.height))
        return;
    const vec4 color = texelFetch(u_color, px, 0);
    const vec2 vN = texelFetch(u_neighborMax, px / MOTION_BLUR_TILE, 0).xy;
    const float rN = 0.5 * length(vN);
    if (rN < 0.5) // under half a pixel of blur in the whole neighbourhood
    {
        imageStore(u_out, px, color);
        return;
    }

    const vec4 vx = texelFetch(u_velocity, px, 0);
    const float rX = max(0.5 * length(vx.xy), 0.5);
    const float zX = vx.z;
    vec3 sum = color.rgb / rX;
    float wsum = 1.0 / rX;

    // Samples stay inside the editor viewport (outside it the image is the UI's).
    const vec2 size = vec2(pc.width, pc.height);
    const ivec2 lo = ivec2(u_viewportRect.xy * size);
    const ivec2 hi = ivec2((u_viewportRect.xy + u_viewportRect.zw) * size) - 1;
    // Interleaved gradient noise, moving per frame: the sample offsets dither instead of banding.
    const vec2 noisePx = vec2(px) + float(u_frameIndex % 64u) * 5.588238;
    const float jitter = fract(52.9829189 * fract(dot(noisePx, vec2(0.06711056, 0.00583715)))) - 0.5;
    const float invSamples = 1.0 / float(pc.samples);
    for (uint i = 0u; i < pc.samples; ++i)
    {
        const float t = ((float(i) + 0.5 + jitter) * invSamples) * 2.0 - 1.0; // -1 .. 1 along vN
        const vec2 offset = vN * (0.5 * t);
        const ivec2 py = clamp(ivec2(floor(vec2(px) + 0.5 + offset)), lo, hi);
        const vec4 vy = texelFetch(u_velocity, py, 0);
        const float rY = 0.5 * length(vy.xy);
        const float d = length(offset);
        const float f = softDepthCompare(vy.z, zX); // Y in front: its blur covers X
        const float b = softDepthCompare(zX, vy.z); // X in front: X's blur reaches Y's place
        const float w = f * cone(d, rY) + b * cone(d, rX) + 2.0 * cylinder(d, rY) * cylinder(d, rX);
        sum += texelFetch(u_color, py, 0).rgb * w;
        wsum += w;
    }
    imageStore(u_out, px, vec4(sum / wsum, color.a));
}
