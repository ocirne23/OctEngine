// BAKED TREE RECORDS (RendererTrees.cpp): the trees are NOT in the per-frame instance stream. The CPU only claims a range
// of it (u_present_treeRangeBase / u_present_treeRangeLength = TREE_CULL_RECORDS per tree), and every cull builds that
// range's records from STATIC device-local data (TreeCullPiece, TreeCullType), deciding per tree from the camera distance
// what draws: the meshes, the meshes on their crossfade materials, the voxels, or nothing (past the far-tree volume's
// start). Nothing may read the range's stream entries: the TLAS writer (gi_tlas_instances.cs.glsl) builds ONE RT instance
// per tree from this data too (treeCullRtPiece), with the material in its custom index (bit 23 set) - and near the
// scene focus a second one, its bark mesh for RTAO's rays (treeCullAoPiece).
//
// A tree has 3 FIXED record slots: 0 = bark, 1 = leaves, 2 = the far representation.
// THE MAIN PASS: the meshes before the type's band, its VOXEL record after it, both inside it (the meshes on their
// fade-OUT materials, the voxels' material fades IN: TreeSystem::applyFadeBands). A type without a voxel record draws
// its meshes at every distance.
// THE SHADOW PASSES (and the rain shelter): slot 2 alone - the type's SHADOW BILLBOARD at every distance, or with
// "Trees/Voxel shadows" its voxel record. A type with neither casts its meshes. The shadow billboard is never in the
// main pass; it is also the tree in the TLAS.
//
// A ROCK is a type with ONE mesh in the bark slot (LitRock): no voxels, fade, billboard or wind, but a mesh LOD pick. In
// the main pass it hands over to the far-tree volume at its start, as a tree does; it keeps casting its sun shadow.
//
// The includer defines TREE_CULL_PIECES_BINDING / TREE_CULL_TYPES_BINDING / TREE_CULL_LIST_BINDING. Requires ubo.inc.glsl
// and the PASS_* constants. The layouts mirror TreeCullPieceGpu / TreeCullTypeGpu (the type's: tree_type.inc.glsl) - keep
// them in step.

#ifndef TREE_CULL_INC_GLSL
#define TREE_CULL_INC_GLSL

#include "tree_type.inc.glsl" // TreeCullRecord, TreeCullType

const uint TREE_CULL_RECORDS = 3u; // RendererTrees.cpp TREE_RECORDS_PER_PIECE

struct TreeCullPiece
{
    vec4 posScale;
    vec4 quat;
    vec3 centre;                   // world centre of the whole piece (the band test, the RT range)
    float radius;
    uint type;
    uint pad0, pad1, pad2;
};

layout (binding = TREE_CULL_PIECES_BINDING, std430) readonly buffer TreeCullPieces { TreeCullPiece in_treePieces[]; };
layout (binding = TREE_CULL_TYPES_BINDING, std430) readonly buffer TreeCullTypes { TreeCullType in_treeTypes[]; };
layout (binding = TREE_CULL_LIST_BINDING, std430) readonly buffer TreeCullList { uint in_treeList[]; };

const uint TREE_CULL_ABSENT = 0xFFFFFFFFu;

bool treeCullIsTree(uint instanceIdx) { return instanceIdx - u_present_treeRangeBase < u_present_treeRangeLength; } // unsigned: below base wraps

TreeCullRecord treeCullNone() { return TreeCullRecord(TREE_CULL_ABSENT, 0u); }

// A type's wind scales (the species' `Wind` block): x = the trunk bend, y = the branch sway.
vec2 treeCullWind(uint typeIdx)
{
    const uint w = in_treeTypes[typeIdx].wind;
    return vec2(float(w & 0xFFFFu), float(w >> 16)) * (1.0 / 4096.0);
}

// A thread / slot -> its stream instance, when the first `listed` entries of the tree list take one thread each: the
// stream below the range, then the listed pieces, then the stream above the range. The LIST (in_treeList, this frame's,
// from RenderTreeSet::render) holds piece | passMask << 28, the PASS_* bits of the piece's terrain chunk. Returns the
// stream instance index (for a tree, its FIRST record) and the pass bits (all of them for a stream thread).
uint treeCullListedInstance(uint gid, uint listed, out bool isTree, out uint pieceIdx, out uint passBits)
{
    const uint listIdx = gid - u_present_treeRangeBase; // unsigned: below base wraps
    isTree = listIdx < listed;
    pieceIdx = 0u;
    passBits = PASS_MAIN | PASS_SHADOW | PASS_GI;
    if (isTree)
    {
        const uint entry = in_treeList[listIdx];
        pieceIdx = entry & 0x0FFFFFFFu;
        passBits = entry >> 28;
    }
    return isTree ? u_present_treeRangeBase + listIdx * TREE_CULL_RECORDS
                  : (gid < u_present_treeRangeBase ? gid : gid + (u_present_treeRangeLength - listed));
}

