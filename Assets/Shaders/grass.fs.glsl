#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// THE GRASS BLADES' surface (grass.vs.glsl): the scene's lit core (instanced_indirect_lit.inc.glsl - sun + shadow,
// RTAO, GI, the light grid) on a two-sided blade. Albedo from root to tip plus per-blade variation and dry patches;
// the root occlusion stands in for the blades shadowing each other (they cast no shadow map); the sun through the
// blade from behind (transmission, as the tree leaves). Toward the range the normal blends to the ground's, so the
// far blades shade like the terrain they fade into. No discard: the blades keep early depth.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec3 in_groundNormal;
layout (location = 3) in vec4 in_blade; // x = along the blade, y = albedo factor (variation x cold), z = dryness, w = ground normal blend
layout (location = 4) in vec3 in_prevWorldDelta;
layout (location = 5) in vec2 in_canopy; // x = depth below the canopy top (m), y = the canopy's extinction (1/m)

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target (the opaque family)

#define MOTION_WORLD_DELTA in_prevWorldDelta
#define SUN_SHADOW_FIRST
#include "instanced_indirect_lit.inc.glsl"
#include "grass.inc.glsl" // grassCanopySun (in_vertices: the lit core's binding 14)

void main()
{
    const vec3 pos = in_pos;
    const vec3 V = normalize(u_viewPos - pos);
    const vec3 L = u_sunDirection.xyz;
    vec3 bladeN = normalize(in_normal);
    if (!gl_FrontFacing)
        bladeN = -bladeN;
    // THE SHADOW FIRST (the register peak, nothing of the surface live yet), from the blade's SUN side: it is thin,
    // and the side facing away from the sun still needs its real shadow for the transmission.
    // The lookup moves TOWARD THE SUN by "Shadow bias (m)", full at the root and none at the tip: the root sinks below
    // the terrain mesh (root sink + half the tessellated relief), into the ground's own shadow-map surface - a dark
    // band at the bottom of every blade.
    const vec3 shadowPos = pos + L * (u_grassParams11.x * (1.0 - in_blade.x));
    g_sunShadowFirst = sunShadowVisibility(shadowPos, dot(bladeN, L) >= 0.0 ? bladeN : -bladeN);
    // THE CANOPY's shadow (grass.inc.glsl grassCanopySun): the blades above this point along the sun, as a volume with
    // its clumps and sun flecks - the self-shadowing a shadow map cannot resolve. On the direct sun and the
    // transmission alike.
    // Near the camera the REAL blade shadows (the near grass cascade) take over from it.
    float nearWeight;
    const float nearSun = grassNearShadow(pos, dot(bladeN, L) >= 0.0 ? bladeN : -bladeN, nearWeight);
    const float canopySun = nearWeight < 1.0 ? grassCanopySun(pos, in_canopy.x, in_canopy.y, distance(pos, u_viewPos)) : 1.0;
    g_sunShadowFirst *= mix(canopySun, nearSun, nearWeight);

    const float t = in_blade.x;
    vec3 albedo = mix(u_grassColor0.rgb, u_grassColor1.rgb, t);
    albedo = mix(albedo, u_grassColor2.rgb * mix(0.75, 1.0, t), in_blade.z);
    albedo *= in_blade.y; // the per-blade variation and the cold darkening (grass.vs.glsl)
    const float ao = mix(1.0 - u_grassShade.x, 1.0, smoothstep(0.0, 1.0, t));

    const vec3 N = normalize(mix(bladeN, normalize(in_groundNormal), in_blade.w));
    // Lit from behind: the sun through the blade, tinted by it. Formed before computeLitColor (its light loop is the
    // register peak): only this half colour is live across it.
    const float back = max(-dot(N, L), 0.0);
    const f16vec3 transmit = f16vec3(albedo * min(back * g_sunShadowFirst * ao * u_grassShade.y * INV_PI, MEDIUMP_FLT_MAX));

    vec3 color = computeLitColor(pos, V, f16vec3(N), f16vec3(albedo), float16_t(u_grassColor0.w), float16_t(0.0), float16_t(ao));
    color += vec3(transmit) * (u_sunTransmittance * u_sunColor.rgb);
    out_color = vec4(color, 1.0);
    out_motion = motionVector(in_prevWorldDelta);
}
