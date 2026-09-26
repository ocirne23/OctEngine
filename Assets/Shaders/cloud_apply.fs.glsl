#version 450

// Cloud apply, FOG OFF only: fullscreen pass in the scene-colour render pass. With the fog on, the fog apply
// (vol_apply.fs.glsl) composites the clouds itself, inside the fog. Upsamples the half-res accumulated clouds
// (cloud_upsample.inc.glsl) and blends (srcColor = ONE, dstColor = SRC_ALPHA): out = inScatter + scene *
// transmittance. Alpha is not written (the scene colour's alpha is TAA's ocean flag).

#include "shared.inc.glsl"
#include "cloud_upsample.inc.glsl"

layout (location = 0) in vec2 v_uv;
layout (binding = 1) uniform sampler2D u_depth;
layout (binding = 2) uniform sampler2D u_cloudColor;
layout (binding = 3) uniform sampler2D u_cloudDepth;

layout (location = 0) out vec4 out_color;

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

void main()
{
    g_viewIndex = int(u_viewIndex);
    const float depth = texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0).r;
    const float logScene = depth > 0.0
        ? log2(max(length(viewRelFromDepth(v_uv - taaJitterUv(u_taaJitter.xy), depth)), 1.0))
        : 1e30;
    float logCloudDist;
    out_color = cloudUpsample(u_cloudColor, u_cloudDepth, gl_FragCoord.xy, logScene, logCloudDist);
}
