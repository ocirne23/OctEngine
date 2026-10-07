#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable

#include "shared.inc.glsl"

// Sun shadow caster cull. Tests each instance against every cascade's orthographic frustum and emits
// it into a single draw list (built like the camera cull) when it overlaps at least one cascade. The
// set of overlapping cascades is packed into a bitmask stored in the out-instance's meshIdxMaterialIdx
// slot (unused by the depth-only vertex shader); the multiview depth pass uses it to skip the
// cascades a caster does not touch. pipelineIndex is forced to 0 (single depth pipeline).

struct RenderNodeTransform  { vec4 posScale; vec4 quat; };
struct InMeshInstance       { uint renderNodeIdx; uint instanceOffsetIdx; uint meshIdxMaterialIdx; uint pipelineIdxAlphaMode; };
struct InMeshInstanceOffset { vec4 posScale; vec4 quat; };
struct InMeshInfo           { vec3 center; float radius; uint indexCount; uint firstIndex; int vertexOffset; uint _padding; };
struct MaterialInfo         { uint flags; float opacity; uint diffuseNormalTexIdx; uint metalRoughnessTexIdxAlphaMode; };
// alphaTexIdxCascadeMask: high 16 = alpha-mask texture index (0xFFFF when the material has no mask),
// low 16 = cascade overlap bitmask. Matches RendererVKLayout::OutShadowMeshInstance.
struct OutMeshInstance      { vec4 posScale; vec4 quat; uint alphaTexIdxCascadeMask; };
struct OutIndirectCommand   { uint pipelineIndex; uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };

layout (binding = 1, std430) readonly buffer InRenderNodeTransformsBuffer  { RenderNodeTransform  in_renderNodeTransforms[]; };
layout (binding = 2, std430) readonly buffer InMeshInstancesBuffer         { InMeshInstance       in_instances[]; };
layout (binding = 3, std430) readonly buffer InMeshInstanceOffsetsBuffer   { InMeshInstanceOffset in_instanceOffsets[]; };
layout (binding = 4, std430) readonly buffer InMeshInfoBuffer              { InMeshInfo           in_meshInfos[]; };
layout (binding = 5, std430) readonly buffer InFirstInstancesBuffer        { uint                 in_firstInstances[]; };
layout (binding = 6, std430) writeonly buffer OutMeshInstancesBuffer       { OutMeshInstance      out_meshInstances[]; };
layout (binding = 7, std430) writeonly buffer OutMeshInstanceIndexesBuffer { uint                 out_meshInstanceIndexes[]; };
layout (binding = 8, std430) writeonly buffer OutIndirectCommandBuffer     { OutIndirectCommand   out_indirectCommands[]; };
layout (binding = 9, std430) readonly buffer InMaterialInfos               { MaterialInfo         in_materialInfos[]; };
layout (binding = 10, std430) readonly buffer InNodePassMasksBuffer        { uint                 in_nodePassMasks[]; };

#include "mesh_lod.inc.glsl"

layout (binding = 11, std430) readonly buffer InMeshLodGroupIdxBuffer      { uint                 in_meshLodGroupIdx[]; };
layout (binding = 12, std430) readonly buffer InMeshLodGroupsBuffer        { MeshLodGroup         in_meshLodGroups[]; };
// 13 + 14: the baked tree records' static data (tree_cull.inc.glsl); 15: this frame's list of the trees to cull.
#define TREE_CULL_PIECES_BINDING 13
#define TREE_CULL_TYPES_BINDING 14
#define TREE_CULL_LIST_BINDING 15
#include "tree_cull.inc.glsl"

vec3 quat_transform(vec3 v, vec4 q) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
vec4 quat_multiply(vec4 q, vec4 p)
{
    vec4 c, r;
    c.xyz = cross(q.xyz, p.xyz);
    c.w = -dot(q.xyz, p.xyz);
    r = p * q.w + c;
    r.xyz = (q * p.w + r).xyz;
    return r;
}

