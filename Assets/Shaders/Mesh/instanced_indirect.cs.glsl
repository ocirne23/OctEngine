#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
//#extension GL_EXT_debug_printf : enable

#include "shared.inc.glsl"
#include "instance_cull.inc.glsl"
#include "tree_wind.inc.glsl" // a tree record's wind (treeWindOfTree, treeWindPack)

// Matches RendererVKLayout::OutMeshInstance. Written at its DRAW SLOT (firstInstance + its index in the bucket): the scene
// vertex shaders read it by gl_InstanceIndex, with no index buffer in between.
struct OutMeshInstance
{
    vec4 posScale;
    vec4 quat;
    vec4 prevPosScale;     // w = 0: did not move
    uint meshIdxMaterialIdx;
    uint prevVertexDelta;
    uvec2 prevQuat;        // packSnorm2x16 (x, y), (z, w)
};

layout (binding = 6, std430) writeonly buffer OutMeshInstancesBuffer
{
    OutMeshInstance out_meshInstances[];
};
layout (binding = 9, std430) writeonly buffer OutTransparentIndirectCommandBuffer
{
    OutIndirectCommand out_transparentIndirectCommands[];
};
// LOD hysteresis. Frames in flight may race on a slot; lodSelectLevel clamps stale values.
layout (binding = 13, std430) buffer LodLevelStateBuffer
{
    uint lodLevelState[];
};
layout (binding = 15, std430) buffer OutLodStatsBuffer
{
    uint out_lodStats[];
};
#ifndef TERRAIN_TESS_ROUTE
#define TERRAIN_TESS_ROUTE 0
#endif
// Tess pipelines cannot join the DGC execution set: plain indirect draws.
layout (binding = 16, std430) buffer OutTerrainTessCommandBuffer
{
    OutIndirectCommand out_terrainTessCommands[];
};
// Drawn before the transparent execute, so the ocean blends over it.
layout (binding = 17, std430) buffer OutTerrainFilmCommandBuffer
{
    OutIndirectCommand out_terrainFilmCommands[];
};
// The trees' VOXEL tier (LitTreeVoxel) writes FragDepth, which no other pipeline of the DGC set does: plain indirect draws.
layout (binding = 23, std430) buffer OutTreeVoxelCommandBuffer
{
    OutIndirectCommand out_treeVoxelCommands[];
};
layout (binding = 18, std430) readonly buffer InPrevRenderNodeTransformsBuffer
{
    RenderNodeTransform in_prevRenderNodeTransforms[];
};
layout (binding = 19, std430) readonly buffer InPrevNodeStatesBuffer
{
    uvec2 in_prevNodeStates[]; // last frame's in_nodeStates: x = PASS_* byte + push frame << 8
};

#define TREE_CULL_PIECES_BINDING 20
#define TREE_CULL_TYPES_BINDING 21
#define TREE_CULL_LIST_BINDING 22
#include "tree_cull.inc.glsl"

// Sphere vs the wetness clipmap window.
bool terrainOverlayCovers(vec3 pos, float radius)
{
    if (u_terrainWater_enabled < 0.5)
        return false;
    const vec2 lo = u_terrain_wetOrigin * u_terrainWater_texelSize;
    const vec2 hi = lo + float(TERRAIN_WET_RES) * u_terrainWater_texelSize;
    const vec2 d = pos.xz - clamp(pos.xz, lo, hi);
    return dot(d, d) <= radius * radius;
}

