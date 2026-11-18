#version 450

// BEAM CARDS (BeamCardPipeline): ONE beam's glow in the air on the pixels of its card (beam_card.vs.glsl), added to the
// scene colour (blend ONE, ONE; alpha masked). The same glow as the fog apply's columns (beamGlow, beam_glow.inc.glsl),
// cut by the scene depth the same way and dimmed by a cloud in front of it, so a card and a column look alike - at a
// cost by the card's pixels instead of the whole screen. The ray is the fog apply's (fogRay): u_rayFromNdc through the
// pixel, minus the TAA jitter.

#include "shared.inc.glsl"
#include "cloud_upsample.inc.glsl"
#include "beam_glow.inc.glsl"

layout (binding = 1, std430) readonly buffer BeamCards { vec4 bc_data[]; };
layout (binding = 2) uniform sampler2D u_depth;      // the scene depth, this eye
layout (binding = 3) uniform sampler2D u_cloudColor; // the accumulated clouds (half res), this eye / slot
layout (binding = 4) uniform sampler2D u_cloudDepth;

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) flat in uint v_card;

layout (location = 0) out vec4 out_color;

void main()
{
    g_viewIndex = int(u_viewIndex);
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw;
    const vec2 rayVpUv = (uv - u_viewportRect.xy) * u_viewportInvSize;
    const vec2 rayNdc = vec2(rayVpUv.x * 2.0 - 1.0, 1.0 - rayVpUv.y * 2.0) - u_taaJitter.xy;
    const vec3 dir = normalize(vec3(rayNdc, 1.0) * u_rayFromNdc);

    const float depth = texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0).r;
    const float tMax = depth > 0.0 ? length(viewRelFromDepth(uv, depth)) : 1e9;

    const vec4 footR = bc_data[v_card * 4u + 0u];
    const vec4 axisLen = bc_data[v_card * 4u + 1u];
    const vec4 colorRange = bc_data[v_card * 4u + 2u];
    const float tipFade = bc_data[v_card * 4u + 3u].x;
    float tp;
    float glow = beamGlow(u_viewPos, dir, tMax, footR.xyz, axisLen.xyz, axisLen.w, footR.w, colorRange.w, tipFade, tp);
    if (glow <= 0.0)
        discard;

#ifdef CLOUDS
    // A cloud in front of the beam dims it by its transmittance (the fog apply's rule for its columns).
    if (u_clouds_enabled > 0.5)
    {
        float logCloudDist;
        const vec4 cloud = cloudUpsample(u_cloudColor, u_cloudDepth, gl_FragCoord.xy, depth > 0.0 ? log2(max(tMax, 1.0)) : 1e30, logCloudDist);
        if (cloud.a < 0.999 && tp > exp2(logCloudDist))
            glow *= cloud.a;
    }
#endif
    out_color = vec4(colorRange.rgb * glow, 0.0);
}
