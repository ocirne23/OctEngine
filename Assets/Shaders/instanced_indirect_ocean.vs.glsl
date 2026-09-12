#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// FFT ocean clipmap vertex shader (EPipelineIndex::Ocean) - the former OCEAN variant of
// instanced_indirect.vs.glsl, split into its own file. Same pipeline layout, vertex input and output
// interface as the shared static-mesh VS (it feeds the same DGC execution set), but the position path
// is the ocean displacement: the texcoord carries (ring cell size, morph weight). Over each ring's
// outer band the CDLOD morph collapses odd vertices onto the next ring's coarser (2*cell) lattice
// while the sampled mip blends +1, so adjacent rings meet exactly. The displaced position samples the
// maps at the ring-matched mip (fixed per world position - waves don't morph with camera motion). The
// fragment shader (ocean.fs.glsl) re-derives the shading normal per pixel from the (morphed) world XZ
// in out_uv. The G-buffer prepass (gbuffer.vs.glsl, OCEAN branch) MUST keep identical morph + lod +
// cull math or its depth diverges from the drawn surface.

// Depth-prepass reuse (gbuffer.vs.glsl) needs bit-exact positions across programs - see the note there.
invariant gl_Position;

#include "shared.inc.glsl"

#define TERRAIN_HEIGHT_BINDING 19
#include "ocean_wave.inc.glsl"

#ifdef STEREO
// VR renders one eye per pass (no multiview here: the forward pass's DGC execution set forbids it), so the
// view index (1 = left eye, 2 = right eye) is a push constant selecting the per-eye matrices via g_viewIndex.
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
layout (location = 3) in vec2 in_uv; // (ring cell size, morph weight) baked by OceanGenerator
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out mat3 out_tbn;
layout (location = 4) out vec2 out_uv;
layout (location = 5) out flat uint out_meshIdxMaterialIdx;

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
    vec3  inst_pos   = inst.posScale.xyz;
    float inst_scale = inst.posScale.w;
    vec4  inst_quat  = inst.quat;

    out_meshIdxMaterialIdx = inst.meshIdxMaterialIdx;

    // The morph snaps in MESH-LOCAL space: ring vertices are exact lattice multiples there by
    // construction, so a ring's collapsed edge coincides with the next ring's vertices for ANY node
    // position (snapping in world space aligned to an absolute lattice the node need not sit on, which
    // opened gaps at coarse-ring boundaries). floor(x+0.5) instead of round(): odd vertices sit exactly
    // at .5 and GLSL round() is implementation-defined there.
    // Negative cell size = HORIZON BAND vertex: exempt from the land cull. Its triangles run from the
    // ring edge out to the far plane, so they break the cull's +-3-cell footprint assumption - culling
    // one buried inner vertex would tear a huge slice out of the horizon.
    const float ringCell = abs(in_uv.x);
    const bool horizonBand = in_uv.x < 0.0;
    const float ringMorph = in_uv.y;
    vec3 localPos = in_pos;
    localPos.xz = mix(localPos.xz, floor(localPos.xz / (2.0 * ringCell) + 0.5) * (2.0 * ringCell), ringMorph);
    vec3 basePos = quat_transform(localPos * inst_scale, inst_quat) + inst_pos;
    // ONE shore fetch per vertex, shared by the land cull, the water-table lift and the displacement
    // (they each re-fetched it before). The prepass MUST share it the same way.
    const vec2 shoreHW = oceanSampleShoreData(basePos.xz); // (terrain height, water level)
    if (!horizonBand && oceanVertexCulled(basePos.xz, ringCell, shoreHW))
    {
        // Whole triangle footprint is buried under land: a NaN position discards every primitive using
        // this vertex before rasterization (the G-buffer prepass applies the identical test).
        out_pos = basePos;
        out_tbn = mat3(1.0);
        out_uv = basePos.xz;
        gl_Position = vec4(uintBitsToFloat(0x7FC00000u));
        return;
    }
    basePos.y += shoreHW.y - u_oceanParams2.w; // lift onto the local water table (lakes/rivers at altitude)
    out_pos = basePos;
    if (horizonBand)
    {
        // The band is FLAT: no wave displacement, just "Horizon level offset" (a pure Y shift - routing
        // it through shoreHW would change the depth and re-shoal the waves). Its triangles span
        // kilometres, so displacing their vertices interpolates the wave field across the entire
        // horizon and the far sea visibly heaves with the sea state; the coarse ring-matched mip damps
        // that but never removes it. The FS still shades the band with wave normals, which is all that
        // resolves at this range - and skipping the fetches makes band vertices nearly free.
        out_pos.y += u_oceanParams3.x;
    }
    else
        out_pos += oceanSampleDisplacement(basePos.xz, ringCell, ringMorph, shoreHW);
    out_tbn = mat3(vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0));
    out_uv  = basePos.xz;

    // Per-eye projection in VR (g_viewIndex set above) / centre view on desktop, with the same TAA
    // sub-pixel jitter both eyes (per-eye TAA accumulates it just like desktop).
    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
