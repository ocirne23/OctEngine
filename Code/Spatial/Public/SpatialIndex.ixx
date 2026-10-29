module;

#include <immintrin.h>

export module Spatial:SpatialIndex;

import Core;
import Core.glm;
import Core.Camera;
import Core.Frustum;
import :SpatialTypes;
import :Types;
import :RecordPool;
import :SpatialIndexState; // the storage, the commit, the traversals, the stamps and the cull job

// THE SPATIAL INDEX (Globals::spatialIndex): culling and position lookup over an implicit 64-ary hierarchy of per-level hashed grids, independent of the
// entity tree. Cell-changing mutations queue and apply in commitFrame(). Threading contract: "Threading contract" in Spatial CONTEXT.md. The state and the
// private helpers are in SpatialIndexState.ixx; the multi-line bodies are below the class.
export class SpatialIndex final : public SpatialIndexState
{
public:

    void initialize(const SpatialIndexDesc& desc = {});

    // ---- Entries ----

    // spawnVisible: visible in every pass until the first real stamp (entities: true; streamed geometry: false). Wrap the handle in a SpatialEntry.
    SpatialHandle registerEntry(const glm::dvec3& pos, float radius, uint64 userData, uint32 layerMask = 1, bool spawnVisible = true);
    void updateEntry(SpatialHandle handle, const glm::dvec3& pos, float radius);
    void commitFrame(); // headless: in place of the cull job
    bool isAlive(SpatialHandle handle) const { return m_pool.isValidAlive(handle); }
    uint64 userData(SpatialHandle handle) const { return m_pool.isValidAlive(handle) ? m_pool.userData[handle.idx] : 0; }

    // ---- The frame ----

    // The per-frame visibility pass (commit, then the Main, Near and Shadow stamps) as the High "Spatial cull" job. An invalid view skips the frame. The
    // index must stay quiescent between kick and join.
    void kickUpdateJob(const CullView& view);
    void joinUpdateJob();
    const SpatialCullingConfig& getCullingConfig() const { return m_culling; }

    // ---- Queries (read-only, valid between commits) ----

    uint32 querySphere(const glm::dvec3& center, float radius, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    uint32 queryAABB(const glm::dvec3& boxMin, const glm::dvec3& boxMax, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    uint32 queryFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                        oc::vector<uint64>& outUserData, IOcclusionTester* occlusion = nullptr) const;
    uint32 queryRay(const glm::dvec3& origin, const glm::dvec3& dir, double maxDist, uint32 layerMask,
                    oc::vector<uint64>& outUserData) const; // broadphase: entries whose bounds cross the segment
    uint64 queryNearest(const glm::dvec3& pos, float maxRadius, uint32 layerMask, uint64 excludeUserData = 0) const;

    // emit(userData) per hit, straight out of the traversal (no result buffer): the form to use from job code. Never wait inside emit.
    // skipStampedIn: SpatialPassBit_* - hits currently stamped in any of these passes are skipped.
    template<typename Func>
    void forEachInSphere(const glm::dvec3& center, float radius, uint32 layerMask, Func&& emit, uint32 skipStampedIn = 0) const;
    template<typename Func>
    void forEachInFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                          Func&& emit, IOcclusionTester* occlusion = nullptr) const;

    // ---- Visibility stamps ----

    // Main pass; a never-stamped entry counts as visible unless registered with spawnVisible = false.
    bool isVisible(SpatialHandle handle) const;
    uint32 getPassMask(SpatialHandle handle) const; // SpatialPassBit_* bits; spawn guard as in isVisible
    void advanceStamp(ESpatialPass pass);           // opens a new generation for one pass (the World: VisibleRoot, every pass)

    // ---- The visible set hand-over ----

    // The cull job's Main-pass hits per collect slot (0 = the World's handles, 1 = the terrain / ocean userData). Valid from joinUpdateJob to the next
    // kick. See "The visible set hand-over" in CONTEXT.md.
    static constexpr uint32 NumVisibleCollectSlots = SpatialIndexState::NumVisibleCollectSlots;
    using ECollect = SpatialIndexState::ECollect; // an alias, not a using-declaration: MSVC keeps the base's access on the nested enum's name
    void setVisibleCollect(uint32 layerMask, uint32 slot = 0, ECollect what = ECollect::Handles)
    {
        m_visibleCollectLayers[slot] = layerMask;
        m_visibleCollectWhat[slot] = what;
    }
    const oc::vector<SpatialHandle>& visibleHandles(uint32 slot = 0) const { return m_visibleCollected[slot]; }
    const oc::vector<uint64>& visibleUserData(uint32 slot) const { return m_visibleCollectedUserData[slot]; }
    // Bumped per collect-list rebuild: a retired userData address is safe to free once this moves past it.
    uint32 visibleCollectGeneration() const { return m_visibleCollectGeneration; }

