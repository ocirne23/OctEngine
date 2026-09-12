#version 460

// FFT ocean, pass 1/3: initial spectrum + time evolution (Tessendorf 2001; Horvath 2015).
//
// For every wavevector k of every cascade this evaluates the TMA spectrum - JONSWAP S_j(w) (Hasselmann et
// al. 1973) times the Kitaigorodskii finite-depth attenuation PHI(w, D) (Bouws et al. 1985) - with the
// Hasselmann directional spreading D(theta, w), converts the variance to a wave amplitude on the k-lattice,
// draws the deterministic per-texel complex gaussian (Box-Muller from an integer hash, so the sea is stable
// across frames and parameter changes), and time-evolves it with the FINITE-DEPTH dispersion relation
//   w^2 = g k tanh(k D)                                                     (Horvath 2015, eq. 10)
// The whole spectrum is re-evaluated every frame (256^2 x cascades is trivial), so every parameter is live.
//
// Ten real signals are produced per texel: h, Dx, Dz (displacement), dh/dx, dh/dz, dDx/dx, dDz/dz,
// dDx/dz (derivatives for normals + the fold Jacobian), plus the surface's vertical acceleration
// d2h/dt2 = -w^2 h~ and velocity dh/dt (exact per-component time derivatives) for the Longuet-Higgins
// breaking-crest foam criterion - a crest whose downward acceleration exceeds a fraction of g is
// breaking, which is what makes LARGE waves foam (their Jacobian folding is choppiness-scaled and weak).
// All spectra are Hermitian, so pairs pack as one complex signal each (IFFT(A + iB) = a + ib for real
// a, b): 5 complex signals = 3 RGBA32F layers per cascade (rg = first complex, ba = second).
//
// Cascades band-split the wavenumber range (each keeps [kmin, kmax)) so summing them never duplicates
// energy - the standard multi-patch setup used by e.g. the Atlas ocean (GDC 2019) and GodotOceanWaves.

#include "ubo.inc.glsl"

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout (binding = 1, rgba32f) uniform writeonly image2DArray out_spectrum;

const float PI = 3.14159265359;
const float G = 9.81;

// --- Deterministic gaussian pair per (signed lattice coord, cascade) --------------------------------
uvec3 pcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec2 gaussianPair(ivec2 m, uint cascade, uint which)
{
    uvec3 h = pcg3d(uvec3(uint(m.x + 0x8000), uint(m.y + 0x8000), cascade * 4u + which + 1u));
    vec2 u = vec2(h.xy) * (1.0 / 4294967295.0);
    float r = sqrt(-2.0 * log(max(u.x, 1e-9)));
    float a = 6.2831853 * u.y;
    return r * vec2(cos(a), sin(a));
}

// --- Finite-depth dispersion: w^2 = g k tanh(k D) ---------------------------------------------------
float dispersion(float k, float depth)
{
    return sqrt(G * k * tanh(min(k * depth, 20.0)));
}
float dispersionDeriv(float k, float depth) // dw/dk
{
    float kd = min(k * depth, 20.0);
    float th = tanh(kd);
    float ch = cosh(kd);
    return G * (th + kd / (ch * ch)) / (2.0 * dispersion(k, depth));
}

// --- TMA spectrum (JONSWAP x Kitaigorodskii) + Hasselmann directional spreading ---------------------
float jonswap(float w, float U, float F)
{
    float alpha = 0.076 * pow(U * U / (F * G), 0.22);
    float wp    = 22.0 * pow(G * G / (U * F), 1.0 / 3.0);
    float sigma = w <= wp ? 0.07 : 0.09;
    float r     = exp(-(w - wp) * (w - wp) / (2.0 * sigma * sigma * wp * wp));
    return (alpha * G * G / (w * w * w * w * w)) * exp(-1.25 * pow(wp / w, 4.0)) * pow(3.3, r);
}
float kitaigorodskii(float w, float depth) // PHI(w, D): shallow water drains the high-frequency energy
{
    float wh = w * sqrt(depth / G);
    if (wh <= 1.0) return 0.5 * wh * wh;
    if (wh <  2.0) return 1.0 - 0.5 * (2.0 - wh) * (2.0 - wh);
    return 1.0;
}
float hasselmann(float theta, float w, float U, float F)
{
    float wp = 22.0 * pow(G * G / (U * F), 1.0 / 3.0);
    float s;
    if (w <= wp)
        s = 6.97 * pow(w / wp, 4.06);
    else
        s = 9.77 * pow(w / wp, -2.33 - 1.45 * (U * wp / G - 1.17));
    s = clamp(s, 0.0, 30.0);
    // N(s) = Gamma(s+1) / (2 sqrt(pi) Gamma(s+1/2)) ~= sqrt(s + 0.25) / (2 sqrt(pi))
    float norm = sqrt(s + 0.25) / (2.0 * sqrt(PI));
    return norm * pow(abs(cos(theta * 0.5)), 2.0 * s);
}

