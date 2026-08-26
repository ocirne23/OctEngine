#version 460

// Depth-aware upsample of the HALF-RES union march target (force_union.fs.glsl) into scene color:
// each full-res pixel blends the 4 covering half-res texels with bilinear weights scaled by DEPTH
// SIMILARITY, so a shell marched in front of geometry never bleeds across the silhouette onto the
// pixel behind it (and vice versa). A neighbour's representative depth is the full-res depth at
// its own march uv — the exact value that texel's march clamped against, no extra march output
// needed. All-weights-dead (a pixel whose 4 neighbours all sit across a depth edge) falls back to
// the single nearest-depth texel. Premultiplied blend over the lit scene, exactly the blend the
// full-res union draw used; alpha 0 everywhere (union pass off / uncovered) discards.

#include "shared.inc.glsl"

layout (binding = 1) uniform sampler2D u_unionMarch;   // half-res premultiplied march result
layout (binding = 2) uniform sampler2D u_gbufferDepth; // full-res scene depth (reversed-Z)

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) out vec4 out_color;

float sceneDistAt(vec2 uv)
{
    const float d = texture(u_gbufferDepth, uv).r;
    return d > 0.0 ? distance(u_viewPos, worldPosFromDepth(uv, d)) : 1e30; // reversed-Z: 0 = sky
}

void main()
{
    g_viewIndex = int(u_viewIndex);
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw;
    const float dist0 = sceneDistAt(uv);

    // The 2x2 half-res texels around this pixel (half texel x covers full pixels 2x/2x+1; its
    // march sampled the full-res uv at (x + 0.5) * 2 texels — the quad center).
    const vec2 halfCoord = gl_FragCoord.xy * 0.5 - 0.5;
    const ivec2 base = ivec2(floor(halfCoord));
    const vec2 f = halfCoord - vec2(base);
    const ivec2 texMax = textureSize(u_unionMarch, 0) - 1;

    vec4 accum = vec4(0.0);
    float weightSum = 0.0;
    vec4 nearest = vec4(0.0);
    float nearestScore = -1.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
        {
            const ivec2 tc = clamp(base + ivec2(i, j), ivec2(0), texMax);
            const vec4 c = texelFetch(u_unionMarch, tc, 0);
            const vec2 marchUv = (vec2(tc) + 0.5) * 2.0 * u_screenSize.zw;
            const float distN = sceneDistAt(marchUv);
            // Relative depth tolerance (~12%): inside a smooth surface every neighbour passes;
            // across a silhouette the far side's weight collapses.
            const float similarity = 1.0 / (1.0 + abs(distN - dist0) * (8.0 / max(dist0, 1.0)));
            const float bilinear = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            const float w = bilinear * similarity;
            accum += c * w;
            weightSum += w;
            if (similarity > nearestScore)
            {
                nearestScore = similarity;
                nearest = c;
            }
        }
    const vec4 color = weightSum > 1e-4 ? accum / weightSum : nearest;
    if (color.a <= 0.002 && dot(color.rgb, color.rgb) < 1e-6)
        discard;
    out_color = color;
}
