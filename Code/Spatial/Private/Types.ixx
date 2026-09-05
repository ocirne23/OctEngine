export module Spatial:Types;

import Core;
import Core.glm;
import Core.Frustum;
import :Morton;

export struct SpatialHandle
{
    uint32 idx = UINT32_MAX;
    uint32 gen = 0;

    bool isValid() const { return idx != UINT32_MAX; }
};

// RAII registration in the SpatialIndex, move-only like RenderNode/PhysicsBody. Adopts a
// handle from SpatialIndex::registerEntry and unregisters it on destruction.
export class SpatialEntry final
{
public:

    SpatialEntry() = default;
    explicit SpatialEntry(SpatialHandle handle) : m_handle(handle) {}
    SpatialEntry(const SpatialEntry&) = delete;
    SpatialEntry& operator=(const SpatialEntry&) = delete;
    SpatialEntry(SpatialEntry&& other) noexcept : m_handle(other.m_handle) { other.m_handle = {}; }
    SpatialEntry& operator=(SpatialEntry&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_handle = other.m_handle;
            other.m_handle = {};
        }
        return *this;
    }
    ~SpatialEntry() { reset(); }

    void reset(); // unregisters from Globals::spatialIndex, defined in spatialIndex.cpp

    bool isValid() const { return m_handle.isValid(); }
    SpatialHandle handle() const { return m_handle; }

private:

    SpatialHandle m_handle;
};

// Optional per-cell visibility test for hierarchical queries (CPU software occlusion, GPU Hi-Z
// readback, ...). Called after the frustum test passes; bounds are camera-relative floats.
export struct IOcclusionTester
{
    virtual bool isVisible(const glm::vec3& centerRelCamera, const glm::vec3& halfExtent) = 0;
};

export constexpr uint32 SpatialLayer_Render = 1u << 0;  // entities with a RenderComponent
export constexpr uint32 SpatialLayer_Stress = 1u << 1;  // synthetic stress-test entries
export constexpr uint32 SpatialLayer_Terrain = 1u << 2; // procedural terrain chunks (render culling only:
                                                        // NOT entities — their userData is not an Entity*,
                                                        // so gameplay queries must never include this layer)
static_assert((SpatialLayer_Render | SpatialLayer_Stress | SpatialLayer_Terrain | (1u << 3)) < 256, "RecordPool stores the layer mask in a byte");
export constexpr uint32 SpatialLayer_Entity = 1u << 3;  // EVERY non-global entity (userData = Entity*): the
                                                        // World's update-selection layer. Render is the
                                                        // subset with a render node (gameplay queries)

// Rebase a world-space frustum to camera-relative space (in double, so the planes stay exact at
// planet-scale camera positions): dot(n, p) + w == dot(n, p - camPos) + (w + dot(n, camPos)).
export inline Frustum rebaseFrustum(const Frustum& world, const glm::dvec3& cameraPos)
{
    Frustum out = world;
    for (uint32 i = 0; i < 6; ++i)
        out.planes[i].w = float(double(world.planes[i].w) + glm::dot(glm::dvec3(world.planes[i]), cameraPos));
    return out;
}

export inline void inflateFrustum(Frustum& frustum, float margin)
{
    for (uint32 i = 0; i < 6; ++i)
        frustum.planes[i].w += margin;
}

// The two visibility sets the render gate maintains. Main is the camera frustum (the set CPU
// occlusion culling can shrink further); Near is a camera ball that keeps off-screen shadow
// casters and ray-traced geometry pushed — the GPU shadow cull and the TLAS range bound do the
// per-pass refinement from there.
// The three UpdateTier passes are the World's SIM LOD selection (not rendering): balls around
// every focus point (the players) at the tier radii, stamped in the same cull job via
// setUpdateLod; the World reads an entity's mask to pick its tick rate and to decide which
// children a visited parent emits.
export enum class ESpatialPass : uint32
{
    Main = 0,
    Near,
    UpdateTier0,
    UpdateTier1,
    UpdateTier2,
    // World ROOT-DEDUPE stamps (from here on: never stamped on link, no visibility meaning):
    // UpdateRoot = "this root is in the current periodic selection result" (generation advances
    // with the selection job), VisibleRoot = "already queued from this frame's visible set"
    // (advances every pass). Read with the exact accessors only.
    UpdateRoot,
    VisibleRoot,
    Count,
};

