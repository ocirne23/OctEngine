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

//
// TREE_TEMPORAL (TreeVolumePipeline's instance): the same pass over the FAR-TREE march (tree_volume_march.cs) - no
// wind; the history weight ("Trees/Far temporal blend") and the march's max distance from the push constant, the
// scale (TREE_TEMPORAL_SCALE: 1 = full res, 2 = "Far half res") and the checkerboard (TREE_TEMPORAL_CHECKER, "Far
// pixel skip") baked, instead of the clouds' baked / UBO values; u_outMeanDistance only at full res (at half res the
// upsample writes the full-res pair).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef TREE_TEMPORAL
#undef CLOUD_CHECKERBOARD // the preamble's cloud toggles do not apply
#undef CLOUD_DEBUG_MODE
#endif
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
#ifdef TREE_TEMPORAL
// The fog apply's tree layer depth (m), as the march alone writes it (u_outDepth is the next frame's history).
layout (binding = 8, r16f) uniform writeonly image2D u_outMeanDistance;
#endif

// The neighbourhood clamp takes only the neighbours whose march LIMIT (log2) is within this of the pixel's own:
// at a silhouette a texel over the near surface holds "no cloud" (its march stopped under the clouds), and in
// the range of a sky texel it let the history fade to clear sky - a sky-coloured outline around every object.
const float LIMIT_MATCH = 0.15;

#ifdef TREE_TEMPORAL
// The trees' push block: TreeVolumePipeline's TemporalPush (pc_vol_rMax, the march's max distance: lockable).
#include "push.generated.glsl"
#define u_viewIndex pc_viewIndex
#define u_width pc_width
#define u_height pc_height
#define u_historyWeight pc_historyWeight
#else
layout (push_constant) uniform CloudPC
{
    uint u_viewIndex;
    uint u_width;
    uint u_height;
    uint u_pad;
};
#endif

// The trees' variant BAKES its scale (1, or 2 at "Far half res") and checkerboard (TreeVolumePipeline compiles one per
// setting); at full res it also writes u_outMeanDistance.
#ifndef TREE_TEMPORAL_SCALE
#define TREE_TEMPORAL_SCALE 1
#endif
#ifndef TREE_TEMPORAL_CHECKER
#define TREE_TEMPORAL_CHECKER 0
#endif
#if defined(TREE_TEMPORAL)
#define TT_CHECKER (TREE_TEMPORAL_CHECKER != 0)
#define TT_SCALE TREE_TEMPORAL_SCALE
#define TT_MAX_DIST pc_vol_rMax
#elif defined(CLOUD_CHECKERBOARD)
#define TT_CHECKER true
#define TT_SCALE 2
#define TT_MAX_DIST u_clouds_maxDistance
#else
#define TT_CHECKER false
#define TT_SCALE 2
#define TT_MAX_DIST u_clouds_maxDistance
#endif

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(u_width) || px.y >= int(u_height))
        return;
    g_viewIndex = int(u_viewIndex);

    const ivec2 last = ivec2(u_width, u_height) - 1;
    const vec2 uv = (vec2(px * TT_SCALE) + 0.5 * float(TT_SCALE)) * u_screenSize.zw; // the block's centre
    const vec2 uvJ = uv - taaJitterUv(u_taaJitter.xy);
    vec4 cur;
    vec4 curDepth;
    vec4 lo, hi;
    // The neighbourhood's range of weighted cloud distance (log2): the history test (below).
    float dLo = 1e30, dHi = -1e30;
    if (TT_CHECKER && ((px.x + px.y + int(u_frameIndex)) & 1) == 0)
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
            const vec4 d = texelFetch(u_curDepth, q, 0);
            if (abs(d.z - curDepth.z) >= LIMIT_MATCH)
                continue;
            const vec4 n = texelFetch(u_curColor, q, 0);
            lo = min(lo, n);
            hi = max(hi, n);
            dLo = min(dLo, d.y);
            dHi = max(dHi, d.y);
        }
    }
    else if (TT_CHECKER)
    {
        // The 4 side neighbours (marched this frame), weighted by how well their march LIMIT matches this
        // pixel's own (the march's rule, from the scene depth): their average is the current value, their
        // nearest front and mean weighted distance its depth. A plain average let a neighbour over a near
        // surface - its march stopped under the clouds: "no cloud" - into a sky texel at every silhouette, a
        // clear-sky outline around it. The front comes only from neighbours that HOLD cloud: an empty march
        // stores its own limit as the front, so the near neighbour's limit became the front of cloud from the
        // sky neighbours, and the upsample let that cloud onto the near surface.
        const ivec2 full = px * TT_SCALE;
        const ivec2 lastFull = textureSize(u_sceneDepth, 0) - 1;
        const ivec2 corner = ivec2(TT_SCALE - 1); // the block's far corner (the same texel at full res)
        const float depth = min(min(texelFetch(u_sceneDepth, min(full, lastFull), 0).r, texelFetch(u_sceneDepth, min(full + ivec2(corner.x, 0), lastFull), 0).r),
                                min(texelFetch(u_sceneDepth, min(full + ivec2(0, corner.y), lastFull), 0).r, texelFetch(u_sceneDepth, min(full + corner, lastFull), 0).r));
        const float maxDist = TT_MAX_DIST;
        const float limit = depth > 0.0 ? min(length(viewRelFromDepth(uvJ, depth)), maxDist) : maxDist;
        const float logLimit = log2(max(limit, 1.0));
        cur = vec4(0.0);
        float wSum = 0.0;
        float front = 1e30;
        float weighted = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            const ivec2 o = i < 2 ? ivec2(i * 2 - 1, 0) : ivec2(0, i * 2 - 5);
            const ivec2 q = clamp(px + o, ivec2(0), last);
            const vec4 n = texelFetch(u_curColor, q, 0);
            const vec4 d = texelFetch(u_curDepth, q, 0);
            const float w = 1.0 / (1.0 + 16.0 * abs(d.z - logLimit));
            cur += n * w;
            weighted += d.y * w;
            wSum += w;
            if (n.a < 0.999 && abs(d.z - logLimit) < LIMIT_MATCH)
                front = min(front, d.x);
        }
        cur /= wSum;
        curDepth = vec4(min(front, logLimit), min(weighted / wSum, logLimit), logLimit, 0.0);
        // The clamp range: the matching neighbours only (the same leak through the history clamp), always
        // holding the current value.
        lo = cur;
        hi = cur;
        dLo = curDepth.y;
        dHi = curDepth.y;
        for (int i = 0; i < 4; ++i)
        {
            const ivec2 o = i < 2 ? ivec2(i * 2 - 1, 0) : ivec2(0, i * 2 - 5);
            const ivec2 q = clamp(px + o, ivec2(0), last);
            const vec4 d = texelFetch(u_curDepth, q, 0);
            if (abs(d.z - logLimit) >= LIMIT_MATCH)
                continue;
            const vec4 n = texelFetch(u_curColor, q, 0);
            lo = min(lo, n);
            hi = max(hi, n);
            dLo = min(dLo, d.y);
            dHi = max(dHi, d.y);
        }
    }
    else
    {
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
            const vec4 d = texelFetch(u_curDepth, q, 0);
            if (abs(d.z - curDepth.z) >= LIMIT_MATCH)
                continue;
            const vec4 n = texelFetch(u_curColor, q, 0);
            lo = min(lo, n);
            hi = max(hi, n);
            dLo = min(dLo, d.y);
            dHi = max(dHi, d.y);
        }
    }

    const vec3 dir = normalize(viewRelFromDepth(uvJ, 1.0));
    const float tCloud = exp2(curDepth.y);
