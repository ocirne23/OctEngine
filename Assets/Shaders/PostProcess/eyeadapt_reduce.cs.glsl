#version 450

// Eye-adaptation pass 2: reduce the luminance histogram (eyeadapt_histogram) to a single adapted exposure.
// A weighted average of the non-black bins gives the scene's average log-luminance; that target luminance is
// approached over time (exponential smoothing with time constant u_tau, frame-rate independent via u_dt) and
// stored persistently in u_adapt. The composite pass reads u_adapt.exposure as the auto-exposure multiplier.
// Runs as a single workgroup of 256 threads (one per bin).

layout (local_size_x = 256) in;

layout (binding = 0, std430) buffer Histogram { uint u_bins[256]; };
layout (binding = 1, std430) buffer Adapt { float u_avgLum; float u_exposure; };
layout (binding = 2, std140) uniform Params
{
    float u_minLogLum;   // lower end of the log2-luminance range (matches the histogram pass)
    float u_invLogLumRange;
    float u_logLumRange; // maxLogLum - minLogLum
    float u_dt;          // delta time (s) for frame-rate-independent adaptation
    float u_tau;         // adaptation time constant (s); larger = slower eye
    float u_key;         // target middle-grey luminance (0.18 = photographic key)
    float u_minExposure; // exposure clamp (linear, = exp2(minEV))
    float u_maxExposure; // exposure clamp (linear, = exp2(maxEV))
};

layout (push_constant) uniform PC
{
    uint u_pixelCount;   // number of pixels accumulated (viewport area)
};

shared float s_weighted[256];

void main()
{
    uint i = gl_LocalInvocationIndex;
    uint count = u_bins[i];
    s_weighted[i] = float(count) * float(i); // weight each bin by its index
    barrier();

    for (uint s = 128u; s > 0u; s >>= 1u)
    {
        if (i < s)
            s_weighted[i] += s_weighted[i + s];
        barrier();
    }

    if (i != 0u)
        return;

    // Exclude the near-black bin (0) from the average so dark scenes don't pin to bin 0.
    float denom = max(float(u_pixelCount) - float(u_bins[0]), 1.0);
    float weightedAvgBin = (s_weighted[0] / denom) - 1.0;           // 0..254
    float avgLogLum = u_minLogLum + (weightedAvgBin / 254.0) * u_logLumRange;
    float targetLum = exp2(avgLogLum);

    // Exponential adaptation toward the target luminance. On the first frame (u_avgLum == 0) snap.
    float prev = u_avgLum > 0.0 ? u_avgLum : targetLum;
    float adapted = mix(targetLum, prev, exp(-u_dt / max(u_tau, 1e-3)));

    u_avgLum = adapted;
    u_exposure = clamp(u_key / max(adapted, 1e-4), u_minExposure, u_maxExposure);
}