// THE CULLS RUN ONE THREAD PER LISTED TREE (instanced_indirect[_shadow].cs.glsl): it decides once and emits 0-3 of its
// records (most emit none). u_present_treeCount pieces are listed; u_present_treeThreads = the thread count.
uint treeCullThreadInstance(uint gid, out bool isTree, out uint pieceIdx, out uint passBits)
{
    return treeCullListedInstance(gid, u_present_treeCount, isTree, pieceIdx, passBits);
}

// The TLAS writer's SLOT layout (gi_tlas_instances.cs.glsl): only the list's RT SECTION - its first `rtPieces` entries
// (the RT-capable trees in RT range) - takes tree slots; every other listed piece takes none. A tree slot's stream
// instance is unused: its instance is built from the static data.
// Then `aoPieces` MORE slots, for the section's first `aoPieces` entries again (the trees of the chunks in RTAO's range):
// `bark` = this slot is the tree's BARK mesh for RTAO's rays (treeCullAoPiece), not its RT representation.
uint treeCullTlasInstance(uint slot, uint rtPieces, uint aoPieces, out bool isTree, out uint pieceIdx, out uint passBits, out bool bark)
{
    const uint listIdx = slot - u_present_treeRangeBase; // unsigned: below base wraps
    const uint listed = rtPieces + aoPieces;
    isTree = listIdx < listed;
    bark = isTree && listIdx >= rtPieces;
    pieceIdx = 0u;
    passBits = PASS_MAIN | PASS_SHADOW | PASS_GI;
    if (isTree)
    {
        const uint entry = in_treeList[bark ? listIdx - rtPieces : listIdx];
        pieceIdx = entry & 0x0FFFFFFFu;
        passBits = entry >> 28;
    }
    return isTree ? u_present_treeRangeBase + listIdx * TREE_CULL_RECORDS
                  : (slot < u_present_treeRangeBase ? slot : slot + (u_present_treeRangeLength - listed));
}

// A piece's decision: records [kBegin, kEnd) may draw (what slot k holds: the header; an ABSENT one is skipped).
struct TreeCullPiecePick
{
    uint typeIdx;
    uint kBegin;
    uint kEnd;
    bool fade;          // the main pass: the meshes draw on their fade-OUT materials (inside the band)
    vec4 posScale;
    vec4 quat;
    uint lodStateBase;  // a plain instance's LOD hysteresis slot (the main cull); 0 for a tree - no LOD chains
};

// LOADS PER FIELD, the transform LAST: a piece is two 32-byte sectors - posScale + quat, then centre / radius / type -
// and the decision needs only the second, so a piece that draws nothing never fetches the transform's sector.
void treeCullLoadTransform(uint pieceIdx, inout TreeCullPiecePick pick)
{
    pick.posScale = in_treePieces[pieceIdx].posScale;
    pick.quat = in_treePieces[pieceIdx].quat;
    pick.lodStateBase = 0u;
}

// The MAIN pass: the meshes (slots 0, 1) before the band, the voxels (slot 2) after it, inside it the meshes on their
// fade-OUT materials AND the voxels (the fragment dither splits the pixels); past the far-tree volume's start
// (u_present_treeVolumeStart > 0) nothing. False = nothing draws (and `pick` is not set).
bool treeCullMainPiece(uint pieceIdx, out TreeCullPiecePick pick)
{
    // Past the far-tree volume's start the volume draws the tree (a tree and a rock alike): no type loads.
    const float dist = distance(u_views_viewPos[VIEW_CENTER].xyz, in_treePieces[pieceIdx].centre);
    if (u_present_treeVolumeStart > 0.0 && dist > u_present_treeVolumeStart)
        return false;

    const uint typeIdx = in_treePieces[pieceIdx].type;
    pick.typeIdx = typeIdx;
    bool mesh = true, fade = false, voxel = false;
    if (in_treeTypes[typeIdx].voxel.meshMaterial != TREE_CULL_ABSENT)
    {
        const float switchDistance = in_treeTypes[typeIdx].farDistance * u_present_treeFarScale;
        const float fadeWidth = in_treeTypes[typeIdx].fadeWidth;
        const float bandStart = max(switchDistance - fadeWidth * 0.5, 0.0);
        const float bandEnd = bandStart + fadeWidth;
        // A pixel's distance differs by up to the piece radius from the centre's (the material's band is per pixel).
        const float radius = in_treePieces[pieceIdx].radius;
        if (u_present_treeForceFar > 0.5)
        {
            mesh = false;
            voxel = true;
        }
        else if (switchDistance <= 0.0 || dist + radius < bandStart)
        {
            // before the band: the meshes only
        }
        else if (dist - radius > bandEnd)
        {
            mesh = false;
            voxel = true;
        }
        else
        {
            fade = true;
            voxel = true;
        }
    }

    // Before the volume something always draws: the meshes, or (they gone) the voxels.
    pick.fade = fade;
    pick.kBegin = mesh ? 0u : 2u;
    pick.kEnd = voxel ? 3u : 2u;
    treeCullLoadTransform(pieceIdx, pick);
    return true;
}

