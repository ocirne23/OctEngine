#version 460

#extension GL_GOOGLE_include_directive : enable

// DLSS (DlssPipeline): the per-pixel inputs DLSS reads beyond colour and depth.
//  * The full motion. The scene's motion target holds object motion only (w = 0 = the camera alone), so a
//    pixel without it reprojects through the camera from its depth, exactly like TAA (prevScreenUVMotion).
//    Out: RG16F, render px, current -> previous (DLSS: prev = cur + mv), unjittered.
//  * The bias-current-colour mask (DLSS: lerp(history, current, bias)). The ocean writes no motion vectors
//    (dual-source blend: one fragment output), so its waves reproject camera-only and history lands on the
//    wrong wave: bias toward the current frame there - TAA's ocean feedback cap. Ocean = scene colour ALPHA 0
//    on a non-sky pixel (the TAA ocean flag, see taa.cs.glsl).
//  * pc.mbEnabled (DLAA only - no motion blur while upscaling): the motion blur velocity + 8 x 8 sub-tiles, the
//    side product TAA otherwise writes (motion_blur.inc.glsl). The sub-tile grid starts at pixel 0, so the
//    dispatch then covers the WHOLE target (pc.base = 0), not just the render rect.

layout (local_size_x = MOTION_BLUR_SUBTILE, local_size_y = MOTION_BLUR_SUBTILE, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_sceneDepth; // this frame's depth (jittered)
layout (binding = 2) uniform sampler2D u_motion;     // this frame's motion target
layout (binding = 3, rg16f) uniform restrict writeonly image2D u_mvecOut;
layout (binding = 4) uniform sampler2D u_sceneColor; // this frame's scene colour (.a = 0 on ocean pixels)
layout (binding = 5, r16f) uniform restrict writeonly image2D u_biasOut;
layout (binding = 6, rg16f) uniform restrict writeonly image2D u_mbVelocityOut; // motion blur velocity
layout (binding = 7, rg16f) uniform restrict writeonly image2D u_mbSubTileOut;  // per 8 x 8: the longest velocity

layout (push_constant) uniform PC
{
    ivec2 base;        // the dispatch's first pixel: the render rect's origin, or 0 with the motion blur
    ivec2 origin;      // the render rect in the render-size targets (px)
    ivec2 size;
    float oceanBias;   // the mask value on ocean pixels
    uint  mbEnabled;   // 1 = write the motion blur velocity + sub-tiles
    float mbShutter;
    float mbMaxRadius; // already clamped (MotionBlurPipeline::clampMaxRadius)
    float mbCameraScale;
} pc;

#define MOTION_BLUR_REDUCE u_mbSubTileOut
#include "motion_blur.inc.glsl"

void main()
{
    const ivec2 px = pc.base + ivec2(gl_GlobalInvocationID.xy);
    const ivec2 targetSize = ivec2(u_screenSize.xy);
    const bool inImage = all(lessThan(px, targetSize));
    const float depth = inImage ? texelFetch(u_sceneDepth, px, 0).r : 0.0;
    const vec4 motion = inImage ? texelFetch(u_motion, px, 0) : vec4(0.0);

    // Before any early return: the sub-tile reduction has barriers every lane must reach.
    if (pc.mbEnabled != 0u)
    {
        vec2 vel = vec2(0.0);
        if (inImage)
        {
            vel = motionBlurVelocity(px, u_screenSize.xy, depth, motion, pc.mbShutter, pc.mbMaxRadius, pc.mbCameraScale);
            imageStore(u_mbVelocityOut, px, vec4(vel, 0.0, 0.0));
        }
        motionBlurReduceTile(vel);
    }
    if (any(lessThan(px, pc.origin)) || any(greaterThanEqual(px, pc.origin + pc.size)))
        return;

    const vec2 uvUnjit = (vec2(px) + 0.5) * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
    bool valid;
    const vec2 prevUv = prevScreenUVMotion(uvUnjit, depth, motion, valid);
    imageStore(u_mvecOut, px, vec4((prevUv - uvUnjit) * u_screenSize.xy, 0.0, 0.0));

    // The colour is read for its alpha only, and the sky (depth 0) is never ocean: skip its 8 B there.
    const bool ocean = depth > 0.0 && texelFetch(u_sceneColor, px, 0).a < 0.004;
    imageStore(u_biasOut, px, vec4(ocean ? pc.oceanBias : 0.0));
}
