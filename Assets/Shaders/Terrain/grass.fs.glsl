#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable

// THE GRASS BLADES' surface (grass.vs.glsl): the scene's lit core (no RTAO, see LIT_NO_RTAO) on a two-sided blade.
// Albedo from root to tip plus per-blade variation and dry patches; the root occlusion; the sun through the blade from
// behind (transmission, as the tree leaves). Toward the range the normal blends to the ground's, so the far blades
// shade like the terrain they fade into. No discard: the blades keep early depth.
// THE INTERFACE is small: the POSITION comes from the pixel's depth
// (viewRelFromDepth: camera-relative, exact near the camera), the NORMAL from the flat width axis (XZ, unit) and the
// interpolated normal's y (it lies across the width axis, on the upright stem's side) + the edge tilt from the across
// value, the flat values from one packed uvec4 (grass_blade.inc.glsl grassPackFlat).

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_blade;          // x = along the blade (0 root .. 1 tip), y = across (-1 .. 1), z = the normal's y
                                                 // (without the edge tilt)
layout (location = 1) in vec3 in_prevWorldDelta; // to last frame's point
layout (location = 2) flat in uvec4 in_flat;     // x = half2(albedo factor, dryness), y = the ground normal blend (float
                                                 // bits), z = the facet normal (grassUnpackGroundNormal), w = the
                                                 // look bits (the grass type | seed stalk << 2 | the seed stalks' cover << 8) | the
                                                 // width axis (its x, 15 bits | its z's sign) << 16

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target (the opaque family)

#define MOTION_WORLD_DELTA in_prevWorldDelta
#define SUN_SHADOW_FIRST
#ifdef GRASS_SIMPLE
// SIMPLE SHADING (the far rings' pipeline: "Grass/LOD/Simple shading distance", StaticMeshGraphicsPipeline): a far blade
// is a pixel or two wide and its fragments are most of the grass's cost - the thin triangles shade whole 2 x 2 pixel
// blocks. The same lit core, the cheap way: ONE shadow map tap for the sun (hardware PCF, the cascade cross-fade
// dithered - no blocker search, no soft penumbra: 1 read where the search takes 13), no near grass cascade (it ends
// metres from the camera), no underwater light (grass is never deep under water), the local lights' diffuse alone.
#define LIT_SUN_SHADOW_HARD
#define LIT_NO_UNDERWATER
#define LIT_LIGHTS_DIFFUSE
#else
// A LIGHTER SHADOW SEARCH than the scene's 6 gathers + 12 taps (shadows.inc.glsl): the search is this shader's largest
// cost, and a blade is a thin, moving surface that TAA resolves - the coarser penumbra grain does not show on it.
#define PCSS_BLOCKER_GATHERS 4
#define PCSS_FILTER_SAMPLES 8
#define LIT_NO_UNDERWATER // the grass cover ends at the water: no blade stands deep under it
// instanced_indirect_lit.inc.glsl: the local lights form the view vector and read the roughness again per light, so
// neither is live across a light's shadow ray (the register peak: 64 -> 56, the local memory 16 -> 32 B).
#define LIT_LIGHTS_ROUGHNESS float16_t(u_grass_roughness)
#endif
// NO RTAO: the trace works from the depth image, and on a blade that is a near-random normal from a thin sliver of
// depth a few centimetres over the ground (the blades are not in the TLAS): heavy noise, worst in shadow. The root
// occlusion stands in.
#define LIT_NO_RTAO
#define LIT_DIELECTRIC   // instanced_indirect_lit.inc.glsl: a blade is never metal
#define LIT_EXTRA_LIGHT  // the sun through the blade, added inside computeLitColor
#define LIT_CANOPY_HEIGHT 0.0 // instanced_indirect_lit.inc.glsl: on the ground under the trees' canopy
#include "instanced_indirect_lit.inc.glsl"
#include "grass.inc.glsl" // grassNearShadow, grassRootOcclusion (in_terrainVertices: the lit core's binding 14)
#define FACTION_MAP_BINDING 24
#include "terrain_faction.inc.glsl" // the game's faction layer: the blades take the ground's faction look

