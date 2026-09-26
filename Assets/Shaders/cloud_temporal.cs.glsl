#version 460

// Cloud temporal accumulation (half res): reprojects last frame's accumulated clouds to this frame's march
// and blends. The reprojection point is the transmittance-weighted cloud distance, moved back by this
// frame's wind displacement (the field travels). History is rejected when its cloud distance differs
// (a different layer / disocclusion) and clamped to this pixel's 3x3 neighbourhood (camera motion through
// the medium has parallax at every depth, which one reprojection distance cannot follow).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_curColor;
layout (binding = 2) uniform sampler2D u_curDepth;
layout (binding = 3) uniform sampler2D u_histColor;
layout (binding = 4) uniform sampler2D u_histDepth;
layout (binding = 5, rgba16f) uniform writeonly image2D u_outColor;
layout (binding = 6, rgba16f) uniform writeonly image2D u_outDepth;

layout (push_constant) uniform CloudPC
{
    uint u_viewIndex;
    uint u_width;
    uint u_height;
    uint u_pad;
};

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(u_width) || px.y >= int(u_height))
        return;
    g_viewIndex = int(u_viewIndex);

    const ivec2 last = ivec2(u_width, u_height) - 1;
    const vec4 cur = texelFetch(u_curColor, px, 0);
    const vec4 curDepth = texelFetch(u_curDepth, px, 0);
    vec4 lo = cur, hi = cur;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        if (x == 0 && y == 0)
            continue;
        const vec4 n = texelFetch(u_curColor, clamp(px + ivec2(x, y), ivec2(0), last), 0);
        lo = min(lo, n);
        hi = max(hi, n);
    }

    const vec2 uv = (vec2(px * 2) + 1.0) * u_screenSize.zw;
    const vec2 uvJ = uv - taaJitterUv(u_taaJitter.xy);
    const vec3 dir = normalize(viewRelFromDepth(uvJ, 1.0));
    const float tCloud = exp2(curDepth.y);
    const vec3 prevWorld = u_viewPos + dir * tCloud - u_cloudWind.xyz;
    float clipW;
    // Last frame marched the direction of (texel uv - ITS jitter), so the texel holding this point sits at
    // the unjittered projection + last frame's jitter.
    const vec2 prevUv = prevScreenUV(prevWorld, clipW) + taaJitterUv(u_taaJitter.zw);
    const vec2 prevVp = (prevUv - u_viewportRect.xy) / u_viewportRect.zw;
    bool valid = clipW > 0.0 && all(greaterThanEqual(prevVp, vec2(0.0))) && all(lessThanEqual(prevVp, vec2(1.0)));

    vec4 result = cur;
    if (valid)
    {
        vec4 hist = textureLod(u_histColor, prevUv, 0.0);
        const vec4 histDepth = textureLod(u_histDepth, prevUv, 0.0);
        // Different cloud distance (0.3 in log2 = 23 %) = a different surface of the medium, unless
        // neither frame saw any cloud there.
        valid = abs(histDepth.y - curDepth.y) < 0.3 || (cur.a > 0.995 && hist.a > 0.995);
        if (valid)
        {
            const vec4 pad = (hi - lo) * 0.1 + vec4(0.002);
            hist = clamp(hist, lo - pad, hi + pad);
            result = mix(cur, hist, u_cloudMarch1.z);
        }
    }
    if (u_cloudMarch1.w == 3.0)
        result = vec4(valid ? vec3(0.0, 0.3, 0.0) : vec3(0.6, 0.0, 0.0), 0.0);

    imageStore(u_outColor, px, result);
    imageStore(u_outDepth, px, curDepth);
}
