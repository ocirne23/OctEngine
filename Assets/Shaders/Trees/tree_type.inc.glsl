// A TREE SET's TYPE TABLE (RendererTrees.cpp TreeCullTypeGpu - keep in step): one entry per baked variant of a species, or
// per rock variant. 128 B = four 32-byte sectors: the representations (two), then the type's voxel grid. Read by the
// culls (tree_cull.inc.glsl: what draws, and the band), by the LitTree shaders (tree_leaf_sun.inc.glsl: the mesh leaves'
// sun self-shadow) and by the voxel tier's fragment shader (instanced_indirect_tree_voxel.fs.glsl: the box). A tree's
// record names its type in the low 16 bits of its meshIdxMaterialIdx (the main cull).
// TREE_TYPES_BINDING (the includer's define) declares the table as in_treeTypes[]; tree_cull.inc.glsl declares its own.
#ifndef TREE_TYPE_INC_GLSL
#define TREE_TYPE_INC_GLSL

struct TreeCullRecord
{
    uint meshMaterial;  // meshIdx | materialIdx << 16; TREE_CULL_ABSENT = none
    uint pipelineAlpha; // pipelineIdx | alphaMode << 16
};

struct TreeCullType
{
    TreeCullRecord bark;
    TreeCullRecord barkFade;
    TreeCullRecord leaves;
    TreeCullRecord leavesFade;
    TreeCullRecord voxel;          // the VOXEL tier: the main pass past the band (absent = the meshes at every distance)
    TreeCullRecord shadowBillboard; // the shadow passes and the TLAS only (absent = the meshes cast)
    float farDistance;             // mesh -> voxels switch distance (m); x u_present_treeFarScale
    float fadeWidth;               // crossfade band (m), centred on it
    float shadowDistance;          // m from the cascades' centre beyond which it casts no sun shadow; 0 = no limit
    uint wind;                     // x the trunk bend | x the branch sway << 16, 4.12 fixed point (treeCullWind)
    // THE TYPE'S VOXEL GRID: its box in the tree's own space (mesh-local, scale 1), its textures and its header. All 0 for
    // a type without one.
    vec3 gridInvSize;              // 1 / the box's size per axis
    float gridCbrtVolume;          // the cube root of the box's volume (m); 0 = NO GRID
    uint gridTextures;             // the opacity atlas | the index texture << 16 (u_textures)
    uint gridHeader;               // log2(res) | levels << 8 | log2(cols) << 16 (tree_voxel.inc.glsl VoxelGrid)
    uint pad0, pad1;
    vec3 gridMin;                  // the box's min corner
    float pad2;
    vec3 gridSize;                 // the box's size
    float pad3;
};

#ifdef TREE_TYPES_BINDING
layout (binding = TREE_TYPES_BINDING, std430) readonly buffer TreeTypes { TreeCullType in_treeTypes[]; };
#endif

#endif
