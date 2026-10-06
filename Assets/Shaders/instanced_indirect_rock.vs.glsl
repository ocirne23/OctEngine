#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// The procedural rocks' variant of instanced_indirect.vs.glsl (EPipelineIndex::LitRock). The rock fragment shader
// takes its material from the climate and projects it in world space, so this VS passes only the world position, the
// geometric normal (no tangent, no uv - as the terrain's) and the vertex's baked cavity, and evaluates the
// terrain-data fields here, per vertex: the ground height under the vertex (the contact band) and the climate.

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// The main cull's OutMeshInstance (instanced_indirect.cs.glsl; the prev* fields are the motion vectors' - a rock is
// static and writes none, but the stride must match).
struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    vec4 prevPosScale;
    uint meshIdxMaterialIdx;
    uint prevVertexDelta;
    uvec2 prevQuat;
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};

layout (location = 0) in vec4 in_posU;   // xyz = position, w = uv.x: a rock has no uv - the u channel is its CAVITY
layout (location = 1) in vec3 in_normal;
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_rockFields; // x = ground height under the vertex (world Y), y = temperature C, z = humidity, w = water level
layout (location = 3) out float out_cavity;    // 1 = open, 0 = deep in a crevice (Procedural RockGenerator, from the shape's field)

vec3 quat_transform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[inst_idx];

    out_normal = quat_transform(in_normal, inst.quat);
    out_pos    = quat_transform(in_posU.xyz * inst.posScale.w, inst.quat) + inst.posScale.xyz;
    out_cavity = in_posU.w;

    // The baked terrain fields PER VERTEX, interpolated - the terrain VS's reasoning: each is band-limited far
    // below a rock's vertex spacing, and temperature = baseline + lapse x height is linear in height (the top of a
    // tall boulder is colder than its foot, as the ground at that height would be). Without a map: the instance
    // origin as the ground and a mild climate.
    float groundHeight = inst.posScale.y;
    float temperature = 12.5;
    float humidity = 0.5;
    float waterLevel = u_terrainLive_seaLevel;
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(out_pos.xz);
        groundHeight = td.x;
        waterLevel = td.y;
        const vec4 climate = terrainClimateAt(out_pos.xz);
        humidity = climate.w;
        temperature = terrainTemperatureAt(climate, out_pos.y);
    }
    out_rockFields = vec4(groundHeight, temperature, humidity, waterLevel);

    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