#ifndef GRASS_SIMPLE
float g_nearWeight = 0.0, g_nearSun = 1.0; // the near grass cascade at the fragment (main: taken before the sun's search)
#endif

// The blade's normal (one face): across the width axis at the interpolated y, on the upright stem's side, + the edge
// tilt (grass_blade.inc.glsl: the width axis x the across value x the "Roundness", fading to the tip), turned to the
// viewer. From the inputs alone: main forms it TWICE (before the shadow search and after) - an input costs no register
// between two reads, a normal held across the search costs three.
vec3 grassFragmentNormal()
{
    const uint sideBits = in_flat.w >> 16; // the width axis: its x (15 bits) and its z's sign; unit, so |z| follows
    const float sideX = float(sideBits & 0x7FFFu) * (2.0 / 32767.0) - 1.0;
    const float sideZ = sqrt(max(1.0 - sideX * sideX, 0.0));
    const vec3 side = vec3(sideX, 0.0, (sideBits & 0x8000u) != 0u ? -sideZ : sideZ);
    const float normalY = clamp(in_blade.z, -1.0, 1.0);
    const vec3 bladeN = normalize(vec3(side.z, 0.0, -side.x) * sqrt(1.0 - normalY * normalY) + vec3(0.0, normalY, 0.0)
        + side * (in_blade.y * u_grass_roundness * (1.0 - in_blade.x)));
    return gl_FrontFacing ? bladeN : -bladeN;
}

