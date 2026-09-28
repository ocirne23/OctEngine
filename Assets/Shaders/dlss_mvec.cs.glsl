#version 460

#extension GL_GOOGLE_include_directive : enable

// DLSS (DlssPipeline): the full per-pixel motion DLSS reads. The scene's motion target holds object motion only
// (w = 0 = the camera alone), so a pixel without it reprojects through the camera from its depth, exactly like
// TAA (prevScreenUVMotion). Out: RG16F, render px, current -> previous (DLSS: prev = cur + mv), unjittered.

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_sceneDepth; // this frame's depth (jittered)
layout (binding = 2) uniform sampler2D u_motion;     // this frame's motion target
layout (binding = 3, rg16f) uniform restrict writeonly image2D u_mvecOut;

layout (push_constant) uniform PC
{
    ivec2 origin; // the render rect in the render-size targets (px)
    ivec2 size;
} pc;

void main()
{
    if (any(greaterThanEqual(ivec2(gl_GlobalInvocationID.xy), pc.size)))
        return;
    const ivec2 px = pc.origin + ivec2(gl_GlobalInvocationID.xy);
    const float depth = texelFetch(u_sceneDepth, px, 0).r;
    const vec2 uvUnjit = (vec2(px) + 0.5) * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
    bool valid;
    const vec2 prevUv = prevScreenUVMotion(uvUnjit, depth, texelFetch(u_motion, px, 0), valid);
    imageStore(u_mvecOut, px, vec4((prevUv - uvUnjit) * u_screenSize.xy, 0.0, 0.0));
}
