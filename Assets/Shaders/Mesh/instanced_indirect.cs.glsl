#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
//#extension GL_EXT_debug_printf : enable

#include "shared.inc.glsl"

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
    uint prevVertexDelta; // skinned output: last frame's positions sit this many vertices on (0 = not skinned)
};
// Matches RendererVKLayout::OutMeshInstance. prev* = the MOTION VECTORS' input (the scene vertex shaders).
struct OutMeshInstance
{
    vec4 posScale;
    vec4 quat;
    vec4 prevPosScale;     // the instance transform last frame; w = 0: the node did not move
    uint meshIdxMaterialIdx;
    uint prevVertexDelta;  // MeshInfo::prevVertexDelta of the instance's mesh
    uvec2 prevQuat;        // packSnorm2x16 (x, y), (z, w)
};
// Matches RendererVKLayout::IndirectDrawSequence: an EXECUTION_SET pipelineIndex followed by a
// VkDrawIndexedIndirectCommand. Consumed by vkCmdExecuteGeneratedCommandsEXT.
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
layout (binding = 6, std430) writeonly buffer OutMeshInstancesBuffer
{
    OutMeshInstance out_meshInstances[];
};
layout (binding = 7, std430) writeonly buffer OutMeshInstanceIndexesBuffer
{
    uint out_meshInstanceIndexes[];
};

layout (binding = 8, std430) writeonly buffer OutIndirectCommandBuffer
{
    OutIndirectCommand out_indirectCommands[]; // opaque
};

layout (binding = 9, std430) writeonly buffer OutTransparentIndirectCommandBuffer
{
    OutIndirectCommand out_transparentIndirectCommands[];
};

layout (binding = 10, std430) readonly buffer InNodePassMasksBuffer
{
    uint in_nodePassMasks[]; // per render node, written at push time (PASS_* bits)
};

#include "mesh_lod.inc.glsl"

layout (binding = 11, std430) readonly buffer InMeshLodGroupIdxBuffer
{
    uint in_meshLodGroupIdx[]; // per mesh: MeshLodGroup index, 0xFFFFFFFF = no chain
};
layout (binding = 12, std430) readonly buffer InMeshLodGroupsBuffer
{
    MeshLodGroup in_meshLodGroups[];
};
// Per-instance LOD hysteresis state, addressed via the node's per-frame slot bias (below). Written by
// this pass only; frames in flight may race on a slot, but stale/garbage values are clamped into the
// current frame's valid band before use, so torn reads degrade to a fresh pick - never a wrong level.
layout (binding = 13, std430) buffer LodLevelStateBuffer
{
    uint lodLevelState[];
};
layout (binding = 14, std430) readonly buffer InNodeLodStateBiasBuffer
{
    int in_nodeLodStateBias[]; // per render node: stateSlot = instanceIdx + bias (nodes with LOD chains only)
};
layout (binding = 15, std430) buffer OutLodStatsBuffer
{
    uint out_lodStats[]; // per-level pick counts this frame (stats readback; written under SHADER_STATS only)
};
#ifndef TERRAIN_TESS_ROUTE
#define TERRAIN_TESS_ROUTE 0
#endif
// The TESSELLATED terrain ground (TERRAIN_TESS_ROUTE), same per-mesh-slot layout, consumed by plain
// vkCmdDrawIndexedIndirectCount (the pipelineIndex word is skipped) - a tess pipeline cannot join the DGC
// execution set, whose pipelines must all share the vertex + fragment stages. (The film is never tessellated.)
layout (binding = 16, std430) buffer OutTerrainTessCommandBuffer
{
    OutIndirectCommand out_terrainTessCommands[];
};
// The terrain FILM (EPipelineIndex::TerrainOverlay), same layout: its own list, drawn with plain indirect draws
// BEFORE the transparent execute, so the ocean always blends over it.
layout (binding = 17, std430) buffer OutTerrainFilmCommandBuffer
{
    OutIndirectCommand out_terrainFilmCommands[];
};
// The SKY list (RendererVKLayout::MAX_SKY_DRAWS): one plain indexed indirect draw per Sky-variant instance, drawn after
// the opaque execute and the tessellated ground - so early depth rejects every sky pixel the scene covers. (In the
// DGC sequence the sky draws in mesh-slot order: the scene's sky sphere came first, and its atmosphere march ran
// under the whole terrain.) The count is cleared to 0 before the cull.
struct SkyDraw
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
layout (binding = 23, std430) buffer OutSkyCommandBuffer
{
    uint out_skyCount;
    SkyDraw out_skyDraws[];
};
// LAST frame's node transforms + stamped pass masks (InstanceStream::recordPrevCopy): the motion vectors.
layout (binding = 18, std430) readonly buffer InPrevRenderNodeTransformsBuffer
{
    RenderNodeTransform in_prevRenderNodeTransforms[];
};
layout (binding = 19, std430) readonly buffer InPrevNodePassMasksBuffer
{
    uint in_prevNodePassMasks[]; // PASS_* byte + the push frame above it (InstanceStream::stampedPassMask)
};

