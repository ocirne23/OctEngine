#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_multiview : require

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"
#include "tree_wind.inc.glsl"

// Depth-only vertex shader for the sun shadow cascades, rendered in a single multiview pass: gl_ViewIndex selects the
// cascade. The shadow cull packed the cascades each caster overlaps into a bitmask; for a cascade a caster does not
// touch, the primitive collapses to a degenerate point, discarded before rasterization.
struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    uint alphaTexIdxCascadeMask; // high 16 = alpha-mask tex idx (0xFFFF = opaque), bit 15 = a tree (wind), bits 8..14 its
                                 // trunk-bend scale x 32 - no bit 15: bit 8 = a terrain node - bit 7 = a tree billboard,
                                 // low 7 = cascade mask
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};
// The one pipeline draws both vertex formats, so the vertices are not attributes: two blocks on the mega-buffer.
layout (binding = 2, std430) readonly buffer InVertices { MeshVertex in_vertices[]; };
layout (binding = 2, std430) readonly buffer InTerrainVertices { TerrainVertex in_terrainVertices[]; };

#ifdef TREE_FAR_SHADOW
// THE FAR SHADOW CASTERS (ShadowMapGraphicsPipeline's far pipeline): a far ring tree's record (TreeVoxelFarPipeline
// FAR_SHADOW, tree_voxel_far.cs.glsl) instance-rate - its BILLBOARD, the packed word built here as the shadow cull would.
layout (location = 5) in vec4 inst_posScale;
layout (location = 6) in uvec4 inst_data; // the yaw (float bits), the alpha-mask texture | the cascade mask << 16, the trunk-bend scale (float bits), bucket | index
#endif

layout (location = 0) out vec2 out_uv;
layout (location = 1) out flat uint out_alphaTexIdx; // 0xFFFF = opaque (fragment skips the mask test)
#ifdef TREE_VOXEL
// THE CASTERS THAT WRITE THEIR DEPTH (ShadowMapGraphicsPipeline's voxel pipeline), two kinds:
// The trees' VOXEL casters ("Trees/Voxel shadows"): the box mesh of the main pass's voxel tier (its normal = the corner's
// grid coordinate, its tangent = the box size); the alpha index is the grid's opacity atlas (its header names the brick
// index: tree_voxel.inc.glsl). The FS marches along the light from the face the sun sees.
// The trees' SHADOW BILLBOARDS (bit 7): a flat card whose mask holds, in its green, how far the tree's surface stands off
// the card (Procedural bakeBillboardMask: 0..1 of the card's depth range, the LENGTH of its vertex normal). The card is
// drawn the full range toward the sun, and the FS pushes each fragment back by what its texel lacks - so the caster's
// depth is the tree's sun side, and it is exact with the sun along the card's normal. The range is weighted by |N.L|: a
// card the sun grazes covers little, and its depth along its own normal says nothing along the light (unweighted, the
// grazed card's lift passed a round crown's surface by 40 % of its radius - and the nearest of the three cards wins).
layout (location = 2) out vec3 out_voxelGrid;         // a billboard: per axis the way along the sun out of the tree's box (m)
layout (location = 3) out flat vec3 out_voxelDirG;    // grid units per metre along the light's travel, no zero component
layout (location = 4) out flat vec2 out_voxelStepLod; // x = the cube root of the box's volume (world metres), y = log2(the cascade's texel / x) + "Voxel shadow LOD bias"
                                                      // x < 0: a BILLBOARD
layout (location = 5) out flat vec2 out_voxelDepth;   // x = depth per metre along the light, y = the coverage that casts
                                                      // a billboard: x = the depth of its whole range along the light, y = depth per metre
#endif

void main()
{
#ifdef TREE_FAR_SHADOW
    InMeshInstancesData inst;
    {
        const float yaw = uintBitsToFloat(inst_data.x);
        const float windTrunk = uintBitsToFloat(inst_data.z);
        inst.posScale = inst_posScale;
        inst.quat = vec4(0.0, sin(0.5 * yaw), 0.0, cos(0.5 * yaw)); // about up
        inst.alphaTexIdxCascadeMask = ((inst_data.y & 0xFFFFu) << 16) | ((inst_data.y >> 16) & 0x7Fu) | 0x00000080u
                                    | (uint(clamp(round(windTrunk * 32.0), 0.0, 127.0)) << 8) | 0x00008000u;
    }
#else
    const InMeshInstancesData inst = in_instances[gl_InstanceIndex]; // the shadow cull writes it at its draw slot
#endif
    const uint cascadeMask = inst.alphaTexIdxCascadeMask & 0x0000007Fu;
    out_uv = vec2(0.0);
    out_alphaTexIdx = inst.alphaTexIdxCascadeMask >> 16;
    if ((cascadeMask & (1u << gl_ViewIndex)) == 0u)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // not in this cascade: degenerate -> discarded
        return;
    }

    const uint vertexIdx = uint(gl_VertexIndex);
    vec3 localPos;
    if ((inst.alphaTexIdxCascadeMask & 0x00008100u) == 0x00000100u)
    {
        const vec2 localXZ = terrainVertexXZ(in_terrainVertices[vertexIdx].data, terrainVertexLod(in_terrainVertices[vertexIdx].normalLod), u_terrain_stitchBands.w);
        localPos = vec3(localXZ.x, in_terrainVertices[vertexIdx].y, localXZ.y);
    }
    else
    {
        const vec4 posU = in_vertices[vertexIdx].positionU;
        localPos = posU.xyz;
        if (out_alphaTexIdx != 0xFFFFu)
            out_uv = vec2(posU.w, in_vertices[vertexIdx].normalV.w);
    }

    vec3 worldPos = quat_transform(localPos * inst.posScale.w, inst.quat) + inst.posScale.xyz;
    // A tree (the shadow cull's bit 15): the wind's TRUNK bend only (its casters are mostly the billboards; no payload
    // attribute in this pass) - the same bend the main pass gives every representation.
    if ((inst.alphaTexIdxCascadeMask & 0x00008000u) != 0u)
    {
        const float windTrunk = float((inst.alphaTexIdxCascadeMask >> 8) & 127u) * (1.0 / 32.0);
        worldPos += treeWindTrunkOffset(localPos, inst.posScale.xyz, inst.posScale.w, windTrunk);
    }
