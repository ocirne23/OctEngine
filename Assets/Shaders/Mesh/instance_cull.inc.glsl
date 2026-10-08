#ifndef INSTANCE_CULL_INC_GLSL
#define INSTANCE_CULL_INC_GLSL

// Shared by the main cull (instanced_indirect.cs.glsl) and the shadow cull (instanced_indirect_shadow.cs.glsl): the
// bindings both have, the instance transform and the LOD redirect. Include after shared.inc.glsl. Binding 6 (the out
// instances) and the rest are each cull's own.

#include "mesh_lod.inc.glsl"

struct RenderNodeTransform
{
    vec4 posScale;
    vec4 quat;
};
struct InMeshInstance
{
    uint renderNodeIdx;
    uint instanceOffsetIdx;
    uint meshIdxMaterialIdx;
    uint pipelineIdxAlphaMode;
};
struct InMeshInstanceOffset
{
    vec4 posScale;
    vec4 quat;
};
struct InMeshInfo
{
    vec3 center;
    float radius;
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint prevVertexDelta; // skinned: offset to last frame's vertices (0 = not skinned)
};
// Matches RendererVKLayout::IndirectDrawSequence.
struct OutIndirectCommand
{
    uint pipelineIndex;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

layout (binding = 1, std430) readonly buffer InRenderNodeTransformsBuffer
{
    RenderNodeTransform in_renderNodeTransforms[];
};
layout (binding = 2, std430) readonly buffer InMeshInstancesBuffer
{
    InMeshInstance in_instances[];
};
layout (binding = 3, std430) readonly buffer InMeshInstanceOffsetsBuffer
{
    InMeshInstanceOffset in_instanceOffsets[];
};
layout (binding = 4, std430) readonly buffer InMeshInfoBuffer
{
    InMeshInfo in_meshInfos[];
};
layout (binding = 5, std430) readonly buffer InFirstInstancesBuffer
{
    uint in_firstInstances[];
};
layout (binding = 7, std430) writeonly buffer OutMeshInstanceIndexesBuffer
{
    uint out_meshInstanceIndexes[];
};
layout (binding = 8, std430) writeonly buffer OutIndirectCommandBuffer
{
    OutIndirectCommand out_indirectCommands[]; // the opaque list (the shadow cull's only one)
};
layout (binding = 10, std430) readonly buffer InNodePassMasksBuffer
{
    uint in_nodePassMasks[];
};
layout (binding = 11, std430) readonly buffer InMeshLodGroupIdxBuffer
{
    uint in_meshLodGroupIdx[]; // 0xFFFFFFFF = no chain
};
layout (binding = 12, std430) readonly buffer InMeshLodGroupsBuffer
{
    MeshLodGroup in_meshLodGroups[];
};

// The instance's world transform: its render node composed with its instance offset.
void instanceTransform(InMeshInstance instance, out vec4 posScale, out vec4 quat)
{
    const RenderNodeTransform node = in_renderNodeTransforms[instance.renderNodeIdx];
    const InMeshInstanceOffset offset = in_instanceOffsets[instance.instanceOffsetIdx];
    quat = quat_multiply(node.quat, offset.quat);
    posScale = vec4(node.posScale.xyz + quat_transform(offset.posScale.xyz * node.posScale.w, node.quat),
                    node.posScale.w * offset.posScale.w);
}

// The level to draw: the picked one, or while it is streamed out the nearest resident level.
int lodResidentLevel(MeshLodGroup group, int level)
{
    if (in_meshInfos[lodMeshAt(group, level)].indexCount != 0u)
        return level;
    for (int d = 1; d < int(group.numLods); ++d)
    {
        if (level - d >= 0 && in_meshInfos[lodMeshAt(group, level - d)].indexCount != 0u)
            return level - d;
        if (level + d < int(group.numLods) && in_meshInfos[lodMeshAt(group, level + d)].indexCount != 0u)
            return level + d;
    }
    return level;
}

#endif
