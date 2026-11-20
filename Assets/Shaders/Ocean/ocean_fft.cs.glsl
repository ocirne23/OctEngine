#version 460

// FFT ocean, step 2: radix-2 inverse FFT, one axis per pipeline - rows first, then columns (OCEAN_FFT_COLUMNS). One
// workgroup per line per CASCADE: it transforms the cascade's two RGBA16F layers together (they share every twiddle),
// each texel TWO complex signals (rg / ba), so a butterfly works on vec4s in fp32 (only the image load / store round to
// half). The inverse uses e^{+i...} with no 1/N scale - exactly Tessendorf's h(x) = sum h~(k) e^{+ikx}.
//
// In place (decimation in time, bit-reversed load) in shared memory: at half-size h, thread t owns the pair (i, i + h),
// i = (t / h) 2h + t mod h, and no other thread touches it, so a stage needs no second buffer. Two layers per thread
// cost 33 registers against 24 for one layer per thread, with half the threads: less in total.
//   - THE INPUT IS BAND-LIMITED: only |m| < N/4 is non-zero (ocean_spectrum.cs.glsl), and only that block is stored
//     (index = m + N/4). FFT index t carries m = t (t < N/2) and t + N/2 carries m = t - N/2, so of a thread's two inputs
//     exactly ONE is in the band: one load per layer, not two. The row pass runs on the block's N/2 rows alone (a zero
//     row transforms to zero), and its N x N/2 result is the column pass's band.
//   - The first stage (h = 1, twiddle 1) is that load: the pair is (v, v) or (v, -v).
//   - The last stage (h = N/2) writes its pair straight to the image: they are the thread's own two texels.
// So log2(N) - 2 stages go through shared memory.
//
// The column pass writes THE MAPS (mip 0), the layout every sampler reads (ocean_wave.inc.glsl):
//   layer c                    : (Dx, h, Dz, 0)                           displacement
//   layer OCEAN_CASCADES + c   : (dh/dx, dh/dz, dhx^2, dhz^2)             slopes + their second moments (LEAN)
//   layer 2 OCEAN_CASCADES + c : (dDx/dx, dDz/dz, dDx/dz, d2h/dt2)        the fold Jacobian's terms + the acceleration
// The acceleration is g (dDx/dx + dDz/dz): see ocean_spectrum.cs.glsl.

layout (local_size_x = OCEAN_FFT_SIZE / 2, local_size_y = 1, local_size_z = 1) in;

layout (binding = 0, rgba16f) uniform readonly image2DArray in_data;
#ifdef OCEAN_FFT_COLUMNS
layout (binding = 1, rgba16f) uniform writeonly image2DArray out_maps;
#else
layout (binding = 1, rgba16f) uniform writeonly image2DArray out_data;
#endif

const uint N = OCEAN_FFT_SIZE;
shared vec4 s_data[2u * N]; // the cascade's first layer, then its second

vec4 cmul4(vec4 v, vec2 w) // two complex numbers times one twiddle
{
    return vec4(v.x * w.x - v.y * w.y, v.x * w.y + v.y * w.x,
                v.z * w.x - v.w * w.y, v.z * w.y + v.w * w.x);
}