#ifdef TREE_VOXEL
    // A billboard: out to its depth range (m along the light), where its mask's depth is 1.
    const bool card = (inst.alphaTexIdxCascadeMask & 0x00008080u) == 0x00008080u;
    float cardRange = 0.0;
    if (card)
    {
        cardRange = abs(dot(quat_transform(in_vertices[vertexIdx].normalV.xyz, inst.quat), u_sunDirection.xyz)) * inst.posScale.w;
        worldPos += u_sunDirection.xyz * cardRange;
    }
#endif

    // Restore the canonical [0,0,0,1] bottom row (its slots carry packed per-cascade scalars).
    mat4 m = u_cascadeViewProj[gl_ViewIndex];
    m[0][3] = 0.0; m[1][3] = 0.0; m[2][3] = 0.0; m[3][3] = 1.0;
    gl_Position = m * vec4(worldPos, 1.0);

#ifdef TREE_VOXEL
    if (card)
    {
        // THE TREE'S BOX about its trunk axis, from the card itself (Procedural billboardMesh): its tangent = the half
        // extents; its top = a vertical card's upper edge (u 1), and the horizontal card's height + its depth range.
        const vec3 cardN = in_vertices[vertexIdx].normalV.xyz;
        const vec3 radii = in_vertices[vertexIdx].tangent.xyz;
        const float range = length(cardN);
        const float boxTop = abs(cardN.y) > 0.5 * range ? localPos.y + range
                                                         : localPos.y + (1.0 - in_vertices[vertexIdx].positionU.w) * 2.0 * radii.y;
        const vec3 fromCentre = localPos - vec3(0.0, boxTop - radii.y, 0.0);
        // Per axis the way along the sun to the box face it leaves through (m): linear over the card, so the FS takes
        // their minimum - the lift never passes the box, where the tree's own leaves look the cascades up
        // (instanced_indirect_tree.fs.glsl).
        const vec3 sunO = quat_transform(u_sunDirection.xyz, vec4(-inst.quat.xyz, inst.quat.w));
        out_voxelGrid = (radii - fromCentre * sign(sunO)) / max(abs(sunO), vec3(1e-6)) * inst.posScale.w;
        out_voxelDirG = vec3(1.0);
        out_voxelStepLod = vec2(-1.0, 0.0);
        const float depthPerMetre = (m * vec4(-u_sunDirection.xyz, 0.0)).z;
        out_voxelDepth = vec2(depthPerMetre * cardRange, depthPerMetre);
    }
    else
    {
        // The box's sides are > 0 (Procedural billboardBox), and so is a tree's scale: no clamp.
        const vec3 worldSize = in_vertices[vertexIdx].tangent.xyz * inst.posScale.w;
        const vec3 travel = -u_sunDirection.xyz; // from the sun into the scene
        out_voxelGrid = in_vertices[vertexIdx].normalV.xyz;
        const vec3 dirG = quat_transform(travel, vec4(-inst.quat.xyz, inst.quat.w)) / worldSize;
        out_voxelDirG = mix(vec3(1e-6), dirG, greaterThan(abs(dirG), vec3(1e-6))); // the empty bricks' jump divides by it
        // The cascade's texel in metres: its ortho x row maps world to clip -1..1 over the map's resolution.
        const float texel = 2.0 * u_shadow_invResolution / length(vec3(m[0][0], m[1][0], m[2][0]));
        const float cubeRoot = pow(worldSize.x * worldSize.y * worldSize.z, 1.0 / 3.0);
        out_voxelStepLod = vec2(cubeRoot, log2(texel / cubeRoot) + u_foliage_voxelShadowLodBias);
        out_voxelDepth = vec2((m * vec4(travel, 0.0)).z, u_foliage_voxelShadowCoverage);
    }
#endif
}
