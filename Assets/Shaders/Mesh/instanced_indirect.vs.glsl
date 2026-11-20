#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_EXT_nonuniform_qualifier : enable // the LitTree variant's voxel grid lookups (tree_voxel.inc.glsl)

#include "shared.inc.glsl"
#include "mesh_vertex.inc.glsl"
// The tree variants (LitTree: TREE_ORIGIN, LitTreeVoxel: TREE_VOXEL) draw tree pieces only - instances that never move
// and are never skinned, so their only motion is the wind's (TREE_STATIC). The voxel box carries no leaf-card TIP
// vertex: its wind compiles without the flutter.
#if defined(TREE_ORIGIN) || defined(TREE_VOXEL)
#define TREE_STATIC
#endif
#ifdef TREE_VOXEL
#define TREE_WIND_NO_FLUTTER
#endif
#include "tree_wind.inc.glsl"
#ifdef TREE_ORIGIN
// LitTree: the leaf materials (binding 2) and the bindless textures (25), for the leaves' voxel grid link.
struct MaterialInfo
{
    uint flags;
    float opacity;
    uint diffuseNormalTexIdx;
    uint metalRoughnessTexIdxAlphaMode;
};
layout (binding = 2, std430) readonly buffer InMaterialInfos { MaterialInfo in_materialInfos[]; };
layout (binding = 25) uniform sampler2D u_textures[]; // highest binding in the set: variable descriptor count
#include "tree_voxel.inc.glsl"
#include "tree_leaf_sun.inc.glsl"
#endif

#ifdef STEREO
// VR renders one eye per pass (no multiview here: the forward pass's DGC execution set forbids it), so the view index
// (1 = left eye, 2 = right eye) is a push constant selecting the per-eye matrices via g_viewIndex.
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

// The main cull's OutMeshInstance (instanced_indirect.cs.glsl), written at its draw slot: firstInstance + its index in the
// bucket, so gl_InstanceIndex reads it directly.
struct InMeshInstancesData
{
    vec4 posScale;
    vec4 quat;
    vec4 prevPosScale;    // the instance transform last frame; w = 0: the node did not move; w <= -1: a tree record's wind
    uint meshIdxMaterialIdx;
    uint prevVertexDelta; // skinned: last frame's position sits this many vertices on (0 = not skinned)
    uvec2 prevQuat;       // packSnorm2x16 (x, y), (z, w)
};
layout (binding = 1, std430) readonly buffer InMeshInstances
{
    InMeshInstancesData in_instances[];
};
// The vertex mega-buffer: a skinned vertex's position last frame (the skin pass keeps it; motion vectors).
layout (binding = 14, std430) readonly buffer InVertices { MeshVertex in_vertices[]; };

layout (location = 0) in vec4 in_posU;    // MeshVertex: xyz = position, w = uv.x
layout (location = 1) in vec4 in_normalV; // xyz = normal, w = uv.y
layout (location = 2) in vec4 in_tangent;

