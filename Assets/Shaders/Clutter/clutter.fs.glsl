#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// THE RIGID CLUTTER's surface (clutter.vs.glsl): the scene's lit core (NOT RTAO, as the grass: the objects are not in
// the TLAS, and a pebble is far below the AO's resolution) with a material per KIND:
//   PEBBLE   - the climate's BEDROCK (the splat's rock entries, picked as a cliff there picks them), world-space
//              triplanar at a finer scale, tinted by the type's colour;
//   BRANCH   - the type's bark colour with streaks along the wood, the end grain (part 1) in its second colour;
//   MUSHROOM - the cap (part 1) in the type's colour with optional white spots, the stem (part 0) and the gills (2) in
//              its second colour.
// The foot darkens where the object meets the ground ("Contact darkening"); the near grass cascade shades it as it
// shades the blades.

#include "shared.inc.glsl"

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec3 in_local;
layout (location = 3) in vec4 in_fields; // x ao, y height above the base (m), z temperature C, w humidity
layout (location = 4) flat in uvec4 in_look; // x albedo0, y albedo1, z kind, w part
layout (location = 5) in vec4 in_uv;         // the mesh's pattern coordinates (a branch's in metres)
layout (location = 6) flat in float in_height; // the object's height (m)
layout (location = 7) in vec3 in_relPos;       // the position relative to the camera (exact near it: the bump's derivatives)

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec4 out_motion; // the scene's motion target (the opaque family): static

#define SUN_SHADOW_FIRST
#define LIT_NO_RTAO
#include "instanced_indirect_lit.inc.glsl"
#include "terrain_splat.inc.glsl"
#include "grass.inc.glsl"   // grassNearShadow, grassValueNoise (in_vertices: the lit core's binding 14)
#include "clutter.inc.glsl"

// A spot pattern on the mushroom cap: jittered cells in mesh space, a spot where the point is near its cell's centre.
float clutterSpots(vec3 p, float amount)
{
    const vec3 cellPos = p * 9.0;
    const ivec3 c = ivec3(floor(cellPos));
    float spot = 0.0;
    for (int i = 0; i < 2; ++i)
    {
        const ivec3 cc = c + ivec3(i, 0, 0);
        const uint h = grassHash(grassHash2(cc.xz) ^ uint(cc.y) * 0x27D4EB2Du);
        const vec3 centre = vec3(cc) + vec3(grassUnit(h), grassUnit(grassHash(h + 1u)), grassUnit(grassHash(h + 2u)));
        const float r = 0.18 + 0.25 * amount * grassUnit(grassHash(h + 3u));
        spot = max(spot, 1.0 - smoothstep(r * 0.8, r, distance(cellPos, centre)));
    }
    return spot * step(0.01, amount);
}

// BARK (a branch's part 0; uv: x along the wood, y around it, metres): plates split by FISSURES that run along the wood
// (ridged noise, stretched ~4:1 along it), a fine grain on the plates. 1 = a plate's top, 0 = the bottom of a fissure.
float clutterBarkHeight(vec2 uv)
{
    const vec2 p = vec2(uv.x * 16.0, uv.y * 64.0);
    const float n = 0.65 * grassValueNoise(p) + 0.35 * grassValueNoise(p * 2.3 + 7.1);
    const float line = 1.0 - abs(n * 2.0 - 1.0); // 1 on the lines between the plates
    const float fissure = smoothstep(0.72, 0.93, line);
    const float grain = grassValueNoise(vec2(uv.x * 60.0, uv.y * 260.0) + 3.3);
    return (1.0 - fissure) * (0.85 + 0.15 * grain);
}

// A bump normal from a height (m) over the surface, by its screen derivatives (Mikkelsen 2010): no tangent frame needed.
// `pos` must be smooth across the quad (the camera-relative position, not the world one). A degenerate frame (the
// surface edge-on, or no area: det ~ 0) keeps N - its gradient would be noise divided by nothing.
vec3 clutterBump(vec3 pos, vec3 N, float h)
{
    const vec3 dpdx = dFdx(pos), dpdy = dFdy(pos);
    const vec3 r1 = cross(dpdy, N), r2 = cross(N, dpdx);
    const float det = dot(dpdx, r1);
    if (abs(det) < 1e-12)
        return N;
    const vec3 grad = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
    const vec3 bumped = abs(det) * N - grad;
    // Never past the horizon: a bump tilts the normal, it does not turn it away from the surface.
    return dot(bumped, N) > 0.1 * length(bumped) ? normalize(bumped) : N;
}

