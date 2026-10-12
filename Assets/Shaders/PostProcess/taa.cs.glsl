#version 460

#extension GL_GOOGLE_include_directive : enable

// Temporal anti-aliasing resolve, full resolution: reproject the history through the camera (u_reprojClip) or, on a
// moving surface, through the MOTION VECTOR (shared.inc.glsl); variance-clip it to the current 3x3 colour box; reject
// it on disocclusion or outside the viewport; blend with a fixed feedback. The projection is jittered per frame
// (Renderer::beginFrame). The resolve is linear HDR (exposure and tonemap come in the composite); the result is both
// the displayed image and next frame's history (ping-ponged per frame in flight).

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_currentColor;   // this frame's scene colour (jittered); .a = 0 on ocean pixels
layout (binding = 2) uniform sampler2D u_historyColor;   // last frame's resolved colour
layout (binding = 3) uniform sampler2D u_sceneDepth;     // this frame's camera depth
layout (binding = 4) uniform sampler2D u_prevSceneDepth; // last frame's camera depth (disocclusion test)
layout (binding = 5, rgba16f) uniform restrict writeonly image2D u_resolveOut;
layout (binding = 6) uniform sampler2D u_motion;         // this frame's motion target (prevScreenUVMotion)
// MOTION BLUR, fused in (MotionBlurPipeline): this pass already reads the depth and the motion target, so it writes the
// blur velocity and the 8 x 8 sub-tile maxima; the blur has no full-res pass of its own.
layout (binding = 7, rg16f) uniform restrict writeonly image2D u_mbVelocityOut;
layout (binding = 8, rg16f) uniform restrict writeonly image2D u_mbSubTileOut;

layout (push_constant) uniform PC
{
    uint  width;
    uint  height;
    uint  viewIndex; // view to reconstruct in (0 = centre/desktop, 1 = left eye, 2 = right eye)
    uint  mbEnabled; // 1 = write the motion blur velocity + sub-tiles
} pc; // the feedbacks and the blur's tweaks: u_post

#define MOTION_BLUR_REDUCE u_mbSubTileOut
#include "motion_blur.inc.glsl"

bool insideViewport(vec2 uv)
{
    return all(greaterThanEqual(uv, u_viewportRect.xy)) && all(lessThanEqual(uv, u_viewportRect.xy + u_viewportRect.zw));
}