void main()
{
    const uint t       = gl_LocalInvocationID.x; // [0, N/2)
    const uint line    = gl_WorkGroupID.x;
    const uint cascade = gl_WorkGroupID.y;

    {
        const bool low = t < N / 4u;                     // m = t is in the band; else m = t - N/2 is
        const uint band = low ? t + N / 4u : t - N / 4u; // its index in the stored block
#ifdef OCEAN_FFT_COLUMNS
        const ivec2 p = ivec2(int(line), int(band));
#else
        const ivec2 p = ivec2(int(band), int(line));
#endif
        const uint r = bitfieldReverse(t) >> (32u - uint(findMSB(N))); // even: t < N/2
        const float sgn = low ? 1.0 : -1.0;
        const vec4 a = imageLoad(in_data, ivec3(p, int(cascade * 2u)));
        s_data[r] = a; s_data[r + 1u] = a * sgn;
        const vec4 b = imageLoad(in_data, ivec3(p, int(cascade * 2u + 1u)));
        s_data[N + r] = b; s_data[N + r + 1u] = b * sgn;
    }

    for (uint h = 2u; h < N / 2u; h <<= 1u)
    {
        barrier();
        const uint q = t & (h - 1u); // t mod h
        const float angle = 6.2831853 * float(q) / float(2u * h); // +i: inverse transform
        const vec2 w = vec2(cos(angle), sin(angle));
        const uint i = ((t - q) << 1u) + q; // (t / h) * 2h + q

        const vec4 a0 = s_data[i];
        const vec4 a1 = cmul4(s_data[i + h], w);
        s_data[i]     = a0 + a1;
        s_data[i + h] = a0 - a1;
        const vec4 b0 = s_data[N + i];
        const vec4 b1 = cmul4(s_data[N + i + h], w);
        s_data[N + i]     = b0 + b1;
        s_data[N + i + h] = b0 - b1;
    }
    barrier();

    // The last stage: the pair (t, t + N/2), one layer after the other.
    const float angle = 6.2831853 * float(t) / float(N);
    const vec2 w = vec2(cos(angle), sin(angle));
#ifdef OCEAN_FFT_COLUMNS
    const ivec2 p0 = ivec2(int(line), int(t));
    const ivec2 p1 = ivec2(int(line), int(t + N / 2u));
    vec2 dxz; // dDx/dz of the two texels: the first layer's one value the fold layer takes
    {
        const vec4 a0 = s_data[t];
        const vec4 a1 = cmul4(s_data[t + N / 2u], w);
        const vec4 lo = a0 + a1, hi = a0 - a1; // (Dx, h, Dz, dDx/dz)
        imageStore(out_maps, ivec3(p0, int(cascade)), vec4(lo.xyz, 0.0));
        imageStore(out_maps, ivec3(p1, int(cascade)), vec4(hi.xyz, 0.0));
        dxz = vec2(lo.w, hi.w);
    }
    {
        const vec4 b0 = s_data[N + t];
        const vec4 b1 = cmul4(s_data[N + t + N / 2u], w);
        const vec4 lo = b0 + b1, hi = b0 - b1; // (dh/dx, dh/dz, dDx/dx, dDz/dz)
        imageStore(out_maps, ivec3(p0, OCEAN_CASCADES + int(cascade)),     vec4(lo.xy, lo.xy * lo.xy));
        imageStore(out_maps, ivec3(p1, OCEAN_CASCADES + int(cascade)),     vec4(hi.xy, hi.xy * hi.xy));
        imageStore(out_maps, ivec3(p0, 2 * OCEAN_CASCADES + int(cascade)), vec4(lo.zw, dxz.x, 9.81 * (lo.z + lo.w)));
        imageStore(out_maps, ivec3(p1, 2 * OCEAN_CASCADES + int(cascade)), vec4(hi.zw, dxz.y, 9.81 * (hi.z + hi.w)));
    }
#else
    {
        const vec4 a0 = s_data[t];
        const vec4 a1 = cmul4(s_data[t + N / 2u], w);
        imageStore(out_data, ivec3(int(t), int(line), int(cascade * 2u)),          a0 + a1);
        imageStore(out_data, ivec3(int(t + N / 2u), int(line), int(cascade * 2u)), a0 - a1);
    }
    {
        const vec4 b0 = s_data[N + t];
        const vec4 b1 = cmul4(s_data[N + t + N / 2u], w);
        imageStore(out_data, ivec3(int(t), int(line), int(cascade * 2u + 1u)),          b0 + b1);
        imageStore(out_data, ivec3(int(t + N / 2u), int(line), int(cascade * 2u + 1u)), b0 - b1);
    }
#endif
}