// The main pass's record in slot k of [kBegin, kEnd).
TreeCullRecord treeCullMainRecord(TreeCullPiecePick pick, uint k)
{
    const uint t = pick.typeIdx;
    if (k == 0u)
        return pick.fade ? in_treeTypes[t].barkFade : in_treeTypes[t].bark;
    if (k == 1u)
        return pick.fade ? in_treeTypes[t].leavesFade : in_treeTypes[t].leaves;
    return in_treeTypes[t].voxel;
}

// The shadow passes' FAR CASTER (slot 2): the type's shadow billboard, or with "Trees/Voxel shadows" its VOXEL record
// where it has one (the shadow cull routes it to its own list: the caster marches its grid toward the sun and writes
// FragDepth). Absent = the type has neither: its meshes cast.
TreeCullRecord treeCullShadowFar(uint t)
{
    const TreeCullRecord voxel = in_treeTypes[t].voxel;
    return u_foliage_voxelShadows > 0.5 && voxel.meshMaterial != TREE_CULL_ABSENT ? voxel : in_treeTypes[t].shadowBillboard;
}

// The SHADOW (and rain-shelter) passes: the far caster (slot 2) stands in for the tree at every distance; the meshes
// (slots 0, 1) cast only for a type without one. The caller skips an absent record.
void treeCullShadowPiece(uint pieceIdx, out TreeCullPiecePick pick)
{
    const uint typeIdx = in_treePieces[pieceIdx].type;
    pick.typeIdx = typeIdx;
    const bool hasCaster = treeCullShadowFar(typeIdx).meshMaterial != TREE_CULL_ABSENT;
    pick.kBegin = hasCaster ? 2u : 0u;
    pick.kEnd = hasCaster ? 3u : 2u;
    pick.fade = false;
    treeCullLoadTransform(pieceIdx, pick);
}

TreeCullRecord treeCullShadowRecord(TreeCullPiecePick pick, uint k)
{
    if (k == 0u)
        return in_treeTypes[pick.typeIdx].bark;
    if (k == 1u)
        return in_treeTypes[pick.typeIdx].leaves;
    return treeCullShadowFar(pick.typeIdx);
}

// The TLAS writer's (gi_tlas_instances.cs.glsl) ONE instance per tree: its shadow billboard, else its leaves only (the
// bark of such a type is not traced), else its bark (a ROCK). Keep Renderer::initTreeSetTypes' RT-capable test in step. Off
// beyond `rtRange` (m) from the scene focus ("Trees/RT range"; <= 0 = no limit), tested before the transform loads:
// every tree in the TLAS is an overlapping box every ray has to traverse. A mesh without a BLAS (bushes) comes out
// inactive in the writer anyway.
bool treeCullRtPiece(uint pieceIdx, float rtRange, out TreeCullRecord rec, out vec4 posScale, out vec4 quat)
{
    rec = treeCullNone();
    posScale = vec4(0.0);
    quat = vec4(0.0, 0.0, 0.0, 1.0);
    if (rtRange > 0.0 && distance(in_treePieces[pieceIdx].centre, u_sceneFocus.xyz) - in_treePieces[pieceIdx].radius > rtRange)
        return false;

    const uint typeIdx = in_treePieces[pieceIdx].type;
    rec = in_treeTypes[typeIdx].shadowBillboard;
    if (rec.meshMaterial == TREE_CULL_ABSENT)
        rec = in_treeTypes[typeIdx].leaves;
    if (rec.meshMaterial == TREE_CULL_ABSENT)
        rec = in_treeTypes[typeIdx].bark;
    if (rec.meshMaterial == TREE_CULL_ABSENT)
        return false;

    posScale = in_treePieces[pieceIdx].posScale;
    quat = in_treePieces[pieceIdx].quat;
    return true;
}

// The TLAS writer's SECOND instance of a tree: its BARK mesh (the trunk and the branches), for RTAO's rays only - the
// contact occlusion on the ground at the trunk, in the forks, on the bark. Only within `aoRange` (m) of the scene focus
// (RTAO's max distance + its radius), and only where the bark is not the tree's RT representation already (a rock). A
// bark mesh without a BLAS (a bush) comes out inactive in the writer.
bool treeCullAoPiece(uint pieceIdx, float aoRange, out TreeCullRecord rec, out vec4 posScale, out vec4 quat)
{
    rec = treeCullNone();
    posScale = vec4(0.0);
    quat = vec4(0.0, 0.0, 0.0, 1.0);
    if (distance(in_treePieces[pieceIdx].centre, u_sceneFocus.xyz) - in_treePieces[pieceIdx].radius > aoRange)
        return false;

    const uint typeIdx = in_treePieces[pieceIdx].type;
    if (in_treeTypes[typeIdx].shadowBillboard.meshMaterial == TREE_CULL_ABSENT && in_treeTypes[typeIdx].leaves.meshMaterial == TREE_CULL_ABSENT)
        return false;
    rec = in_treeTypes[typeIdx].bark;
    if (rec.meshMaterial == TREE_CULL_ABSENT)
        return false;

    posScale = in_treePieces[pieceIdx].posScale;
    quat = in_treePieces[pieceIdx].quat;
    return true;
}

#endif
