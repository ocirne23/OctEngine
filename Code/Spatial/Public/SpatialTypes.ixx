export module Spatial:SpatialTypes;

import Core;
import Core.glm;
import Core.Frustum;
export import Settings.Spatial; // ESpatialCullMode / SpatialCullingConfig: getCullingConfig's type
import :Morton;

// The value types of the spatial index's API (SpatialIndex.ixx).

// ---- Handles ----

export struct SpatialHandle
{
    uint32 idx = UINT32_MAX;
    uint32 gen = 0;

    bool isValid() const { return idx != UINT32_MAX; }
};

// RAII, move-only: adopts a SpatialIndex::registerEntry handle, unregisters it on destruction.
export class SpatialEntry final
{
public:

    SpatialEntry() = default;
    explicit SpatialEntry(SpatialHandle handle) : m_handle(handle) {}
    SpatialEntry(SpatialEntry&& other) noexcept : m_handle(other.m_handle) { other.m_handle = {}; }
    SpatialEntry& operator=(SpatialEntry&& other) noexcept;
    ~SpatialEntry() { reset(); }

    void reset();
    bool isValid() const { return m_handle.isValid(); }
    SpatialHandle handle() const { return m_handle; }

    SpatialEntry(const SpatialEntry&) = delete;
    SpatialEntry& operator=(const SpatialEntry&) = delete;

private:

    SpatialHandle m_handle;
};

// ---- Layers ----

export constexpr uint32 SpatialLayer_Render = 1u << 0;  // entities with a RenderComponent
export constexpr uint32 SpatialLayer_Stress = 1u << 1;  // synthetic stress-test entries
export constexpr uint32 SpatialLayer_Terrain = 1u << 2; // terrain chunks + ocean sectors: userData is never an Entity*
export constexpr uint32 SpatialLayer_Entity = 1u << 3;  // every non-global entity (userData = Entity*)
static_assert((SpatialLayer_Render | SpatialLayer_Stress | SpatialLayer_Terrain | SpatialLayer_Entity) < 256, "RecordPool stores the layer mask in a byte");

// SpatialLayer_Terrain userData bit (above the user-mode address range): set on ocean sectors.
export constexpr uint64 SpatialTerrainTag_Ocean = 1ull << 63;

// ---- Passes ----

// Main: the camera frustum. Near: the camera ball (off-screen shadow casters, RT). UpdateTier*: the World's SIM LOD balls.
export enum class ESpatialPass : uint32
{
    Main = 0,
    Near,
    Shadow, // the Main frustum swept toward the sun: off-screen sun shadow casters
    UpdateTier0,
    UpdateTier1,
    UpdateTier2,
    // The World's root-dedupe stamps (no visibility meaning; exact accessors only).
    UpdateRoot,
    VisibleRoot,
    Count,
};

// bit p == 1 << uint32(ESpatialPass p).
export constexpr uint32 SpatialPassBit_Main = 1u << 0;
export constexpr uint32 SpatialPassBit_Near = 1u << 1;
export constexpr uint32 SpatialPassBit_Shadow = 1u << 2;
export constexpr uint32 SpatialPassBit_UpdateTier0 = 1u << 3;
export constexpr uint32 SpatialPassBit_UpdateTier1 = 1u << 4;
export constexpr uint32 SpatialPassBit_UpdateTier2 = 1u << 5;
export constexpr uint32 SpatialPassBits_UpdateTiers = SpatialPassBit_UpdateTier0 | SpatialPassBit_UpdateTier1 | SpatialPassBit_UpdateTier2;

static_assert(SpatialStatsMaxLevels == Morton::MaxLevels && SpatialStatsMaxPasses == uint32(ESpatialPass::Count),
    "SpatialStats (Settings.Spatial) is sized for the level and pass counts");
static_assert(uint32(ESpatialPass::Main) == 0 && uint32(ESpatialPass::Near) == 1 && uint32(ESpatialPass::Shadow) == 2,
    "the Spatial/Stats Visible rows (Settings.Spatial) index visiblePerPass by pass");

// ---- Queries and setup ----

// World-space frustum to camera-relative (in double): dot(n, p) + w == dot(n, p - camPos) + (w + dot(n, camPos)).
export inline Frustum rebaseFrustum(const Frustum& world, const glm::dvec3& cameraPos)
{
    Frustum out = world;
    for (uint32 i = 0; i < 6; ++i)
        out.planes[i].w = float(double(world.planes[i].w) + glm::dot(glm::dvec3(world.planes[i]), cameraPos));
    return out;
}

export struct SpatialIndexDesc
{
    uint32 numLevels = Morton::MaxLevels;
    uint32 initialCellCapacity = 4096;  // per level, rounded up to a power of two
    uint32 initialEntryCapacity = 4096;
};

// ---- Inline bodies ----

inline SpatialEntry& SpatialEntry::operator=(SpatialEntry&& other) noexcept
{
    if (this != &other)
    {
        reset();
        m_handle = other.m_handle;
        other.m_handle = {};
    }
    return *this;
}
