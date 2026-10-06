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
//
// BLOOM SHARES THIS READ (pc_bloom, BloomPipeline): the block is 2 x 2 QUADS of 2 x 2 pixels, and each quad is
// also one texel of bloom level 0 (half res, viewport-relative) - so bloom never reads the full-res image
// itself. The quad's texel is the Karis average (weights 1 / (1 + luma)): one very bright pixel cannot become
// a flickering bloom blob.

#define PIXELS_PER_THREAD 4 // keep in sync with EyeAdaptationPipeline's dispatch

layout (local_size_x = 16, local_size_y = 16) in;

layout (binding = 0) uniform sampler2D u_resolved;
layout (binding = 1, std430) buffer Histogram { uint u_bins[256]; };
layout (binding = 3, r11f_g11f_b10f) uniform restrict writeonly image2D u_bloomOut; // bloom level 0
// The persistent exposure the reduce pass writes AFTER this one: the bloom threshold reads LAST frame's (it
// moves slowly, the adaptation is smoothed over seconds).
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

// PC_DECL_ / PC_CONSTS: the lockable values (EyeAdaptationPipeline::registerPushFields, PushFields.ixx).
layout (push_constant) uniform PC
{
    ivec2 pc_vpMin;        // viewport origin in the resolved image
    ivec2 pc_vpSize;       // viewport size (pixels sampled)
    int   pc_bloom;        // 1 = also write bloom level 0
    PC_DECL_bloomThreshold; // float: the soft threshold, in EXPOSED units (1 = display white before the tonemap); 0 = off
    PC_DECL_bloomKnee;      // float: the knee's half width (exposed units)
    PC_DECL_manualExposure; // float: exp2 of the EV tweak (the composite's u_exposure)
    PC_DECL_autoExposure;   // int: 1 = times the eye-adaptation exposure (the composite's rule)
};
PC_CONSTS

// The soft threshold on the quad's (Karis-averaged) colour: nothing below threshold - knee, the excess above
// threshold + knee, a quadratic curve between (the Unity / Unreal knee). Measured on the brightest channel
// after the exposure, so it tracks what the display will show.
vec3 bloomThreshold(vec3 c)
{
    if (pc_bloomThreshold <= 0.0)
        return c;
    const float exposure = pc_manualExposure * (pc_autoExposure != 0 ? u_autoExposure : 1.0);
    const float brightness = max(max(c.r, c.g), c.b) * exposure;
    const float knee = max(pc_bloomKnee, 1e-4);
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
