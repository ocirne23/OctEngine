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

// LOADS PER FIELD, the transform LAST: a piece is two 32-byte sectors - posScale + quat, then centre / radius / type /
// lodStateBase - and the decision needs only the second. Most records draw nothing (the shadow cull's mesh records
// of every billboard type; in the main cull every record past the far-tree volume's start), and those never fetch
// the transform's sector. (Copying the whole piece first fetched both for every record.)

// The MAIN pass's record: the mesh before the crossfade band, the billboard after it, inside it the mesh on its
// fade-OUT materials AND the billboard (the lit FS's dither splits the pixels); past the far-tree volume's start
// (u_treeCullParams.z > 0) nothing - the volume draws it. posScale / quat / stateSlot only when it draws.
bool treeCullMain(uint instanceIdx, out TreeCullRecord rec, out vec4 posScale, out vec4 quat, out uint stateSlot)
{
    const uint v = instanceIdx - u_treeCull.x;
    const uint k = v % 3u;
    const uint pieceIdx = v / 3u;
    const uint typeIdx = in_treePieces[pieceIdx].type;
    const bool hasBillboard = in_treeTypes[typeIdx].billboard.meshMaterial != TREE_CULL_ABSENT;
    bool mesh = true, fade = false, billboard = false;
    if (hasBillboard)
    {
        const float switchDistance = in_treeTypes[typeIdx].farDistance * u_treeCullParams.x;
        const float fadeWidth = in_treeTypes[typeIdx].fadeWidth;
        const float bandStart = max(switchDistance - fadeWidth * 0.5, 0.0);
        const float bandEnd = bandStart + fadeWidth;
        // A pixel's distance varies by up to the piece radius from the centre's (the material's band is per pixel).
        const float dist = distance(u_views[VIEW_CENTER].viewPos.xyz, in_treePieces[pieceIdx].centre);
        const float radius = in_treePieces[pieceIdx].radius;
        if (u_treeCullParams.y > 0.5)
        {
            mesh = false;
            billboard = true;
        }
        else if (switchDistance <= 0.0 || dist + radius < bandStart)
        {
        }
        else if (dist - radius > bandEnd)
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
    rec = k == 0u ? (mesh ? (fade ? in_treeTypes[typeIdx].barkFade : in_treeTypes[typeIdx].bark) : treeCullNone())
        : k == 1u ? (mesh ? (fade ? in_treeTypes[typeIdx].leavesFade : in_treeTypes[typeIdx].leaves) : treeCullNone())
        : (billboard ? in_treeTypes[typeIdx].billboard : treeCullNone());
    posScale = vec4(0.0);
    quat = vec4(0.0, 0.0, 0.0, 1.0);
    stateSlot = 0u;
    if (rec.meshMaterial == TREE_CULL_ABSENT)
        return false;
    posScale = in_treePieces[pieceIdx].posScale;
    quat = in_treePieces[pieceIdx].quat;
    stateSlot = in_treePieces[pieceIdx].lodStateBase + k;
    return true;
}

// The SHADOW (and rain-shelter) passes' record: the BILLBOARD stands in for the tree at every distance; the mesh
// casts only for a type without one. posScale / quat only when it draws.
bool treeCullShadow(uint instanceIdx, out TreeCullRecord rec, out vec4 posScale, out vec4 quat)
{
    const uint v = instanceIdx - u_treeCull.x;
    const uint k = v % 3u;
    const uint pieceIdx = v / 3u;
    const uint typeIdx = in_treePieces[pieceIdx].type;
    const TreeCullRecord billboard = in_treeTypes[typeIdx].billboard;
    const bool hasBillboard = billboard.meshMaterial != TREE_CULL_ABSENT;
    rec = k == 2u ? billboard : (hasBillboard ? treeCullNone() : (k == 0u ? in_treeTypes[typeIdx].bark : in_treeTypes[typeIdx].leaves));
    posScale = vec4(0.0);
    quat = vec4(0.0, 0.0, 0.0, 1.0);
    if (rec.meshMaterial == TREE_CULL_ABSENT)
        return false;
    posScale = in_treePieces[pieceIdx].posScale;
    quat = in_treePieces[pieceIdx].quat;
    return true;
}

#endif
