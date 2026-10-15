#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// THE FLOWERS' surface (clutter_flower.vs.glsl): the lit core, two-sided, without RTAO (as the grass). The stem in the grass's
// root -> tip colours, the petals in the type's colour (darker at the base), the centre in the second colour; the sun
// through a petal from behind ("Flower transmission"). CLUTTER_TEXTURED (the textured buckets' pipeline: ferns, radial
// petals, grain ears): mix(Color, Color2, texture G) x texture R on the textured part, alpha-tested.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec4 in_flower; // x part (0 stem, 1 petal, 2 centre), y along it, z across a textured frond
layout (location = 3) in vec3 in_prevWorldDelta;
layout (location = 4) flat in uvec2 in_look; // x albedo0 (petals), y albedo1 (centre)
layout (location = 5) flat in uint in_texture; // u_textures index + 1 (0 = none)

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion;

#define MOTION_WORLD_DELTA in_prevWorldDelta
#define SUN_SHADOW_FIRST
#define LIT_NO_RTAO
#include "instanced_indirect_lit.inc.glsl"
#include "grass.inc.glsl" // grassNearShadow
#include "clutter.inc.glsl"

void main()
{
#ifdef CLUTTER_TEXTURED
    // Before any lighting work: most of a frond's / petal's / ear's strip is cut away. Only the textured part (1) samples
    // (the stem and the centre stay solid); the derivatives are taken outside that branch.
    vec4 texel = vec4(1.0, 0.0, 0.0, 1.0);
    const vec2 uv = in_flower.zy;
    const vec2 uvDx = dFdx(uv), uvDy = dFdy(uv);
    if (in_texture != 0u && abs(in_flower.x - 1.0) < 0.5)
    {
        texel = textureGrad(u_textures[nonuniformEXT(in_texture - 1u)], uv, uvDx, uvDy);
        if (texel.a < 0.5)
            discard;
    }
#endif
    const vec3 L = u_sunDirection.xyz;
    const vec3 V = normalize(u_viewPos - in_pos);
    // The VS normal is ONE face of a two-sided card, bent (toward up / the head's axis) and not tied to the winding:
    // turned to the VIEWER (gl_FrontFacing left half the cards facing away - N.V clamped to 0 there, and the sun's GGX
    // at N.V = 0 lit them many times brighter than their diffuse), then bent toward V until N.V reaches the leaves'
    // minimum (as the foliage's edge-on leaves: "Foliage" min N.V).
    vec3 N = normalize(in_normal);
    if (dot(N, V) < 0.0)
        N = -N;
    const float noV = dot(N, V);
    if (noV < u_foliage_minNoV)
        N = normalize(N + V * (u_foliage_minNoV - noV));
    const vec3 litN = dot(N, L) >= 0.0 ? N : -N; // thin: the sun-facing side takes the shadow
    g_sunShadowFirst = sunShadowVisibility(in_pos, litN);
    float nearWeight;
    const float nearSun = grassNearShadow(in_pos, litN, nearWeight);
    g_sunShadowFirst *= mix(1.0, nearSun, nearWeight);

    const float part = in_flower.x;
    const float t = in_flower.y; // the VS writes `along` in [0, 1]
    float unused;
    vec3 albedo;
    float ao = 1.0;
    if (part < 0.5)
    {
        albedo = mix(u_grass_rootAlbedo, u_grass_tipAlbedo, t);
        ao = mix(1.0 - u_grass_rootOcclusion, 1.0, smoothstep(0.0, 0.6, t));
    }
    else if (part < 1.5)
    {
#ifdef CLUTTER_TEXTURED
        // The texture: R the shading, G a mask toward the second colour (a grain's awns, a fern's rachis).
        if (in_texture != 0u)
            albedo = mix(clutterUnpackAlbedo(in_look.x, unused), clutterUnpackAlbedo(in_look.y, unused), texel.g) * texel.r; // its shading is in R
        else
#endif
        albedo = clutterUnpackAlbedo(in_look.x, unused) * mix(0.75, 1.0, t);
    }
    else
        albedo = clutterUnpackAlbedo(in_look.y, unused);

    const float back = max(-dot(N, L), 0.0);
    const f16vec3 transmit = f16vec3(albedo * min(back * g_sunShadowFirst * u_clutter_flowerTransmission * INV_PI, MEDIUMP_FLT_MAX));
    vec3 color = computeLitColor(in_pos, V, f16vec3(N), f16vec3(albedo), float16_t(u_clutter_flowerRoughness), float16_t(0.0), float16_t(ao));
    color += vec3(transmit) * (u_sunTransmittance * u_sunColor.rgb);

    out_color = vec4(color, 1.0);
    out_motion = motionVectorNoAo(in_prevWorldDelta);
}