// Packed into 4 locations (fewer ISBE / TRAM slots per vertex); the fragment shader rebuilds the bitangent from N, T
// and the sign. The world position stays INTERPOLATED: an interpolant costs no register until it is read, while a
// position rebuilt from gl_FragCoord + the depth stays live across the whole lit core (lit FS spill 16 -> 64 B, slower).
layout (location = 0) out vec4 out_posU;    // xyz = world position, w = uv.x
#ifndef TREE_VOXEL // the voxel box has no surface frame: its normal and tangent attributes carry the grid coordinate and the box size
layout (location = 1) out vec4 out_normalV; // xyz = normal, w = uv.y
layout (location = 2) out vec4 out_tangent; // xyz = tangent, w = bitangent sign
#endif
layout (location = 3) out flat uint out_meshIdxMaterialIdx;
// The MOTION VECTORS: this point's world position last frame minus now (motion_vector.inc.glsl). Exactly 0 for a still,
// unskinned instance - its fragments then write no motion.
layout (location = 4) out vec3 out_prevWorldDelta;
#ifndef TREE_VOXEL
// THE WIND'S SWAY of this point WITHOUT the trunk's bend (its branch + its leaf flutter; 0 with no wind payload). The sun
// shadow is looked up at the position less this: the shadow casters bend by the trunk alone (shadow_depth.vs.glsl), so a
// swaying branch would move through its own tree's shadow. The voxel box bends by the trunk only: it has none.
layout (location = 10) out vec3 out_windSway;
#endif
#ifdef TREE_ORIGIN
// LitTree: the camera's distance to the tree's origin - its distance fade (tree_fade.inc.glsl), one value a tree.
layout (location = 5) out flat float out_treeDistance;
// THE LEAVES' SUN SELF-SHADOW (tree_leaf_sun.inc.glsl treeLeafGridVertex): a leaf material links its variant's voxel grid
// (its metal-roughness slot = the opacity atlas), and the grid holds its own box - read HERE, once per vertex.
//   6: the vertex in the grid's space (0..1 per axis, unbent by the wind).
//   7: per tree, xyz = the sun in grid units per metre, w = the cube root of the grid box's volume (m); w 0 = no grid.
//   8: per tree, the grid's textures and its header (packed).
//   9: the vertex's height above the tree's base (m): the canopy map's occlusion above it (canopy_occlusion.inc.glsl).
layout (location = 6) out vec3 out_treeGrid;
layout (location = 7) out flat vec4 out_treeSun;
layout (location = 8) out flat uvec2 out_treeVoxel;
layout (location = 9) out float out_treeHeight;
#endif
#ifdef TREE_VOXEL
// The LitTreeVoxel variant (the trees' voxel tier, Procedural treeVoxelBoxMesh): the box vertex carries its GRID
// coordinate (0 / 1 per axis) in its normal and the box size in its tangent. The FS marches in grid space [0, 1]^3.
layout (location = 5) out flat vec3 out_voxelCam;       // the camera in grid space (unbent by the wind)
layout (location = 6) out vec3 out_voxelGrid;           // this point of the box in grid space
layout (location = 7) out flat vec4 out_voxelQuat;      // the instance rotation (grid axes -> world)
layout (location = 8) out flat vec4 out_voxelWorldSize; // xyz = the box in world metres, w = the cube root of its volume
layout (location = 9) out flat float out_treeDistance;  // the camera's distance to the tree's origin: its distance fade (tree_fade.inc.glsl)
layout (location = 10) out flat vec4 out_voxelSunLod;   // xyz = the sun in grid units per metre, w = the LOD scale
#include "tree_voxel_box.inc.glsl"
#endif

#ifndef TREE_STATIC
// Last frame minus now, as a DIFFERENCE of the two transforms' terms (the instance translations cancel before they meet
// the rotated offsets), so the result keeps its precision far from the world origin.
vec3 prevWorldDelta(InMeshInstancesData inst)
{
    // Not a tree record (a tree never moves; its record's fields carry its wind, tree_wind.inc.glsl).
    const bool skinned = inst.prevVertexDelta != 0u && !treeWindIsRecord(inst.prevPosScale);
    if (inst.prevPosScale.w <= 0.0 && !skinned)
        return vec3(0.0);
    const vec3 prevLocal = skinned ? in_vertices[uint(gl_VertexIndex) + inst.prevVertexDelta].positionU.xyz : in_posU.xyz;
    if (inst.prevPosScale.w <= 0.0) // the node did not move: only the skin did
        return quat_transform((prevLocal - in_posU.xyz) * inst.posScale.w, inst.quat);
    const vec4 prevQuat = normalize(vec4(unpackSnorm2x16(inst.prevQuat.x), unpackSnorm2x16(inst.prevQuat.y)));
    return (inst.prevPosScale.xyz - inst.posScale.xyz)
         + (quat_transform(prevLocal * inst.prevPosScale.w, prevQuat) - quat_transform(in_posU.xyz * inst.posScale.w, inst.quat));
}
#endif