    // ---- SIM LOD (the World's selection job) ----

    void advanceUpdateTiers(); // the three tiers + UpdateRoot
    // One traversal of the ball, each hit emitted and stamped in every tier whose radius it is inside (tierRadius <= 0: not stamped). One call at a time;
    // hits per traversal chunk, valid until the next call.
    const oc::vector<oc::vector<uint64>>& queryUpdateTiers(const glm::dvec3& center, float queryRadius, const float tierRadius[3],
                                                           bool horizontal, uint32 layerMask);
    // Exact compares, no spawn guard.
    uint32 getPassMaskExact(SpatialHandle handle) const;
    bool hasStamp(SpatialHandle handle, ESpatialPass pass) const; // a generation was ever written in `pass` (not 0, not SpatialStamp_Linked)
    bool isStampedCurrent(SpatialHandle handle, ESpatialPass pass) const;
    void stampCurrent(SpatialHandle handle, ESpatialPass pass);
    bool stampCurrentOnce(SpatialHandle handle, ESpatialPass pass); // stamps current; true when it was not current before (atomic: one job wins)
};

export namespace Globals
{
    SpatialIndex spatialIndex;
}

// ---- Inline bodies ----

template<typename Func>
void SpatialIndex::forEachInSphere(const glm::dvec3& center, float radius, uint32 layerMask, Func&& emit, uint32 skipStampedIn) const
{
    forEachInSphereImpl(center, radius, layerMask, skipStampedIn, &emit,
        [](void* ctx, uint64 userData) { (*static_cast<std::remove_reference_t<Func>*>(ctx))(userData); });
}

template<typename Func>
void SpatialIndex::forEachInFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                                    Func&& emit, IOcclusionTester* occlusion) const
{
    forEachInFrustumImpl(frustumRelCamera, cameraPos, maxDist, layerMask, occlusion, &emit,
        [](void* ctx, uint64 userData) { (*static_cast<std::remove_reference_t<Func>*>(ctx))(userData); });
}

inline bool SpatialIndex::isVisible(SpatialHandle handle) const
{
    if (!m_pool.isValidAlive(handle))
        return false;
    const SpatialStamp stamp = m_pool.stamps[handle.idx].pass[uint32(ESpatialPass::Main)];
    return stamp == m_visibleQueryId[uint32(ESpatialPass::Main)]
        || (stamp == 0 && !(m_pool.flags[handle.idx] & RecordFlag_NoSpawnGuard));
}

inline uint32 SpatialIndex::getPassMask(SpatialHandle handle) const
{
    if (!m_pool.isValidAlive(handle))
        return 0;
    const __m128i stamps = stampLanes(handle.idx);
    __m128i lanes = currentLanes(stamps);
    if (!(m_pool.flags[handle.idx] & RecordFlag_NoSpawnGuard))
        lanes = _mm_or_si128(lanes, _mm_cmpeq_epi16(stamps, _mm_setzero_si128()));
    return passBits(lanes);
}

inline uint32 SpatialIndex::getPassMaskExact(SpatialHandle handle) const
{
    if (!m_pool.isValidAlive(handle))
        return 0;
    return passBits(currentLanes(stampLanes(handle.idx)));
}

inline bool SpatialIndex::hasStamp(SpatialHandle handle, ESpatialPass pass) const
{
    if (!m_pool.isValidAlive(handle))
        return false;
    const SpatialStamp stamp = m_pool.stamps[handle.idx].pass[uint32(pass)];
    return stamp != 0 && stamp != SpatialStamp_Linked;
}

inline bool SpatialIndex::isStampedCurrent(SpatialHandle handle, ESpatialPass pass) const
{
    return m_pool.isValidAlive(handle) && m_pool.stamps[handle.idx].pass[uint32(pass)] == m_visibleQueryId[uint32(pass)];
}

inline void SpatialIndex::stampCurrent(SpatialHandle handle, ESpatialPass pass)
{
    if (m_pool.isValidAlive(handle))
        m_pool.stamps[handle.idx].pass[uint32(pass)] = m_visibleQueryId[uint32(pass)];
}

inline bool SpatialIndex::stampCurrentOnce(SpatialHandle handle, ESpatialPass pass)
{
    if (!m_pool.isValidAlive(handle))
        return false;
    const SpatialStamp id = m_visibleQueryId[uint32(pass)];
    return oc::atomic_ref<SpatialStamp>(m_pool.stamps[handle.idx].pass[uint32(pass)]).exchange(id, oc::memory_order_relaxed) != id;
}
