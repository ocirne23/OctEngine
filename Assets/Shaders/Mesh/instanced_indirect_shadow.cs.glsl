#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable

#include "shared.inc.glsl"
#include "instance_cull.inc.glsl"

// Sun shadow caster cull: one draw list for all cascades. Each caster carries the mask of the cascades it overlaps;
// the multiview depth pass skips the others.

struct MaterialInfo         { uint flags; float opacity; uint diffuseNormalTexIdx; uint metalRoughnessTexIdxAlphaMode; };
// Matches RendererVKLayout::OutShadowMeshInstance. alphaTexIdxCascadeMask: alpha-mask texture << 16 (0xFFFF = none),
// bit 15 = sways in the wind, bits 8..14 = its trunk-bend scale x 32 (no bit 15: bit 8 = a terrain node), bit 7 = a tree
// billboard, bits 0..6 = the cascade mask.
struct OutMeshInstance      { vec4 posScale; vec4 quat; uint alphaTexIdxCascadeMask; };

layout (binding = 6, std430) writeonly buffer OutMeshInstancesBuffer       { OutMeshInstance      out_meshInstances[]; };
layout (binding = 9, std430) readonly buffer InMaterialInfos               { MaterialInfo         in_materialInfos[]; };
#define TREE_CULL_PIECES_BINDING 13
#define TREE_CULL_TYPES_BINDING 14
#define TREE_CULL_LIST_BINDING 15
#include "tree_cull.inc.glsl"
// The trees' VOXEL casters ("Trees/Voxel shadows"): their own list, drawn with plain indirect draws by the voxel shadow
// pipeline (it writes FragDepth; ShadowMapGraphicsPipeline). The main cull's voxel list, borrowed (it runs later).
layout (binding = 16, std430) buffer OutVoxelCommandBuffer { OutIndirectCommand out_voxelCommands[]; };

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

