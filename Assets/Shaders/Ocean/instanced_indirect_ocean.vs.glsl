#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// FFT ocean clipmap VS (EPipelineIndex::Ocean). Same pipeline layout and vertex input as the static-mesh VS (it feeds the
// same DGC execution set). The texcoord carries (ring cell size, morph weight): over each ring's outer band the CDLOD morph
// collapses odd vertices onto the next ring's 2*cell lattice while the sampled mip blends +1, so adjacent rings meet
// exactly. The mip is fixed per world position (waves don't morph with camera motion). ocean.fs.glsl re-derives the
// normal per pixel from the morphed world XZ in out_uv.

#include "shared.inc.glsl"

#define TERRAIN_HEIGHT_BINDING 19
#include "ocean_wave.inc.glsl"

#ifdef STEREO
// VR renders one eye per pass (the DGC execution set forbids multiview): view index 1 = left, 2 = right.
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// The main cull's OutMeshInstance (instanced_indirect.cs.glsl). The ocean cannot write motion vectors (dual-source blend),
// but the stride must match.
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

// The texCoord (ring cell size, morph weight), baked by OceanGenerator, rides the two .w.
layout (location = 0) in vec4 in_posU;
layout (location = 1) in vec4 in_normalV;
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec3 out_pos;
layout (location = 4) out vec2 out_uv;
layout (location = 5) out flat uint out_meshIdxMaterialIdx;

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[inst_idx];
    vec3  inst_pos   = inst.posScale.xyz;
    float inst_scale = inst.posScale.w;
    vec4  inst_quat  = inst.quat;

    out_meshIdxMaterialIdx = inst.meshIdxMaterialIdx;

    // The morph snaps in MESH-LOCAL space: ring vertices are exact lattice multiples there, so a collapsed edge meets the
    // next ring for ANY node position (a world-space snap opened gaps). floor(x+0.5), not round(): odd vertices sit
    // exactly at .5, where round() is implementation-defined.
    // Negative cell size = HORIZON BAND vertex, exempt from the land cull: its triangles reach the far plane, past the
    // cull's +-3-cell footprint, so culling one buried vertex would tear a huge slice out of the horizon.
    const float ringCell = abs(in_posU.w);
    const bool horizonBand = in_posU.w < 0.0;
    const float ringMorph = in_normalV.w;
    vec3 localPos = in_posU.xyz;
    localPos.xz = mix(localPos.xz, floor(localPos.xz / (2.0 * ringCell) + 0.5) * (2.0 * ringCell), ringMorph);
    vec3 basePos = quat_transform(localPos * inst_scale, inst_quat) + inst_pos;

    // ONE shore fetch per vertex, shared by the land cull, the water-table lift and the displacement.
    const vec2 shoreHW = oceanSampleShoreData(basePos.xz); // (terrain height, water level)
    if (!horizonBand && oceanVertexCulled(basePos.xz, ringCell, shoreHW))
    {
        // The whole footprint is under land: a NaN position discards every primitive that uses this vertex.
        out_pos = basePos;
        out_uv = basePos.xz;
        gl_Position = vec4(uintBitsToFloat(0x7FC00000u));
        return;
    }

    basePos.y += shoreHW.y - u_ocean_seaLevel; // lift onto the local water table (lakes/rivers at altitude)
    out_pos = basePos;
    if (horizonBand)
    {
        // The band is FLAT, only "Horizon level offset" (a pure Y shift; through shoreHW it would re-shoal the waves).
        // Its triangles span kilometres, so displaced vertices would make the far sea heave with the sea state. The FS
        // still shades it with wave normals.
        out_pos.y += u_ocean_horizonLevelOffset;
    }
    else
        out_pos += oceanSampleDisplacement(basePos.xz, ringCell, ringMorph, shoreHW);
    out_uv  = basePos.xz;

    // Same TAA sub-pixel jitter for both VR eyes and desktop.
    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