// Last frame's transform, only for a node pushed last frame that moved since; else prevPosScale.w = 0.
void prevInstanceTransform(InMeshInstance instance, out vec4 prevPosScale, out uvec2 prevQuat)
{
    prevPosScale = vec4(0.0);
    prevQuat = uvec2(0u);
    if ((in_prevNodeStates[instance.renderNodeIdx].x >> 8) != ((u_frameIndex - 1u) & 0xFFFFFFu))
        return;
    const RenderNodeTransform prev = in_prevRenderNodeTransforms[instance.renderNodeIdx];
    const RenderNodeTransform cur = in_renderNodeTransforms[instance.renderNodeIdx];
    if (prev.posScale == cur.posScale && prev.quat == cur.quat)
        return;
    const InMeshInstanceOffset offset = in_instanceOffsets[instance.instanceOffsetIdx];
    const vec4 q = quat_multiply(prev.quat, offset.quat);
    prevPosScale = vec4(prev.posScale.xyz + quat_transform(offset.posScale.xyz * prev.posScale.w, prev.quat), prev.posScale.w * offset.posScale.w);
    prevQuat = uvec2(packSnorm2x16(q.xy), packSnorm2x16(q.zw));
}

// The bucket lists a placed instance counts into: 0 opaque, 1 transparent, 2 the trees' voxel tier.
uint bucketAdd(uint list, uint meshIdx, uint count)
{
    if (list == 2u)
        return atomicAdd(out_treeVoxelCommands[meshIdx].instanceCount, count);
    if (list == 1u)
        return atomicAdd(out_transparentIndirectCommands[meshIdx].instanceCount, count);
    return atomicAdd(out_indirectCommands[meshIdx].instanceCount, count);
}

// The instance's index in its bucket. ONE atomic per (list, mesh) and subgroup, not one per instance: the lanes with the
// first remaining lane's key take their ranks in one claim, and the rest go round again. Correct for any set of active
// lanes (a lane leaves only after its claim), so divergence costs extra rounds, never a wrong index.
uint bucketClaim(uint list, uint meshIdx)
{
    const uint key = (list << 16) | meshIdx;
    for (;;)
    {
        if (key == subgroupBroadcastFirst(key))
        {
            const uvec4 lanes = subgroupBallot(true);
            uint base = 0u;
            if (subgroupElect())
                base = bucketAdd(list, meshIdx, subgroupBallotBitCount(lanes));
            return subgroupBroadcastFirst(base) + subgroupBallotExclusiveBitCount(lanes);
        }
    }
    return 0u;
}

bool frustumCheck(vec3 pos, float radius)
{
    for (int i = 0; i < 6; i++)
    {
        if (dot(vec4(pos, 1.0), u_frustumPlanes[i]) + radius < 0.0)
        {
            return false;
        }
    }
    return true;
}

