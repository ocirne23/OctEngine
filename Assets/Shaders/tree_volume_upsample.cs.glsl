#version 460

// FAR-TREE UPSAMPLE ("Far half res", TreeVolumePipeline): the half-res temporal result -> the full-res pair every
// composite reads (tree_volume_apply / vol_apply / cloud_apply): in-scatter + transmittance, and the weighted mean
// distance (R16F, m). Depth-aware, as cloud_upsample.inc.glsl: the four half-res texels around the pixel, bilinear,
// and
//  - a texel whose FIRST tree lies behind this pixel's surface contributes "no tree" (its march ran on to a farther
//    surface of the 2x2 block - the march takes the block's farthest);
//  - a texel whose march ended at a different distance than this pixel's surface is trusted less.

#include "shared.inc.glsl"

layout (local_size_x = 8, local_size_y = 8) in;

layout (binding = 1) uniform sampler2D u_sceneDepth;
layout (binding = 2) uniform sampler2D u_treeColor; // half res: in-scatter, T
layout (binding = 3) uniform sampler2D u_treeDepth; // half res: log2 of the first tree, the weighted mean, the limit
layout (binding = 4, rgba16f) uniform writeonly image2D u_outColor;
layout (binding = 5, r16f) uniform writeonly image2D u_outDistance;

// PC_DECL_ / PC_CONSTS: the march's lockable values (TreeVolumePipeline::registerPushFields, PushFields.ixx).
layout (push_constant) uniform PC
{
    uvec2 pc_size;     // the render size
    PC_DECL_vol_rMax;  // float: the march's max distance (the far volume's end)
};
PC_CONSTS

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(uvec2(px), pc_size)))
        return;
    const vec2 uv = (vec2(px) + 0.5) / vec2(pc_size);
    const vec2 vpUv = (uv - u_viewportRect.xy) / u_viewportRect.zw;
    if (any(lessThan(vpUv, vec2(0.0))) || any(greaterThan(vpUv, vec2(1.0))))
    {
        imageStore(u_outColor, px, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    const float depth = texelFetch(u_sceneDepth, px, 0).r;
    const float logMax = log2(max(pc_vol_rMax, 1.0));
    const float logScene = depth > 0.0 ? min(log2(max(length(viewRelFromDepth(uv - taaJitterUv(u_taaJitter.xy), depth)), 1.0)), logMax) : logMax;

    const ivec2 size = textureSize(u_treeColor, 0);
    const vec2 hp = vec2(px) * 0.5 - 0.25; // this pixel's centre in half-res texel space (texel centres at +0.5)
    const ivec2 base = ivec2(floor(hp));
    const vec2 f = hp - vec2(base);
    vec4 sum = vec4(0.0);
    float wSum = 0.0, logDistSum = 0.0, distWeight = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const ivec2 o = ivec2(i & 1, i >> 1);
        const ivec2 q = clamp(base + o, ivec2(0), size - 1);
        vec4 c = texelFetch(u_treeColor, q, 0);
        const vec4 d = texelFetch(u_treeDepth, q, 0);
        const vec2 bw = mix(1.0 - f, f, vec2(o));
        float w = bw.x * bw.y;
        if (logScene < d.x - 0.02)
            c = vec4(0.0, 0.0, 0.0, 1.0);
        w *= 1.0 / (1.0 + 16.0 * abs(d.z - logScene));
        sum += c * w;
        wSum += w;
        const float cw = w * (1.0 - c.a); // the distance of the texels that hold trees, weighted by how much
        logDistSum += d.y * cw;
        distWeight += cw;
    }
    imageStore(u_outColor, px, wSum > 1e-6 ? sum / wSum : vec4(0.0, 0.0, 0.0, 1.0));
    imageStore(u_outDistance, px, vec4(exp2(distWeight > 1e-6 ? logDistSum / distWeight : logScene)));
}
