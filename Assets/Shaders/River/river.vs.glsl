#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// River / lake water VS (EPipelineIndex::River). Same pipeline layout and vertex input as the shared static-mesh VS (the
// same DGC execution set); its outputs are its own. MeshVertex as Procedural RiverSystem fills it:
//   positionU = (unit-local position, u): u = across a ribbon (-1 .. 1), 2 = a lake
//   normalV   = (dense near-cell mark (1/0), the channel's centre depth (m; lake 1e4), the water column under a dense vertex (m),
//               v = the distance along the river (m))
//   tangent   = (flow x speed (m/s) in x / z, the whitewater amount in y, the handedness)
// SIZE = centre depth / "Full size depth" (at most 1) scales the waves and the flow (mix("Small river flow", 1, size)).
// The waves displace only the DENSE ribbons and lake grids; a LIGHT ribbon or flat lake quad sinks "Near drop" wherever a
// dense cell can be (river.fs.glsl discards it where all cells are built).

#include "shared.inc.glsl"
#define RIVER_WAVE_TEX(uvLayer) textureLod(u_oceanMaps, uvLayer, 0.0)
#include "river_wave.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// The main cull's OutMeshInstance (instanced_indirect.cs.glsl). The prev* fields are unused (the dual-source blend writes no
// motion vectors), but the stride must match.
struct InMeshInstancesData
{
    vec4  posScale;
    vec4  quat;
    vec4  prevPosScale;
    uint  meshIdxMaterialIdx;
    uint  prevVertexDelta;
    uvec2 prevQuat;
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};

layout (location = 0) in vec4 in_posU;
layout (location = 1) in vec4 in_normalV;
layout (location = 2) in vec4 in_tangent;
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec2 out_uv;          // (across, along)
layout (location = 2) out vec3 out_flow;        // xz = the flow (m/s), y = the whitewater amount
layout (location = 3) out flat float out_dense; // 1 = a dense near-cell ribbon
layout (location = 4) out float out_waveSize;   // the waves' scale with the river's size

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[inst_idx];
    out_pos = quat_transform(in_posU.xyz * inst.posScale.w, inst.quat) + inst.posScale.xyz;
    out_uv = vec2(in_posU.w, in_normalV.w);
    out_waveSize = clamp(in_normalV.y / u_river_fullSizeDepth, 0.0, 1.0);
    const vec3 flow = quat_transform(vec3(in_tangent.x, 0.0, in_tangent.z), inst.quat) * mix(u_river_smallFlow, 1.0, out_waveSize);
    out_flow = vec3(flow.x, in_tangent.y, flow.z);
    out_dense = in_normalV.x;

    const bool lake = in_posU.w > 1.5;
    const float R = u_river_nearRadius;
    const float dist = distance(out_pos, u_views_viewPos[VIEW_CENTER].xyz); // the centre view: both eyes alike
    if (in_normalV.x > 0.5)
    {
        // Faded out toward "Near radius", a ribbon also over its last 40 % to each bank. A dense lake grid takes the waves
        // with its slow drift and "Lake ripple" (as river.fs.glsl shades it), out to its shore (the ground cuts the shoreline).
        const float near = R > 0.0 ? 1.0 - smoothstep(0.7 * R, R, dist) : 0.0;
        const float bank = lake ? 1.0 : 1.0 - smoothstep(0.6, 1.0, abs(in_posU.w));
        if (near * bank > 0.0)
        {
            const vec2 waveFlow = lake ? vec2(0.05, 0.03) * u_river_flowSpeed : flow.xz * u_river_flowSpeed;
            float h = riverWaveHeight(out_pos.xz, waveFlow, lake ? 0.0 : in_tangent.y) * ((lake ? u_river_lakeRipple : 1.0) * out_waveSize);

            // A trough is soft-limited to 70 % of the water column under the vertex, else a shallow stream shows its bed.
            const float floorDepth = 0.7 * in_normalV.z;
            if (h < 0.0)
                h = -floorDepth * (1.0 - exp(h / floorDepth)); // the CPU keeps the column >= 2e-4
            out_pos.y += h * near * bank;
        }
    }
    else if (R > 0.0)
        out_pos.y -= u_river_nearDrop * (1.0 - smoothstep(R + 2.0 * RIVER_NEAR_CELL, R + 3.0 * RIVER_NEAR_CELL, dist));

    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
