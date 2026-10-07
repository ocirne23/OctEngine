export module Spatial:Types;

import Core;
import Core.glm;
import Core.Frustum;
export import Settings.Spatial;
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
export constexpr uint32 SpatialLayer_Terrain = 1u << 2; // procedural terrain chunks and ocean sectors (render
                                                        // culling only: NOT entities - the userData is a
                                                        // chunk's RenderNode*, a sector's RenderNode* |
                                                        // SpatialTerrainTag_Ocean, or 0, never an Entity*,
                                                        // so gameplay queries must never include this layer)
export constexpr uint32 SpatialLayer_Entity = 1u << 3;  // EVERY non-global entity (userData = Entity*): the
                                                        // World's update-selection layer. Render is the
                                                        // subset with a render node (gameplay queries)
static_assert((SpatialLayer_Render | SpatialLayer_Stress | SpatialLayer_Terrain | SpatialLayer_Entity) < 256, "RecordPool stores the layer mask in a byte");

// SpatialLayer_Terrain userData type bit, above the 47-bit user-mode address range: set on ocean
// sectors' RenderNode pointers, clear on terrain chunks' (and on a 0 userData). The collect list
// masks it off and pushes both alike; the terrain's shadow ball skips the tagged ones.
export constexpr uint64 SpatialTerrainTag_Ocean = 1ull << 63;

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
// casters and ray-traced geometry pushed - the GPU shadow cull and the TLAS range bound do the
// per-pass refinement from there.
// The three UpdateTier passes are the World's SIM LOD selection (not rendering): balls around
// every focus point (the players) at the tier radii, stamped in the same cull job via
// setUpdateLod; the World reads an entity's mask to pick its tick rate and to decide which
// children a visited parent emits.
export enum class ESpatialPass : uint32
{
    Main = 0,
    Near,
    Shadow, // the Main frustum swept toward the sun (SpatialCullingConfig::shadowReach): off-screen sun shadow casters
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
// a real tier). Never equals a generation - SpatialIndex::hasStamp.
export constexpr SpatialStamp SpatialStamp_Linked = 0xFFFF;

// Pass bits as returned by SpatialIndex::getPassMask, bit p == 1 << uint32(ESpatialPass p).
export constexpr uint32 SpatialPassBit_Main = 1u << 0;
export constexpr uint32 SpatialPassBit_Near = 1u << 1;
export constexpr uint32 SpatialPassBit_Shadow = 1u << 2;
export constexpr uint32 SpatialPassBit_UpdateTier0 = 1u << 3;
export constexpr uint32 SpatialPassBit_UpdateTier1 = 1u << 4;
export constexpr uint32 SpatialPassBit_UpdateTier2 = 1u << 5;
export constexpr uint32 SpatialPassBits_UpdateTiers = SpatialPassBit_UpdateTier0 | SpatialPassBit_UpdateTier1 | SpatialPassBit_UpdateTier2;

// ESpatialCullMode, SpatialCullingConfig and SpatialStats are Settings.Spatial's (Globals::settings.spatial).
static_assert(SpatialStatsMaxLevels == Morton::MaxLevels && SpatialStatsMaxPasses == uint32(ESpatialPass::Count),
    "SpatialStats (Settings.Spatial) is sized for the level and pass counts");
static_assert(uint32(ESpatialPass::Main) == 0 && uint32(ESpatialPass::Near) == 1 && uint32(ESpatialPass::Shadow) == 2,
    "the Spatial/Stats Visible rows (Settings.Spatial) index visiblePerPass by pass");

export struct SpatialIndexDesc
{
    uint32 numLevels = Morton::MaxLevels;
    uint32 initialCellCapacity = 4096;  // per level, rounded up to a power of two
    uint32 initialEntryCapacity = 4096;
};
