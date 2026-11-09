#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE FAR VOXEL TREES' vertex shader (StaticMeshGraphicsPipeline's far voxel pipeline; tree_voxel_far.cs.glsl builds the
// records): instanced_indirect.vs.glsl's TREE_VOXEL path with the instance as an instance-rate record (TreeVoxelFarPipeline
// InstanceGpu) instead of the main cull's. Hands the same interface to instanced_indirect_tree_voxel.fs.glsl.

#include "shared.inc.glsl"
#include "tree_wind.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (location = 0) in vec4 in_posU;    // MeshVertex: the box corner, w = uv.x
layout (location = 1) in vec4 in_normalV; // the corner's GRID coordinate (Procedural treeVoxelBoxMesh)
layout (location = 2) in vec4 in_tangent; // xyz = the box size, w = the trunk-bend code
layout (location = 4) in vec4 inst_posScale;
layout (location = 5) in uvec4 inst_data;   // the yaw, meshIdx | materialIdx << 16, the trunk-bend scale (floats as bits), bucket | index

layout (location = 0) out vec4 out_posU;
layout (location = 3) out flat uint out_meshIdxMaterialIdx;
layout (location = 4) out vec3 out_prevWorldDelta;
layout (location = 5) out flat vec3 out_voxelCam;
layout (location = 6) out vec3 out_voxelGrid;
layout (location = 7) out flat vec4 out_voxelQuat;
layout (location = 8) out flat vec3 out_voxelWorldSize;
layout (location = 9) out flat vec3 out_instanceOrigin;

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const vec3 instPos = inst_posScale.xyz;
    const float instScale = inst_posScale.w;
    const float yaw = uintBitsToFloat(inst_data.x);
    const vec4 inst_quat = vec4(0.0, sin(0.5 * yaw), 0.0, cos(0.5 * yaw)); // about up
    vec3 pos = quat_transform(in_posU.xyz * instScale, inst_quat) + instPos;
    vec3 windPrevDelta = vec3(0.0);
    uint windPayload;
    if (treeWindPayload(in_tangent.w, windPayload))
        pos += treeWind(in_posU.xyz, instPos, instScale, vec3(0.0, 1.0, 0.0), windPayload, vec2(uintBitsToFloat(inst_data.z), 1.0), windPrevDelta);

    out_posU = vec4(pos, in_posU.w);
    out_meshIdxMaterialIdx = inst_data.y;
    out_prevWorldDelta = windPrevDelta; // the trees never move

    const vec3 grid = in_normalV.xyz;
    const vec3 boxSize = max(in_tangent.xyz, vec3(1e-4));
    const vec3 boxMin = in_posU.xyz - grid * boxSize;
    const vec3 camLocal = quat_transform(u_viewPos - instPos, vec4(-inst_quat.xyz, inst_quat.w)) / max(instScale, 1e-6);
    out_voxelCam = (camLocal - boxMin) / boxSize;
    out_voxelGrid = grid;
    out_voxelQuat = inst_quat;
    out_voxelWorldSize = boxSize * instScale;
    out_instanceOrigin = instPos;

    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w;
}
