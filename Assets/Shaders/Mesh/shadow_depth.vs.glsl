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

layout (location = 4) in uint inst_idx;

layout (location = 0) out vec2 out_uv;
layout (location = 1) out flat uint out_alphaTexIdx; // 0xFFFF = opaque (fragment skips the mask test)

void main()
{
    const InMeshInstancesData inst = in_instances[inst_idx];
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
        vec3 unusedPrevDelta;
        const float windTrunk = float((inst.alphaTexIdxCascadeMask >> 8) & 127u) * (1.0 / 32.0);
        worldPos += treeWind(localPos, inst.posScale.xyz, inst.posScale.w, vec3(0.0), 0u, vec2(windTrunk, 1.0), unusedPrevDelta);
        // A tree BILLBOARD (bit 7) shifted toward the sun by "Trees/Billboard shadow shift (m)" (< 0: away from it): its
        // flat cards stand at the crown's middle, so its shadow starts there, not at the crown's sun side.
        if ((inst.alphaTexIdxCascadeMask & 0x00000080u) != 0u)
            worldPos += u_sunDirection.xyz * u_foliage_billboardShadowShift;
    }

    // Restore the canonical [0,0,0,1] bottom row (its slots carry packed per-cascade scalars).
    mat4 m = u_cascadeViewProj[gl_ViewIndex];
    m[0][3] = 0.0; m[1][3] = 0.0; m[2][3] = 0.0; m[3][3] = 1.0;
    gl_Position = m * vec4(worldPos, 1.0);
}
