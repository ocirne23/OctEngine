// The half-res accumulated clouds (cloud_temporal.cs.glsl) upsampled to a full-res pixel, depth-aware. Shared
// by the two passes that composite the clouds: cloud_apply.fs.glsl (fog off) and vol_apply.fs.glsl (fog on,
// which composites the clouds INSIDE the fog). Requires ubo.inc.glsl.
//
// The four half-res texels around the pixel, bilinear, and:
//  - a texel whose FIRST cloud lies behind this pixel's surface contributes "no cloud" (its march ran on to a
//    farther surface of the 2x2 block);
//  - a texel whose march ended at a different distance than this pixel's surface is trusted less.
// Returns (in-scatter, transmittance); logCloudDist = log2 of the transmittance-weighted cloud distance.

#ifndef CLOUD_UPSAMPLE_INC_GLSL
#define CLOUD_UPSAMPLE_INC_GLSL

vec4 cloudUpsample(sampler2D cloudColor, sampler2D cloudDepth, vec2 fragCoord, float logScene, out float logCloudDist)
{
    const float logMax = log2(u_cloudMarch0.y);
    const ivec2 size = textureSize(cloudColor, 0);
    const vec2 hp = fragCoord * 0.5 - 0.5;
    const ivec2 base = ivec2(floor(hp));
    const vec2 f = hp - vec2(base);
    const float logSceneC = min(logScene, logMax);
    vec4 sum = vec4(0.0);
    float wSum = 0.0;
    float logDistSum = 0.0;
    float distWeight = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const ivec2 o = ivec2(i & 1, i >> 1);
        const ivec2 q = clamp(base + o, ivec2(0), size - 1);
        vec4 c = texelFetch(cloudColor, q, 0);
        const vec4 d = texelFetch(cloudDepth, q, 0);
        const vec2 bw = mix(1.0 - f, f, vec2(o));
        float w = bw.x * bw.y;
        if (logScene < d.x - 0.02)
            c = vec4(0.0, 0.0, 0.0, 1.0);
        w *= 1.0 / (1.0 + 16.0 * abs(d.z - logSceneC));
        sum += c * w;
        wSum += w;
        // The distance of the texels that hold cloud, weighted by how much they hold.
        const float cw = w * (1.0 - c.a);
        logDistSum += d.y * cw;
        distWeight += cw;
    }
    logCloudDist = distWeight > 1e-6 ? logDistSum / distWeight : logSceneC;
    return wSum > 1e-6 ? sum / wSum : vec4(0.0, 0.0, 0.0, 1.0);
}

#endif
