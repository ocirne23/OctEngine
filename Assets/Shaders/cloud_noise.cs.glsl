#version 460

// Generates the cloud noise textures ONCE (CloudPipeline::generateNoise; see clouds.inc.glsl for the
// channel layout). Every field TILES: the Worley feature points and the Perlin gradients are hashed from
// the lattice cell wrapped by the period, so the textures repeat seamlessly under REPEAT addressing.
// NOISE_3D: binding 0 is a 3D image (modes 0 base, 1 detail); else a 2D image (modes 2 weather, 3 curl).

layout (local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#ifdef NOISE_3D
layout (binding = 0, rgba8) uniform writeonly image3D u_out;
#else
layout (binding = 0, rgba8) uniform writeonly image2D u_out;
#endif

layout (push_constant) uniform NoisePC
{
    uint u_mode; // 0 base, 1 detail, 2 weather, 3 curl
    uint u_size;
};

uvec3 pcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 hash33(ivec3 c, int seed)
{
    return vec3(pcg3d(uvec3(c + ivec3(seed * 157, seed * 311, seed * 541)))) * (1.0 / 4294967295.0);
}
ivec3 wrapCell(ivec3 c, int period) { return ((c % period) + period) % period; }

// Inverted Worley (1 at a feature point) of p in cell units [0, period)^3.
float worley(vec3 p, int period, int seed)
{
    const ivec3 cell = ivec3(floor(p));
    const vec3 f = p - vec3(cell);
    float d2 = 1e9;
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        const ivec3 o = ivec3(x, y, z);
        const vec3 fp = vec3(o) + hash33(wrapCell(cell + o, period), seed);
        const vec3 d = fp - f;
        d2 = min(d2, dot(d, d));
    }
    return 1.0 - clamp(sqrt(d2), 0.0, 1.0);
}
// Three octaves at period, 2 * period and 4 * period; uvw in [0, 1)^3.
float worleyFbm(vec3 uvw, int period, int seed)
{
    return worley(uvw * float(period), period, seed) * 0.625
         + worley(uvw * float(period * 2), period * 2, seed + 1) * 0.25
         + worley(uvw * float(period * 4), period * 4, seed + 2) * 0.125;
}

// Tileable gradient noise in [-1, 1] of p in cell units.
float perlin(vec3 p, int period, int seed)
{
    const ivec3 i = ivec3(floor(p));
    const vec3 f = p - vec3(i);
    const vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n[8];
    for (int k = 0; k < 8; ++k)
    {
        const ivec3 o = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const vec3 g = normalize(hash33(wrapCell(i + o, period), seed) * 2.0 - 1.0 + 1e-6);
        n[k] = dot(g, f - vec3(o));
    }
    const float x0 = mix(n[0], n[1], u.x), x1 = mix(n[2], n[3], u.x);
    const float x2 = mix(n[4], n[5], u.x), x3 = mix(n[6], n[7], u.x);
    return mix(mix(x0, x1, u.y), mix(x2, x3, u.y), u.z) * 1.5;
}
// fBm in [0, 1]: each octave doubles the frequency AND the period, so the sum still tiles.
float perlinFbm(vec3 uvw, int period, int octaves, int seed)
{
    float v = 0.0, a = 0.5, norm = 0.0;
    for (int o = 0; o < octaves; ++o)
    {
        const int per = period << o;
        v += a * perlin(uvw * float(per), per, seed + o);
        norm += a;
        a *= 0.5;
    }
    return clamp(v / norm * 0.5 + 0.5, 0.0, 1.0);
}

void main()
{
    const uvec3 id = gl_GlobalInvocationID;
#ifdef NOISE_3D
    if (any(greaterThanEqual(id, uvec3(u_size))))
        return;
    const vec3 uvw = (vec3(id) + 0.5) / float(u_size);
    vec4 result;
    if (u_mode == 0u)
    {
        // Perlin-Worley: billowy Perlin, dilated by the low Worley fBm (Schneider, GPU Pro 7).
        const float wf = worleyFbm(uvw, 4, 11);
        const float p = perlinFbm(uvw, 4, 5, 3);
        const float pw = wf + p * (1.0 - wf);
        result = vec4(pw, wf, worleyFbm(uvw, 8, 21), worleyFbm(uvw, 16, 31));
    }
    else
    {
        result = vec4(worleyFbm(uvw, 2, 41), worleyFbm(uvw, 4, 51), worleyFbm(uvw, 8, 61), 1.0);
    }
    imageStore(u_out, ivec3(id), result);
#else
    if (id.z != 0u || any(greaterThanEqual(id.xy, uvec2(u_size))))
        return;
    const vec3 uvw = vec3((vec2(id.xy) + 0.5) / float(u_size), 0.37);
    vec4 result;
    if (u_mode == 2u)
    {
        // Contrast-stretched: the fBm clusters around 0.5, the coverage wants both clear and dense regions.
        const float coverage = smoothstep(0.2, 0.8, perlinFbm(uvw, 6, 5, 71));
        const float type = smoothstep(0.25, 0.75, perlinFbm(uvw, 3, 3, 81));
        const float density = smoothstep(0.15, 0.85, perlinFbm(uvw, 12, 3, 91));
        const float towerHeight = smoothstep(0.2, 0.8, perlinFbm(uvw, 16, 4, 111)); // per-column top height
        result = vec4(coverage, type, density, towerHeight);
    }
    else
    {
        // Curl of a tileable potential by central differences (one texel), normalized to [-1, 1].
        const float e = 1.0 / float(u_size);
        const float dx = perlinFbm(uvw + vec3(e, 0.0, 0.0), 4, 3, 101) - perlinFbm(uvw - vec3(e, 0.0, 0.0), 4, 3, 101);
        const float dy = perlinFbm(uvw + vec3(0.0, e, 0.0), 4, 3, 101) - perlinFbm(uvw - vec3(0.0, e, 0.0), 4, 3, 101);
        vec2 curl = vec2(dy, -dx) / (2.0 * e);
        curl /= length(curl) + 4.0; // soft normalize: strong swirls saturate toward length 1
        result = vec4(curl * 0.5 + 0.5, 0.0, 1.0);
    }
    imageStore(u_out, ivec2(id.xy), result);
#endif
}