#ifdef TREE_TEMPORAL
    const vec3 prevWorld = u_viewPos + dir * tCloud; // trees do not move
#else
    const vec3 prevWorld = u_viewPos + dir * tCloud - u_cloudsLive_windStep;
#endif
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
        // And the history's MARCH LIMIT must match this frame's (0.15 in log2 = 11 %): at a silhouette the
        // bilinear history fetch pulls in part of the sky neighbour's cloud, the neighbourhood clamp lets it
        // through (the sky neighbours hold cloud), and the feedback builds it up in the texel over the near
        // surface - whose first-cloud distance is that surface's own, so the upsample kept it: a cloud-coloured
        // outline on every edge against clouds, thicker the higher the blend. The sky keeps its history (both
        // limits are the max distance there).
        valid = ((histDepth.y >= dLo - 0.2 && histDepth.y <= dHi + 0.2) || (cur.a > 0.995 && hist.a > 0.995))
             && abs(histDepth.z - curDepth.z) < LIMIT_MATCH;
        if (valid)
        {
            const vec4 pad = (hi - lo) * 0.1 + vec4(0.002);
            hist = clamp(hist, lo - pad, hi + pad);
#ifdef TREE_TEMPORAL
            result = mix(cur, hist, u_historyWeight);
#else
            result = mix(cur, hist, u_clouds_temporalBlend);
#endif
        }
    }
#if CLOUD_DEBUG_MODE == 3
    result = vec4(valid ? vec3(0.0, 0.3, 0.0) : vec3(0.6, 0.0, 0.0), 0.0);
#endif

    imageStore(u_outColor, px, result);
    imageStore(u_outDepth, px, curDepth);
#if defined(TREE_TEMPORAL) && TREE_TEMPORAL_SCALE == 1
    imageStore(u_outMeanDistance, px, vec4(exp2(curDepth.y))); // at half res the upsample writes the full-res pair
#endif
}
