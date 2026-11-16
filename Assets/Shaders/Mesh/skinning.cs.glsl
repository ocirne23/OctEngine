#version 450

// GPU vertex skinning. Reads a base mesh's vertices + per-vertex bone influences and a bone-matrix palette, writes the
// deformed vertices (same MeshVertex format, model space) into a per-instance output region of the shared vertex
// buffer, and moves the positions they replace into the region's second half. One indirect dispatch covers every
// skinned instance pushed this frame: the packed job list lives in a per-frame SSBO, each job owns a run of workgroups
// from its firstGroup on, so adding/removing skinned instances never re-records the command buffer (dispatch dims come
// from a CPU-written indirect buffer; the groups run x first, then y rows).

#include "mesh_vertex.inc.glsl"

layout(local_size_x = 64) in; // RendererVKLayout::SKINNING_THREADS_PER_GROUP

layout(binding = 0, std430) buffer VertexBuffer { MeshVertex v_data[]; };

struct SkinningVertex { uvec4 boneIndices; vec4 boneWeights; };
layout(binding = 1, std430) readonly buffer SkinningBuffer { SkinningVertex s_data[]; };

layout(binding = 2, std430) readonly buffer PaletteBuffer { mat4 palette[]; };

// Must match RendererVKLayout::SkinningJob. Offsets in element units (MeshVertex / SkinningVertex / mat4).
struct SkinningJob
{
    uint baseVertexOffset;
    uint skinVertexOffset;
    uint outVertexOffset;
    uint vertexCount;
    uint paletteOffset;
    uint prevValid;  // 0 when not skinned last frame: no last frame in the region
    uint firstGroup; // the job's first workgroup
    uint pad;
};
// RendererVKLayout::SKINNING_JOB_HEADER_BYTES: the job count, then the jobs in firstGroup order.
layout(binding = 3, std430) readonly buffer JobBuffer { uint u_jobCount; uint u_jobPad0; uint u_jobPad1; uint u_jobPad2; SkinningJob u_jobs[]; };

void main()
{
    const uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    if (u_jobCount == 0u)
        return;
    // The last job whose firstGroup <= group (the same for the whole workgroup).
    uint lo = 0u;
    uint hi = u_jobCount;
    while (hi - lo > 1u)
    {
        const uint mid = (lo + hi) >> 1;
        if (u_jobs[mid].firstGroup <= group)
            lo = mid;
        else
            hi = mid;
    }
    const SkinningJob job = u_jobs[lo];
    const uint i = (group - job.firstGroup) * gl_WorkGroupSize.x + gl_LocalInvocationID.x;
    if (i >= job.vertexCount)
        return; // the job's last group, or a padding group of the last y row

    const MeshVertex v = v_data[job.baseVertexOffset + i];

    const SkinningVertex sv = s_data[job.skinVertexOffset + i];
    mat4 skin = mat4(0.0);
    for (int b = 0; b < 4; ++b)
    {
        const float w = sv.boneWeights[b];
        if (w > 0.0)
            skin += palette[job.paletteOffset + sv.boneIndices[b]] * w;
    }

    const mat3 skRot = mat3(skin);
    MeshVertex o;
    o.positionU = vec4((skin * vec4(v.positionU.xyz, 1.0)).xyz, v.positionU.w); // .w = uv.x, carried
    o.normalV   = vec4(normalize(skRot * v.normalV.xyz), v.normalV.w);          // .w = uv.y, carried
    o.tangent   = vec4(skRot * v.tangent.xyz, v.tangent.w);

    // The MOTION VECTORS: the region's second half keeps LAST frame's positions (MeshInfo::prevVertexDelta =
    // vertexCount), copied out before this frame's overwrite. Position only: nothing reads the rest.
    const uint outIdx = job.outVertexOffset + i;
    v_data[outIdx + job.vertexCount].positionU = job.prevValid != 0u ? v_data[outIdx].positionU : o.positionU;
    v_data[outIdx] = o;
}
