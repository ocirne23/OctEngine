#version 460

#extension GL_GOOGLE_include_directive : enable

// Temporal anti-aliasing resolve. One full-resolution compute pass per frame:
//   - reconstruct this pixel's world position from the (jittered) camera depth,
//   - reproject it into last frame's screen via u_prevMvp to fetch the history color,
//   - clamp the history to the current 3x3 neighborhood colour box (variance clipping) to suppress ghosting,
//   - reject history on disocclusion (reprojected depth mismatch) or when it lands outside the viewport,
//   - blend current and (clamped) history with a fixed feedback weight.
// The camera projection is jittered by a sub-pixel offset each frame (see Renderer::beginFrame); accumulating
// the jittered samples over time is what yields the anti-aliasing. Resolve happens in the scene-colour space
// (swapchain format), so this is an LDR resolve. The result is both the displayed image (composited into the
// swapchain) and next frame's history (ping-ponged per frame-in-flight, like the RTAO/GI buffers).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_currentColor;     // this frame's scene colour (jittered render)
layout (binding = 2) uniform sampler2D u_historyColor;     // last frame's resolved colour (reprojected)
layout (binding = 3) uniform sampler2D u_gbufferDepth;     // this frame's camera depth
layout (binding = 4) uniform sampler2D u_prevGbufferDepth; // last frame's camera depth (disocclusion test)
layout (binding = 5, rgba16f) uniform restrict writeonly image2D u_resolveOut;
layout (binding = 6) uniform sampler2D u_gbufferNormal;    // this frame's normals; .a = ocean flag

layout (push_constant) uniform PC
{
    uint  width;
    uint  height;
    float feedback;  // history weight in [0,1]; 0 disables temporal accumulation
    uint  viewIndex; // view to reconstruct in (0 = centre/desktop, 1 = left eye, 2 = right eye)
    float oceanFeedback; // history weight cap on ocean pixels - see the ocean block below
} pc;

bool insideViewport(vec2 uv)
{
    return all(greaterThanEqual(uv, u_viewportRect.xy)) &&
           all(lessThanEqual(uv, u_viewportRect.xy + u_viewportRect.zw));
}

void main()
{
    g_viewIndex = int(pc.viewIndex);
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(pc.width) || px.y >= int(pc.height))
        return;

    const vec2 texel = 1.0 / vec2(pc.width, pc.height);
    const vec2 uv = (vec2(px) + 0.5) * texel;

    const vec3 current = texture(u_currentColor, uv).rgb;
    const float depth = texture(u_gbufferDepth, uv).r;

    // Pixels outside the editor viewport panel carry no reliable motion; pass straight through.
    if (!insideViewport(uv) || pc.feedback <= 0.0)
    {
        imageStore(u_resolveOut, px, vec4(current, 1.0));
        return;
    }

    // Sky pixels (the sky sphere is excluded from the G-buffer, so its depth stays at the cleared far
    // plane - 0.0 under reversed-Z) reproject AT the far plane: the reprojection is then purely rotational
    // (parallax-free, correct for content at infinity) and the jittered sky raster still accumulates.
    const bool sky = depth <= 0.0;
    // The depth image is JITTERED (the prepass rasterizes identically to the forward pass, for exact
    // depth-prepass reuse): the surface sampled at uv truly sits at uv - jitter. Compensate every
    // GEOMETRIC use of the sampled depth - reconstruction and reprojection - with the known jitter;
    // this is exact, and what keeps reprojection wobble-free with a jittered reference.
    const vec2 uvUnjit = uv - taaJitterUv(u_taaJitter.xy);
    const vec3 worldPos = worldPosFromDepth(uvUnjit, depth); // disocclusion test only
    float clipW;
    // Clip-space reprojection (u_reprojClip): the world-space round trip drifts pixel-scale away from
    // the world origin, which made TAA fetch history off-target and turned every stochastic input
    // (jitter accumulation, shadow dither, RTAO, sky clouds) into visible per-frame noise.
    const vec2 prevUv = prevScreenUVClip(uvUnjit, depth, clipW);

    const bool histValid = clipW > 0.0 && insideViewport(prevUv);
    if (!histValid)
    {
        imageStore(u_resolveOut, px, vec4(current, 1.0));
        return;
    }

    // Ocean pixels (prepass normal .a): the waves ANIMATE but this reprojection is camera-only - no
    // motion vectors - so ocean history lands on the wrong wave every frame and full-weight accumulation
    // averages the specular sparkle into blur. Cap the feedback there (TAA/Ocean feedback tweak) and
    // tighten the variance clamp so the moving highlights stay crisp; water has no hard edges, so the
    // reduced accumulation costs no visible aliasing.
    const float ocean = texture(u_gbufferNormal, uv).a;

    // 3x3 current-colour statistics for the variance-clipping box.
    vec3 nmin = current, nmax = current, m1 = vec3(0.0), m2 = vec3(0.0);
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        const vec3 c = texture(u_currentColor, uv + vec2(dx, dy) * texel).rgb;
        nmin = min(nmin, c);
        nmax = max(nmax, c);
        m1 += c;
        m2 += c * c;
    }
    const vec3 mean  = m1 / 9.0;
    const vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    const float clampWidth = mix(1.25, 0.75, ocean);
    const vec3 cmin  = max(nmin, mean - clampWidth * sigma);
    const vec3 cmax  = min(nmax, mean + clampWidth * sigma);

    vec3 history = texture(u_historyColor, prevUv).rgb;
    history = clamp(history, cmin, cmax);

    // Disocclusion: reconstruct last frame's world position at the reprojected pixel and reject the history
    // if the surface moved more than a view-distance-scaled threshold (something else was there last frame).
    // Last frame's depth image is jittered by LAST frame's jitter (u_taaJitter.zw): the surface at unjittered
    // position prevUv lives at pixel prevUv + prevJitter, so fetch there and reconstruct at prevUv.
    const float prevDepth = texture(u_prevGbufferDepth, prevUv + taaJitterUv(u_taaJitter.zw)).r;
    float fb = mix(pc.feedback, min(pc.feedback, pc.oceanFeedback), ocean);
    const bool prevSky = prevDepth <= 0.0;
    if (sky != prevSky)
    {
        // Sky/geometry class mismatch: at silhouettes the sub-pixel jitter legitimately flips a pixel's
        // coverage between sky and geometry frame to frame. A hard fb = 0 here left those pixels
        // permanently unaccumulated - raw jitter shimmer along every silhouette - so keep REDUCED
        // feedback instead: the variance clamp above already bounds what mismatched history can
        // contribute, and true reveals still converge in a few frames at this weight.
        fb *= 0.25;
    }
    else if (!sky)
    {
        const vec3 prevWorld = worldPosFromDepthMat(prevUv, prevDepth, u_prevInvMvp);
        const float thresh = 0.05 * (1.0 + distance(u_viewPos, worldPos));
        if (distance(prevWorld, worldPos) > thresh)
            fb = 0.0; // real parallax disocclusion: something else was here last frame
    }

    const vec3 result = mix(current, history, fb);
    imageStore(u_resolveOut, px, vec4(result, 1.0));
}
