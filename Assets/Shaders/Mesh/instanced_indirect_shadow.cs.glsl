#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable

#include "shared.inc.glsl"
#include "instance_cull.inc.glsl"

// Sun shadow caster cull: one draw list for all cascades. Each caster carries the mask of the cascades it overlaps;
// the multiview depth pass skips the others.

struct MaterialInfo         { uint flags; float opacity; uint diffuseNormalTexIdx; uint metalRoughnessTexIdxAlphaMode; };
// Matches RendererVKLayout::OutShadowMeshInstance. alphaTexIdxCascadeMask: alpha-mask texture << 16 (0xFFFF = none),
// bit 15 = sways in the wind, bits 0..14 = the cascade mask.
struct OutMeshInstance      { vec4 posScale; vec4 quat; uint alphaTexIdxCascadeMask; };

layout (binding = 6, std430) writeonly buffer OutMeshInstancesBuffer       { OutMeshInstance      out_meshInstances[]; };
layout (binding = 9, std430) readonly buffer InMaterialInfos               { MaterialInfo         in_materialInfos[]; };
#define TREE_CULL_PIECES_BINDING 13
#define TREE_CULL_TYPES_BINDING 14
#define TREE_CULL_LIST_BINDING 15
#include "tree_cull.inc.glsl"

// Bit c set if the sphere overlaps cascade c. The planes are normalized on the CPU (buildUboSunShadow).
uint cascadeOverlapMask(vec3 center, float radius)
{
    uint mask = 0u;
    for (uint c = 0; c < NUM_SHADOW_CASCADES; ++c)
    {
        bool inside = true;
        for (uint p = 0; p < 6; ++p)
        {
            const vec4 pl = u_cascadePlanes[c * 6u + p];
            if (dot(pl.xyz, center) + pl.w < -radius)
            {
                inside = false;
                break;
            }
        }
        if (inside)
            mask |= (1u << c);
    }
    return mask;
}

void cullCaster(uint instanceIdx, InMeshInstance instance, vec4 instancePosScale, vec4 quat, bool isTree)
{
    const uint materialIdx = (instance.meshIdxMaterialIdx & 0xFFFF0000u) >> 16;
    const MaterialInfo material = in_materialInfos[materialIdx];
    if ((material.flags & MATERIAL_FLAG_NO_RAYTRACING) != 0u)
        return; // gizmos cast no shadow
    uint meshIdx                  = instance.meshIdxMaterialIdx & 0x0000FFFF;
    const InMeshInfo meshInfo     = in_meshInfos[meshIdx];

    const vec3 centerOffset           = quat_transform(meshInfo.center * instancePosScale.w, quat);
    // Tree-set rocks: LOD chain, no wind.
    const bool isRock                 = isTree && (instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_LIT_ROCK;
    const bool sways                  = isTree && !isRock;
    const float radius                = meshInfo.radius * instancePosScale.w + (sways ? u_foliage_windReach : 0.0);
    const vec3 centerPos              = instancePosScale.xyz + centerOffset;

    uint cascadeMask = cascadeOverlapMask(centerPos, radius);
    // A cascade's box reaches far up-sun and takes in distant trees: keep a tree only within the cascade's split
    // (u_cascadeViewProj[c][0][3]) + the margin, from the scene focus.
    if (isTree)
    {
        const float reach = distance(centerPos, u_sceneFocus.xyz) - radius - u_present_treeShadowMargin;
        for (uint c = 0u; c < NUM_SHADOW_CASCADES; ++c)
            if (reach > u_cascadeViewProj[c][0][3])
                cascadeMask &= ~(1u << c);
    }
    if (cascadeMask == 0u)
        return;

    const uint alphaMode = (material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000u) >> 16;
    const uint alphaTexIdx = (alphaMode == ALPHA_MODE_MASK) ? (material.diffuseNormalTexIdx & 0x0000FFFFu) : 0xFFFFu;
    const uint packed = (alphaTexIdx << 16) | (cascadeMask & 0x00007FFFu) | (sways ? 0x00008000u : 0u);

    // Stateless LOD, two levels coarser than the main view (4x the error budget, +2 levels).
    InMeshInfo drawMeshInfo = meshInfo;
    const uint lodGroupIdx = sways ? 0xFFFFFFFFu : in_meshLodGroupIdx[meshIdx];
    if (lodGroupIdx != 0xFFFFFFFFu && u_lod_enabled > 0.5)
    {
        const MeshLodGroup group = in_meshLodGroups[lodGroupIdx];
        const float dist = max(0.01, length(centerPos - u_views_viewPos[VIEW_CENTER].xyz) - radius);
        const int level = lodResidentLevel(group, lodSelectLevel(group, dist, radius, instancePosScale.w,
            u_lod_maxErrorPx * 4.0, 2.0, -1));
        const uint chosenMeshIdx = lodMeshAt(group, level);
        if (chosenMeshIdx != meshIdx)
        {
            meshIdx = chosenMeshIdx;
            drawMeshInfo = in_meshInfos[meshIdx];
        }
    }

    const uint firstInstance = in_firstInstances[meshIdx];
    const uint idx = atomicAdd(out_indirectCommands[meshIdx].instanceCount, 1);
    if (idx == 0)
    {
        out_indirectCommands[meshIdx].pipelineIndex = 0u; // one depth pipeline
        out_indirectCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
        out_indirectCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
        out_indirectCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
        out_indirectCommands[meshIdx].firstInstance = firstInstance;
    }

    out_meshInstanceIndexes[firstInstance + idx]           = instanceIdx;
    out_meshInstances[instanceIdx].posScale                = instancePosScale;
    out_meshInstances[instanceIdx].quat                    = quat;
    out_meshInstances[instanceIdx].alphaTexIdxCascadeMask  = packed;
}

layout (local_size_x = 64) in; // one thread per stream instance, and one per tree

void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    if (gid >= u_present_treeThreads)
        return;
    bool isTree;
    uint pieceIdx, passBits;
    const uint instanceIdx = treeCullThreadInstance(gid, isTree, pieceIdx, passBits);
    // One loop for trees and plain instances (k = 0), so cullCaster is inlined once.
    TreeCullPiecePick pick;
    InMeshInstance instance;
    if (isTree)
    {
        if ((passBits & PASS_SHADOW) == 0u)
            return;
        // The type's shadow distance (bushes): beyond it the piece covers a texel or less.
        const float shadowDistance = in_treeTypes[in_treePieces[pieceIdx].type].shadowDistance;
        if (shadowDistance > 0.0
            && distance(in_treePieces[pieceIdx].centre, u_sceneFocus.xyz) - in_treePieces[pieceIdx].radius > shadowDistance)
            return;
        treeCullShadowPiece(pieceIdx, pick);
    }
    else
    {
        instance = in_instances[instanceIdx];
        if ((in_nodePassMasks[instance.renderNodeIdx] & PASS_SHADOW) == 0u)
            return;
        pick.kBegin = 0u;
        pick.kEnd = 1u;
        instanceTransform(instance, pick.posScale, pick.quat);
    }
    for (uint k = pick.kBegin; k < pick.kEnd; ++k)
    {
        if (isTree)
        {
            const TreeCullRecord rec = treeCullShadowRecord(pick, k);
            if (rec.meshMaterial == TREE_CULL_ABSENT)
                continue;
            instance = InMeshInstance(0u, 0u, rec.meshMaterial, rec.pipelineAlpha);
        }
        cullCaster(instanceIdx + k, instance, pick.posScale, pick.quat, isTree);
    }
}
