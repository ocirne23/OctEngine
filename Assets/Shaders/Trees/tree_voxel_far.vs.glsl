#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// THE FAR VOXEL TREES' vertex shader (StaticMeshGraphicsPipeline's far voxel pipeline; tree_voxel_far.cs.glsl builds the
// records): instanced_indirect.vs.glsl's TREE_VOXEL path with the instance as an instance-rate record (TreeVoxelFarPipeline
// InstanceGpu) instead of the main cull's. NO BOX IS DRAWN: the box mesh's first quad (its four bottom corners) is placed
// on the screen rectangle around the box, and instanced_indirect_tree_voxel.fs.glsl intersects the pixel's ray with it.

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

// THE INTERFACE: 10 components over 2 triangles a tree. The fragment shader takes the box from the tree set's type table
// (the type rides the record's mesh index: tree_voxel_far.cs.glsl) and builds the ray, the sun in grid space and the LOD
// scale from the two flat values. NO WIND: these trees stand past the near set (hundreds of metres), where a trunk's sway
// is a few hundredths of a pixel a frame - the box stands still, and there is no motion vector.
layout (location = 0) out vec3 out_voxelRay;                // a world point on this pixel's view ray (linear over the rectangle)
layout (location = 3) out flat uint out_meshIdxMaterialIdx; // the set TYPE | the material << 16
layout (location = 5) out flat vec2 out_voxelYaw;           // sin, cos of half the yaw (the tree turns about up)
layout (location = 7) out flat vec4 out_voxelTree;          // xyz = the tree's origin (world), w = its scale

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const float yaw = uintBitsToFloat(inst_data.x);
    const vec2 halfYaw = vec2(sin(0.5 * yaw), cos(0.5 * yaw));
    out_meshIdxMaterialIdx = inst_data.y;
    out_voxelYaw = halfYaw;
    out_voxelTree = inst_posScale;

    // The corner's grid x / z = its corner of the rectangle. 8 corners: the box as it stands.
    const vec3 boxSize = in_tangent.xyz;
    const vec3 boxMin = in_posU.xyz - in_normalV.xyz * boxSize;
    gl_Position = treeVoxelBoxRect(boxMin, boxSize, inst_posScale.xyz, inst_posScale.w, vec4(0.0, halfYaw.x, 0.0, halfYaw.y), vec3(0.0), 8u,
        in_normalV.xz, out_voxelRay);
}
