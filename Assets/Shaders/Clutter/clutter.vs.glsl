#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE RIGID CLUTTER (pebbles, branches, mushrooms; with CLUTTER_NEAR_SHADOW: the near grass cascade's casters). One indexed
// draw per bucket (a mesh LOD): the mesh's vertices (ClutterVertexGpu, binding 0), the sorted record instance-rate
// (binding 1, firstInstance = the bucket's range). Static: no motion. The climate per vertex, as the rock VS.

#include "shared.inc.glsl"
#include "clutter.inc.glsl"
#ifndef CLUTTER_NEAR_SHADOW
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"
#endif

layout (location = 0) in vec4 in_posAo;
layout (location = 1) in vec4 in_normalPart;
layout (location = 2) in vec4 in_instPosScale;
layout (location = 3) in uvec4 in_instData;
layout (location = 4) in uvec4 in_instLook;
#ifndef CLUTTER_NEAR_SHADOW
layout (location = 5) in vec4 in_uv;


// THE INTERFACE: 20 components on triangles of a few pixels (each one is a plane equation per triangle), so what only ONE
// kind reads shares its slots with the others'.
// BOTH positions stay interpolated. The world position: the lit core holds a rebuilt one (relPos + the camera) across its
// light loop - FS 64 / 16 B -> 72 / 32 B. The position relative to the camera (mesh offset + (instance - camera), exact
// near the camera): the bump's screen derivatives read it (the world position's rounding, ~2 mm at 20 km, made them
// sparkle) and the FS's V.
layout (location = 0) out vec3 out_pos;
layout (location = 5) out vec3 out_relPos;
layout (location = 1) out vec3 out_normal;
// x ao, y height above the object's base (m); zw BY KIND - a pebble: temperature C, humidity (the only kind that reads the
// climate); a mushroom: its mesh-local position's x, y (scale 1: the cap's spots; z rides out_uv.z).
layout (location = 2) out vec4 out_fields;
// The mesh's pattern coordinates (a branch's in metres). A pebble has none; a mushroom reads xy alone (its z = local z).
layout (location = 3) out vec4 out_uv;
// x albedo0, y albedo1, z = kind | part << 4 | its snow (6 bits) << 8 | the object's height (m, a half: the contact band's
// scale) << 16.
layout (location = 4) flat out uvec3 out_look;
#endif

void main()
{
    const vec4 q = clutterUnpackQuat(in_instData.xy);
    const float scale = in_instPosScale.w;
    const vec3 offset = quat_transform(in_posAo.xyz * scale, q);
    const vec3 pos = offset + in_instPosScale.xyz;

#ifdef CLUTTER_NEAR_SHADOW
    gl_Position = u_grass_shadowViewProj * vec4(pos, 1.0);
#else
    const uint kind = in_instData.z >> 24;
    out_pos = pos;
    out_relPos = offset + (in_instPosScale.xyz - u_viewPos);
    out_normal = quat_transform(in_normalPart.xyz, q);

    // What only ONE kind reads shares its slots with the others' (the interface above).
    vec2 kindFields = vec2(0.0);
    vec4 uv = in_uv;
    if (kind == CLUTTER_KIND_PEBBLE)
    {
        // The climate, for the bedrock: only a pebble fetches it.
        kindFields = vec2(12.5, 0.5);
        if (terrainHeightMapPresent())
        {
            const vec4 climate = terrainClimateAt(pos.xz);
            kindFields = vec2(terrainTemperatureAt(climate, pos.y), climate.w);
        }
    }
    else if (kind == CLUTTER_KIND_BRANCH)
        uv = in_uv * scale; // bark: metres
    else if (kind == CLUTTER_KIND_MUSHROOM)
    {
        kindFields = in_posAo.xy;
        uv.z = in_posAo.z;
    }
    out_fields = vec4(in_posAo.w, in_posAo.y * scale, kindFields);
    out_uv = uv;
    // The part: fp32, written as exact 0 / 1 / 2. The snow: the record's 6 bits. The height: the low half of the record's
    // look.z, as it is.
    out_look = uvec3(in_instLook.x, in_instLook.y, kind | (uint(in_normalPart.w) << 4) | (((in_instData.z >> 18) & 63u) << 8)
        | ((in_instLook.z & 0xFFFFu) << 16));

    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
}