// `windTrunk`: a tree's trunk-bend scale (treeCullWind), into bits 8..14 of the packed word (shadow_depth.vs.glsl).
void cullCaster(uint instanceIdx, InMeshInstance instance, vec4 instancePosScale, vec4 quat, bool isTree, float windTrunk)
{
    const uint materialIdx = (instance.meshIdxMaterialIdx & 0xFFFF0000u) >> 16;
    const MaterialInfo material = in_materialInfos[materialIdx];
    if ((material.flags & MATERIAL_FLAG_NO_RAYTRACING) != 0u)
        return; // gizmos cast no shadow

    uint meshIdx              = instance.meshIdxMaterialIdx & 0x0000FFFF;
    const InMeshInfo meshInfo = in_meshInfos[meshIdx];
    const vec3 centerOffset   = quat_transform(meshInfo.center * instancePosScale.w, quat);
    const bool isRock         = isTree && (instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_LIT_ROCK; // tree-set rocks: LOD chain, no wind
    const bool sways          = isTree && !isRock;
    const float radius        = meshInfo.radius * instancePosScale.w
                              + (sways ? u_foliage_windReach * max(windTrunk, 1.0) + abs(u_foliage_billboardShadowShift) : 0.0);
    const vec3 centerPos      = instancePosScale.xyz + centerOffset;

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

    const bool voxel = sways && (instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_LIT_TREE_VOXEL;
    const uint alphaMode = (material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000u) >> 16;
    // A voxel caster: its grid's albedo atlas (A = the opacity) in the same field.
    const uint alphaTexIdx = (alphaMode == ALPHA_MODE_MASK || voxel) ? (material.diffuseNormalTexIdx & 0x0000FFFFu) : 0xFFFFu;
    // Bits 0..6 the cascade mask (NUM_SHADOW_CASCADES = 4), 7 = a tree BILLBOARD (shifted toward the sun in the VS),
    // 8..14 the trunk-bend scale x 32 (0..~4), 15 = sways. Not swaying: bit 8 = a terrain node (TerrainVertex).
    const bool billboard = sways && (material.flags & MATERIAL_FLAG_BILLBOARD) != 0u;
    const uint windBits = sways ? uint(clamp(round(windTrunk * 32.0), 0.0, 127.0)) << 8
                                : ((material.flags & MATERIAL_FLAG_TERRAIN) != 0u ? 0x00000100u : 0u);
    const uint packed = (alphaTexIdx << 16) | (cascadeMask & 0x0000007Fu) | (billboard ? 0x00000080u : 0u) | windBits
                      | (sways ? 0x00008000u : 0u);

    // Stateless LOD, two levels coarser than the main view (4x the error budget, +2 levels).
    InMeshInfo drawMeshInfo = meshInfo;
    const uint lodGroupIdx = sways ? 0xFFFFFFFFu : in_meshLodGroupIdx[meshIdx];
    if (lodGroupIdx != 0xFFFFFFFFu && u_lod_enabled > 0.5)
    {
        const MeshLodGroup group = in_meshLodGroups[lodGroupIdx];
        const float dist = max(0.01, length(centerPos - u_views_viewPos[VIEW_CENTER].xyz) - radius);
        const int level = lodResidentLevel(group, lodSelectLevel(group, dist, radius, instancePosScale.w, u_lod_maxErrorPx * 4.0, 2.0, -1));
        const uint chosenMeshIdx = lodMeshAt(group, level);
        if (chosenMeshIdx != meshIdx)
        {
            meshIdx = chosenMeshIdx;
            drawMeshInfo = in_meshInfos[meshIdx];
        }
    }
    // A terrain node (its own mesh, one instance): a coarser shared index range over the same vertices by distance. The
    // pass is bound by its vertices, and only the vertices these indices name run.
    else if ((material.flags & MATERIAL_FLAG_TERRAIN) != 0u && u_terrain_shadowIndices.y > 0u)
    {
        const float reach = max(0.0, distance(centerPos, u_sceneFocus.xyz) - radius); // 0 = every node
        if (reach >= u_shadow_terrainQuarterFrom)
        {
            drawMeshInfo.firstIndex = u_terrain_shadowIndices.z;
            drawMeshInfo.indexCount = u_terrain_shadowIndices.w;
        }
        else if (reach >= u_shadow_terrainHalfFrom)
        {
            drawMeshInfo.firstIndex = u_terrain_shadowIndices.x;
            drawMeshInfo.indexCount = u_terrain_shadowIndices.y;
        }
    }

    const uint firstInstance = in_firstInstances[meshIdx];
    uint idx;
    if (voxel)
    {
        // Its own bucket counter: no other list draws a voxel box mesh.
        idx = atomicAdd(out_voxelCommands[meshIdx].instanceCount, 1);
        if (idx == 0)
        {
            out_voxelCommands[meshIdx].pipelineIndex = 0u;
            out_voxelCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
            out_voxelCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
            out_voxelCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
            out_voxelCommands[meshIdx].firstInstance = firstInstance;
        }
    }
    else
    {
        idx = atomicAdd(out_indirectCommands[meshIdx].instanceCount, 1);
        if (idx == 0)
        {
            out_indirectCommands[meshIdx].pipelineIndex = 0u; // one depth pipeline
            out_indirectCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
            out_indirectCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
            out_indirectCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
            out_indirectCommands[meshIdx].firstInstance = firstInstance;
        }
    }

    out_meshInstanceIndexes[firstInstance + idx]          = instanceIdx;
    out_meshInstances[instanceIdx].posScale               = instancePosScale;
    out_meshInstances[instanceIdx].quat                   = quat;
    out_meshInstances[instanceIdx].alphaTexIdxCascadeMask = packed;
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
    float treeWindTrunk = 1.0;
    if (isTree)
    {
        if ((passBits & PASS_SHADOW) == 0u)
            return;
        // The type's shadow distance (bushes): beyond it the piece covers a texel or less.
        const float shadowDistance = in_treeTypes[in_treePieces[pieceIdx].type].shadowDistance;
        if (shadowDistance > 0.0 && distance(in_treePieces[pieceIdx].centre, u_sceneFocus.xyz) - in_treePieces[pieceIdx].radius > shadowDistance)
            return;
        treeCullShadowPiece(pieceIdx, pick);
        treeWindTrunk = treeCullWind(pick.typeIdx).x;
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
        cullCaster(instanceIdx + k, instance, pick.posScale, pick.quat, isTree, treeWindTrunk);
    }
}
