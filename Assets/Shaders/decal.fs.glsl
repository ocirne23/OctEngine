#version 460

#extension GL_EXT_nonuniform_qualifier : enable

// Projected decal fragment shader: reconstructs the opaque surface under this fragment from the
// G-buffer depth, projects it into the decal's box space (discarding outside the volume), samples the
// decal texture across local XY and blends it over the already-lit scene (premultiplied). Lit decals
// approximate the forward shading with sun N.L (no shadow) + GI probe irradiance at the surface, so
// they track the scene's lighting; unlit decals composite the tint as-is (blob shadows, projected
// emissive markers). This runs in the scene-color pass right after the opaque forward draw, so
// particles, fog and TAA all layer on top naturally.

#include "shared.inc.glsl"
#include "decal.inc.glsl"

layout (binding = 1, std430) readonly buffer Decals { Decal d_decals[]; };
layout (binding = 2) uniform sampler2D u_gbufferDepth;
layout (binding = 3) uniform sampler2D u_gbufferNormal;
layout (binding = 4, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };
layout (binding = 20) uniform sampler2D u_textures[]; // bindless texture array (variable count)

#define GI_GRID_DATA_NAME gi_gridData
#include "gi_probe.inc.glsl"

layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) in flat uint v_decalIdx;

layout (location = 0) out vec4 out_color;

void main()
{
    g_viewIndex = int(u_viewIndex);
    const Decal decal = d_decals[v_decalIdx];

    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw;
    const float depth = texture(u_gbufferDepth, uv).r;
    if (depth <= 0.0) // reversed-Z: sky/far — nothing to project onto
        discard;
    const vec3 worldPos = worldPosFromDepth(uv, depth);

    // Into decal space; anything outside the unit box is not on the projected surface.
    const vec3 local = decalQuatUnrotate(decal.rotation, worldPos - decal.posOpacity.xyz)
        / decal.halfExtentsAngleFade.xyz;
    if (any(greaterThan(abs(local), vec3(1.0))))
        discard;

    vec4 texel = decal.tint;
    if (decal.params.x != PARTICLE_TEX_NONE)
    {
        const vec2 decalUv = vec2(local.x, -local.y) * 0.5 + 0.5;
        texel *= texture(u_textures[nonuniformEXT(decal.params.x)], decalUv);
    }

    // Fade out on surfaces facing away from the projection direction (stretch smearing).
    const vec3 n = normalize(texture(u_gbufferNormal, uv).xyz + vec3(0.0, 1e-4, 0.0));
    const vec3 projDir = decalQuatRotate(decal.rotation, vec3(0.0, 0.0, -1.0)); // toward the surface
    const float facing = dot(n, -projDir);
    const float cutoff = decal.halfExtentsAngleFade.w;
    const float angleFade = smoothstep(cutoff, cutoff + max(decal.emissiveFadeWidth.w, 1e-3), facing);
    // Soften the box's near/far clip so the projection doesn't end in a hard line on steep geometry.
    const float depthFade = 1.0 - smoothstep(0.8, 1.0, abs(local.z));

    float alpha = texel.a * decal.posOpacity.w * angleFade * depthFade;
    if (alpha <= 0.002)
        discard;

    vec3 color = texel.rgb;
    if ((decal.params.y & DECAL_FLAG_LIT) != 0u)
    {
        float coverage;
        const vec3 E = evalProbeSHCoverage(worldPos, n, coverage);
        const vec3 irr = mix(giEvalSkySH(n), E, coverage);
        const vec3 sun = atmosTransmittanceToLight(0.0, normalize(u_sunDirection), u_skyUp)
            * u_sunColor.rgb * u_eclipseParams.x * max(dot(n, normalize(u_sunDirection)), 0.0);
        color *= (irr + sun) * (1.0 / PI) + u_ambientColor;
    }
    color += decal.emissiveFadeWidth.rgb;

    out_color = vec4(color * alpha, alpha);
}
