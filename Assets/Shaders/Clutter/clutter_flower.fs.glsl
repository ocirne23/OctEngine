#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// THE FLOWERS' surface (clutter_flower.vs.glsl): the lit core on two-sided parts, without RTAO (as the grass). The stem
// in the grass's root -> tip colours, the petals in the type's colour (a little darker at their base), the centre in
// its second colour; the sun through a petal from behind ("Flower transmission"). The near grass cascade shades it.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec4 in_flower; // x part (0 stem, 1 petal, 2 centre), y along it
layout (location = 3) in vec3 in_prevWorldDelta;
layout (location = 4) flat in uvec2 in_look;

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
    const vec3 L = u_sunDirection.xyz;
    vec3 N = normalize(in_normal);
    if (!gl_FrontFacing)
        N = -N;
    const vec3 litN = dot(N, L) >= 0.0 ? N : -N; // thin: the side facing the sun takes the shadow
    g_sunShadowFirst = sunShadowVisibility(in_pos, litN);
    float nearWeight;
    const float nearSun = grassNearShadow(in_pos, litN, nearWeight);
    g_sunShadowFirst *= mix(1.0, nearSun, nearWeight);

    const float part = in_flower.x;
    const float t = clamp(in_flower.y, 0.0, 1.0);
    float roughness = u_clutter_flowerRoughness, unused;
    vec3 albedo;
    float ao = 1.0;
    if (part < 0.5)
    {
        albedo = mix(u_grass_rootAlbedo, u_grass_tipAlbedo, t);
        ao = mix(1.0 - u_grass_rootOcclusion, 1.0, smoothstep(0.0, 0.6, t));
    }
    else if (part < 1.5)
        albedo = clutterUnpackAlbedo(in_look.x, unused) * mix(0.75, 1.0, t);
    else
        albedo = clutterUnpackAlbedo(in_look.y, unused);

    const float back = max(-dot(N, L), 0.0);
    const f16vec3 transmit = f16vec3(albedo * min(back * g_sunShadowFirst * u_clutter_flowerTransmission * INV_PI, MEDIUMP_FLT_MAX));
    const vec3 V = normalize(u_viewPos - in_pos);
    vec3 color = computeLitColor(in_pos, V, f16vec3(N), f16vec3(albedo), float16_t(roughness), float16_t(0.0), float16_t(ao));
    color += vec3(transmit) * (u_sunTransmittance * u_sunColor.rgb);
    out_color = vec4(color, 1.0);
    out_motion = motionVector(in_prevWorldDelta);
}
