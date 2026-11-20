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
layout (location = 5) in uvec4 inst_data;   // the yaw (a float's bits), meshIdx | materialIdx << 16, the tree's trunk vector (two halves), bucket | index

layout (location = 0) out vec4 out_posU;
layout (location = 3) out flat uint out_meshIdxMaterialIdx;
layout (location = 4) out vec3 out_prevWorldDelta;
layout (location = 5) out vec3 out_voxelCam;
layout (location = 6) out vec3 out_voxelGrid;
layout (location = 7) out flat vec4 out_voxelQuat;
layout (location = 8) out flat vec4 out_voxelWorldSize;
layout (location = 9) out flat float out_treeDistance;
layout (location = 10) out flat vec4 out_voxelSunLod;
#include "tree_voxel_box.inc.glsl"

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const vec3 instPos = inst_posScale.xyz;
    const float instScale = inst_posScale.w;
    const float yaw = uintBitsToFloat(inst_data.x);
    const vec4 inst_quat = vec4(0.0, sin(0.5 * yaw), 0.0, cos(0.5 * yaw)); // about up
    // THE WIND: the trunk's bend only, x the tree's trunk vector the cull worked out once (tree_voxel_far.cs.glsl) - no
    // gust, no sine and no fade per vertex. NO MOTION VECTOR for it: these trees stand past the near set (hundreds of
    // metres), where a trunk's sway is a few hundredths of a pixel a frame.
    const vec3 pos = quat_transform(in_posU.xyz * instScale, inst_quat) + instPos
                   + treeWindTrunkBend(unpackHalf2x16(inst_data.z), in_posU.y, instScale);

    out_posU = vec4(pos, 0.0); // the box has no texture coordinate
    out_meshIdxMaterialIdx = inst_data.y;
    out_prevWorldDelta = vec3(0.0); // the trees never move

    out_voxelGrid = in_normalV.xyz;
    out_voxelQuat = inst_quat;
    // The camera as it is: no shear for the trunk's bend (instanced_indirect.vs.glsl has it: only that tier crossfades
    // with the meshes).
    const vec3 toCam = u_viewPos - instPos;
    treeVoxelBoxInterface(in_posU.xyz, in_normalV.xyz, in_tangent.xyz, toCam, instScale, inst_quat, out_voxelCam, out_voxelWorldSize, out_voxelSunLod);
    out_treeDistance = length(toCam);

    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w;
}
