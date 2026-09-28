#version 460

#extension GL_GOOGLE_include_directive : enable

// Motion blur, pass 1 of 3 (MotionBlurPipeline): every pixel's blur VELOCITY - its displacement over the
// exposure, in pixels - and the longest velocity of each MOTION_BLUR_TILE^2 tile.
// The camera's part comes from the depth (u_reprojClip, exact for a still surface), the object's part from the
// motion target (shared.inc.glsl prevScreenUVMotion): object = motion target - camera, so the camera's share can
// be scaled on its own. Velocity = (this frame - last frame) uv x the shutter, clamped to twice the max radius.

layout (local_size_x = MOTION_BLUR_TILE, local_size_y = MOTION_BLUR_TILE, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_sceneDepth; // this frame's depth (jittered)
layout (binding = 2) uniform sampler2D u_motion;     // this frame's motion target
layout (binding = 3, rgba16f) uniform restrict writeonly image2D u_velocityOut; // xy = velocity (px), z = view distance (m)
layout (binding = 4, rg16f) uniform restrict writeonly image2D u_tileMaxOut;    // per tile: the longest velocity

layout (push_constant) uniform PC
{
    uint  width;
    uint  height;
    float shutter;     // exposure / frame time (0.5 = a 180 degree shutter)
    float maxRadius;   // px: the longest blur is 2 x this
    float cameraScale; // the camera's share of the velocity (0 = objects only)
    uint  samples;
} pc;

shared vec2  s_vel[MOTION_BLUR_TILE * MOTION_BLUR_TILE];
shared float s_len2[MOTION_BLUR_TILE * MOTION_BLUR_TILE];

// VIEW_DISTANCE_SKY: the gather's depth weights treat the sky as behind everything.
const float VIEW_DISTANCE_SKY = 60000.0;

void main()
{
    const ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    vec2 vel = vec2(0.0);
    if (px.x < int(pc.width) && px.y < int(pc.height))
    {
        float dist = VIEW_DISTANCE_SKY;
        const vec2 size = vec2(pc.width, pc.height);
        const vec2 uv = (vec2(px) + 0.5) / size;
        if (all(greaterThanEqual(uv, u_viewportRect.xy)) && all(lessThanEqual(uv, u_viewportRect.xy + u_viewportRect.zw)))
        {
            const float depth = texelFetch(u_sceneDepth, px, 0).r;
            const vec4 motion = texelFetch(u_motion, px, 0);
            const vec2 uvUnjit = uv - taaJitterUv(u_taaJitter.xy);
            float clipW;
            const vec2 camPrev = prevScreenUVClip(uvUnjit, depth, clipW);
            const vec2 camVel = clipW > 0.0 ? uvUnjit - camPrev : vec2(0.0);
            const vec2 total = (motionIsObject(motion) && motion.z > 0.0) ? motion.xy : camVel;
            const vec2 framePx = (camVel * pc.cameraScale + (total - camVel)) * size;
            // A camera cut or teleport moves (nearly) everything by a large part of the screen: no blur.
            if (dot(framePx, framePx) < 0.09 * dot(size, size))
            {
                vel = framePx * pc.shutter;
                const float len = length(vel);
                const float maxLen = 2.0 * pc.maxRadius;
                if (len > maxLen)
                    vel *= maxLen / len;
            }
            if (depth > 0.0)
                dist = length(viewRelFromDepth(uvUnjit, depth));
        }
        imageStore(u_velocityOut, px, vec4(vel, dist, 0.0));
    }

    // The tile's longest velocity: a shared-memory reduction (no early exit above: every lane reaches the barriers).
    const uint li = gl_LocalInvocationIndex;
    s_vel[li] = vel;
    s_len2[li] = dot(vel, vel);
    barrier();
    for (uint stride = (MOTION_BLUR_TILE * MOTION_BLUR_TILE) / 2u; stride > 0u; stride >>= 1)
    {
        if (li < stride && s_len2[li + stride] > s_len2[li])
        {
            s_len2[li] = s_len2[li + stride];
            s_vel[li] = s_vel[li + stride];
        }
        barrier();
    }
    if (li == 0u)
        imageStore(u_tileMaxOut, ivec2(gl_WorkGroupID.xy), vec4(s_vel[0], 0.0, 0.0));
}
