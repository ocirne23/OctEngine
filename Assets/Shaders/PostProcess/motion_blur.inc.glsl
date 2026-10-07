// MOTION BLUR, the shared parts (see MotionBlurPipeline). Needs shared.inc.glsl (the UBO) first.
//  * motionBlurVelocity - a pixel's blur velocity: its displacement over the EXPOSURE in px. Written by TAA (it
//    already reads the depth and the motion target), or by motion_blur_tiles.cs.glsl with TAA off.
//  * motionBlurReduceTile (MOTION_BLUR_REDUCE, compute only) - the longest velocity of the 8 x 8 workgroup,
//    into the sub-tile image (MOTION_BLUR_SUBTILE).
//  * motionBlurGather - McGuire's reconstruction filter; run by the COMPOSITE for the pixels of moving tiles.
#ifndef MOTION_BLUR_INC_GLSL
#define MOTION_BLUR_INC_GLSL

// The camera part from the depth (u_reprojClip, exact for a still surface), the object part from the motion
// target: object = motion target - camera, so the camera's share scales alone. (this - last frame) uv x the
// shutter, clamped to twice the max radius. A frame motion over ~30 % of the screen diagonal is a cut or a
// teleport: no blur. Outside the editor viewport: 0.
vec2 motionBlurVelocity(ivec2 px, vec2 size, float depth, vec4 motion, float shutter, float maxRadius, float cameraScale)
{
    const vec2 uv = (vec2(px) + 0.5) / size;
    if (any(lessThan(uv, u_viewportRect.xy)) || any(greaterThan(uv, u_viewportRect.xy + u_viewportRect.zw)))
        return vec2(0.0);
    const vec2 uvUnjit = uv - taaJitterUv(u_taaJitter.xy);
    float clipW;
    const vec2 camPrev = prevScreenUVClip(uvUnjit, depth, clipW);
    const vec2 camVel = clipW > 0.0 ? uvUnjit - camPrev : vec2(0.0);
    const vec2 total = (motionIsObject(motion) && motion.z > 0.0) ? motion.xy : camVel;
    const vec2 framePx = (camVel * cameraScale + (total - camVel)) * size;
    if (dot(framePx, framePx) >= 0.09 * dot(size, size))
        return vec2(0.0);
    vec2 vel = framePx * shutter;
    const float len = length(vel);
    const float maxLen = 2.0 * maxRadius;
    return len > maxLen ? vel * (maxLen / len) : vel;
}

#ifdef MOTION_BLUR_REDUCE
// The workgroup must be MOTION_BLUR_SUBTILE^2 (8 x 8) and EVERY invocation must call this (barriers inside):
// out-of-image lanes pass 0. The includer defines MOTION_BLUR_REDUCE as its rg16f sub-tile image.
shared vec2  s_mbVel[MOTION_BLUR_SUBTILE * MOTION_BLUR_SUBTILE];
shared float s_mbLen2[MOTION_BLUR_SUBTILE * MOTION_BLUR_SUBTILE];
void motionBlurReduceTile(vec2 vel)
{
    const uint li = gl_LocalInvocationIndex;
    s_mbVel[li] = vel;
    s_mbLen2[li] = dot(vel, vel);
    barrier();
    for (uint stride = (MOTION_BLUR_SUBTILE * MOTION_BLUR_SUBTILE) / 2u; stride > 0u; stride >>= 1)
    {
        if (li < stride && s_mbLen2[li + stride] > s_mbLen2[li])
        {
            s_mbLen2[li] = s_mbLen2[li + stride];
            s_mbVel[li] = s_mbVel[li + stride];
        }
        barrier();
    }
    if (li == 0u)
        imageStore(MOTION_BLUR_REDUCE, ivec2(gl_WorkGroupID.xy), vec4(s_mbVel[0], 0.0, 0.0));
}
#endif

// 1 where za is in front of zb, fading to 0 over 2 % of the distance behind. z = 1 / hardware depth: proportional
// to the view distance under the reversed-Z projection (far >> near), and the test is a RATIO, so the constant
// does not matter.
float motionBlurSoftDepth(float za, float zb) { return clamp(1.0 - (za - zb) / (0.02 * min(za, zb)), 0.0, 1.0); }
float motionBlurZ(float depth) { return 1.0 / max(depth, 1e-7); } // the sky (depth 0): behind everything
// A blur of radius r covers a point d away: linear falloff (the cone), and a hard edge (the cylinder).
float motionBlurCone(float d, float r) { return clamp(1.0 - d / max(r, 1e-3), 0.0, 1.0); }
float motionBlurCylinder(float d, float r) { return 1.0 - smoothstep(0.95 * r, 1.05 * r, d); }

// McGuire et al., "A Reconstruction Filter for Plausible Motion Blur" (2012): samples along the neighbourhood's
// longest velocity vN; each sample Y weighs by whether ITS blur reaches this pixel X (a moving foreground object
// smears over the background behind it) or X's own blur reaches Y, with soft depth tests choosing the front one.
// A blur spans half of its velocity each way. Samples stay inside [lo, hi] (the viewport, px).
vec3 motionBlurGather(sampler2D colorTex, sampler2D velocityTex, sampler2D depthTex, ivec2 px, vec2 vN,
    uint samples, ivec2 lo, ivec2 hi)
{
    const vec3 color = texelFetch(colorTex, px, 0).rgb;
    const float rX = max(0.5 * length(texelFetch(velocityTex, px, 0).xy), 0.5);
    const float zX = motionBlurZ(texelFetch(depthTex, px, 0).r);
    vec3 sum = color / rX;
    float wsum = 1.0 / rX;
    // Interleaved gradient noise, moving per frame: the sample offsets dither instead of banding.
    const vec2 noisePx = vec2(px) + float(u_frameIndex % 64u) * 5.588238;
    const float jitter = fract(52.9829189 * fract(dot(noisePx, vec2(0.06711056, 0.00583715)))) - 0.5;
    const float invSamples = 1.0 / float(samples);
    for (uint i = 0u; i < samples; ++i)
    {
        const float t = ((float(i) + 0.5 + jitter) * invSamples) * 2.0 - 1.0; // -1 .. 1 along vN
        const vec2 offset = vN * (0.5 * t);
        const ivec2 py = clamp(ivec2(floor(vec2(px) + 0.5 + offset)), lo, hi);
        const float rY = 0.5 * length(texelFetch(velocityTex, py, 0).xy);
        const float zY = motionBlurZ(texelFetch(depthTex, py, 0).r);
        const float d = length(offset);
        const float f = motionBlurSoftDepth(zY, zX); // Y in front: its blur covers X
        const float b = motionBlurSoftDepth(zX, zY); // X in front: X's blur reaches Y's place
        const float w = f * motionBlurCone(d, rY) + b * motionBlurCone(d, rX)
                      + 2.0 * motionBlurCylinder(d, rY) * motionBlurCylinder(d, rX);
        sum += texelFetch(colorTex, py, 0).rgb * w;
        wsum += w;
    }
    return sum / wsum;
}

#endif
