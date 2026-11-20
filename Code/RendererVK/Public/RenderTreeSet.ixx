export module RendererVK:RenderTreeSet;

import Core;
import Core.glm;
import Core.Transform;

import :Layout;
import :RenderMesh;
import :RenderTreeSetState; // the state

// RAII owner of a set of PROCEDURAL TREE PIECES (move-only, like RenderMesh), baked on the GPU (tree_cull.inc.glsl; RendererTrees.cpp). A set is a fixed table
// of piece types plus CHUNKS of placed pieces, added and removed at run time into a fixed capacity of piece slots (Procedural's world trees and rocks). Per
// frame, render() costs one instance claim and one count per distinct mesh - nothing per piece on the CPU, and no pass of its own on the GPU: the culls build
// the records. ONE set draws per frame. MAIN THREAD, except render. Destroying it drains the GPU first (rare: a respawn / reload). The set must die before the
// meshes and the materials its types name: declare it after them. The state is in RenderTreeSetState.ixx; the multi-line bodies are below the class.
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
        // The MESHES (the main pass up to the voxel tier). barkFade / leavesFade: the same meshes on LitTree with fade-OUT materials (the crossfade band).
        Rep bark, barkFade, leaves, leavesFade;
        // The VOXEL tier (LitTreeVoxel; none = mesh nullptr: the meshes draw at every distance): past farDistance it takes the meshes' place in the MAIN
        // pass, on a material with the band's fade-IN.
        Rep voxel;
        // The FAR VOXEL TREES' mesh for this type's voxel tier, on the voxel material (nullptr = voxel.mesh): the same box with fewer vertices.
        const RenderMesh* voxelFarMesh = nullptr;
        float farDistance = 0.0f; // the mesh -> voxels switch distance (m); 0 = always the mesh
        float fadeWidth = 1.0f;   // crossfade band (m), centred on farDistance
        // The SHADOW BILLBOARD (none = mesh nullptr: the meshes cast): crossed cards on a MATERIAL_FLAG_BILLBOARD material - the tree's sun-shadow caster
        // and rain shelter at every distance, and its ONE representation for the rays (a BLAS: GI, RT shadows, reflections). NEVER in the main pass, so its
        // `pipeline` is not read.
        Rep shadowBillboard;
        // The FAR VOXEL TREES (TreeVoxelFarPipeline: the record chunks past the set) draw this type too. Off: a bush - a few metres tall, far out.
        bool farRing = true;
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
        Transform transform;      // the piece's transform (it never moves)
        glm::vec3 centre{ 0.0f }; // world centre + radius of the whole piece (the band test, the RT range)
        float radius = 0.0f;
        uint32 type = 0;
    };

    struct ChunkDraw { uint32 chunk; uint32 passMask; };  // one drawn chunk, with the passes it draws in (PASS_*)

    // ---- Lifetime ----

    // The types fixed, no pieces yet: CHUNKS are added and removed at run time into pieceCapacity piece slots (no growth; no GPU drain). After beginFrame.
    static RenderTreeSet create(oc::span<const Type> types, uint32 pieceCapacity);

    RenderTreeSet() = default;
    RenderTreeSet(RenderTreeSet&& other) noexcept { moveFrom(other); }
    RenderTreeSet& operator=(RenderTreeSet&& other) noexcept;
    ~RenderTreeSet() { destroy(); }

    bool isValid() const { return m_setId != UINT32_MAX; }
    void destroy();

    // ---- The chunks ----

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
