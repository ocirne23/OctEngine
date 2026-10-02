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

// THE CULLS RUN ONE THREAD PER TREE (instanced_indirect[_shadow].cs.glsl): the range's 3 records per piece collapse
// into one thread that decides once and emits 0-3 of them - most emit none (in the main cull every tree past the
// far-tree volume's start; in the shadow cull the mesh records of every billboard type). Threads: the stream below
// the range, then one per piece (u_treeCull.w), then the stream above it; u_treeCull.z = the thread count.
// Returns the stream instance index - for a tree thread, its piece's FIRST record (pieceIdx = the thread's offset).
uint treeCullThreadInstance(uint gid, out bool isTree, out uint pieceIdx)
{
    pieceIdx = gid - u_treeCull.x; // unsigned: below base wraps
    isTree = pieceIdx < u_treeCull.w;
    return isTree ? u_treeCull.x + pieceIdx * 3u : (gid < u_treeCull.x ? gid : gid + 2u * u_treeCull.w);
}

// A piece's decision: records [kBegin, kEnd) may draw (0 = bark, 1 = leaves, 2 = billboard; an ABSENT one is skipped).
struct TreeCullPiecePick
{
    uint typeIdx;
    uint kBegin;
    uint kEnd;
    bool fade;          // main pass: the mesh on its fade-OUT materials
    vec4 posScale;
    vec4 quat;
    uint lodStateBase;  // record k -> lodStateBase + k
};

// LOADS PER FIELD, the transform LAST: a piece is two 32-byte sectors - posScale + quat, then centre / radius / type /
// lodStateBase - and the decision needs only the second. A piece that draws nothing never fetches the transform's
// sector. (Copying the whole piece first fetched both for every record.)
void treeCullLoadTransform(uint pieceIdx, inout TreeCullPiecePick pick)
{
    pick.posScale = in_treePieces[pieceIdx].posScale;
    pick.quat = in_treePieces[pieceIdx].quat;
    pick.lodStateBase = in_treePieces[pieceIdx].lodStateBase;
}

// The MAIN pass: the mesh before the crossfade band, the billboard after it, inside it the mesh on its fade-OUT
// materials AND the billboard (the lit FS's dither splits the pixels); past the far-tree volume's start
// (u_treeCullParams.z > 0) nothing - the volume draws it. False = nothing draws.
bool treeCullMainPiece(uint pieceIdx, out TreeCullPiecePick pick)
{
    const uint typeIdx = in_treePieces[pieceIdx].type;
    pick.typeIdx = typeIdx;
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
    pick.kBegin = mesh ? 0u : 2u;
    pick.kEnd = billboard ? 3u : 2u;
    pick.fade = fade;
    pick.posScale = vec4(0.0);
    pick.quat = vec4(0.0, 0.0, 0.0, 1.0);
    pick.lodStateBase = 0u;
    if (!mesh && !billboard)
        return false;
    treeCullLoadTransform(pieceIdx, pick);
    return true;
}

TreeCullRecord treeCullMainRecord(TreeCullPiecePick pick, uint k)
{
    return k == 0u ? (pick.fade ? in_treeTypes[pick.typeIdx].barkFade : in_treeTypes[pick.typeIdx].bark)
         : k == 1u ? (pick.fade ? in_treeTypes[pick.typeIdx].leavesFade : in_treeTypes[pick.typeIdx].leaves)
         : in_treeTypes[pick.typeIdx].billboard;
}

// The SHADOW (and rain-shelter) passes: the BILLBOARD stands in for the tree at every distance; the mesh casts only
// for a type without one. Always draws something (an absent record is skipped by the caller).
void treeCullShadowPiece(uint pieceIdx, out TreeCullPiecePick pick)
{
    const uint typeIdx = in_treePieces[pieceIdx].type;
    pick.typeIdx = typeIdx;
    const bool hasBillboard = in_treeTypes[typeIdx].billboard.meshMaterial != TREE_CULL_ABSENT;
    pick.kBegin = hasBillboard ? 2u : 0u;
    pick.kEnd = hasBillboard ? 3u : 2u;
    pick.fade = false;
    treeCullLoadTransform(pieceIdx, pick);
}

TreeCullRecord treeCullShadowRecord(TreeCullPiecePick pick, uint k)
{
    return k == 0u ? in_treeTypes[pick.typeIdx].bark : k == 1u ? in_treeTypes[pick.typeIdx].leaves : in_treeTypes[pick.typeIdx].billboard;
}

// PER RECORD, for the TLAS writer (gi_tlas_instances.cs.glsl), whose slots are the stream's instance indices: the
// shadow pick of one record. posScale / quat only when it draws.
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
