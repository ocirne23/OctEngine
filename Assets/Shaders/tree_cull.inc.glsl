// BAKED TREE RECORDS (Renderer tree instance sets, RendererTrees.cpp): the trees are NOT written into the per-frame
// instance stream. The CPU only claims a range of it per frame (u_treeCull.x = its first index, .y = its length =
// 3 records per tree: bark, leaves, billboard) and every cull builds a record of that range from STATIC
// device-local data - the placed trees (TreeCullPiece) and their types (TreeCullType) - deciding per record from
// the camera distance which representation draws: the mesh, the mesh on its crossfade materials, the billboard,
// or nothing (the far-tree volume past its start). The range's stream entries are never written - nothing may read
// them: the TLAS writer (gi_tlas_instances.cs.glsl) builds a tree's RT instance from this data too (treeCullShadow),
// with the material in its custom index (bit 23 set), which the ray-query consumers read instead of the stream.
//
// The includer defines TREE_CULL_PIECES_BINDING / TREE_CULL_TYPES_BINDING. Requires ubo.inc.glsl.
// The layouts mirror RendererVK's TreeCullPieceGpu / TreeCullTypeGpu - keep them in step.

#ifndef TREE_CULL_INC_GLSL
#define TREE_CULL_INC_GLSL

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
    TreeCullRecord billboard;
    float farDistance; // billboard switch distance (m); x u_treeCullParams.x
    float fadeWidth;   // crossfade band (m), centred on it
};
struct TreeCullPiece
{
    vec4 posScale;
    vec4 quat;
    vec3 centre;       // world centre of the far representation (the band test)
    float radius;
    uint type;
    uint lodStateBase; // 3 LOD hysteresis slots: record k -> lodStateBase + k
    uint pad0, pad1;
};

layout (binding = TREE_CULL_PIECES_BINDING, std430) readonly buffer TreeCullPieces { TreeCullPiece in_treePieces[]; };
layout (binding = TREE_CULL_TYPES_BINDING, std430) readonly buffer TreeCullTypes { TreeCullType in_treeTypes[]; };

const uint TREE_CULL_ABSENT = 0xFFFFFFFFu;

bool treeCullIsTree(uint instanceIdx) { return instanceIdx - u_treeCull.x < u_treeCull.y; } // unsigned: below base wraps

TreeCullRecord treeCullNone() { return TreeCullRecord(TREE_CULL_ABSENT, 0u); }

// The MAIN pass's record: the mesh before the crossfade band, the billboard after it, inside it the mesh on its
// fade-OUT materials AND the billboard (the lit FS's dither splits the pixels); past the far-tree volume's start
// (u_treeCullParams.z > 0) nothing - the volume draws it.
bool treeCullMain(uint instanceIdx, out TreeCullRecord rec, out TreeCullPiece piece, out uint stateSlot)
{
    const uint v = instanceIdx - u_treeCull.x;
    const uint k = v % 3u;
    piece = in_treePieces[v / 3u];
    stateSlot = piece.lodStateBase + k;
    const TreeCullType type = in_treeTypes[piece.type];
    const bool hasBillboard = type.billboard.meshMaterial != TREE_CULL_ABSENT;
    bool mesh = true, fade = false, billboard = false;
    if (hasBillboard)
    {
        const float switchDistance = type.farDistance * u_treeCullParams.x;
        const float bandStart = max(switchDistance - type.fadeWidth * 0.5, 0.0);
        const float bandEnd = bandStart + type.fadeWidth;
        // A pixel's distance varies by up to the piece radius from the centre's (the material's band is per pixel).
        const float dist = distance(u_views[VIEW_CENTER].viewPos.xyz, piece.centre);
        if (u_treeCullParams.y > 0.5)
        {
            mesh = false;
            billboard = true;
        }
        else if (switchDistance <= 0.0 || dist + piece.radius < bandStart)
        {
        }
        else if (dist - piece.radius > bandEnd)
        {
            mesh = false;
            billboard = true;
        }
        else
        {
            fade = true;
            billboard = true;
        }
        if (u_treeCullParams.z > 0.0 && dist > u_treeCullParams.z)
        {
            mesh = false;
            billboard = false;
        }
    }
    rec = k == 0u ? (mesh ? (fade ? type.barkFade : type.bark) : treeCullNone())
        : k == 1u ? (mesh ? (fade ? type.leavesFade : type.leaves) : treeCullNone())
        : (billboard ? type.billboard : treeCullNone());
    return rec.meshMaterial != TREE_CULL_ABSENT;
}

// The SHADOW (and rain-shelter) passes' record: the BILLBOARD stands in for the tree at every distance; the mesh
// casts only for a type without one.
bool treeCullShadow(uint instanceIdx, out TreeCullRecord rec, out TreeCullPiece piece)
{
    const uint v = instanceIdx - u_treeCull.x;
    const uint k = v % 3u;
    piece = in_treePieces[v / 3u];
    const TreeCullType type = in_treeTypes[piece.type];
    const bool hasBillboard = type.billboard.meshMaterial != TREE_CULL_ABSENT;
    rec = k == 2u ? type.billboard : (hasBillboard ? treeCullNone() : (k == 0u ? type.bark : type.leaves));
    return rec.meshMaterial != TREE_CULL_ABSENT;
}

#endif
