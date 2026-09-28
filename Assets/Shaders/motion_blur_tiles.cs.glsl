#version 460

#extension GL_GOOGLE_include_directive : enable

// Motion blur with TAA OFF only (MotionBlurPipeline): the per-pixel blur velocity and the 8 x 8 sub-tile maxima
// that taa.cs.glsl otherwise writes as a side product (motion_blur.inc.glsl).

layout (local_size_x = MOTION_BLUR_SUBTILE, local_size_y = MOTION_BLUR_SUBTILE, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_sceneDepth; // this frame's depth (jittered)
layout (binding = 2) uniform sampler2D u_motion;     // this frame's motion target
layout (binding = 3, rg16f) uniform restrict writeonly image2D u_velocityOut; // velocity (px over the exposure)
layout (binding = 4, rg16f) uniform restrict writeonly image2D u_subTileOut;  // per 8 x 8: the longest velocity

layout (push_constant) uniform PC
{
    uint  width;
    uint  height;
    float shutter;     // exposure / frame time (0.5 = a 180 degree shutter)
    float maxRadius;   // px: the longest blur is 2 x this
    float cameraScale; // the camera's share of the velocity (0 = objects only)
} pc;

#define MOTION_BLUR_REDUCE u_subTileOut
#include "motion_blur.inc.glsl"

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    vec2 vel = vec2(0.0);
    if (px.x < int(pc.width) && px.y < int(pc.height))
    {
        vel = motionBlurVelocity(px, vec2(pc.width, pc.height), texelFetch(u_sceneDepth, px, 0).r,
            texelFetch(u_motion, px, 0), pc.shutter, pc.maxRadius, pc.cameraScale);
        imageStore(u_velocityOut, px, vec4(vel, 0.0, 0.0));
    }
    motionBlurReduceTile(vel); // every lane: barriers inside
}