// Complex amplitude h0(k) for one wavevector (Tessendorf eq. 43 with the directional spectrum of
// Horvath eq. 20: variance in a lattice cell = S(w) D(theta) (dw/dk) / k * dk^2).
vec2 h0(ivec2 m, uint cascade, float L, float kMin, float kMax, uint which)
{
    vec2 kv = 6.2831853 * vec2(m) / L;
    float k = length(kv);
    if (k < 1e-5 || k < kMin || k >= kMax)
        return vec2(0.0);

    float U = u_oceanParams1.x;
    float F = u_oceanParams1.y;
    float depth = u_oceanParams1.z;
    float w = dispersion(k, depth);
    float theta = atan(kv.y, kv.x) - atan(u_oceanParams0.y, u_oceanParams0.x);

    float S = jonswap(w, U, F) * kitaigorodskii(w, depth) * hasselmann(theta, w, U, F);
    float dk = 6.2831853 / L;
    float amp = sqrt(2.0 * S * dispersionDeriv(k, depth) / k) * dk * u_oceanParams0.z;

    // Wave-age generation limit: wind cannot generate waves whose phase speed much exceeds the wind
    // itself, but JONSWAP extrapolated below its validity range (U < ~5 m/s) still assigns energy to
    // fast long swells - leaving visible waves at near-zero wind. Softly suppress components with
    // c = w/k beyond ~2.5 U: negligible for developed seas (the spectral peak sits inside the limit),
    // and collapses the sea to glass as the wind dies.
    const float phaseSpeed = w / k;
    const float ageRatio = phaseSpeed / (2.5 * U + 0.05);
    amp *= exp(-(ageRatio * ageRatio) * (ageRatio * ageRatio));

    return gaussianPair(m, cascade, which) * (amp / sqrt(2.0));
}

vec2 cmul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

void main()
{
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const uint cascade = gl_GlobalInvocationID.z;
    const int N = OCEAN_FFT_SIZE;
    const float L = u_oceanParams2[cascade];

    // Band split: each cascade keeps a disjoint wavenumber range. Boundaries at half the coarser
    // cascade's Nyquist keep well-resolved detail in the finest cascade that can represent it.
    float kMin = cascade == 0u ? 0.0 : 0.5 * PI * float(N) / u_oceanParams2[cascade - 1u];
    float kMax = 0.5 * PI * float(N) / L;

    const ivec2 m = xy - ivec2(N / 2);
    const vec2 kv = 6.2831853 * vec2(m) / L;
    const float k = length(kv);

    // h~(k, t) = h0(k) e^{iwt} + h0*(-k) e^{-iwt}   (Tessendorf eq. 43)
    const vec2 h0p = h0( m, cascade, L, kMin, kMax, 0u);
    const vec2 h0m = h0(-m, cascade, L, kMin, kMax, 0u);
    const float w = dispersion(max(k, 1e-5), u_oceanParams1.z);
    // The clock rate (u_oceanParams10.y) is sqrt(world scale): the Froude-scaled inputs shrink the sea
    // but shorten its periods by the same sqrt(s), and this holds them at the model sea's. Only the
    // evolution reads it - the accel/velocity signals below stay in spectrum time on purpose, so the
    // breaking criterion (accel as a fraction of g) does not change with the scale.
    const float wt = w * u_timeSeconds * u_oceanParams10.y;
    const vec2 ep = vec2(cos(wt), sin(wt));
    const vec2 termP = cmul(h0p, ep);                              // h0(k) e^{+iwt}
    const vec2 termM = cmul(vec2(h0m.x, -h0m.y), vec2(ep.x, -ep.y)); // h0*(-k) e^{-iwt}
    const vec2 ht = termP + termM;

    const vec2 kn = k > 1e-5 ? kv / k : vec2(0.0);
    const vec2 iht = vec2(-ht.y, ht.x); // i * h~

    // The 8 real signals' spectra, packed as 4 complex (re = first signal, im = second):
    //   P0 = h        + i Dx        P1 = Dz       + i dh/dx
    //   P2 = dh/dz    + i dDx/dx    P3 = dDz/dz   + i dDx/dz
    // Dx = i kx/k h~, Dz = i kz/k h~ (choppy displacement, Tessendorf eq. 44 NEGATED: under this e^{+ikx}
    // transform the paper's -i sign converges water into the TROUGHS - sharp valleys, round crests; +i
    // pinches the crests, and the dD derivatives flip with it so the fold Jacobian stays consistent);
    // dh/dx = i kx h~; dDx/dx = -kx^2/k h~; dDz/dz = -kz^2/k h~; dDx/dz = -kx kz / k h~.
    const vec2 Dx    = iht * kn.x;
    const vec2 Dz    = iht * kn.y;
    const vec2 dhx   = iht * kv.x;
    const vec2 dhz   = iht * kv.y;
    const vec2 dDxx  = ht * -(kv.x * kn.x);
    const vec2 dDzz  = ht * -(kv.y * kn.y);
    const vec2 dDxz  = ht * -(kv.x * kn.y);

    const vec2 P0 = ht   + vec2(-Dx.y,   Dx.x);
    const vec2 P1 = Dz   + vec2(-dhx.y,  dhx.x);
    const vec2 P2 = dhz  + vec2(-dDxx.y, dDxx.x);
    const vec2 P3 = dDzz + vec2(-dDxz.y, dDxz.x);

    // Exact time derivatives per component: dh/dt~ = iw (termP - termM), d2h/dt2~ = -w^2 h~.
    // P4 packs (accel + i velocity); both Hermitian (time derivatives of a real field).
    const vec2 dHdt = w * vec2(-(termP.y - termM.y), termP.x - termM.x);
    const vec2 accel = -w * w * ht;
    const vec2 P4 = accel + vec2(-dHdt.y, dHdt.x);

    imageStore(out_spectrum, ivec3(xy, int(cascade * 3u + 0u)), vec4(P0, P1));
    imageStore(out_spectrum, ivec3(xy, int(cascade * 3u + 1u)), vec4(P2, P3));
    imageStore(out_spectrum, ivec3(xy, int(cascade * 3u + 2u)), vec4(P4, vec2(0.0)));
}
