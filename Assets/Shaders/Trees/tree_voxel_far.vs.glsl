#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE FAR VOXEL TREES' vertex shader (StaticMeshGraphicsPipeline's far march pipeline, in the voxel march pass;
// tree_voxel_far.cs.glsl builds the records): instanced_indirect.vs.glsl's TREE_VOXEL path with the instance as an
// instance-rate record (TreeVoxelFarPipeline InstanceGpu) instead of the main cull's. NO BOX IS DRAWN: the box mesh's
// first quad (its four bottom corners) is placed on the screen rectangle around the box, and
// Trees/tree_voxel_march.fs.glsl intersects the pixel's ray with it.

#include "shared.inc.glsl"
#include "tree_voxel_box.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (location = 0) in vec4 in_posU;    // MeshVertex: the box corner, w = uv.x
layout (location = 1) in vec4 in_normalV; // the corner's GRID coordinate (Procedural treeVoxelBoxMesh)
layout (location = 2) in vec4 in_tangent; // xyz = the box size
layout (location = 4) in vec4 inst_posScale;
layout (location = 5) in uvec4 inst_data;   // the yaw (a float's bits), the set type | materialIdx << 16, 0, bucket | index

// THE INTERFACE: one flat word, the tree - its record's index in the sorted list (the fragment shaders read the record
// there: tree_voxel_ray.inc.glsl) + the far flag. NO WIND: these trees stand past the near set (hundreds of metres),
// where a trunk's sway is a few hundredths of a pixel a frame - the box stands still, and there is no motion vector.
layout (location = 0) out flat uint out_voxelTree;

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    // gl_InstanceIndex = the bucket's first record + the instance (the draw's firstInstance: tree_voxel_far.cs.glsl).
    out_voxelTree = uint(gl_InstanceIndex) | VOXEL_HIT_FAR;

    // The corner's grid x / z = its corner of the rectangle. 8 corners: the box as it stands.
    const float halfYaw = 0.5 * uintBitsToFloat(inst_data.x);
    const vec3 boxSize = in_tangent.xyz;
    const vec3 boxMin = in_posU.xyz - in_normalV.xyz * boxSize;
    gl_Position = treeVoxelBoxRect(boxMin, boxSize, inst_posScale.xyz, inst_posScale.w, vec4(0.0, sin(halfYaw), 0.0, cos(halfYaw)), vec3(0.0), 8u,
        in_normalV.xz);
}