void main()
{
    g_viewIndex = int(pc.viewIndex);
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    const bool inImage = px.x < int(pc.width) && px.y < int(pc.height);

    // Before any early return: the sub-tile reduction has barriers every lane must reach. The pixel's OWN velocity
    // (not the dilated one TAA's history follows).
    if (pc.mbEnabled != 0u)
    {
        vec2 vel = vec2(0.0);
        if (inImage)
        {
            vel = motionBlurVelocity(px, vec2(pc.width, pc.height), texelFetch(u_sceneDepth, px, 0).r, texelFetch(u_motion, px, 0),
                                     u_post_mbShutter, u_post_mbMaxRadius, u_post_mbCameraScale);
            imageStore(u_mbVelocityOut, px, vec4(vel, 0.0, 0.0));
        }
        motionBlurReduceTile(vel);
    }

    if (!inImage)
        return;

    const vec2 texel = 1.0 / vec2(pc.width, pc.height);
    const vec2 uv = (vec2(px) + 0.5) * texel;

    const vec4 currentRgba = texture(u_currentColor, uv);
    const vec3 current = currentRgba.rgb;

    // Pixels outside the editor viewport panel carry no reliable motion; pass straight through.
    if (!insideViewport(uv))
    {
        imageStore(u_resolveOut, px, vec4(current, 1.0));
        return;
    }

    // Sky pixels (no depth written: the cleared 0.0 under reversed Z) reproject AT the far plane: purely rotational,
    // correct for content at infinity, and the jittered sky raster still accumulates.
    const float depth = texture(u_sceneDepth, uv).r;
    const bool sky = depth <= 0.0;

    // The depth image is JITTERED: the surface sampled at uv truly sits at uv - jitter. Every GEOMETRIC use of the
    // depth (reconstruction, reprojection) compensates for it exactly, which keeps the reprojection wobble-free.
    const vec2 jitterUv = taaJitterUv(u_taaJitter.xy);
    const vec2 uvUnjit = uv - jitterUv;

    // The motion of the NEAREST surface in the 3x3: at a moving silhouette the jittered edge pixel is partly the object,
    // and its history must follow the object. A nearest surface without object motion (w = 0) reprojects this pixel
    // through the camera, with its own depth.
    vec4 motion = vec4(0.0);
    {
        const ivec2 last = ivec2(pc.width, pc.height) - 1;
        float nearest = depth;
        ivec2 nearestPx = px;
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const ivec2 q = clamp(px + ivec2(dx, dy), ivec2(0), last);
            const float d = texelFetch(u_sceneDepth, q, 0).r;
            if (d > nearest) // reversed-Z: nearer = greater
            {
                nearest = d;
                nearestPx = q;
            }
        }
        motion = texelFetch(u_motion, nearestPx, 0);
    }

    // Clip-space reprojection (u_reprojClip) where the surface did not move: a world-space round trip drifts by a pixel
    // far from the world origin, and every stochastic input (jitter, shadow dither, RTAO, clouds) becomes noise.
    bool prevValid;
    const vec2 prevUv = prevScreenUVMotion(uvUnjit, depth, motion, prevValid);
    // The HISTORY is the converged, UNJITTERED image. This pixel converges to the surface at uv (the sampled surface
    // + jitter), so fetch at the reprojection + jitter (= uv under a still camera); at prevUv alone the history would
    // shift by this frame's jitter every frame (sub-pixel wobble).
    const vec2 historyUv = prevUv + jitterUv;

    const bool histValid = prevValid && insideViewport(historyUv);
    if (!histValid)
    {
        imageStore(u_resolveOut, px, vec4(current, 1.0));
        return;
    }

    // Ocean pixels (scene colour ALPHA == 0: only ocean.fs.glsl writes it; other opaque surfaces write alpha > 0 and
    // the layers over the scene leave alpha alone - GraphicsPipelineLayout::colorWriteAlpha). The ocean writes no motion
    // vectors (dual-source blend: one output), so its history lands on the wrong wave and full accumulation blurs the
    // sparkle. Cap the feedback there (TAA/Ocean feedback) and tighten the clamp; water has no hard edges to alias.
    const float ocean = (!sky && currentRgba.a < 0.004) ? 1.0 : 0.0;

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

    vec3 history = texture(u_historyColor, historyUv).rgb;
    history = clamp(history, cmin, cmax);

    // Disocclusion: reject the history when last frame's surface at the reprojected pixel is farther than a
    // distance-scaled threshold. Last frame's depth carries LAST frame's jitter (u_taaJitter.zw): fetch at
    // prevUv + prevJitter, reconstruct at prevUv.
    const float prevDepth = texture(u_prevSceneDepth, prevUv + taaJitterUv(u_taaJitter.zw)).r;
    float fb = mix(u_post_taaFeedback, u_post_taaOceanFeedback, ocean);
    const bool prevSky = prevDepth <= 0.0;
    if (sky != prevSky)
    {
        // Sky / geometry mismatch: at silhouettes the jitter flips a pixel's coverage frame to frame. fb = 0 would leave
        // them unaccumulated (shimmer along every silhouette); the variance clamp already bounds the mismatched history,
        // and true reveals still converge in a few frames at this weight.
        fb *= 0.25;
    }
    else if (!sky)
    {
        const vec3 worldPos = worldPosFromDepth(uvUnjit, depth); // disocclusion test only
        const vec3 prevWorld = worldPosFromDepthMat(prevUv, prevDepth, u_prevInvMvp);
        // Where the surface WAS: here for a still one; a moving one at its motion vector's depth on last frame's ray
        // through prevUv, so the test compares depths along one ray.
        const vec3 refPos = motionIsObject(motion) ? worldPosFromDepthMat(prevUv, motion.z, u_prevInvMvp) : worldPos;
        const float thresh = 0.05 * (1.0 + distance(u_viewPos, worldPos));
        if (distance(prevWorld, refPos) > thresh)
            fb = 0.0; // real parallax disocclusion: something else was here last frame
    }

    const vec3 result = mix(current, history, fb);
    imageStore(u_resolveOut, px, vec4(result, 1.0));
}
