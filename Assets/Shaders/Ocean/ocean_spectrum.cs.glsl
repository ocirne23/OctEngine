#version 460

// FFT ocean, step 1: initial spectrum + time evolution (Tessendorf 2001; Horvath 2015). Per wavevector k of every cascade:
// the TMA spectrum - JONSWAP S_j(w) (Hasselmann et al. 1973) x the Kitaigorodskii finite-depth attenuation PHI(w, D)
// (Bouws et al. 1985) - with Hasselmann directional spreading D(theta, w), as an amplitude on the k-lattice, times a
// deterministic per-texel complex gaussian (integer hash: the sea is stable across frames and parameter changes),
// time-evolved with the FINITE-DEPTH dispersion w^2 = g k tanh(k D) (Horvath 2015, eq. 10). Re-evaluated every frame, so
// every parameter is live.
//
// ONLY THE BAND IS STORED. A cascade keeps k < half its Nyquist (the band split below), so every lattice coordinate
// outside |m| < N/4 is zero: this pass runs on that (N/2)^2 block alone (texel = m + N/4), and the FFT passes know the
// rest is zero (ocean_fft.cs.glsl).
//
// Eight real signals per texel: h, Dx, Dz, dh/dx, dh/dz, dDx/dx, dDz/dz, dDx/dz (normals + the fold Jacobian). All spectra
// are Hermitian, so pairs pack as one complex signal (IFFT(A + iB) = a + ib for real a, b): 4 complex signals = 2 RGBA16F
// layers per cascade (rg = first, ba = second), in the ORDER THE MAPS STORE THEM, so the column FFT writes the maps itself:
//   layer 2c     : (Dx + i h | Dz + i dDx/dz)
//   layer 2c + 1 : (dh/dx + i dh/dz | dDx/dx + i dDz/dz)
// The breaking-crest acceleration d2h/dt2 = -w^2 h~ has NO signal of its own: -w^2 = -g k tanh(k D), and dDx/dx + dDz/dz
// transforms -k h~, so the column FFT stores g (dDx/dx + dDz/dz). Exact in deep water; a wave longer than ~2x "Depth"
// reads up to 1 / tanh(k D) too strong, and such a wave has next to no acceleration.
// Cascades band-split the wavenumber range (each keeps [kmin, kmax)), so their sum never duplicates energy.

#include "ubo.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 1, rgba16f) uniform writeonly image2DArray out_spectrum;

const float PI = 3.14159265359;
const float G = 9.81;

// Deterministic gaussian pair per (signed lattice coord, cascade).
#include "mathutils.inc.glsl"
vec2 gaussianPair(ivec2 m, uint cascade)
{
    uvec3 h = pcg3d(uvec3(uint(m.x + 0x8000), uint(m.y + 0x8000), cascade * 4u + 1u));
    vec2 u = vec2(h.xy) * (1.0 / 4294967295.0);
    float r = sqrt(-2.0 * log(max(u.x, 1e-9)));
    float a = 6.2831853 * u.y;
    return r * vec2(cos(a), sin(a));
}

float kitaigorodskii(float w, float depth) // PHI(w, D): shallow water drains the high-frequency energy
{
    float wh = w * sqrt(depth / G);
    if (wh <= 1.0) return 0.5 * wh * wh;
    if (wh <  2.0) return 1.0 - 0.5 * (2.0 - wh) * (2.0 - wh);
    return 1.0;
}

vec2 cmul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