// 20 + 21: the baked tree records' static data (tree_cull.inc.glsl): this frame's tree range of the stream is
// built from them instead of read. 22: this frame's list of the trees to cull.
#define TREE_CULL_PIECES_BINDING 20
#define TREE_CULL_TYPES_BINDING 21
#define TREE_CULL_LIST_BINDING 22
#include "tree_cull.inc.glsl"

vec3 quat_transform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec4 quat_multiply(vec4 q, vec4 p)
{
    vec4 c, r;
    c.xyz = cross(q.xyz, p.xyz);
    c.w = -dot(q.xyz, p.xyz);
    r = p * q.w + c;
    r.xyz = (q * p.w + r).xyz;
    return r;
}

// The terrain overlay's reach: the wetness clipmap window (terrain_wetness.inc.glsl - origin lattice
// coord u_terrain_wetOrigin, texel size u_terrainWater_texelSize, present flag u_terrainWater_enabled).
bool terrainOverlayCovers(vec3 pos, float radius)
{
    if (u_terrainWater_enabled < 0.5)
        return false;
    const vec2 lo = u_terrain_wetOrigin * u_terrainWater_texelSize;
    const vec2 hi = lo + float(TERRAIN_WET_RES) * u_terrainWater_texelSize;
    const vec2 d = pos.xz - clamp(pos.xz, lo, hi);
    return dot(d, d) <= radius * radius;
}

// The MOTION VECTORS' instance transform LAST frame, only for a node that moved since: its previous
// transform composed with the same instance offset. prevPosScale.w = 0 = no node motion - also when the
// node was not pushed last frame (the push stamp above the PASS_* byte), so a slot recycled by a spawn or
// a node back on screen never reads a stale transform. A still node keeps its exact current transform in
// the vertex shader: the snorm quaternion is for moving nodes only.
void prevInstanceTransform(InMeshInstance instance, out vec4 prevPosScale, out uvec2 prevQuat)
{
    prevPosScale = vec4(0.0);
    prevQuat = uvec2(0u);
    if ((in_prevNodePassMasks[instance.renderNodeIdx] >> 8) != ((u_frameIndex - 1u) & 0xFFFFFFu))
        return;
    const RenderNodeTransform prev = in_prevRenderNodeTransforms[instance.renderNodeIdx];
    const RenderNodeTransform cur = in_renderNodeTransforms[instance.renderNodeIdx];
    if (prev.posScale == cur.posScale && prev.quat == cur.quat)
        return;
    const InMeshInstanceOffset offset = in_instanceOffsets[instance.instanceOffsetIdx];
    const vec4 q = quat_multiply(prev.quat, offset.quat);
    prevPosScale = vec4(prev.posScale.xyz + quat_transform(offset.posScale.xyz * prev.posScale.w, prev.quat),
                        prev.posScale.w * offset.posScale.w);
    prevQuat = uvec2(packSnorm2x16(q.xy), packSnorm2x16(q.zw));
}

bool frustumCheck(vec3 pos, float radius)
{
    // Check sphere against frustum planes
    for (int i = 0; i < 6; i++) 
    {
        if (dot(vec4(pos, 1.0), u_frustumPlanes[i]) + radius < 0.0)
        {
            return false;
        }
    }
    return true;
}

