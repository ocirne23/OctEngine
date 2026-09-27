#version 460

// Cloud temporal accumulation (half res): reprojects last frame's accumulated clouds to this frame's march
// and blends. The reprojection point is the transmittance-weighted cloud distance, moved back by this
// frame's wind displacement (the field travels). History is rejected when its cloud distance differs
// (a different layer / disocclusion) and clamped to this pixel's neighbourhood (camera motion through
// the medium has parallax at every depth, which one reprojection distance cannot follow).
//
// CLOUD_CHECKERBOARD (baked, "Quality/Checkerboard"): the march covered only this frame's checker parity
// ((x + y + frame) even). The parity flips every frame, and so does the frame slot, so the OTHER half of this
// slot's march images is never valid - only this frame's parity is read:
//  - a marched pixel: itself, its neighbourhood = its 4 diagonals (the same parity);
//  - the other pixels: the current value = the average of the 4 side neighbours (marched this frame), the
//    history = last frame's march AT this pixel (the parity flipped); the march limit from the scene depth.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifndef CLOUD_DEBUG_MODE
#define CLOUD_DEBUG_MODE 0 // baked by the preamble while the clouds are on ("Sky/Clouds/Quality/Debug mode")
#endif

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_curColor;
layout (binding = 2) uniform sampler2D u_curDepth;
layout (binding = 3) uniform sampler2D u_histColor;
layout (binding = 4) uniform sampler2D u_histDepth;
layout (binding = 5, rgba16f) uniform writeonly image2D u_outColor;
layout (binding = 6, rgba16f) uniform writeonly image2D u_outDepth;
layout (binding = 7) uniform sampler2D u_sceneDepth; // the checkerboard's unmarched pixels: their march limit

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
    const vec2 uv = (vec2(px * 2) + 1.0) * u_screenSize.zw;
    const vec2 uvJ = uv - taaJitterUv(u_taaJitter.xy);
    vec4 cur;
    vec4 curDepth;
    vec4 lo, hi;
    // The neighbourhood's range of weighted cloud distance (log2): the history test (below).
    float dLo = 1e30, dHi = -1e30;
#ifdef CLOUD_CHECKERBOARD
    if (((px.x + px.y + int(u_frameIndex)) & 1) == 0)
    {
        cur = texelFetch(u_curColor, px, 0);
        curDepth = texelFetch(u_curDepth, px, 0);
        lo = cur;
        hi = cur;
        dLo = curDepth.y;
        dHi = curDepth.y;
        for (int i = 0; i < 4; ++i) // the diagonals: the same parity, marched this frame
        {
            const ivec2 q = clamp(px + ivec2((i & 1) * 2 - 1, (i >> 1) * 2 - 1), ivec2(0), last);
            const vec4 n = texelFetch(u_curColor, q, 0);
            const float d = texelFetch(u_curDepth, q, 0).y;
            lo = min(lo, n);
            hi = max(hi, n);
            dLo = min(dLo, d);
            dHi = max(dHi, d);
        }
    }
    else
    {
        // The 4 side neighbours (marched this frame): their average is the current value, their nearest
        // front and mean weighted distance its depth. The limit is this pixel's own (the march's rule).
        cur = vec4(0.0);
        lo = vec4(1e30);
        hi = vec4(-1e30);
        float front = 1e30;
        float weighted = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            const ivec2 o = i < 2 ? ivec2(i * 2 - 1, 0) : ivec2(0, i * 2 - 5);
            const ivec2 q = clamp(px + o, ivec2(0), last);
            const vec4 n = texelFetch(u_curColor, q, 0);
            const vec4 d = texelFetch(u_curDepth, q, 0);
            cur += n;
            lo = min(lo, n);
            hi = max(hi, n);
            front = min(front, d.x);
            weighted += d.y;
            dLo = min(dLo, d.y);
            dHi = max(dHi, d.y);
        }
        cur *= 0.25;
        const ivec2 full = px * 2;
        const ivec2 lastFull = textureSize(u_sceneDepth, 0) - 1;
        const float depth = min(min(texelFetch(u_sceneDepth, min(full, lastFull), 0).r, texelFetch(u_sceneDepth, min(full + ivec2(1, 0), lastFull), 0).r),
                                min(texelFetch(u_sceneDepth, min(full + ivec2(0, 1), lastFull), 0).r, texelFetch(u_sceneDepth, min(full + ivec2(1, 1), lastFull), 0).r));
        const float maxDist = u_cloudMarch0.y;
        const float limit = depth > 0.0 ? min(length(viewRelFromDepth(uvJ, depth)), maxDist) : maxDist;
        const float logLimit = log2(max(limit, 1.0));
        curDepth = vec4(min(front, logLimit), min(weighted * 0.25, logLimit), logLimit, 0.0);
    }
#else
    cur = texelFetch(u_curColor, px, 0);
    curDepth = texelFetch(u_curDepth, px, 0);
    lo = cur;
    hi = cur;
    dLo = curDepth.y;
    dHi = curDepth.y;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        if (x == 0 && y == 0)
            continue;
        const ivec2 q = clamp(px + ivec2(x, y), ivec2(0), last);
        const vec4 n = texelFetch(u_curColor, q, 0);
        const float d = texelFetch(u_curDepth, q, 0).y;
        lo = min(lo, n);
        hi = max(hi, n);
        dLo = min(dLo, d);
        dHi = max(dHi, d);
    }
#endif

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
        // A history cloud distance OUTSIDE this frame's neighbourhood range (padded 0.2 in log2 = 15 %) is a
        // different surface of the medium - a disocclusion - unless neither frame saw any cloud there. Not the
        // pixel's own distance: a ray that runs flat through the layer passes clouds at very different
        // distances, so its one weighted distance jumps with the step jitter every frame, and a per-pixel
        // test rejected the history exactly where the raw march is noisiest.
        valid = (histDepth.y >= dLo - 0.2 && histDepth.y <= dHi + 0.2) || (cur.a > 0.995 && hist.a > 0.995);
        if (valid)
        {
            const vec4 pad = (hi - lo) * 0.1 + vec4(0.002);
            hist = clamp(hist, lo - pad, hi + pad);
            result = mix(cur, hist, u_cloudMarch1.z);
        }
    }
#if CLOUD_DEBUG_MODE == 3
    result = vec4(valid ? vec3(0.0, 0.3, 0.0) : vec3(0.6, 0.0, 0.0), 0.0);
#endif

    imageStore(u_outColor, px, result);
    imageStore(u_outDepth, px, curDepth);
}
