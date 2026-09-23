#version 450

// Eye-adaptation pass 1: build a 256-bin luminance histogram of the TAA-resolved scene colour, restricted
// to the editor viewport rect (the area outside it holds the scene clear colour, which would bias the
// average). Luminance is binned in log2 space over [minLogLum, maxLogLum]; bin 0 collects near-black pixels
// (excluded from the average). One workgroup accumulates into shared memory, then flushes to the global
// histogram with atomics. The reduce pass (eyeadapt_reduce) turns this into an adapted exposure.
// Every pixel still counts (the reduce divides by the viewport area), but each thread bins a
// PIXELS_PER_THREAD^2 block: neighbours mostly share a bin, so a run of equal bins is ONE shared atomic,
// and only the non-empty bins go to the global histogram. One pixel per thread measured 0.155 ms at
// 1440p: ~3600 workgroups x 256 global atomics, and shared atomics serialized on the few busy bins.

#define PIXELS_PER_THREAD 4 // keep in sync with EyeAdaptationPipeline's dispatch

layout (local_size_x = 16, local_size_y = 16) in;

layout (binding = 0) uniform sampler2D u_resolved;
layout (binding = 1, std430) buffer Histogram { uint u_bins[256]; };
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

layout (push_constant) uniform PC
{
    ivec2 u_vpMin;        // viewport origin in the resolved image
    ivec2 u_vpSize;       // viewport size (pixels sampled)
};

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

    // The block is strided by the workgroup size, so a warp's 32 reads per step stay adjacent pixels.
    const ivec2 groupBase = ivec2(gl_WorkGroupID.xy) * (16 * PIXELS_PER_THREAD);
    uint runBin = 0xFFFFFFFFu;
    uint runCount = 0u;
    for (int y = 0; y < PIXELS_PER_THREAD; ++y)
    {
        for (int x = 0; x < PIXELS_PER_THREAD; ++x)
        {
            const ivec2 id = groupBase + ivec2(gl_LocalInvocationID.xy) + ivec2(x, y) * 16;
            if (id.x >= u_vpSize.x || id.y >= u_vpSize.y)
                continue;
            const uint bin = binIndex(luminance(texelFetch(u_resolved, u_vpMin + id, 0).rgb));
            if (bin != runBin)
            {
                if (runCount != 0u)
                    atomicAdd(s_bins[runBin], runCount);
                runBin = bin;
                runCount = 0u;
            }
            ++runCount;
        }
    }
    if (runCount != 0u)
        atomicAdd(s_bins[runBin], runCount);
    barrier();

    const uint count = s_bins[gl_LocalInvocationIndex];
    if (count != 0u)
        atomicAdd(u_bins[gl_LocalInvocationIndex], count);
}