void main()
{
#ifdef STEREO
    g_viewIndex = int(u_viewIndex);
#endif
    const InMeshInstancesData inst = in_instances[gl_InstanceIndex];
    vec3  inst_pos   = inst.posScale.xyz;
    float inst_scale = inst.posScale.w;
    vec4  inst_quat  = inst.quat;

    out_meshIdxMaterialIdx = inst.meshIdxMaterialIdx;

#ifdef TREE_ORIGIN
    {
        // Only a LEAF material links a grid (the bark's slot is "no texture").
        const uint materialIdx = inst.meshIdxMaterialIdx >> 16;
        const bool leafMaterial = (in_materialInfos[materialIdx].flags & MATERIAL_FLAG_LEAF) != 0u;
        treeLeafGridVertex(leafMaterial ? in_materialInfos[materialIdx].metalRoughnessTexIdxAlphaMode & 0xFFFFu : 0xFFFFu,
            in_posU.xyz, inst_quat, inst_scale, out_treeGrid, out_treeSun, out_treeVoxel);
        out_treeHeight = in_posU.y * inst_scale;
    }
#endif
#ifndef TREE_VOXEL
    const vec3 normal = quat_transform(in_normalV.xyz, inst_quat);
#else
    const vec3 normal = vec3(0.0); // no leaf flutter on the box
#endif

    // The procedural trees sway in the wind (tree_wind.inc.glsl; its payload rides the tangent's w magnitude). Worked out
    // BEFORE the vertex's own transform: nothing of that is then held across it (the generic lit variant: 63 -> 48
    // registers). The voxel box is the exception (measured: its transform first is the lower count).
#ifdef TREE_VOXEL
    const vec3 restPos = quat_transform(in_posU.xyz * inst_scale, inst_quat) + inst_pos;
#endif
    vec3 windOffset = vec3(0.0);
    vec3 windPrevDelta = vec3(0.0);
    // A tree RECORD (the main cull) carries its tree's whole wind in its three "last frame" fields (tree_wind.inc.glsl
    // treeWindPack: the gusts, the trunk's sway and the fades, once per tree). Any other node with a wind payload works
    // it out here, per vertex.
    const bool treeRecord = treeWindIsRecord(inst.prevPosScale);
#ifndef TREE_VOXEL
    vec3 windSway = vec3(0.0);
    uint windPayload;
    if (treeWindPayload(in_tangent.w, windPayload))
#else
    // The voxel box (its tangent's code 2.5): the trunk bend ONLY - payload 0 as a constant, so the branch sway compiles out.
    const uint windPayload = 0u;
#endif
    {
        TreeWind tree;
        if (treeRecord)
            tree = treeWindUnpack(inst.prevPosScale, inst.prevQuat, inst.prevVertexDelta);
        else
            tree = treeWindOfTree(inst_pos, inst_scale, vec2(1.0));
        windOffset = treeWindVertex(tree, in_posU.xyz, inst_pos, inst_scale, normal, windPayload, windPrevDelta);
#ifndef TREE_VOXEL
        windSway = windOffset - treeWindTrunkPart(tree, in_posU.xyz, inst_scale);
#endif
    }
#ifdef TREE_VOXEL
    const vec3 pos = restPos + windOffset;
    out_posU   = vec4(pos, 0.0); // the box has no texture coordinate
#else
    const vec3 pos = quat_transform(in_posU.xyz * inst_scale, inst_quat) + inst_pos + windOffset;
    out_normalV = vec4(normal, in_normalV.w);
    out_tangent = vec4(quat_transform(in_tangent.xyz, inst_quat), in_tangent.w);
    out_posU   = vec4(pos, in_posU.w);
    out_windSway = windSway;
#endif
#ifdef TREE_STATIC
    out_prevWorldDelta = windPrevDelta;
#else
    out_prevWorldDelta = prevWorldDelta(inst) + windPrevDelta;
#endif
#if defined(TREE_ORIGIN) || defined(TREE_VOXEL)
    out_treeDistance = distance(u_viewPos, inst_pos);
#endif
#ifdef TREE_VOXEL
    out_voxelGrid = in_normalV.xyz;
    out_voxelQuat = inst_quat;
    treeVoxelBoxInterface(in_posU.xyz, in_normalV.xyz, in_tangent.xyz, inst_pos, inst_scale, inst_quat, out_voxelCam, out_voxelWorldSize, out_voxelSunLod);
#endif

    // Per-eye projection in VR / centre view on desktop, with the same TAA sub-pixel jitter for both eyes.
    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#ifdef FORCE_NEAR_DEPTH
    // GizmoUI: stamp (nearly) the nearest depth - NDC z = 1 under REVERSED-Z, eGreater compare. Only z changes, so the
    // gizmo always wins depth; the residual 1% of the original z keeps its own parts depth-sorted against each other.
    gl_Position.z = mix(gl_Position.w, gl_Position.z, 0.01);
#endif
}
