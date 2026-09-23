#version 450

// GPU vertex skinning. Reads a base mesh's vertices + per-vertex bone influences and a bone-matrix
// palette, writes the deformed vertices (same MeshVertex format, model space) into a per-instance output
// region of the shared vertex buffer. One indirect dispatch covers every skinned instance: the job list
// lives in a per-frame SSBO and gl_WorkGroupID.y selects the job, so adding/removing skinned instances
// never re-records the command buffer (dispatch dims come from a CPU-written indirect buffer).

#include "mesh_vertex.inc.glsl"

layout(local_size_x = 64) in;

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
};
layout(binding = 3, std430) readonly buffer JobBuffer { SkinningJob u_jobs[]; };

void main()
{
    const SkinningJob job = u_jobs[gl_WorkGroupID.y];
    const uint i = gl_GlobalInvocationID.x;
    if (i >= job.vertexCount)
        return;

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
    v_data[job.outVertexOffset + i] = o;
}
