export module RendererVK:RenderTreeSet;

import Core;
import Core.glm;
import Core.Transform;

import :Layout;
import :RenderMesh;
import :RenderTreeSetState; // the state

// RAII owner of a set of PROCEDURAL TREE PIECES (move-only, like RenderMesh), baked on the GPU (tree_cull.inc.glsl; RendererTrees.cpp). A set is a grove of
// placed pieces sharing a table of piece types, uploaded once to device-local memory. Per frame, render() costs one instance claim and one count per distinct
// mesh - nothing per piece on the CPU, and no pass of its own on the GPU: the culls build the records. ONE set draws per frame. MAIN THREAD, except render.
// Destroying it drains the GPU first (rare: a respawn / reload). The set must die before the meshes and the materials its types name: declare it after them.
// The state is in RenderTreeSetState.ixx; the multi-line bodies are below the class.
export class RenderTreeSet final : public RenderTreeSetState
{
public:

    // ---- The value types ----

    struct Rep // one representation; mesh nullptr = none. Spawned-on mesh = a chain's LEVEL 0.
    {
        const RenderMesh* mesh = nullptr;
        uint16 material = 0;
        RendererVKLayout::EPipelineIndex pipeline = RendererVKLayout::EPipelineIndex::LitOpaque;
    };

    struct Type
    {
        // barkFade / leavesFade: the same meshes on LitMasked with fade-OUT materials (the crossfade band). With a mid tier, both fade out over the MID band
        // instead (the card mesh takes over from them there).
        Rep bark, barkFade, leaves, leavesFade, billboard;
        float farDistance = 0.0f; // billboard switch distance (m); 0 = always the mesh
        float fadeWidth = 1.0f;   // crossfade band (m), centred on farDistance
        // The MID tier (main pass only; shadows keep the billboard), from midDistance (0 = none) over a band of midFadeWidth: `bark` is then the BRANCH bark
        // only and gives way with the leaves to the CARD mesh (cardsIn: fades in over the mid band; cardsOut: out over the far band); `trunk` stands through
        // both, fading out only in the far band (trunkFade). Needs a billboard, the card mesh and the trunk.
        Rep trunk, trunkFade, cardsIn, cardsOut;
        float midDistance = 0.0f;
        float midFadeWidth = 1.0f;
        // Beyond this distance (m) from the shadow cascades' centre it casts no sun shadow (small plants: a texel or less in the far cascades). 0 = no limit.
        float shadowDistance = 0.0f;
        // x the wind's trunk bend / branch sway (tree_wind.inc.glsl), 0..4: a species' stiffness.
        float windTrunk = 1.0f;
        float windBranch = 1.0f;
        // The FAR-TREE VOLUME's view of the type (TreeVolumePipeline): its extinction (1/m) over [densityMin, densityMax] in type space, densityRes^3 floats (x
        // fastest); nullptr = the type is not in the volume.
        const float* density = nullptr;
        uint32 densityRes = 0;
        glm::vec3 densityMin{ 0.0f };
        glm::vec3 densityMax{ 0.0f };
        glm::vec3 albedo{ 0.1f, 0.16f, 0.07f }; // the leaf colour of its volume
        // A SOLID (a rock, Procedural's world rocks): `density` is its OCCUPANCY (0..1), and the volume gives it "Trees/Far rock extinction" at any size (a
        // crown's extinction thins with its scale; a rock's does not). Its colour is the climate's bedrock - or, with solidOwnColour (dead wood), `albedo`.
        bool solid = false;
        bool solidOwnColour = false;
    };

    struct Piece
    {
        Transform transform;      // the piece's node transform (static)
        glm::vec3 centre{ 0.0f }; // world centre + radius of the far representation (the band test)
        float radius = 0.0f;
        uint32 type = 0;
    };

    struct Range { uint32 first = 0; uint32 count = 0; }; // a contiguous range of pieces (the caller sorts them - Procedural: per terrain chunk)
    struct ChunkDraw { uint32 chunk; uint32 passMask; };  // one drawn chunk, with the passes it draws in (PASS_*)

    // ---- Lifetime ----

    // A static set: one chunk per range (no ranges = one chunk over every piece).
    static RenderTreeSet create(oc::span<const Type> types, oc::span<const Piece> pieces, oc::span<const Range> chunks);

    // A DYNAMIC set (Procedural's world trees): the types fixed, CHUNKS added and removed at run time into a fixed capacity of piece slots (no growth; no GPU
    // drain). After beginFrame.
    static RenderTreeSet createDynamic(oc::span<const Type> types, uint32 pieceCapacity);

    RenderTreeSet() = default;
    RenderTreeSet(RenderTreeSet&& other) noexcept { moveFrom(other); }
    RenderTreeSet& operator=(RenderTreeSet&& other) noexcept;
    ~RenderTreeSet() { destroy(); }

    bool isValid() const { return m_setId != UINT32_MAX; }
    void destroy();

    // ---- A dynamic set's chunks ----

    uint32 addChunk(oc::span<const Piece> pieces); // the chunk's index (ChunkDraw::chunk), or UINT32_MAX: no room
    // Its piece slots are reused NUM_FRAMES_IN_FLIGHT frames later, its index from the next frame on (this frame's terrain walk may still name it).
    void removeChunk(uint32 chunk);

    // ---- Drawing ----

    void bind() const; // every frame the set draws, before its render (a change re-records)

    // This frame's DRAWN chunks, each with the passes it draws in (PASS_*: the main cull, the shadow culls, the TLAS): the claim covers only their pieces. ANY
    // THREAD, once per frame between beginFrame and present (Procedural: from the terrain's render walk). distanceScale x every type's farDistance.
    void render(oc::span<const ChunkDraw> chunks, float distanceScale, bool forceFar) const;

    // ---- Not copyable ----

    RenderTreeSet(const RenderTreeSet&) = delete;
    RenderTreeSet& operator=(const RenderTreeSet&) = delete;
};

// ---- Inline bodies ----

inline RenderTreeSet& RenderTreeSet::operator=(RenderTreeSet&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(other);
    }
    return *this;
}