// One instance (or one tree record) through the frustum test, the LOD pick and the draw-list emit.
void cullInstance(uint instanceIdx, InMeshInstance instance, vec4 instancePosScale, vec4 quat, uint stateSlot, bool isTree)
{
    uint meshIdx                  = instance.meshIdxMaterialIdx & 0x0000FFFF;
    const InMeshInfo meshInfo     = in_meshInfos[meshIdx];
    const vec3 centerOffset           = quat_transform(meshInfo.center * instancePosScale.w, quat);
    // A ROCK of the tree set (its records draw on LitRock - Procedural's world rocks): a regular mesh with an LOD
    // chain, and no wind.
    const bool isRock                 = isTree && (instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_LIT_ROCK;
    // A tree sways in the wind (tree_wind.inc.glsl): its bound grows by the sway's reach.
    const float radius                = meshInfo.radius * instancePosScale.w + (isTree && !isRock ? u_foliage_windReach : 0.0);
    const vec3 centerPos              = instancePosScale.xyz + centerOffset;

    // The ocean clipmap's mesh is the UNDISPLACED lattice: its vertex shader then moves every vertex by
    // the wave height and by the CHOPPY horizontal displacement, which scales with the "Choppiness"
    // tweak. A sector's own bounding sphere absorbs some of that incidentally (the XZ half-diagonal
    // exceeds the half-width), and that spare slack is what choppiness eventually runs out of - sectors
    // then get culled with their crests still on screen, showing as gaps along the screen edges. Pad by
    // the live extent the CPU measures off the displacement readback. Frustum test only: the LOD
    // selection below wants the real bounds (and the ocean has no LOD chain anyway).
    float cullRadius = radius;
    if ((instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_OCEAN)
        cullRadius += u_ocean_displacementExtent;

    if (frustumCheck(centerPos, cullRadius))
    {
        // GPU LOD selection: instances always arrive referencing LOD0 (whose bounds culled above);
        // redirect to the selected level's mesh. Buckets have room because the CPU sizes every chain
        // member's bucket to the chain's full instance count.
        InMeshInfo drawMeshInfo = meshInfo;
        // Trees have no mesh LOD chains (their own tiers instead; Procedural TreeSystem): no lookup for their records.
        // The tree set's ROCKS have one. A tree-set record has no hysteresis slot, so a rock picks STATELESS (the
        // conservative pick, as the shadow cull's).
        const uint lodGroupIdx = isTree && !isRock ? 0xFFFFFFFFu : in_meshLodGroupIdx[meshIdx];
        if (lodGroupIdx != 0xFFFFFFFFu && u_lod_enabled > 0.5)
        {
            const MeshLodGroup group = in_meshLodGroups[lodGroupIdx];
            const float dist = max(0.01, length(centerPos - u_views_viewPos[VIEW_CENTER].xyz) - radius);
            int level = lodSelectLevel(group, dist, radius, instancePosScale.w,
                u_lod_maxErrorPx, 0.0, isTree ? -1 : int(lodLevelState[stateSlot]));
            if (!isTree)
                lodLevelState[stateSlot] = uint(level);
            uint chosenMeshIdx = lodMeshAt(group, level);
            if (in_meshInfos[chosenMeshIdx].indexCount == 0u)
            {
                // Selected level streamed out (re-stream in flight): nearest resident level meanwhile.
                for (int d = 1; d < int(group.numLods); ++d)
                {
                    if (level - d >= 0 && in_meshInfos[lodMeshAt(group, level - d)].indexCount != 0u) { level -= d; break; }
                    if (level + d < int(group.numLods) && in_meshInfos[lodMeshAt(group, level + d)].indexCount != 0u) { level += d; break; }
                }
                chosenMeshIdx = lodMeshAt(group, level);
            }
#ifdef SHADER_STATS
            atomicAdd(out_lodStats[level], 1);
#endif
            if (chosenMeshIdx != meshIdx)
            {
                meshIdx = chosenMeshIdx;
                drawMeshInfo = in_meshInfos[meshIdx];
            }
        }

        const uint firstInstance      = in_firstInstances[meshIdx];
        const uint16_t pipelineIdx    = uint16_t(instance.pipelineIdxAlphaMode & 0x0000FFFF);
        const uint16_t alphaMode      = uint16_t((instance.pipelineIdxAlphaMode & 0xFFFF0000) >> 16);
        // Routed by the pipeline's FAMILY (PIPELINE_TRANSPARENT_MASK, Layout.ixx), not the alpha mode: each
        // sequence executes with its own DGC set, and a set has ONE fragment output interface (the opaque family
        // writes the motion target too). The OCEAN is of the transparent family: it blends its edge over the ground
        // (ocean.fs.glsl), so it must come after every terrain draw - the tessellated ground and film run between
        // the two executes. (A Blend material always gets a transparent-family pipeline, see ObjectContainer; an
        // override that puts it on an opaque one draws unblended either way.)
        const bool isTransparent      = ((PIPELINE_TRANSPARENT_MASK >> uint(pipelineIdx)) & 1u) != 0u;

        uint idx;
        if (isTransparent)
        {
            idx = atomicAdd(out_transparentIndirectCommands[meshIdx].instanceCount, 1);
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
            // Tessellated terrain: the DGC sequence still allocates the instance slots (atomicAdd below) but
            // draws NO indices; the draw itself goes to the tess sequence, its count raised to cover this slot
            // (as the overlay's), every writer storing the same other fields.
            // TERRAIN_TESS_ROUTE: baked by IndirectCullComputePipeline ("Terrain/Tessellation/Enabled").
            // Only chunks REACHING INTO the fade end: past it the edge factor is 1 and nothing is displaced, so
            // a chunk wholly beyond it is the same surface through the plain DGC path, without the control /
            // evaluation stages (their ISBE storage was the pass's second launch limiter). The margin covers
            // the VR eyes' offset from the centre view.
            const bool terrainTess = TERRAIN_TESS_ROUTE != 0 && pipelineIdx == uint16_t(PIPELINE_IDX_TERRAIN_LIT)
                && distance(centerPos, u_views_viewPos[VIEW_CENTER].xyz) - radius < u_terrainTess_fadeEnd + 1.0;
            // The SKY: its own list (binding 23), drawn late; its DGC entry draws nothing, as the tessellated ground's.
            const bool sky = pipelineIdx == uint16_t(PIPELINE_IDX_SKY);            idx = atomicAdd(out_indirectCommands[meshIdx].instanceCount, 1);
            if (sky)
            {
                const uint s = atomicAdd(out_skyCount, 1u);
                if (s < MAX_SKY_DRAWS)
                    out_skyDraws[s] = SkyDraw(drawMeshInfo.indexCount, 1u, drawMeshInfo.firstIndex, drawMeshInfo.vertexOffset, firstInstance + idx);
            }
            if (idx == 0)
            {
                out_indirectCommands[meshIdx].pipelineIndex = pipelineIdx;
                out_indirectCommands[meshIdx].indexCount    = terrainTess || sky ? 0u : drawMeshInfo.indexCount;
                out_indirectCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                out_indirectCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                out_indirectCommands[meshIdx].firstInstance = firstInstance;
            }
            if (terrainTess)
            {
                atomicMax(out_terrainTessCommands[meshIdx].instanceCount, idx + 1u);
                out_terrainTessCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
                out_terrainTessCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                out_terrainTessCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                out_terrainTessCommands[meshIdx].firstInstance = firstInstance;
            }
            // The TERRAIN OVERLAY (EPipelineIndex::TerrainOverlay: the surface-water film, later more terrain
            // surface layers): the same chunk drawn again over the ground, from its OWN list (binding 17, drawn
            // after the tessellated ground and before the transparent execute - so the ocean blends over it)
            // over the SAME instance list. Only for chunks overlapping the wetness clipmap. The count is
            // raised to this instance's slot + 1, so every overlapping instance lies inside the drawn range
            // (a non-overlapping instance drawn along with it discards every pixel); the other fields are the
            // same values from every writer. NEVER tessellated: also over a tessellated chunk, the film is this
            // untessellated draw (lifted to its water level in the terrain VS, depth test GREATER_OR_EQUAL).
            if (pipelineIdx == uint16_t(PIPELINE_IDX_TERRAIN_LIT) && terrainOverlayCovers(centerPos, radius))
            {
                atomicMax(out_terrainFilmCommands[meshIdx].instanceCount, idx + 1u);
                out_terrainFilmCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
                out_terrainFilmCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                out_terrainFilmCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                out_terrainFilmCommands[meshIdx].firstInstance = firstInstance;
            }
        }

        vec4 prevPosScale = vec4(0.0); // trees never move
        uvec2 prevQuat = uvec2(0u);
        if (!isTree)
            prevInstanceTransform(instance, prevPosScale, prevQuat);
        out_meshInstanceIndexes[firstInstance + idx]      = instanceIdx;
        out_meshInstances[instanceIdx].posScale           = instancePosScale;
        out_meshInstances[instanceIdx].quat               = quat;
        out_meshInstances[instanceIdx].prevPosScale       = prevPosScale;
        out_meshInstances[instanceIdx].meshIdxMaterialIdx = (instance.meshIdxMaterialIdx & 0xFFFF0000u) | meshIdx;
        out_meshInstances[instanceIdx].prevVertexDelta    = meshInfo.prevVertexDelta; // LOD0's: every level shares its region
        out_meshInstances[instanceIdx].prevQuat           = prevQuat;
    }
}

layout (local_size_x = 64) in; // one thread per stream instance, and one per TREE in the tree range (tree_cull.inc.glsl)

void main()
{
    const uint gid = gl_GlobalInvocationID.x;
    if (gid >= u_present_treeThreads)
        return;
    bool isTree;
    uint pieceIdx, passBits;
    const uint instanceIdx = treeCullThreadInstance(gid, isTree, pieceIdx, passBits);
    // A BAKED TREE (tree_cull.inc.glsl): its records built from the static tree data, their stream entries never
    // written. Decided once for the piece; each record it draws then culls on its own mesh bounds. The loop serves
    // the plain instance too (one pass, k = 0), so cullInstance has ONE call site - each is a full inlined copy.
    TreeCullPiecePick pick;
    InMeshInstance instance;
    if (isTree)
    {
        if ((passBits & PASS_MAIN) == 0u)
            return; // its terrain chunk is listed for shadows/GI only
        // The whole tree's sphere (its far representation's, around every mesh) off screen: nothing of it can draw.
        if (!frustumCheck(in_treePieces[pieceIdx].centre, in_treePieces[pieceIdx].radius))
            return;
        if (!treeCullMainPiece(pieceIdx, pick))
            return; // nothing of this tree draws this frame
    }
    else
    {
        instance = in_instances[instanceIdx];
        if ((in_nodePassMasks[instance.renderNodeIdx] & PASS_MAIN) == 0u)
            return; // pushed for shadows/GI only
        pick.kBegin = 0u;
        pick.kEnd = 1u;
        pick.quat                         = quat_multiply(in_renderNodeTransforms[instance.renderNodeIdx].quat, in_instanceOffsets[instance.instanceOffsetIdx].quat);
        const vec4 renderNodePosScale     = in_renderNodeTransforms[instance.renderNodeIdx].posScale;
        const vec4 instanceOffsetPosScale = in_instanceOffsets[instance.instanceOffsetIdx].posScale;
        pick.posScale                     = vec4(renderNodePosScale.xyz + quat_transform(instanceOffsetPosScale.xyz * renderNodePosScale.w, in_renderNodeTransforms[instance.renderNodeIdx].quat),
                                                renderNodePosScale.w * instanceOffsetPosScale.w);
        pick.lodStateBase                 = uint(int(instanceIdx) + in_nodeLodStateBias[instance.renderNodeIdx]);
    }
    // A tree's FIXED record slots (tree_cull.inc.glsl's layouts); a plain instance runs k = 0 only.
    for (uint k = pick.kBegin; k < pick.kEnd; ++k)
    {
        if (isTree)
        {
            const TreeCullRecord rec = treeCullMainRecord(pick, k);
            if (rec.meshMaterial == TREE_CULL_ABSENT)
                continue;
            instance = InMeshInstance(0u, 0u, rec.meshMaterial, rec.pipelineAlpha);
        }
        cullInstance(instanceIdx + k, instance, pick.posScale, pick.quat, pick.lodStateBase + k, isTree);
    }
}