// A visibility stamp: the pass generation an entry was last stamped in. 16-bit: a generation counts
// 1..65534 and the pool row is swept back to SpatialStamp_Linked when it wraps (advanceStamp), so
// an entry stamped ~65k generations ago can never read as current again.
export using SpatialStamp = uint16;
// Stamp value "linked, never stamped in this pass": the link gives the UpdateTier passes this instead
// of the spawn-guard 0 (which reads as "in every pass") or the current generation (which would read as
// a real tier). Never equals a generation — SpatialIndex::hasStamp.
export constexpr SpatialStamp SpatialStamp_Linked = 0xFFFF;

// Pass bits as returned by SpatialIndex::getPassMask, bit p == 1 << uint32(ESpatialPass p).
export constexpr uint32 SpatialPassBit_Main = 1u << 0;
export constexpr uint32 SpatialPassBit_Near = 1u << 1;
export constexpr uint32 SpatialPassBit_UpdateTier0 = 1u << 2;
export constexpr uint32 SpatialPassBit_UpdateTier1 = 1u << 3;
export constexpr uint32 SpatialPassBit_UpdateTier2 = 1u << 4;
export constexpr uint32 SpatialPassBits_UpdateTiers = SpatialPassBit_UpdateTier0 | SpatialPassBit_UpdateTier1 | SpatialPassBit_UpdateTier2;

export enum class ESpatialCullMode : int
{
    Off = 0,   // no queries, every entity is pushed for all render passes
    StatsOnly, // the queries run for their stats, nothing is gated
    Cull,      // main set renders fully; near-only entities are pushed for shadows/ray tracing only
    MainOnly,  // debug: only the main-frustum set is pushed at all (visibly breaks off-screen shadows/GI)
};

// Live-tweakable culling settings (Spatial/Culling): the App drives the markVisible* queries from
// these and Entity::updateTree pushes per-pass masks when mode >= Cull.
export struct SpatialCullingConfig
{
    int mode = int(ESpatialCullMode::Cull);
    bool freeze = false;             // stop re-stamping, fly around to inspect the culled set
    float margin = 4.0f;             // frustum inflation masking the one-frame stamp latency
    float nearRadius = 0.0f;       // shadow-caster + ray-tracing relevance range around the camera
    float nearSlack = 16.0f;         // Near ball inflation; requery only after the camera moves this far (0 = every frame)
    float maxDist = 5000.0f; // main-pass cull distance; overwritten each frame with the camera far plane (setCullMaxDist)
    float skinnedRadiusScale = 1.5f; // animation can exceed the bind-pose bounds sphere
};

export struct SpatialIndexDesc
{
    uint32 numLevels = Morton::MaxLevels;
    uint32 initialCellCapacity = 4096;  // per level, rounded up to a power of two
    uint32 initialEntryCapacity = 4096;
};

export struct SpatialStats
{
    int numEntries = 0;
    int numCells = 0;
    int cellsTested = 0;      // per-frame query counters, reset each commitFrame
    int cellsFullyInside = 0;
    int entityTests = 0;
    int visiblePerPass[uint32(ESpatialPass::Count)] = {};
    int staticEntries = 0;
    int staticRebuilds = 0;
    float commitMs = 0.0f;
    float markVisibleMs = 0.0f;
    float rebuildMs = 0.0f; // duration of the most recent static-tier rebuild
    int perLevelCells[Morton::MaxLevels] = {};
    int perLevelEntities[Morton::MaxLevels] = {};
};