void main()
{
    out_motion = motionVector(vec3(0.0));
    // The meshes' normals point OUT (every generator writes them so); the winding is not consistent between them, so
    // gl_FrontFacing must not flip them (the branch tubes lit from below did).
    const vec3 geoN = normalize(in_normal);
    // THE SHADOW FIRST (the register peak).
    g_sunShadowFirst = dot(geoN, u_sunDirection.xyz) > 0.0 ? sunShadowVisibility(in_pos, geoN) : 0.0;
    float nearWeight;
    const float nearSun = grassNearShadow(in_pos, geoN, nearWeight);
    g_sunShadowFirst *= mix(1.0, nearSun, nearWeight);

    float roughness, spots;
    const vec3 albedo0 = clutterUnpackAlbedo(in_look.x, roughness);
    const vec3 albedo1 = clutterUnpackAlbedo(in_look.y, spots);
    const uint kind = in_look.z;
    const uint part = in_look.w;

    vec3 albedo = albedo0;
    f16vec3 N = f16vec3(geoN);
    float ao = clamp(in_fields.x, 0.0, 1.0);
    if (kind == CLUTTER_KIND_PEBBLE)
    {
        albedo = vec3(0.42, 0.40, 0.38) * albedo0;
        if (u_terrain_splatBase >= 0.0 && u_terrain_numRock >= 1.0)
        {
            const vec2 climate = vec2(clamp((in_fields.z + 25.0) / 75.0, 0.0, 1.0), in_fields.w);
            const float invS2 = 1.0 / (2.0 * u_terrainTex_climateSigma * u_terrainTex_climateSigma);
            const ClimatePick r = pickClimate(climate, int(u_terrain_numGround), int(u_terrain_numRock), invS2);
            // Finer than a boulder: a pebble shows a few centimetres of the bedrock's grain.
            const TerrainSample s = sampleTerrainTriplanar(uint(u_terrain_splatBase) + climatePickIdx(r, 0), in_pos,
                f16vec3(geoN), u_terrainTex_uvScaleRock * u_rock_uvScale * 4.0);
            albedo = vec3(s.albedo) * albedo0;
            N = normalize(s.normal);
            roughness = float(s.rough);
            ao *= float(s.ao);
        }
    }
    else if (kind == CLUTTER_KIND_BRANCH)
    {
        // The detail fades out with the distance (the bump's derivatives turn to noise past a few pixels per fissure).
        const float detail = 1.0 - smoothstep(6.0, 25.0, distance(in_pos, u_viewPos));
        if (part == 1u)
        {
            // THE END GRAIN: growth rings around the cut's axis (uv.zw), a little wobbly, the heartwood darker.
            const float r = length(in_uv.zw);
            const float rings = 0.5 + 0.5 * sin(r * 1500.0 + 4.0 * grassValueNoise(in_uv.zw * 80.0));
            albedo = albedo1 * mix(1.0, mix(0.8, 1.05, rings), detail) * mix(0.8, 1.0, smoothstep(0.0, 0.01, r));
        }
        else
        {
            const float h = clutterBarkHeight(in_uv.xy);
            // Lichen in patches over the bark (grey-green), and larger blotches of weathering.
            const float lichen = smoothstep(0.66, 0.78, grassValueNoise(in_uv.xy * vec2(8.0, 14.0) + 11.0)) * 0.7;
            const float blotch = grassValueNoise(in_uv.xy * vec2(3.0, 6.0) + 3.7);
            albedo = albedo0 * mix(1.0, mix(0.4, 1.2, h), detail) * mix(0.8, 1.15, blotch);
            albedo = mix(albedo, vec3(0.20, 0.23, 0.15) * (0.8 + 0.4 * h), lichen * h);
            N = f16vec3(clutterBump(in_relPos, geoN, h * 0.004 * detail)); // fissures ~4 mm deep
            ao *= mix(1.0, mix(0.6, 1.0, h), detail);
        }
    }
    else // CLUTTER_KIND_MUSHROOM
    {
        // uv.xy = (cos, sin) of the angle around the mushroom: radial lines on the gills, faint streaks on the cap.
        const float around = atan(in_uv.y, in_uv.x);
        if (part == 1u)
            albedo = mix(albedo0 * (0.94 + 0.06 * sin(around * 36.0)), vec3(0.92, 0.9, 0.85), clutterSpots(in_local, spots));
        else if (part == 2u)
            albedo = albedo1 * (0.62 + 0.25 * smoothstep(-0.3, 0.6, sin(around * 90.0)));
        else
            albedo = albedo1;
    }
    // THE FOOT: darker where the object meets the ground - over the contact height, but at most a third of the object
    // (a fallen stick is only a few centimetres high: the whole of it was in the band).
    const float band = max(min(u_clutter_contactHeight, 0.35 * in_height), 1e-3);
    ao *= 1.0 - u_clutter_contactDarkening * (1.0 - smoothstep(0.0, band, in_fields.y));

    const vec3 V = normalize(u_viewPos - in_pos);
    out_color = vec4(computeLitColor(in_pos, V, N, f16vec3(albedo), float16_t(max(roughness, 0.05)), float16_t(0.0), float16_t(ao)), 1.0);
}
