#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE RIGID CLUTTER (pebbles, branches, mushrooms; StaticMeshGraphicsPipeline's clutter pipeline, ClutterPipeline's
// near-shadow casters with CLUTTER_NEAR_SHADOW). One indexed draw per bucket (a mesh LOD): the mesh's vertices
// (ClutterVertexGpu, binding 0) and the sorted record as instance-rate attributes (binding 1, firstInstance = the
// bucket's range). Static: no motion of their own. The climate is read per vertex, as the rock VS does: a pebble takes
// the bedrock of the climate it lies in.

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

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec3 out_local;            // the mesh-local position (scale 1): the procedural patterns
layout (location = 3) out vec4 out_fields;           // x ao, y height above the object's base (m), z temperature C, w humidity
layout (location = 4) flat out uvec4 out_look;       // x albedo0, y albedo1, z kind, w part
layout (location = 5) out vec4 out_uv;               // the mesh's pattern coordinates (ClutterVertexGpu::uv; a branch's x the scale: metres)
layout (location = 6) flat out float out_height;     // the object's height (m): the contact band's scale
#endif

void main()
{
    const vec4 q = clutterUnpackQuat(in_instData.xy);
    const float scale = in_instPosScale.w;
    const vec3 pos = clutterQuatRotate(q, in_posAo.xyz * scale) + in_instPosScale.xyz;
#ifdef CLUTTER_NEAR_SHADOW
    // THE NEAR GRASS CASCADE's casters: the objects around its box only (as the blades).
    if (distance(in_instPosScale.xz, u_grassLive_nearCentre) > u_grassLive_nearRange * 1.5 + 2.0)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    gl_Position = u_grassLive_shadowViewProj * vec4(pos, 1.0);
#else
    out_pos = pos;
    out_normal = clutterQuatRotate(q, in_normalPart.xyz);
    out_local = in_posAo.xyz;
    float temperature = 12.5;
    float humidity = 0.5;
    if (terrainHeightMapPresent())
    {
        const vec4 climate = terrainClimateAt(pos.xz);
        humidity = climate.w;
        temperature = terrainTemperatureAt(climate, pos.y);
    }
    out_fields = vec4(in_posAo.w, in_posAo.y * scale, temperature, humidity);
    out_look = uvec4(in_instLook.x, in_instLook.y, in_instData.z >> 24, uint(in_normalPart.w + 0.5));
    out_uv = (in_instData.z >> 24) == CLUTTER_KIND_BRANCH ? in_uv * scale : in_uv; // bark: real metres
    out_height = unpackHalf2x16(in_instLook.z).x;
    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
}
