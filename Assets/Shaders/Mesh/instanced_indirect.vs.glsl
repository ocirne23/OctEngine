#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

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
#ifdef TREE_VOXEL
#include "tree_voxel_box.inc.glsl" // the screen rectangle around the tree's box
#endif
#ifdef TREE_ORIGIN
// LitTree: the tree set's type table (binding 3), for the leaves' voxel grid (tree_leaf_sun.inc.glsl).
#define TREE_TYPES_BINDING 3
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
// THE VOXEL TIER (TREE_VOXEL) has its own, smaller set (below): it draws a screen rectangle, not a surface.
#ifndef TREE_VOXEL
layout (location = 0) out vec4 out_posU;    // xyz = world position, w = uv.x
layout (location = 1) out vec4 out_normalV; // xyz = normal, w = uv.y
layout (location = 2) out vec4 out_tangent; // xyz = tangent, w = bitangent sign
// The MOTION VECTORS: this point's world position last frame minus now (motion_vector.inc.glsl). Exactly 0 for a still,
// unskinned instance - its fragments then write no motion.
layout (location = 4) out vec3 out_prevWorldDelta;
#endif
layout (location = 3) out flat uint out_meshIdxMaterialIdx;
#ifdef TREE_ORIGIN
// LitTree: THE WIND'S SWAY of this point WITHOUT the trunk's bend (its branch + its leaf flutter). The sun shadow is looked
// up at the position less this: the shadow casters bend by the trunk alone (shadow_depth.vs.glsl), so a swaying leaf would
// move through its own tree's shadow. ONLY LitTree carries it: on the generic lit variant it was 3 interpolated components
// of EVERY lit mesh for the trees' near bark (LitOpaque), whose sway is small - that bark takes its shadow where it is.
layout (location = 10) out vec3 out_windSway;
// LitTree: THE DRAW SLOT (gl_InstanceIndex), one flat value. Everything that is the same for a whole tree the fragment
// reads from its instance record there (its origin: the distance fade and the height up the tree; its rotation and scale:
// the sun in its voxel grid) and from its type (the low 16 bits of out_meshIdxMaterialIdx on a tree record) - a load per
// pixel that every pixel of the tree shares, and no plane equation per triangle for it.
layout (location = 5) out flat uint out_treeInstance;
// THE LEAVES' SUN SELF-SHADOW (tree_leaf_sun.inc.glsl): the vertex in its type's voxel grid (0..1 per axis, unbent by the
// wind; 0 for a type without a grid).
layout (location = 6) out vec3 out_treeGrid;
#endif
#ifdef TREE_VOXEL
// The LitTreeVoxel variant (the trees' voxel tier): NO BOX IS DRAWN. The main cull hands this shader the box mesh's
// first quad - its four bottom corners (Procedural treeVoxelBoxMesh: the normal = the corner's grid coordinate, the
// tangent = the box size) - and it places them on the SCREEN RECTANGLE around the whole box, at the box's nearest depth: 2
// triangles a tree. The fragment shader intersects each pixel's ray with the box itself and marches from there
// (TREE_VOXEL_RECORD in instanced_indirect_tree_voxel.fs.glsl); what is one value per tree it reads from the tree's
// record at the draw slot and from its type. A box of 5 rings was 18 front triangles x 27 components, and the TRAM
// allocation was the tier's launch limit: this is 2 triangles x 5 components.
layout (location = 0) out vec3 out_voxelRay;            // a world point on this pixel's view ray (linear over the rectangle)
layout (location = 7) out flat uint out_treeInstance;   // the draw slot (gl_InstanceIndex)
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
    // Every LitTree instance is a tree record: the main cull wrote its TYPE where a plain instance has its mesh index.
    out_treeInstance = uint(gl_InstanceIndex);
    out_treeGrid = treeLeafGridCoord(inst.meshIdxMaterialIdx & 0xFFFFu, in_posU.xyz);
#endif
#ifdef TREE_VOXEL
    // THE SCREEN RECTANGLE AROUND THE TREE'S BOX (tree_voxel_box.inc.glsl). The box bends in the wind by the trunk alone
    // (every instance of this pipeline is a tree record: its trunk vector is in the record, tree_wind.inc.glsl
    // treeWindPack): the box as it stands and the box moved by its top's bend, 16 corners. The corner's grid x / z = its
    // corner of the rectangle.
    const vec3 boxSize = in_tangent.xyz;
    const vec3 boxMin = in_posU.xyz - in_normalV.xyz * boxSize;
    const vec3 bendTop = treeWindTrunkBend(inst.prevPosScale.xy, boxMin.y + boxSize.y, inst_scale);
    gl_Position = treeVoxelBoxRect(boxMin, boxSize, inst_pos, inst_scale, inst_quat, bendTop, 16u, in_normalV.xz, out_voxelRay);
    out_treeInstance = uint(gl_InstanceIndex);
#else
    const vec3 normal = quat_transform(in_normalV.xyz, inst_quat);

    // The procedural trees sway in the wind (tree_wind.inc.glsl; its payload rides the tangent's w magnitude). Worked out
    // BEFORE the vertex's own transform: nothing of that is then held across it (the generic lit variant: 63 -> 48
    // registers).
    vec3 windOffset = vec3(0.0);
    vec3 windPrevDelta = vec3(0.0);
    vec3 windSway = vec3(0.0);
    uint windPayload;
    if (treeWindPayload(in_tangent.w, windPayload))
    {
        // A tree RECORD (the main cull) carries its tree's whole wind in its three "last frame" fields (tree_wind.inc.glsl
        // treeWindPack: the gusts, the trunk's sway and the fades, once per tree). Any other node with a wind payload works
        // it out here, per vertex.
        TreeWind tree;
        if (treeWindIsRecord(inst.prevPosScale))
            tree = treeWindUnpack(inst.prevPosScale, inst.prevQuat, inst.prevVertexDelta);
        else
            tree = treeWindOfTree(inst_pos, inst_scale, vec2(1.0));
        windOffset = treeWindVertex(tree, in_posU.xyz, inst_pos, inst_scale, normal, windPayload, windPrevDelta, windSway);
    }
    const vec3 pos = quat_transform(in_posU.xyz * inst_scale, inst_quat) + inst_pos + windOffset;
    out_normalV = vec4(normal, in_normalV.w);
    out_tangent = vec4(quat_transform(in_tangent.xyz, inst_quat), in_tangent.w);
    out_posU   = vec4(pos, in_posU.w);
#ifdef TREE_ORIGIN
    out_windSway = windSway;
#endif
#ifdef TREE_STATIC
    out_prevWorldDelta = windPrevDelta;
#else
    out_prevWorldDelta = prevWorldDelta(inst) + windPrevDelta;
#endif

    // Per-eye projection in VR / centre view on desktop, with the same TAA sub-pixel jitter for both eyes.
    gl_Position = u_mvp * vec4(pos, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
#endif
#ifdef FORCE_NEAR_DEPTH
    // GizmoUI: stamp (nearly) the nearest depth - NDC z = 1 under REVERSED-Z, eGreater compare. Only z changes, so the
    // gizmo always wins depth; the residual 1% of the original z keeps its own parts depth-sorted against each other.
    gl_Position.z = mix(gl_Position.w, gl_Position.z, 0.01);
#endif
}
