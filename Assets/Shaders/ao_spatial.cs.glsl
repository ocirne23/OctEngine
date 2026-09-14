#version 460

#extension GL_GOOGLE_include_directive : enable

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_depth;   // full-res hardware depth
layout (binding = 2) uniform sampler2D u_normal;  // full-res world normal
layout (binding = 3) uniform sampler2D u_accumAO; // temporally-accumulated AO (rgb = bent normal, a = AO, half-res)
layout (binding = 4, rgba16f) uniform restrict writeonly image2D u_aoOut;

layout (push_constant) uniform PC
{
    uint  aoWidth;
    uint  aoHeight;
    int   radius;     // kernel half-extent (texels)
    uint  viewIndex;  // view to reconstruct in (0 = centre/desktop, 1 = left eye, 2 = right eye)
} pc;

void main()
{
    g_viewIndex = int(pc.viewIndex);
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.aoWidth) || px.y >= int(pc.aoHeight))
        return;

    const vec2 texel = 1.0 / vec2(pc.aoWidth, pc.aoHeight);
    const vec2 uv = (vec2(px) + 0.5) * texel;

    const float depth = texture(u_depth, uv).r;
    if (depth <= 0.0) // background (reversed-Z: far/cleared = 0)
    {
        // Dilate geometry AO into background texels. The trace writes background as (bentN=+Z, ao=1);
        // the forward pass upsamples this image with plain bilinear, so background texels bleed into
        // silhouette edge pixels and brighten their GI term (bright aliased rim). Averaging the nearby
        // geometry samples instead makes the blend across the silhouette a no-op.
        const int r = max(pc.radius, 1);
        vec4 sum = vec4(0.0);
        float wsum = 0.0;
        for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
        {
            const vec2 nuv = uv + vec2(dx, dy) * texel;
            if (texture(u_depth, nuv).r <= 0.0) continue;
            const float ws = exp(-float(dx * dx + dy * dy) / 8.0);
            sum  += texture(u_accumAO, nuv) * ws;
            wsum += ws;
        }
        vec4 bg;
        if (wsum > 1e-4)
        {
            bg = sum / wsum;
            bg.xyz = (dot(bg.xyz, bg.xyz) > 1e-8) ? normalize(bg.xyz) : vec3(0.0, 0.0, 1.0);
        }
        else
            bg = texture(u_accumAO, uv); // no geometry nearby: keep the background value
        imageStore(u_aoOut, px, bg);
        return;
    }

    const vec3 centerPos = worldPosFromDepth(uv, depth);
    const vec3 centerN   = normalize(texture(u_normal, uv).xyz);
    const float viewDist = length(centerPos - u_viewPos);
    const float sigmaZ   = max(0.05 * viewDist, 0.02);

    vec4 sum = vec4(0.0); // .a = AO, .xyz = bent normal
    float wsum = 0.0;
    for (int dy = -pc.radius; dy <= pc.radius; ++dy)
    for (int dx = -pc.radius; dx <= pc.radius; ++dx)
    {
        const vec2 nuv = uv + vec2(dx, dy) * texel;
        const float nDepth = texture(u_depth, nuv).r;
        if (nDepth <= 0.0) continue;

        const vec3 nPos = worldPosFromDepth(nuv, nDepth);
        const vec3 nN   = normalize(texture(u_normal, nuv).xyz);

        const float dz = length(nPos - centerPos);
        const float wz = exp(-(dz * dz) / (2.0 * sigmaZ * sigmaZ));
        const float wn = pow(max(dot(centerN, nN), 0.0), 32.0);
        const float ws = exp(-float(dx * dx + dy * dy) / 8.0);
        const float w  = wz * wn * ws;

        sum  += texture(u_accumAO, nuv) * w;
        wsum += w;
    }

    vec4 ao = (wsum > 1e-4) ? sum / wsum : texture(u_accumAO, uv);
    ao.xyz = (dot(ao.xyz, ao.xyz) > 1e-8) ? normalize(ao.xyz) : centerN;
    imageStore(u_aoOut, px, ao);
}
