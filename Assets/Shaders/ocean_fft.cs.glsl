#version 460

// FFT ocean, pass 2/3: radix-2 Stockham inverse FFT (one axis per dispatch, push constant selects).
//
// One workgroup per row (or column) per layer; the whole transform runs in shared memory (all log2(N)
// stages in a single dispatch), ping-ponging between the two halves of one shared array. Each RGBA32F
// texel carries TWO packed complex signals (rg / ba) that share every twiddle, so the butterflies operate
// on vec4s. Stockham is auto-sorting: no bit-reversal pass. The inverse transform uses e^{+i...} with no
// 1/N scale - exactly Tessendorf's h(x) = sum h~(k) e^{+ikx}.

layout (local_size_x = OCEAN_FFT_SIZE / 2, local_size_y = 1, local_size_z = 1) in;

layout (binding = 0, rgba32f) uniform readonly image2DArray in_data;
layout (binding = 1, rgba32f) uniform writeonly image2DArray out_data;

layout (push_constant) uniform FftPC { uint u_axis; }; // 0 = rows (x), 1 = columns (y)

const uint N = OCEAN_FFT_SIZE;
shared vec4 s_data[2u * N]; // ping/pong halves

vec4 cmul4(vec4 v, vec2 w) // two complex numbers times one twiddle
{
    return vec4(v.x * w.x - v.y * w.y, v.x * w.y + v.y * w.x,
                v.z * w.x - v.w * w.y, v.z * w.y + v.w * w.x);
}

ivec3 texel(uint idx, uint line, uint layer)
{
    return u_axis == 0u ? ivec3(int(idx), int(line), int(layer)) : ivec3(int(line), int(idx), int(layer));
}

void main()
{
    const uint t     = gl_LocalInvocationID.x; // [0, N/2)
    const uint line  = gl_WorkGroupID.x;
    const uint layer = gl_WorkGroupID.y;

    s_data[t]          = imageLoad(in_data, texel(t, line, layer));
    s_data[t + N / 2u] = imageLoad(in_data, texel(t + N / 2u, line, layer));

    uint srcOff = 0u;
    uint dstOff = N;
    for (uint h = 1u; h < N; h <<= 1u)
    {
        barrier();
        const uint q = t & (h - 1u); // t mod h
        const float angle = 6.2831853 * float(q) / float(2u * h); // +i: inverse transform
        const vec2 w = vec2(cos(angle), sin(angle));

        const vec4 v0 = s_data[srcOff + t];
        const vec4 v1 = cmul4(s_data[srcOff + t + N / 2u], w);
        const uint j = ((t - q) << 1u) + q; // (t / h) * 2h + q
        s_data[dstOff + j]     = v0 + v1;
        s_data[dstOff + j + h] = v0 - v1;

        const uint tmp = srcOff; srcOff = dstOff; dstOff = tmp;
    }
    barrier();

    imageStore(out_data, texel(t, line, layer),          s_data[srcOff + t]);
    imageStore(out_data, texel(t + N / 2u, line, layer), s_data[srcOff + t + N / 2u]);
}
