// BAKED TREE RECORDS (RendererTrees.cpp): the trees are NOT written into the per-frame instance stream. The CPU only
// claims a range of it (u_present_treeRangeBase, u_present_treeRangeLength = TREE_CULL_RECORDS per tree), and every cull
// builds that range's records from STATIC device-local data - the placed trees (TreeCullPiece) and their types
// (TreeCullType) - deciding per tree from the camera distance what draws: the meshes, the meshes on their crossfade
// materials, the billboard, or nothing (past the far-tree volume's start). Nothing may read the range's stream
// entries: the TLAS writer (gi_tlas_instances.cs.glsl) builds a tree's RT instance from this data too, ONE slot per
// tree (treeCullRtPiece), with the material in its custom index (bit 23 set).
//
// THE MID TIER (main pass only): a type with midDistance > 0 has its bark split into `trunk` and `bark` (the module
// branches), plus a CARD mesh (the module placements' cards merged over the species' card atlas). Over the mid band
// the branch bark and the leaves fade OUT while the card mesh fades IN; over the far band the trunk and the card mesh
// fade OUT while the billboard fades IN. The bands are the materials' (TreeSystem::applyFadeBands). At most 4
// representations draw at once, so a tree has 4 FIXED record slots:
//   mid tier:  0 = trunk, 1 = branch bark, 2 = leaves (once they are gone: the billboard), 3 = card mesh
//   otherwise: 0 = bark, 1 = leaves, 3 = billboard
//
// A ROCK (Procedural's world rocks) is a type with ONE mesh in the bark slot, on LitRock: no billboard, no fade, no mid
// tier, no wind, but a mesh LOD pick. In the main pass it hands over to the far-tree volume at its start, as a
// billboard does; it keeps casting its sun shadow.
//
// The includer defines TREE_CULL_PIECES_BINDING / TREE_CULL_TYPES_BINDING / TREE_CULL_LIST_BINDING. Requires
// ubo.inc.glsl and the PASS_* constants. The layouts mirror TreeCullPieceGpu / TreeCullTypeGpu - keep them in step.

#ifndef TREE_CULL_INC_GLSL
#define TREE_CULL_INC_GLSL

const uint TREE_CULL_RECORDS = 4u; // RendererTrees.cpp TREE_RECORDS_PER_PIECE

struct TreeCullRecord
{
    uint meshMaterial;  // meshIdx | materialIdx << 16; TREE_CULL_ABSENT = none
    uint pipelineAlpha; // pipelineIdx | alphaMode << 16
};
struct TreeCullType
{
    TreeCullRecord bark;           // the mid tier: the branch bark only
    TreeCullRecord barkFade;
    TreeCullRecord leaves;
    TreeCullRecord leavesFade;
    TreeCullRecord billboard;
    TreeCullRecord trunk;          // the mid tier only
    TreeCullRecord trunkFade;
    TreeCullRecord cardsIn;        // the mid tier only: the card mesh, fading in (the mid band) / out (the far band)
    TreeCullRecord cardsOut;
    float farDistance;             // billboard switch distance (m); x u_present_treeFarScale
    float fadeWidth;               // crossfade band (m), centred on it
    float midDistance;             // the mid tier's switch distance (m; x u_present_treeFarScale); 0 = no mid tier
    float midFadeWidth;
    float shadowDistance;          // m from the cascades' centre beyond which it casts no sun shadow; 0 = no limit
    uint pad0;
};
struct TreeCullPiece
{
    vec4 posScale;
    vec4 quat;
    vec3 centre;                   // world centre of the far representation (the band test)
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

// A thread / slot -> its stream instance, when the first `listed` entries of the tree list take one thread each: the
// stream below the range, then the listed pieces, then the stream above the range. The LIST (in_treeList, this frame's,
// written by renderTreeInstanceSet) holds piece | passMask << 28, the PASS_* bits of the piece's terrain chunk. Returns
// the stream instance index (for a tree, its FIRST record) and the pass bits (all of them for a stream thread).
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
    return isTree ? u_present_treeRangeBase + listIdx * TREE_CULL_RECORDS : (gid < u_present_treeRangeBase ? gid : gid + (u_present_treeRangeLength - listed));
}

