#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_multiview : require

#include "shared.inc.glsl"
#include "tree_wind.inc.glsl"

// Depth-only vertex shader for the sun shadow cascades, rendered in a single multiview pass:
// gl_ViewIndex selects the cascade. The shadow cull packed the set of cascades each caster overlaps
// into a bitmask (in the meshIdxMaterialIdx slot); for cascades a caster does not touch the primitive
// is collapsed to a degenerate point so it is cheaply discarded before rasterization.
struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    uint alphaTexIdxCascadeMask; // high 16 = alpha-mask tex idx (0xFFFF = opaque), bit 15 = a tree (wind), low 15 = cascade mask
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};

layout (location = 0) in vec4 in_posU; // MeshVertex: xyz = position, w = uv.x
layout (location = 3) in float in_v;   // MeshVertex normalV.w = uv.y (bound alone, not the normal)
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec2 out_uv;
layout (location = 1) out flat uint out_alphaTexIdx; // 0xFFFF = opaque (fragment skips the mask test)

void main()
{
    const InMeshInstancesData inst = in_instances[inst_idx];
    const uint cascadeMask = inst.alphaTexIdxCascadeMask & 0x00007FFFu;
    out_uv = vec2(in_posU.w, in_v);
    out_alphaTexIdx = inst.alphaTexIdxCascadeMask >> 16;
    if ((cascadeMask & (1u << gl_ViewIndex)) == 0u)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // not in this cascade: degenerate -> discarded
        return;
    }
    vec3 worldPos = quat_transform(in_posU.xyz * inst.posScale.w, inst.quat) + inst.posScale.xyz;
    // A tree (the shadow cull's bit 15): the wind's TRUNK bend only (its casters are mostly the billboards; no payload
    // attribute in this pass) - the same bend the main pass gives every representation.
    if ((inst.alphaTexIdxCascadeMask & 0x00008000u) != 0u)
    {
        vec3 unusedPrevDelta;
        worldPos += treeWind(in_posU.xyz, inst.posScale.xyz, inst.posScale.w, vec3(0.0), 0u, unusedPrevDelta);
    }
    // Restore the canonical [0,0,0,1] bottom row (its slots carry packed per-cascade scalars).
    mat4 m = u_cascadeViewProj[gl_ViewIndex];
    m[0][3] = 0.0; m[1][3] = 0.0; m[2][3] = 0.0; m[3][3] = 1.0;
    gl_Position = m * vec4(worldPos, 1.0);
}