void main()
{
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const uint cascade = gl_GlobalInvocationID.z;
    const int N = OCEAN_FFT_SIZE;
    const float L = u_ocean_cascadeSizes[cascade];

    // Band split: each cascade keeps a disjoint wavenumber range. Boundaries at half the coarser cascade's Nyquist keep
    // well-resolved detail in the finest cascade that can represent it.
    const float kMin = cascade == 0u ? 0.0 : 0.5 * PI * float(N) / u_ocean_cascadeSizes[cascade - 1u];
    const float kMax = 0.5 * PI * float(N) / L;

    const ivec2 m = xy - ivec2(N / 4);
    const vec2 kv = 6.2831853 * vec2(m) / L;
    const float k = length(kv);

    vec4 s0 = vec4(0.0), s1 = vec4(0.0);
    if (k >= max(kMin, 1e-5) && k < kMax)
    {
        const float U = u_ocean_windSpeed;
        const float depth = u_ocean_depth;
        const vec2 kn = kv / k;

        // Finite-depth dispersion w^2 = g k tanh(k D) and dw/dk (sech^2 = 1 - tanh^2).
        const float kd = min(k * depth, 20.0);
        const float th = tanh(kd);
        const float w = sqrt(G * k * th);
        const float dwdk = G * (th + kd * (1.0 - th * th)) / (2.0 * w);

        // TMA: JONSWAP x Kitaigorodskii. u_ocean_spectrumShape = (alpha, peak w, the spreading exponent past the peak):
        // the per-frame terms of the wind and the fetch.
        const float alpha = u_ocean_spectrumShape.x;
        const float wp = u_ocean_spectrumShape.y;
        const float wr = w / wp;
        const float sigma = w <= wp ? 0.07 : 0.09;
        const float peak = exp(-(wr - 1.0) * (wr - 1.0) / (2.0 * sigma * sigma));
        const float w2 = w * w;
        const float wr2 = wr * wr;
        const float S = (alpha * G * G / (w2 * w2 * w)) * exp(-1.25 / (wr2 * wr2)) * pow(3.3, peak) * kitaigorodskii(w, depth);

        // Hasselmann spreading D(theta) = N(s) |cos(theta / 2)|^(2 s), theta = the angle from the wind direction:
        // cos^2(theta / 2) = (1 + cos theta) / 2, and the wave at -k (theta + pi) takes (1 - cos theta) / 2. One radial
        // part serves both directions; no atan.
        // N(s) = Gamma(s+1) / (2 sqrt(pi) Gamma(s+1/2)) ~= sqrt(s + 0.25) / (2 sqrt(pi))
        const float s = min(w <= wp ? 6.97 * pow(wr, 4.06) : 9.77 * pow(wr, u_ocean_spectrumShape.z), 30.0);
        const float norm = sqrt(s + 0.25) / (2.0 * sqrt(PI));
        const float cosTheta = dot(kn, u_ocean_windDirection);
        const vec2 spread = pow(max(0.5 + vec2(0.5, -0.5) * cosTheta, vec2(0.0)), vec2(0.5 * s)); // sqrt(D) at +k, -k

        // Amplitude of h0(k) (Tessendorf eq. 43 with Horvath eq. 20's directional spectrum: variance in a lattice cell =
        // S(w) D(theta) (dw/dk) / k * dk^2), x 1 / sqrt(2) for the gaussian pair.
        // Wave-age limit: wind cannot raise waves much faster than itself, but JONSWAP below its range (U < ~5 m/s) still
        // gives fast long swells energy - visible waves at near-zero wind. Softly suppress phase speeds c = w/k past
        // ~2.5 U: a developed sea's peak sits inside the limit, and the sea goes to glass as the wind dies.
        const float ageRatio = w / (k * (2.5 * U + 0.05));
        const float amp = sqrt(S * norm * dwdk / k) * (6.2831853 / L) * u_ocean_amplitude * exp(-(ageRatio * ageRatio) * (ageRatio * ageRatio));
        const vec2 h0p = gaussianPair( m, cascade) * (amp * spread.x);
        const vec2 h0m = gaussianPair(-m, cascade) * (amp * spread.y);

        // h~(k, t) = h0(k) e^{iwt} + h0*(-k) e^{-iwt}   (Tessendorf eq. 43)
        // The clock rate (u_ocean_timeScale) is sqrt(world scale): the Froude-scaled inputs shrink the sea and its periods
        // by sqrt(s); this holds them at the model sea's.
        const float wt = w * u_timeSeconds * u_ocean_timeScale;
        const vec2 ep = vec2(cos(wt), sin(wt));
        const vec2 ht = cmul(h0p, ep) + cmul(vec2(h0m.x, -h0m.y), vec2(ep.x, -ep.y));
        const vec2 iht = vec2(-ht.y, ht.x); // i * h~

        // Dx = i kx/k h~, Dz = i kz/k h~: Tessendorf eq. 44 NEGATED - under this e^{+ikx} transform the paper's -i
        // converges water into the TROUGHS; +i pinches the crests (the dD derivatives flip with it, so the fold Jacobian
        // stays right). dh/dx = i kx h~; dDx/dx = -kx^2/k h~; dDz/dz = -kz^2/k h~; dDx/dz = -kx kz / k h~.
        // A + iB per pair: i * (c h~) = c * (i h~), and i * (c i h~) = -c h~.
        s0 = vec4(iht * kn.x + iht,                    // Dx + i h
                  iht * kn.y - iht * (kv.x * kn.y));   // Dz + i dDx/dz
        s1 = vec4(iht * kv.x - ht * kv.y,              // dh/dx + i dh/dz
                  -ht * (kv.x * kn.x) - iht * (kv.y * kn.y)); // dDx/dx + i dDz/dz
    }

    // Stored after the branch: a store per layer inside it measured 2 registers more.
    imageStore(out_spectrum, ivec3(xy, int(cascade * 2u)), s0);
    imageStore(out_spectrum, ivec3(xy, int(cascade * 2u + 1u)), s1);
}