// THE CULLS RUN ONE THREAD PER LISTED TREE (instanced_indirect[_shadow].cs.glsl): it decides once and emits 0-4 of its
// records (most emit none). u_present_treeCount pieces are listed; u_present_treeThreads = the thread count.
uint treeCullThreadInstance(uint gid, out bool isTree, out uint pieceIdx, out uint passBits)
{
    return treeCullListedInstance(gid, u_present_treeCount, isTree, pieceIdx, passBits);
}

// The TLAS writer's SLOT layout (gi_tlas_instances.cs.glsl): only the list's RT SECTION - its first `rtPieces` entries
// (the RT-capable trees in RT range) - takes tree slots; every other listed piece takes none. A tree slot's stream
// instance is unused: its instance is built from the static data.
uint treeCullTlasInstance(uint slot, uint rtPieces, out bool isTree, out uint pieceIdx, out uint passBits)
{
    return treeCullListedInstance(slot, rtPieces, isTree, pieceIdx, passBits);
}

// A piece's decision: records [kBegin, kEnd) may draw (what slot k holds: the header; an ABSENT one is skipped).
// The main pass's modes: 0 = none, 1 = the plain representation, 2 = on its fade material (out; the cards: in = 1).
struct TreeCullPiecePick
{
    uint typeIdx;
    uint kBegin;
    uint kEnd;
    bool mid;           // the mid tier's slot layout
    bool billboard;
    uint trunk;
    uint bark;
    uint leaves;
    uint cards;         // 1 = cardsIn (the mid band and after), 2 = cardsOut (the far band)
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

// The MAIN pass: the meshes before the far band, the billboard after it, inside it the meshes on their fade-OUT
// materials AND the billboard (the fragment dither splits the pixels); past the far-tree volume's start
// (u_present_treeVolumeStart > 0) nothing. With a mid tier, also the mid band (the header). False = nothing draws.
bool treeCullMainPiece(uint pieceIdx, out TreeCullPiecePick pick)
{
    const uint typeIdx = in_treePieces[pieceIdx].type;
    pick.typeIdx = typeIdx;
    const bool hasBillboard = in_treeTypes[typeIdx].billboard.meshMaterial != TREE_CULL_ABSENT;
    const float dist = distance(u_views_viewPos[VIEW_CENTER].xyz, in_treePieces[pieceIdx].centre);
    bool mesh = true, fade = false, billboard = false, mid = false, inMid = false, meshGone = false;
    if (hasBillboard)
    {
        const float switchDistance = in_treeTypes[typeIdx].farDistance * u_present_treeFarScale;
        const float fadeWidth = in_treeTypes[typeIdx].fadeWidth;
        const float bandStart = max(switchDistance - fadeWidth * 0.5, 0.0);
        const float bandEnd = bandStart + fadeWidth;
        // A pixel's distance differs by up to the piece radius from the centre's (the material's band is per pixel).
        const float radius = in_treePieces[pieceIdx].radius;
        const float midSwitch = in_treeTypes[typeIdx].midDistance * u_present_treeFarScale;
        mid = midSwitch > 0.0;
        if (mid)
        {
            const float midWidth = in_treeTypes[typeIdx].midFadeWidth;
            const float midStart = max(midSwitch - midWidth * 0.5, 0.0);
            inMid = dist + radius >= midStart;               // the card mesh draws
            meshGone = dist - radius > midStart + midWidth;  // the branch bark and the leaves no longer
        }
        if (u_present_treeForceFar > 0.5)
        {
            mesh = false;
            billboard = true;
        }
        else if (switchDistance <= 0.0 || dist + radius < bandStart)
        {
            // before the far band: the meshes only
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
        if (u_present_treeVolumeStart > 0.0 && dist > u_present_treeVolumeStart)
        {
            mesh = false;
            billboard = false;
        }
    }
    else if (u_present_treeVolumeStart > 0.0 && dist > u_present_treeVolumeStart)
        mesh = false; // a ROCK (no billboard): the far volume draws it past its start
    const uint farMode = mesh ? (fade ? 2u : 1u) : 0u; // what fades over the FAR band
    const uint midMode = mesh ? (!inMid ? 1u : meshGone ? 0u : 2u) : 0u; // what fades over the MID band
    pick.mid = mid;
    pick.billboard = billboard;
    pick.trunk = mid ? farMode : 0u;
    pick.bark = mid ? midMode : farMode;
    pick.leaves = pick.bark;
    pick.cards = mid && mesh && inMid ? (fade ? 2u : 1u) : 0u;
    pick.kBegin = 0u;
    pick.kEnd = TREE_CULL_RECORDS;
    pick.posScale = vec4(0.0);
    pick.quat = vec4(0.0, 0.0, 0.0, 1.0);
    pick.lodStateBase = 0u;
    if (!mesh && !billboard)
        return false;
    treeCullLoadTransform(pieceIdx, pick);
    return true;
}

TreeCullRecord treeCullMode(uint mode, TreeCullRecord plain, TreeCullRecord faded)
{
    return mode == 0u ? treeCullNone() : mode == 2u ? faded : plain;
}

// The main pass's record in slot k (the header's layouts). In the mid tier the billboard shares slot 2 with the leaves
// and waits for them (a mid band reaching into the far band would want both at once).
TreeCullRecord treeCullMainRecord(TreeCullPiecePick pick, uint k)
{
    const uint t = pick.typeIdx;
    if (pick.mid)
        return k == 0u ? treeCullMode(pick.trunk, in_treeTypes[t].trunk, in_treeTypes[t].trunkFade)
             : k == 1u ? treeCullMode(pick.bark, in_treeTypes[t].bark, in_treeTypes[t].barkFade)
             : k == 2u ? (pick.leaves != 0u ? treeCullMode(pick.leaves, in_treeTypes[t].leaves, in_treeTypes[t].leavesFade)
                        : pick.billboard ? in_treeTypes[t].billboard : treeCullNone())
             : treeCullMode(pick.cards, in_treeTypes[t].cardsIn, in_treeTypes[t].cardsOut);
    return k == 0u ? treeCullMode(pick.bark, in_treeTypes[t].bark, in_treeTypes[t].barkFade)
         : k == 1u ? treeCullMode(pick.leaves, in_treeTypes[t].leaves, in_treeTypes[t].leavesFade)
         : k == 3u && pick.billboard ? in_treeTypes[t].billboard : treeCullNone();
}

// The SHADOW (and rain-shelter) passes: the BILLBOARD (slot 3) stands in for the tree at every distance; the meshes
// (slots 0, 1) cast only for a type without one (never a mid-tier type). The caller skips an absent record.
void treeCullShadowPiece(uint pieceIdx, out TreeCullPiecePick pick)
{
    const uint typeIdx = in_treePieces[pieceIdx].type;
    pick.typeIdx = typeIdx;
    const bool hasBillboard = in_treeTypes[typeIdx].billboard.meshMaterial != TREE_CULL_ABSENT;
    pick.kBegin = hasBillboard ? 3u : 0u;
    pick.kEnd = hasBillboard ? 4u : 2u;
    pick.mid = false;
    pick.billboard = hasBillboard;
    pick.trunk = 0u;
    pick.bark = 1u;
    pick.leaves = 1u;
    pick.cards = 0u;
    treeCullLoadTransform(pieceIdx, pick);
}

TreeCullRecord treeCullShadowRecord(TreeCullPiecePick pick, uint k)
{
    return k == 0u ? in_treeTypes[pick.typeIdx].bark : k == 1u ? in_treeTypes[pick.typeIdx].leaves : in_treeTypes[pick.typeIdx].billboard;
}

// The TLAS writer's (gi_tlas_instances.cs.glsl) ONE instance per tree: its billboard, else its leaves only (the bark of
// such a type is not traced), else its bark (a ROCK). Keep Renderer::initTreeSetTypes' RT-capable test in step. Off
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
    rec = in_treeTypes[typeIdx].billboard;
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

#endif
