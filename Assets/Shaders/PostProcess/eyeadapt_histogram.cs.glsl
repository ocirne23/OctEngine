#version 450

// Eye-adaptation pass 1: a 256-bin log2-luminance histogram of the resolved scene colour inside the editor viewport
// (the clear colour outside would bias it); bin 0 = near-black, excluded from the average. eyeadapt_reduce turns it
// into an exposure. Each thread bins a PIXELS_PER_THREAD^2 block: neighbours mostly share a bin, so a run of equal
// bins is ONE shared atomic, and only non-empty bins go to the global histogram (one pixel per thread = ~3600 groups
// x 256 global atomics, and shared atomics serialized on the few busy bins).
//
// BLOOM SHARES THIS READ (pc_bloom, BloomPipeline): the block is 2 x 2 QUADS of 2 x 2 pixels, and each quad is one
// texel of bloom level 0 (half res, viewport-relative). The texel is the Karis average (weights 1 / (1 + luma)), so
// one very bright pixel cannot become a flickering bloom blob.

#define PIXELS_PER_THREAD 4 // keep in sync with EyeAdaptationPipeline's dispatch

layout (local_size_x = 16, local_size_y = 16) in;

layout (binding = 0) uniform sampler2D u_resolved;
layout (binding = 1, std430) buffer Histogram { uint u_bins[256]; };
layout (binding = 3, r11f_g11f_b10f) uniform restrict writeonly image2D u_bloomOut; // bloom level 0
// The reduce pass writes the exposure AFTER this one: the bloom threshold reads LAST frame's (smoothed over seconds).
layout (binding = 4, std430) readonly buffer Adapt { float u_avgLum; float u_autoExposure; };
layout (binding = 2, std140) uniform Params
{
    float u_minLogLum;      // lower end of the log2-luminance range
    float u_invLogLumRange; // 1 / (maxLogLum - minLogLum)
    float u_logLumRange;
    float u_dt;
    float u_tau;
    float u_key;
    float u_minExposure;
    float u_maxExposure;
};

// The push block: EyeAdaptationPipeline's HistogramPush (the bloom threshold / knee and the exposure rule: lockable).
#include "push.generated.glsl"

// Soft threshold (the Unity / Unreal knee): nothing below threshold - knee, the excess above threshold + knee, a
// quadratic between. On the brightest channel after the exposure, so it tracks what the display shows.
vec3 bloomThreshold(vec3 c)
{
    if (pc_bloomThreshold <= 0.0)
        return c;

    const float exposure = pc_manualExposure * (pc_autoExposure != 0 ? u_autoExposure : 1.0);
    const float brightness = max(max(c.r, c.g), c.b) * exposure;
    const float knee = pc_bloomKnee;
    float soft = clamp(brightness - pc_bloomThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    return c * (max(soft, brightness - pc_bloomThreshold) / max(brightness, 1e-6));
}

shared uint s_bins[256];

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

uint binIndex(float lum)
{
    if (lum < 1e-4)
        return 0u; // near-black -> dedicated bin, ignored by the average
    float t = clamp((log2(lum) - u_minLogLum) * u_invLogLumRange, 0.0, 1.0);
    return uint(t * 254.0 + 1.0); // bins 1..255
}

void main()
{
    s_bins[gl_LocalInvocationIndex] = 0u;
    barrier();

    // The quads are strided by the workgroup size (x 2), so a warp's reads per step stay a compact 32 x 2 block.
    const ivec2 groupBase = ivec2(gl_WorkGroupID.xy) * (16 * PIXELS_PER_THREAD);
    const ivec2 last = pc_vpSize - 1;
    uint runBin = 0xFFFFFFFFu;
    uint runCount = 0u;
    for (int qy = 0; qy < PIXELS_PER_THREAD / 2; ++qy)
    {
        for (int qx = 0; qx < PIXELS_PER_THREAD / 2; ++qx)
        {
            const ivec2 quad = groupBase + ivec2(gl_LocalInvocationID.xy) * 2 + ivec2(qx, qy) * 32;
            if (quad.x > last.x || quad.y > last.y)
                continue;

            vec3 karisSum = vec3(0.0);
            float karisWeight = 0.0;
            for (int p = 0; p < 4; ++p)
            {
                const ivec2 id = quad + ivec2(p & 1, p >> 1);
                const bool inside = id.x <= last.x && id.y <= last.y;
                // Past an odd viewport edge the quad repeats its edge pixel for bloom; the histogram skips it.
                const vec3 c = texelFetch(u_resolved, pc_vpMin + min(id, last), 0).rgb;
                const float lum = luminance(c);
                if (pc_bloom != 0)
                {
                    const vec3 cs = clamp(c, vec3(0.0), vec3(64512.0)); // finite: the image may hold a stray Inf/NaN
                    const float w = 1.0 / (1.0 + luminance(cs));
                    karisSum += cs * w;
                    karisWeight += w;
                }

                if (!inside)
                    continue;
                const uint bin = binIndex(lum);
                if (bin != runBin)
                {
                    if (runCount != 0u)
                        atomicAdd(s_bins[runBin], runCount);
                    runBin = bin;
                    runCount = 0u;
                }
                ++runCount;
            }

            if (pc_bloom != 0)
                imageStore(u_bloomOut, quad >> 1, vec4(bloomThreshold(karisSum / karisWeight), 0.0));
        }
    }

    if (runCount != 0u)
        atomicAdd(s_bins[runBin], runCount);
    barrier();

    const uint count = s_bins[gl_LocalInvocationIndex];
    if (count != 0u)
        atomicAdd(u_bins[gl_LocalInvocationIndex], count);
}
