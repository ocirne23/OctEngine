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
    uint _padding;
};
struct OutMeshInstance
{
    vec4 posScale;
    vec4 quat;
    uint meshIdxMaterialIdx;
    uint _padding1;
    uint _padding2;
    uint _padding3;
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
// The TESSELLATED terrain (TERRAIN_TESS_ROUTE): its ground and overlay draws, same per-mesh-slot layout,
// consumed by plain vkCmdDrawIndexedIndirectCount (the pipelineIndex word is skipped) - a tess pipeline
// cannot join the DGC execution set, whose pipelines must all share the vertex + fragment stages.
layout (binding = 16, std430) buffer OutTerrainTessCommandBuffer
{
    OutIndirectCommand out_terrainTessCommands[];
};
layout (binding = 17, std430) buffer OutTerrainTessOverlayCommandBuffer
{
    OutIndirectCommand out_terrainTessOverlayCommands[];
};

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

// The terrain overlay's reach: the wetness clipmap window (terrain_wetness.inc.glsl's packing - origin lattice
// coord in u_terrainWetParams0.xy, texel size in u_terrainWetParams1.x, present flag u_terrainWetParams2.x).
bool terrainOverlayCovers(vec3 pos, float radius)
{
    if (u_terrainWetParams2.x < 0.5)
        return false;
    const vec2 lo = u_terrainWetParams0.xy * u_terrainWetParams1.x;
    const vec2 hi = lo + float(TERRAIN_WET_RES) * u_terrainWetParams1.x;
    const vec2 d = pos.xz - clamp(pos.xz, lo, hi);
    return dot(d, d) <= radius * radius;
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

void main()
{
    //debugPrintfEXT("instanceIdx %d\n", gl_GlobalInvocationID.x);
    const uint instanceIdx        = gl_GlobalInvocationID.x;
    const InMeshInstance instance = in_instances[instanceIdx];
    if ((in_nodePassMasks[instance.renderNodeIdx] & PASS_MAIN) == 0u)
        return; // pushed for shadows/GI only
    uint meshIdx                  = instance.meshIdxMaterialIdx & 0x0000FFFF;
    const InMeshInfo meshInfo     = in_meshInfos[meshIdx];

    const vec4 quat                   = quat_multiply(in_renderNodeTransforms[instance.renderNodeIdx].quat, in_instanceOffsets[instance.instanceOffsetIdx].quat);
    const vec4 renderNodePosScale     = in_renderNodeTransforms[instance.renderNodeIdx].posScale;
    const vec4 instanceOffsetPosScale = in_instanceOffsets[instance.instanceOffsetIdx].posScale;
    const vec4 instancePosScale       = vec4(renderNodePosScale.xyz + quat_transform(instanceOffsetPosScale.xyz * renderNodePosScale.w, in_renderNodeTransforms[instance.renderNodeIdx].quat),
                                            renderNodePosScale.w * instanceOffsetPosScale.w);
    const vec3 centerOffset           = quat_transform(meshInfo.center * instancePosScale.w, quat);
    const float radius                = meshInfo.radius * instancePosScale.w;
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
        cullRadius += u_oceanParams10.w;

    if (frustumCheck(centerPos, cullRadius))
    {
        // GPU LOD selection: instances always arrive referencing LOD0 (whose bounds culled above);
        // redirect to the selected level's mesh. Buckets have room because the CPU sizes every chain
        // member's bucket to the chain's full instance count.
        InMeshInfo drawMeshInfo = meshInfo;
        const uint lodGroupIdx = in_meshLodGroupIdx[meshIdx];
        if (lodGroupIdx != 0xFFFFFFFFu && u_lodParams1.z > 0.5)
        {
            const MeshLodGroup group = in_meshLodGroups[lodGroupIdx];
            const float dist = max(0.01, length(centerPos - u_views[VIEW_CENTER].viewPos.xyz) - radius);
            const uint stateSlot = uint(int(instanceIdx) + in_nodeLodStateBias[instance.renderNodeIdx]);
            int level = lodSelectLevel(group, dist, radius, instancePosScale.w,
                u_lodParams0.x, 0.0, int(lodLevelState[stateSlot]));
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
        const bool isTransparent      = alphaMode == ALPHA_MODE_BLEND;

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
                && distance(centerPos, u_views[VIEW_CENTER].viewPos.xyz) - radius < u_terrainTessParams1.y + 1.0;
            idx = atomicAdd(out_indirectCommands[meshIdx].instanceCount, 1);
            if (idx == 0)
            {
                out_indirectCommands[meshIdx].pipelineIndex = pipelineIdx;
                out_indirectCommands[meshIdx].indexCount    = terrainTess ? 0u : drawMeshInfo.indexCount;
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
                if (terrainOverlayCovers(centerPos, radius))
                {
                    atomicMax(out_terrainTessOverlayCommands[meshIdx].instanceCount, idx + 1u);
                    out_terrainTessOverlayCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
                    out_terrainTessOverlayCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                    out_terrainTessOverlayCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                    out_terrainTessOverlayCommands[meshIdx].firstInstance = firstInstance;
                }
            }
            // The TERRAIN OVERLAY (EPipelineIndex::TerrainOverlay: the surface-water film, later more terrain
            // surface layers): the same chunk drawn again over the ground, as the mesh's TRANSPARENT sequence
            // (a terrain mesh has no transparent instances, so it is free, and it executes after every opaque
            // draw) over the SAME instance list. Only for chunks overlapping the wetness clipmap. The count is
            // raised to this instance's slot + 1, so every overlapping instance lies inside the drawn range
            // (a non-overlapping instance drawn along with it discards every pixel); the other fields are the
            // same values from every writer.
            if (!terrainTess && pipelineIdx == uint16_t(PIPELINE_IDX_TERRAIN_LIT) && terrainOverlayCovers(centerPos, radius))
            {
                atomicMax(out_transparentIndirectCommands[meshIdx].instanceCount, idx + 1u);
                out_transparentIndirectCommands[meshIdx].pipelineIndex = PIPELINE_IDX_TERRAIN_OVERLAY;
                out_transparentIndirectCommands[meshIdx].indexCount    = drawMeshInfo.indexCount;
                out_transparentIndirectCommands[meshIdx].firstIndex    = drawMeshInfo.firstIndex;
                out_transparentIndirectCommands[meshIdx].vertexOffset  = drawMeshInfo.vertexOffset;
                out_transparentIndirectCommands[meshIdx].firstInstance = firstInstance;
            }
        }

        out_meshInstanceIndexes[firstInstance + idx]      = instanceIdx;
        out_meshInstances[instanceIdx].posScale           = instancePosScale;
        out_meshInstances[instanceIdx].quat               = quat;
        out_meshInstances[instanceIdx].meshIdxMaterialIdx = (instance.meshIdxMaterialIdx & 0xFFFF0000u) | meshIdx;
    }
}