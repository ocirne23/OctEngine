#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// Terrain variant of instanced_indirect.vs.glsl. The terrain fragment shader needs only world position and
// the geometric normal (it builds its own tangent bases for XZ/triplanar splatting), so this VS skips the
// tangent/bitangent (no full TBN) and the UV entirely. The shared vertex input layout still describes
// tangent (loc 2) and uv (loc 3); leaving them unconsumed is valid.
//
// It ALSO evaluates the baked terrain fields (altitude/temperature/humidity/water level) here, per vertex,
// and hands them to the FS as one interpolant - see the comment at the evaluation below.

// Depth-prepass reuse (gbuffer.vs.glsl) needs bit-exact positions across programs - see the note there.
invariant gl_Position;

#include "shared.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 19
#include "terrain_height.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    uint meshIdxMaterialIdx;
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_terrainFields; // x = macro altitude, y = temperature C, z = humidity, w = water level

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
    out_pos    = quat_transform(in_pos * inst.posScale.w, inst.quat) + inst.posScale.xyz;

    // Baked terrain fields, evaluated PER VERTEX and interpolated (the FS used to fetch these per pixel:
    // 2 cascade taps + a 4-8 texel-decode climate bilinear). Interpolation loses nothing: every field is
    // band-limited far below any LOD's vertex lattice (climate is 30 m/px generator output, altitude is
    // the macro band, water level is sea/lake surfaces), and temperature = baseline + lapse * height is
    // LINEAR in height, so interpolating the evaluated value equals evaluating at the interpolated height.
    // Climate must stay BILINEAR here (terrainClimateAt, not the nearest variant): the splat blend weights
    // derive from it, and nearest sampling quantizes the blending to the data map's texel grid.
    float altitude = out_pos.y - u_terrainParams.z; // mild-climate fallbacks without a map
    float temperature = 12.5;
    float humidity = 0.5;
    float waterLevel = u_terrainParams.z;
    if (terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(out_pos.xz);
        altitude = td.w;
        waterLevel = td.y;
        const vec4 climate = terrainClimateAt(out_pos.xz);
        humidity = climate.w;
        // The map stores a SEA-LEVEL baseline + one lapse rate, evaluated at the shaded height - never
        // bake a temperature sample (only valid at the height it was taken; the cascades' heights differ).
        temperature = terrainTemperatureAt(climate, out_pos.y);
    }
    out_terrainFields = vec4(altitude, temperature, humidity, waterLevel);

    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