// `treeWindScale`: a tree's wind scales (treeCullWind; the reach grows with the larger one above 1). `treeType`: its type.
void cullInstance(InMeshInstance instance, vec4 instancePosScale, vec4 quat, uint stateSlot, bool isTree, vec2 treeWindScale, uint treeType)
{
    uint meshIdx              = instance.meshIdxMaterialIdx & 0x0000FFFF;
    const InMeshInfo meshInfo = in_meshInfos[meshIdx];
    const vec3 centerOffset   = quat_transform(meshInfo.center * instancePosScale.w, quat);
    const bool isRock         = isTree && (instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_LIT_ROCK; // tree-set rocks: LOD chain, no wind
    const float radius        = meshInfo.radius * instancePosScale.w
                              + (isTree && !isRock ? u_foliage_windReach * max(max(treeWindScale.x, treeWindScale.y), 1.0) : 0.0);
    const vec3 centerPos      = instancePosScale.xyz + centerOffset;

    // The ocean mesh is undisplaced: pad by the measured wave displacement. Frustum test only.
    float cullRadius = radius;
    if ((instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_OCEAN)
        cullRadius += u_ocean_displacementExtent;

    if (frustumCheck(centerPos, cullRadius))
    {
        // Instances arrive as LOD0; the CPU sizes every chain member's bucket for the whole chain.
        InMeshInfo drawMeshInfo = meshInfo;
        // Tree records have no hysteresis slot: stateless pick.
        const uint lodGroupIdx = isTree && !isRock ? 0xFFFFFFFFu : in_meshLodGroupIdx[meshIdx];
        if (lodGroupIdx != 0xFFFFFFFFu && u_lod_enabled > 0.5)
        {
            const MeshLodGroup group = in_meshLodGroups[lodGroupIdx];
            const float dist = max(0.01, length(centerPos - u_views_viewPos[VIEW_CENTER].xyz) - radius);
            int level = lodSelectLevel(group, dist, radius, instancePosScale.w, u_lod_maxErrorPx, 0.0, isTree ? -1 : int(lodLevelState[stateSlot]));
            if (!isTree)
                lodLevelState[stateSlot] = uint(level);
            level = lodResidentLevel(group, level);
            const uint chosenMeshIdx = lodMeshAt(group, level);
#ifdef SHADER_STATS
            atomicAdd(out_lodStats[level], 1);
#endif
            if (chosenMeshIdx != meshIdx)
            {
                meshIdx = chosenMeshIdx;
                drawMeshInfo = in_meshInfos[meshIdx];
            }
        }

        const uint firstInstance   = in_firstInstances[meshIdx];
        const uint16_t pipelineIdx = uint16_t(instance.pipelineIdxAlphaMode & 0x0000FFFF);
        // By pipeline family, not alpha mode: each DGC set has one fragment output interface.
        const bool isTransparent   = ((PIPELINE_TRANSPARENT_MASK >> uint(pipelineIdx)) & 1u) != 0u;
        const bool isTerrain       = pipelineIdx == uint16_t(PIPELINE_IDX_TERRAIN_LIT);
        // Only chunks reaching into the tess fade end; +1 covers the VR eye offset.
        const bool terrainTess     = TERRAIN_TESS_ROUTE != 0 && isTerrain
            && distance(centerPos, u_views_viewPos[VIEW_CENTER].xyz) - radius < u_terrainTess_fadeEnd + 1.0;
        const bool terrainFilm     = isTerrain && terrainOverlayCovers(centerPos, radius);

        // The voxel tier: its own list and its own bucket counter (no other list draws a voxel box mesh).
        const bool isVoxel = pipelineIdx == uint16_t(PIPELINE_IDX_LIT_TREE_VOXEL);
        const uint idx = bucketClaim(isVoxel ? 2u : (isTransparent ? 1u : 0u), meshIdx);
        if (isVoxel)
        {
            if (idx == 0)
            {
                // THE BOX MESH'S FIRST QUAD ALONE (its bottom face, 6 indices: Procedural treeVoxelBoxMesh): the tier's vertex
                // shader makes a screen rectangle around the box of those four corners - 2 triangles a tree, not 12.
                out_treeVoxelCommands[meshIdx].pipelineIndex = pipelineIdx;
                out_treeVoxelCommands[meshIdx].indexCount    = 6u;
                out_treeVoxelCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                out_treeVoxelCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                out_treeVoxelCommands[meshIdx].firstInstance = firstInstance;
            }
        }
        else if (isTransparent)
        {
            if (idx == 0)
            {
                out_transparentIndirectCommands[meshIdx].pipelineIndex = pipelineIdx;
                out_transparentIndirectCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
                out_transparentIndirectCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                out_transparentIndirectCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                out_transparentIndirectCommands[meshIdx].firstInstance = firstInstance;
            }
        }
        else
        {
            if (idx == 0)
            {
                out_indirectCommands[meshIdx].pipelineIndex = pipelineIdx;
                // Tess still allocates DGC instance slots, but draws from its own list.
                out_indirectCommands[meshIdx].indexCount    = terrainTess ? 0u : drawMeshInfo.indexCount;
                out_indirectCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                out_indirectCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                out_indirectCommands[meshIdx].firstInstance = firstInstance;
            }

            if (isTerrain)
            {
                // Not exclusive: tess draws the ground, the film draws the (untessellated) water on top. Count raised
                // to cover this slot; instances in range outside the film discard their pixels.
                if (terrainTess)
                {
                    atomicMax(out_terrainTessCommands[meshIdx].instanceCount, idx + 1u);
                    out_terrainTessCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
                    out_terrainTessCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                    out_terrainTessCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                    out_terrainTessCommands[meshIdx].firstInstance = firstInstance;
                }
                if (terrainFilm)
                {
                    atomicMax(out_terrainFilmCommands[meshIdx].instanceCount, idx + 1u);
                    out_terrainFilmCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
                    out_terrainFilmCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                    out_terrainFilmCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                    out_terrainFilmCommands[meshIdx].firstInstance = firstInstance;
                }
            }
        }

        // Trees never move and are never skinned: a tree piece's three "last frame" fields carry its WIND instead
        // (tree_wind.inc.glsl treeWindPack: the gusts at its origin now and last frame, the trunk's sway, the fades - what
        // every vertex of the tree worked out for itself). A rock of the tree set: "did not move", no wind.
        // Worked out HERE, per drawn record, not once per tree before the record loop: held across the loop it took this
        // shader from 51 to 60 registers.
        // The fields that are ready go out first: the wind is then worked out with only the slot held.
        const uint slot = firstInstance + idx; // the draw slot: gl_InstanceIndex in the vertex shaders
        out_meshInstances[slot].posScale           = instancePosScale;
        out_meshInstances[slot].quat               = quat;
        // A TREE's record names its TYPE where a plain instance names its mesh (no scene shader reads a tree's mesh index):
        // the LitTree shaders find the leaves' voxel grid in the type table with it (tree_leaf_sun.inc.glsl).
        out_meshInstances[slot].meshIdxMaterialIdx = (instance.meshIdxMaterialIdx & 0xFFFF0000u) | (isTree && !isRock ? treeType : meshIdx);
        vec4 prevPosScale = vec4(0.0);
        uvec2 prevQuat = uvec2(0u);
        uint prevVertexDelta;
        if (isTree && !isRock)
        {
            treeWindPack(treeWindOfTree(instancePosScale.xyz, instancePosScale.w, treeWindScale), prevPosScale, prevQuat, prevVertexDelta);
        }
        else
        {
            // Read here (all the levels of a LOD chain share it), not from `meshInfo`: that one would be held across the wind.
            prevVertexDelta = in_meshInfos[meshIdx].prevVertexDelta;
            if (!isTree)
                prevInstanceTransform(instance, prevPosScale, prevQuat);
        }
        out_meshInstances[slot].prevPosScale       = prevPosScale;
        out_meshInstances[slot].prevVertexDelta    = prevVertexDelta;
        out_meshInstances[slot].prevQuat           = prevQuat;
    }
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

    // One loop for trees and plain instances (k = 0), so cullInstance is inlined once.
    TreeCullPiecePick pick;
    InMeshInstance instance;
    vec2 treeWindScale = vec2(1.0);
    if (isTree)
    {
        if ((passBits & PASS_MAIN) == 0u)
            return;
        if (!frustumCheck(in_treePieces[pieceIdx].centre, in_treePieces[pieceIdx].radius))
            return;
        if (!treeCullMainPiece(pieceIdx, pick))
            return;
        treeWindScale = treeCullWind(pick.typeIdx);
    }
    else
    {
        instance = in_instances[instanceIdx];
        const uvec2 nodeState = in_nodeStates[instance.renderNodeIdx];
        if ((nodeState.x & PASS_MAIN) == 0u)
            return;
        pick.kBegin = 0u;
        pick.kEnd = 1u;
        instanceTransform(instance, pick.posScale, pick.quat);
        pick.lodStateBase = uint(int(instanceIdx) + int(nodeState.y)); // stateSlot = instanceIdx + bias
    }

    for (uint k = pick.kBegin; k < pick.kEnd; ++k)
    {
        if (isTree)
        {
            const TreeCullRecord rec = treeCullMainRecord(pick, k);
            if (rec.meshMaterial == TREE_CULL_ABSENT)
                continue;
            instance = InMeshInstance(0u, 0u, rec.meshMaterial, rec.pipelineAlpha);
        }
        cullInstance(instance, pick.posScale, pick.quat, pick.lodStateBase + k, isTree, treeWindScale, isTree ? pick.typeIdx : 0u);
    }
}
