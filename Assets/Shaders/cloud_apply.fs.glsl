#version 450

// Cloud apply, FOG OFF only: fullscreen pass in the scene-colour render pass. With the fog on, the fog apply
// (vol_apply.fs.glsl) composites the clouds itself, inside the fog. Upsamples the half-res accumulated clouds
// (cloud_upsample.inc.glsl) and blends (srcColor = ONE, dstColor = SRC_ALPHA): out = inScatter + scene *
// transmittance. Alpha is not written (the scene colour's alpha is TAA's ocean flag).
// THE FAR TREES too while they marched (u_foliageParams4.w; their own "Far trees apply" stage is then skipped): the
// two layers compose front to back by distance, as in vol_apply.fs.glsl without the fog. The volume writes no
// depth, so the clouds' march limit is the terrain BEHIND the trees - composited after them, the clouds between the
// two drew OVER the trees.

#include "shared.inc.glsl"
#include "cloud_upsample.inc.glsl"

layout (location = 0) in vec2 v_uv;
layout (binding = 1) uniform sampler2D u_depth;
layout (binding = 2) uniform sampler2D u_cloudColor;
layout (binding = 3) uniform sampler2D u_cloudDepth;
layout (binding = 4) uniform sampler2D u_farTreesColor; // the far-tree volume (full res): in-scatter, T
layout (binding = 5) uniform sampler2D u_farTreesDepth; // its weighted mean distance (m)

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
    const vec4 cloud = cloudUpsample(u_cloudColor, u_cloudDepth, gl_FragCoord.xy, logScene, logCloudDist);
    out_color = cloud;
    if (u_foliageParams4.w > 0.5)
    {
        const vec4 trees = texelFetch(u_farTreesColor, ivec2(gl_FragCoord.xy), 0);
        if (trees.a < 0.999)
        {
            const float tTrees = texelFetch(u_farTreesDepth, ivec2(gl_FragCoord.xy), 0).r;
            const float tCloud = cloud.a < 0.999 ? exp2(logCloudDist) : 1e30;
            out_color = tTrees <= tCloud
                ? vec4(trees.rgb + trees.a * cloud.rgb, trees.a * cloud.a)
                : vec4(cloud.rgb + cloud.a * trees.rgb, cloud.a * trees.a);
        }
    }
}
