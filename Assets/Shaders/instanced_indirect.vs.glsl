#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// Depth-prepass reuse (gbuffer.vs.glsl) needs bit-exact positions across programs - see the note there.
invariant gl_Position;

#include "shared.inc.glsl"

#ifdef STEREO
// VR renders one eye per pass (no multiview here: the forward pass's DGC execution set forbids it), so the
// view index (1 = left eye, 2 = right eye) is a push constant selecting the per-eye matrices via g_viewIndex.
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    uint meshIdxMaterialIdx;
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec4 in_tangent;
layout (location = 3) in vec2 in_uv;
layout (location = 4) in uint inst_idx;

layout (location = 0) out vec3 out_pos;
layout (location = 1) out mat3 out_tbn;
layout (location = 4) out vec2 out_uv;
layout (location = 5) out flat uint out_meshIdxMaterialIdx;

vec3 quat_transform(vec3 v, vec4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[inst_idx];
    vec3  inst_pos   = inst.posScale.xyz;
    float inst_scale = inst.posScale.w;
    vec4  inst_quat  = inst.quat;

    out_meshIdxMaterialIdx = inst.meshIdxMaterialIdx;

    vec3 N = quat_transform(in_normal, inst_quat);
    vec3 T = quat_transform(in_tangent.xyz, inst_quat);
    vec3 B = cross(N, T) * in_tangent.w;

    out_pos = quat_transform(in_pos * inst_scale, inst_quat) + inst_pos;
    out_tbn = mat3(T, B, N);
    out_uv  = in_uv;

    // Per-eye projection in VR (g_viewIndex set above) / centre view on desktop, with the same TAA
    // sub-pixel jitter both eyes (per-eye TAA accumulates it just like desktop).
    gl_Position = u_mvp * vec4(out_pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#ifdef FORCE_NEAR_DEPTH
    // GizmoUI: stamp (nearly) the nearest depth - NDC z = 1 under REVERSED-Z, eGreater compare. Only z is
    // touched, so x/y/w (and thus the on-screen shape) are unchanged; the gizmo just always wins depth.
    // The residual 1% of the original z keeps the gizmo's own parts depth-sorted against each other.
    gl_Position.z = mix(gl_Position.w, gl_Position.z, 0.01);
#endif
}

/*mat3 version
void main()
{
    out_pos = in_instances[inst_idx].rotation * (in_pos * in_instances[inst_idx].scale) + in_instances[inst_idx].translation;
    out_tbn = in_instances[inst_idx].rotation * mat3(in_tangent.xyz, cross(in_normal, in_tangent.xyz) * in_tangent.w, in_normal);
    out_uv  = in_uv;
    out_meshIdxMaterialIdx = in_instances[inst_idx].meshIdxMaterialIdx;

    gl_Position = u_mvp * vec4(out_pos, 1.0);
}
*/