void main()
{
    // The position from the pixel's depth (the jitter taken out: the surface at this pixel).
    const vec2 uv = gl_FragCoord.xy * u_screenSize.zw - taaJitterUv(u_taaJitter.xy);
    const vec3 pos = u_viewPos + viewRelFromDepth(uv, gl_FragCoord.z);
    const vec3 L = u_sunDirection.xyz;

    // THE SHADOWS FIRST (the search is the register peak: only the position and the near cascade's two values are live
    // across it), from the blade's SUN side: the side facing away still needs its real shadow for the transmission.
    // The lookup moves TOWARD THE SUN by "Shadow bias (m)", full at the root and none at the tip: the root sinks below
    // the terrain mesh, into the ground's own shadow-map surface (a dark band at the bottom of every blade).
    {
        const vec3 bladeN = grassFragmentNormal();
        const vec3 sunSideN = dot(bladeN, L) >= 0.0 ? bladeN : -bladeN;
#ifndef GRASS_SIMPLE
        g_nearSun = grassNearShadow(pos, sunSideN, g_nearWeight);
#endif
        g_sunShadowFirst = sunShadowVisibility(pos + L * (u_grass_shadowBias * (1.0 - in_blade.x)), sunSideN);
    }
#ifdef GRASS_SIMPLE
    const float nearWeight = 0.0, nearSun = 1.0; // the far rings: past the near grass cascade
#else
    const float nearWeight = g_nearWeight, nearSun = g_nearSun;
#endif
    const float t = in_blade.x;
    const vec3 rel = pos - u_viewPos;
    const float dist = length(rel);
    const vec3 V = -rel / dist;
    // THE SEED STALKS ON A FAR BLADE of a seeding type (grass_blade.inc.glsl: from LOD 2 to LOD 3 the stalks themselves
    // stop showing, so the type's blades stand in for them): how much of this point shows as stalks - their cover of the
    // view, less at the root, which they do not hide. The stalks stand ABOVE the blades, so by that share the point
    // takes their colour and loses the root occlusion: as bright as the seed heads near the camera.
    const uint look = in_flat.w;
    const float seedShown = float((look >> 8) & 0xFFu) * (1.0 / 255.0) * smoothstep(0.0, 0.4, t);
    const vec2 factorDryness = unpackHalf2x16(in_flat.x); // the albedo factor (variation x cold), dryness
    // The blades' shadows on each other, on the direct sun and the transmission alike: the near grass cascade; outside
    // it "Shadows/Far root occlusion" stands in ON THE SUN TOO (full at the root, none at the tip, none on the share
    // that shows as seed stalks) - there is no blade shadow there, and on the ambient alone it could not get dark.
    // (u_grass_farRootSun: the tweak x the sun's height factor - more with a low sun, less with a high one.)
    // The far value may be above 1: the light is floored at 0, so the dark part climbs the blade.
    g_sunShadowFirst *= mix(mix(max(1.0 - u_grass_farRootSun * (1.0 - smoothstep(0.0, 1.0, t)), 0.0), 1.0, seedShown), nearSun, nearWeight);

    // THE COLOUR: root -> tip x the grass type's tint; a SEED STALK half straw, its seed head (the top "Seed head
    // length") in "Seed colour". Then the dry patches.
    const uint type = look & 3u;
    const bool stalk = (look & 4u) != 0u;
    vec3 albedo = mix(u_grass_rootAlbedo, u_grass_tipAlbedo, t) * u_grass_typeTint[type].rgb;
    if (stalk)
    {
        // Past the LODs that hold the head's shape, the seed colour takes more of the stalk (grass.inc.glsl grassStalkCoarse).
        const float seedLength = mix(u_grass_seedLength, max(u_grass_seedLength, GRASS_STALK_COARSE_HEAD), grassStalkCoarse(dist));
        albedo = t > 1.0 - seedLength ? u_grass_seedAlbedo : mix(albedo, u_grass_dryAlbedo, 0.5);
    }
    else
    {
        // The stalks' colour on the blade: half seed heads, half straw - none of the type's green (the far tier's
        // u_grass_typeFarStalk has it): the blade's own root and the other blades bring that.
        albedo = mix(albedo, 0.5 * (u_grass_seedAlbedo + u_grass_dryAlbedo), seedShown);
    }
    albedo = mix(albedo, u_grass_dryAlbedo * mix(0.75, 1.0, t), factorDryness.y);
    // THE MIDRIB: a line along the blade's middle, "Midrib" brighter (or darker), fading out up the blade and where the
    // blade is a few pixels wide (the across coordinate's screen rate: a whole blade per pixel and a half).
    const float across = abs(in_blade.y);
    const float acrossRate = fwidth(in_blade.y);
    const float rib = (1.0 - smoothstep(0.0, max(0.25, 1.5 * acrossRate), across)) * (1.0 - smoothstep(0.3, 0.8, acrossRate));
    albedo *= 1.0 + u_grass_midrib * rib * (1.0 - 0.6 * t) * (stalk ? 0.0 : 1.0);
    albedo *= factorDryness.x; // the per-blade variation and the cold darkening (grass_blade.inc.glsl)
    albedo = terrainFactionTint(albedo, pos.xz, 1.0); // the same rule as the ground under it (terrain_faction.inc.glsl)
    // THE ROOT OCCLUSION: "Look/Root occlusion" inside the near grass cascade, "Shadows/Far root occlusion" outside it
    // (grass.inc.glsl grassRootOcclusion), full at the root and none at the tip.
    const float ao = mix(max(1.0 - grassRootOcclusion(nearWeight) * (1.0 - smoothstep(0.0, 1.0, t)), 0.0), 1.0, seedShown);

    const vec3 N = normalize(mix(grassFragmentNormal(), grassUnpackGroundNormal(in_flat.z), uintBitsToFloat(in_flat.y)));
    // Lit from behind: the sun through the blade, tinted by it - the lit core's extra light (added right after its sun:
    // no value of it is live across the light loop, the register peak).
    const float back = max(-dot(N, L), 0.0);
    g_litExtraLight = f16vec3(min(albedo * (u_sunTransmittance * u_sunColor.rgb) * (back * g_sunShadowFirst * ao * u_grass_transmission * INV_PI),
        vec3(MEDIUMP_FLT_MAX)));

    const vec3 color = computeLitColor(pos, V, f16vec3(N), f16vec3(albedo), float16_t(u_grass_roughness), float16_t(0.0), float16_t(ao));
    out_color = vec4(color, 1.0);
    out_motion = motionVectorNoAo(in_prevWorldDelta);
}