// Builds a cascade bitmask: bit c set if the world-space sphere overlaps cascade c's clip volume
// (Vulkan zero-to-one depth: -w<=x,y<=w and 0<=z<=w), extracted from each cascade's view-projection.
uint cascadeOverlapMask(vec3 center, float radius)
{
    uint mask = 0u;
    for (uint c = 0; c < NUM_SHADOW_CASCADES; ++c)
    {
        mat4 m = u_cascadeViewProj[c];
        // Gribb-Hartmann planes; rows of the column-major matrix.
        vec4 rx = vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
        vec4 ry = vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
        vec4 rz = vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
        vec4 rw = vec4(0.0, 0.0, 0.0, 1.0); // true bottom row; the matrix slots hold packed per-cascade scalars
        vec4 planes[6] = vec4[6](rw + rx, rw - rx, rw + ry, rw - ry, rz, rw - rz); // last two are ZO near/far
        bool inside = true;
        for (uint p = 0; p < 6; ++p)
        {
            vec4 pl = planes[p];
            float invLen = inversesqrt(max(dot(pl.xyz, pl.xyz), 1e-12));
            if (dot(pl.xyz, center) * invLen + pl.w * invLen < -radius)
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

// One caster (or one tree record) through the cascade tests, the LOD pick and the draw-list emit.
void cullCaster(uint instanceIdx, InMeshInstance instance, vec4 instancePosScale, vec4 quat, bool isTree)
{
    // Resolve the alpha-mask texture once here (0xFFFF = opaque) so the depth pass can discard cutout
    // fragments without touching the material buffer. Tested first: it needs only the instance word,
    // and a gizmo instance then skips the transform + cascade test below.
    const uint materialIdx = (instance.meshIdxMaterialIdx & 0xFFFF0000u) >> 16;
    const MaterialInfo material = in_materialInfos[materialIdx];
    if ((material.flags & MATERIAL_FLAG_NO_RAYTRACING) != 0u)
        return; // gizmo geometry: never casts shadows (matches its TLAS mask-0 exclusion)
    uint meshIdx                  = instance.meshIdxMaterialIdx & 0x0000FFFF;
    const InMeshInfo meshInfo     = in_meshInfos[meshIdx];

    const vec3 centerOffset           = quat_transform(meshInfo.center * instancePosScale.w, quat);
    // A ROCK of the tree set (its records draw on LitRock): a regular mesh with an LOD chain, and no wind.
    const bool isRock                 = isTree && (instance.pipelineIdxAlphaMode & 0x0000FFFFu) == PIPELINE_IDX_LIT_ROCK;
    const bool sways                  = isTree && !isRock;
    const float radius                = meshInfo.radius * instancePosScale.w + (sways ? u_foliage_windReach : 0.0); // + the wind's sway reach
    const vec3 centerPos              = instancePosScale.xyz + centerOffset;

    uint cascadeMask = cascadeOverlapMask(centerPos, radius);
    // FAR TREES OUT OF THE NEAR CASCADES: a cascade's box runs a long way up-sun (it must hold every caster between the
    // light and its receivers), so it takes in thousands of distant grove trees. A tree stays in cascade c only while
    // its distance from the cascades' centre (the scene focus, getSunCascade's) minus its radius lies within that
    // cascade's split + "Foliage shadow cascade margin" (u_present_treeShadowMargin) - the margin keeps the long shadows of the
    // trees just up-sun of the cascade's range. (cascadeSplit: the packed scalar of shadows.inc.glsl.)
    if (isTree)
    {
        const float reach = distance(centerPos, u_sceneFocus.xyz) - radius - u_present_treeShadowMargin;
        for (uint c = 0u; c < NUM_SHADOW_CASCADES; ++c)
            if (reach > u_cascadeViewProj[c][0][3])
                cascadeMask &= ~(1u << c);
    }
    if (cascadeMask == 0u)
        return; // casts no shadow in any cascade

    const uint alphaMode = (material.metalRoughnessTexIdxAlphaMode & 0xFFFF0000u) >> 16;
    const uint alphaTexIdx = (alphaMode == ALPHA_MODE_MASK) ? (material.diffuseNormalTexIdx & 0x0000FFFFu) : 0xFFFFu;
    // Bit 15: a TREE - the depth VS bends it in the wind (tree_wind.inc.glsl; the cascade mask stays below it). Not a
    // tree-set rock.
    const uint packed = (alphaTexIdx << 16) | (cascadeMask & 0x00007FFFu) | (sways ? 0x00008000u : 0u);

    // GPU LOD selection, stateless and two levels coarser than the main view (matches the old CPU
    // pass bias: 4x the error budget / +2 fallback levels). Off-screen casters never pop on screen,
    // so hysteresis state isn't worth carrying here.
    InMeshInfo drawMeshInfo = meshInfo;
    // Trees have no mesh LOD chains (their own tiers instead; Procedural TreeSystem): no lookup for their records.
    // The tree set's rocks have one.
    const uint lodGroupIdx = sways ? 0xFFFFFFFFu : in_meshLodGroupIdx[meshIdx];
    if (lodGroupIdx != 0xFFFFFFFFu && u_lod_enabled > 0.5)
    {
        const MeshLodGroup group = in_meshLodGroups[lodGroupIdx];
        const float dist = max(0.01, length(centerPos - u_views_viewPos[VIEW_CENTER].xyz) - radius);
        int level = lodSelectLevel(group, dist, radius, instancePosScale.w,
            u_lod_maxErrorPx * 4.0, 2.0, -1);
        uint chosenMeshIdx = lodMeshAt(group, level);
        if (in_meshInfos[chosenMeshIdx].indexCount == 0u)
        {
            for (int d = 1; d < int(group.numLods); ++d)
            {
                if (level - d >= 0 && in_meshInfos[lodMeshAt(group, level - d)].indexCount != 0u) { level -= d; break; }
                if (level + d < int(group.numLods) && in_meshInfos[lodMeshAt(group, level + d)].indexCount != 0u) { level += d; break; }
            }
            chosenMeshIdx = lodMeshAt(group, level);
        }
        if (chosenMeshIdx != meshIdx)
        {
            meshIdx = chosenMeshIdx;
            drawMeshInfo = in_meshInfos[meshIdx];
        }
    }

    const uint firstInstance = in_firstInstances[meshIdx];
    const uint cmdIdx = meshIdx; // single opaque region
    const uint idx = atomicAdd(out_indirectCommands[cmdIdx].instanceCount, 1);
    if (idx == 0)
    {
        out_indirectCommands[cmdIdx].pipelineIndex = 0u;
        out_indirectCommands[cmdIdx].indexCount    = drawMeshInfo.indexCount;
        out_indirectCommands[cmdIdx].firstIndex    = drawMeshInfo.firstIndex;
        out_indirectCommands[cmdIdx].vertexOffset  = drawMeshInfo.vertexOffset;
        out_indirectCommands[cmdIdx].firstInstance = firstInstance;
    }

    out_meshInstanceIndexes[firstInstance + idx]           = instanceIdx;
    out_meshInstances[instanceIdx].posScale                = instancePosScale;
    out_meshInstances[instanceIdx].quat                    = quat;
    out_meshInstances[instanceIdx].alphaTexIdxCascadeMask  = packed;
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
    // A BAKED TREE (tree_cull.inc.glsl): its billboard is the caster (the mesh only for a type without one). The loop
    // serves the plain instance too (k = 0 only), so cullCaster has ONE call site - each is a full inlined copy.
    TreeCullPiecePick pick;
    InMeshInstance instance;
    if (isTree)
    {
        if ((passBits & PASS_SHADOW) == 0u)
            return;
        // A type's SHADOW DISTANCE (bushes): beyond it from the cascades' centre the piece casts into no cascade - it
        // would cover a texel or less there. Tested before the transform loads.
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
            return; // not shadow-relevant this frame
        pick.kBegin = 0u;
        pick.kEnd = 1u;
        pick.quat                         = quat_multiply(in_renderNodeTransforms[instance.renderNodeIdx].quat, in_instanceOffsets[instance.instanceOffsetIdx].quat);
        const vec4 renderNodePosScale     = in_renderNodeTransforms[instance.renderNodeIdx].posScale;
        const vec4 instanceOffsetPosScale = in_instanceOffsets[instance.instanceOffsetIdx].posScale;
        pick.posScale                     = vec4(renderNodePosScale.xyz + quat_transform(instanceOffsetPosScale.xyz * renderNodePosScale.w, in_renderNodeTransforms[instance.renderNodeIdx].quat),
                                                renderNodePosScale.w * instanceOffsetPosScale.w);
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
        // A tree's record k sits at its first record + k; a plain instance runs k = 0 only.
        cullCaster(instanceIdx + k, instance, pick.posScale, pick.quat, isTree);
    }
}
