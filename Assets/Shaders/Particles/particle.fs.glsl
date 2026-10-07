#version 460

#extension GL_EXT_nonuniform_qualifier : enable

// Particle fragment shader: texture (or procedural round sprite) x the per-particle color the vertex
// shader computed, soft-faded against the scene depth. Premultiplied output over the lit scene with
// (ONE, ONE_MINUS_SRC_ALPHA): additivity scales the alpha the destination keeps, so one pipeline covers
// alpha-blended smoke (additivity 0) through additive fire/sparks (additivity 1) per emitter.

#include "shared.inc.glsl"

layout (binding = 4) uniform sampler2D u_sceneDepth; // this frame's opaque scene depth (soft particles)
layout (binding = 20) uniform sampler2D u_textures[];  // bindless texture array (variable count)

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) in vec2 v_uv0;
layout (location = 1) in vec2 v_uv1;
layout (location = 2) in vec4 v_color;
layout (location = 3) in flat float v_flipBlend;
layout (location = 4) in flat uint v_texIdx;
layout (location = 5) in flat float v_additivity;
layout (location = 6) in flat float v_softInv;
layout (location = 7) in float v_clipW;

layout (location = 0) out vec4 out_color;

void main()
{
    g_viewIndex = int(u_viewIndex);

    vec4 texel;
    if (v_texIdx == PARTICLE_TEX_NONE)
    {
        // Procedural soft round sprite.
        const float d = length(v_uv0 * 2.0 - 1.0);
        texel = vec4(1.0, 1.0, 1.0, smoothstep(1.0, 0.15, d));
    }
    else
    {
        texel = texture(u_textures[nonuniformEXT(v_texIdx)], v_uv0);
        if (v_flipBlend > 0.0)
            texel = mix(texel, texture(u_textures[nonuniformEXT(v_texIdx)], v_uv1), v_flipBlend);
    }

    // Soft particles: fade out as the quad approaches the opaque scene surface behind it.
    float soft = 1.0;
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw;
    const float depth = texture(u_sceneDepth, uv).r;
    if (depth > 0.0) // reversed-Z: 0 = sky/far
    {
        const vec3 sceneWorld = worldPosFromDepth(uv, depth);
        const float sceneW = (u_mvp * vec4(sceneWorld, 1.0)).w;
        soft = clamp((sceneW - v_clipW) * v_softInv, 0.0, 1.0);
    }

    const float alpha = v_color.a * texel.a * soft;
    if (alpha <= 0.001)
        discard;
    out_color = vec4(v_color.rgb * texel.rgb * alpha, alpha * (1.0 - v_additivity));
}
