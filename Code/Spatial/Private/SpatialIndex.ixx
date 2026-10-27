module;

#include <immintrin.h>

export module Spatial:SpatialIndex;

import Core;
import Core.glm;
import Core.Camera;
import Core.Frustum;
import Threading;
import Settings;
import :Morton;
import :Types;
import :CellMap;
import :RecordPool;
import :BlockStore;

// An implicit 64-ary hierarchy over per-level hashed grids, independent of the entity tree. Cell-changing
// mutations queue and apply in commitFrame(). Threading contract: "Threading contract" in Spatial CONTEXT.md.
export class SpatialIndex final
{
public:

    void initialize(const SpatialIndexDesc& desc = {});

    // -- Entries --
    // spawnVisible: visible in every pass until the first real stamp (entities: true; streamed geometry: false).
    SpatialHandle registerEntry(const glm::dvec3& pos, float radius, uint64 userData, uint32 layerMask = 1, bool spawnVisible = true);
    void unregisterEntry(SpatialHandle handle); // neutralized immediately, unlinked at commit
    void updateEntry(SpatialHandle handle, const glm::dvec3& pos, float radius);
    void setLayerMask(SpatialHandle handle, uint32 layerMask);
    void commitFrame();

    bool isAlive(SpatialHandle handle) const { return m_pool.isValidAlive(handle); }
    uint64 userData(SpatialHandle handle) const { return m_pool.isValidAlive(handle) ? m_pool.userData[handle.idx] : 0; }
    glm::dvec3 getPosition(SpatialHandle handle) const;
    float getRadius(SpatialHandle handle) const;

    // -- The frame --
    // The per-frame visibility pass: commit, then the Main, Near and Shadow stamps. sunDirection points
    // at the sun; viewProjRelCamera is reversed-Z.
    void update(const Camera& camera, const Frustum& frustum, const glm::mat4& viewProjRelCamera, const glm::vec3& sunDirection);
    // update() as the High "Spatial cull" job. An invalid view skips the frame. The index must stay
    // quiescent between kick and join.
    void kickUpdateJob(const CullView& view);
    void joinUpdateJob();

    void setCullMaxDist(float maxDist) { m_culling.maxDist = maxDist; } // the render camera's far plane
    const SpatialCullingConfig& getCullingConfig() const { return m_culling; }
    const SpatialStats& getStats() const { return m_stats; }

    // -- Queries --
    uint32 querySphere(const glm::dvec3& center, float radius, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    uint32 queryAABB(const glm::dvec3& boxMin, const glm::dvec3& boxMax, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    uint32 queryFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                        oc::vector<uint64>& outUserData, IOcclusionTester* occlusion = nullptr) const;
    uint32 queryRay(const glm::dvec3& origin, const glm::dvec3& dir, double maxDist, uint32 layerMask,
                    oc::vector<uint64>& outUserData) const; // broadphase: entries whose bounds cross the segment
    uint64 queryNearest(const glm::dvec3& pos, float maxRadius, uint32 layerMask, uint64 excludeUserData = 0) const;

    // emit(userData) per hit, straight out of the traversal (no result buffer). Never wait inside emit.
    // skipStampedIn: SpatialPassBit_* - hits currently stamped in any of these passes are skipped.
    template<typename Func>
    void forEachInSphere(const glm::dvec3& center, float radius, uint32 layerMask, Func&& emit, uint32 skipStampedIn = 0) const
    {
        forEachInSphereImpl(center, radius, layerMask, skipStampedIn, &emit,
            [](void* ctx, uint64 userData) { (*static_cast<std::remove_reference_t<Func>*>(ctx))(userData); });
    }
    template<typename Func>
    void forEachInFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                          Func&& emit, IOcclusionTester* occlusion = nullptr) const
    {
        forEachInFrustumImpl(frustumRelCamera, cameraPos, maxDist, layerMask, occlusion, &emit,
            [](void* ctx, uint64 userData) { (*static_cast<std::remove_reference_t<Func>*>(ctx))(userData); });
    }

    // -- Visibility stamps --
    // Each mark call starts a new stamp generation for that pass.
    void markVisibleSet(ESpatialPass pass, const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist,
                        uint32 layerMask, IOcclusionTester* occlusion = nullptr);
    void markVisibleSphere(ESpatialPass pass, const glm::dvec3& center, float radius, uint32 layerMask);
    void advanceStamp(ESpatialPass pass);

    // Main pass; a never-stamped entry counts as visible unless registered with spawnVisible = false.
    bool isVisible(SpatialHandle handle) const
    {
        if (!m_pool.isValidAlive(handle))
            return false;
        const SpatialStamp stamp = m_pool.stamps[handle.idx].pass[uint32(ESpatialPass::Main)];
        return stamp == m_visibleQueryId[uint32(ESpatialPass::Main)]
            || (stamp == 0 && !(m_pool.flags[handle.idx] & RecordFlag_NoSpawnGuard));
    }
    uint32 getPassMask(SpatialHandle handle) const // SpatialPassBit_* bits; spawn guard as in isVisible
    {
        if (!m_pool.isValidAlive(handle))
            return 0;
        const __m128i stamps = stampLanes(handle.idx);
        __m128i lanes = currentLanes(stamps);
        if (!(m_pool.flags[handle.idx] & RecordFlag_NoSpawnGuard))
            lanes = _mm_or_si128(lanes, _mm_cmpeq_epi16(stamps, _mm_setzero_si128()));
        return passBits(lanes);
    }

    // -- The visible set hand-over --
    // The cull job's Main-pass hits per collect slot (0 = the World's handles, 1 = the terrain / ocean
    // userData). Valid from joinUpdateJob to the next kick. See "The visible set hand-over" in CONTEXT.md.
    static constexpr uint32 NumVisibleCollectSlots = 2;
    enum class ECollect : uint8 { Handles, UserData };
    void setVisibleCollect(uint32 layerMask, uint32 slot = 0, ECollect what = ECollect::Handles)
    {
        m_visibleCollectLayers[slot] = layerMask;
        m_visibleCollectWhat[slot] = what;
    }
    const oc::vector<SpatialHandle>& visibleHandles(uint32 slot = 0) const { return m_visibleCollected[slot]; }
    const oc::vector<uint64>& visibleUserData(uint32 slot) const { return m_visibleCollectedUserData[slot]; }
    // Bumped per collect-list rebuild: a retired userData address is safe to free once this moves past it.
    uint32 visibleCollectGeneration() const { return m_visibleCollectGeneration; }

    // -- SIM LOD (the World's selection job) --
    void advanceUpdateTiers(); // the three tiers + UpdateRoot
    // One traversal of the ball, each hit emitted and stamped in every tier whose radius it is inside
    // (tierRadius <= 0: not stamped). One call at a time; hits per traversal chunk, valid until the next call.
    const oc::vector<oc::vector<uint64>>& queryUpdateTiers(const glm::dvec3& center, float queryRadius, const float tierRadius[3],
                                                           bool horizontal, uint32 layerMask);

    // Exact compares, no spawn guard.
    uint32 getPassMaskExact(SpatialHandle handle) const
    {
        if (!m_pool.isValidAlive(handle))
            return 0;
        return passBits(currentLanes(stampLanes(handle.idx)));
    }
    // A generation was ever written in `pass` (not 0, not SpatialStamp_Linked).
    bool hasStamp(SpatialHandle handle, ESpatialPass pass) const
    {
        if (!m_pool.isValidAlive(handle))
            return false;
        const SpatialStamp stamp = m_pool.stamps[handle.idx].pass[uint32(pass)];
        return stamp != 0 && stamp != SpatialStamp_Linked;
    }
    bool isStampedCurrent(SpatialHandle handle, ESpatialPass pass) const
    {
        return m_pool.isValidAlive(handle) && m_pool.stamps[handle.idx].pass[uint32(pass)] == m_visibleQueryId[uint32(pass)];
    }
    void stampCurrent(SpatialHandle handle, ESpatialPass pass)
    {
        if (m_pool.isValidAlive(handle))
            m_pool.stamps[handle.idx].pass[uint32(pass)] = m_visibleQueryId[uint32(pass)];
    }
    // Stamps the entry current in `pass`; true when it was not current before (atomic: one job wins).
    bool stampCurrentOnce(SpatialHandle handle, ESpatialPass pass)
    {
        if (!m_pool.isValidAlive(handle))
            return false;
        const SpatialStamp id = m_visibleQueryId[uint32(pass)];
        return oc::atomic_ref<SpatialStamp>(m_pool.stamps[handle.idx].pass[uint32(pass)]).exchange(id, oc::memory_order_relaxed) != id;
    }

private:

    struct PendingOp
    {
        enum EType : uint8 { Link, Move, Unlink };
        uint64 newKey;
        glm::vec3 newRelPos;
        float newRadius;
        uint32 idx;
        uint32 gen;
        EType type;
        uint8 newLevel;
    };
    struct EmptyCandidate
    {
        uint64 key;
        uint32 level;
    };
    struct CompactEntry { float x, y, z, radius; uint32 layer, poolIdx; };
    // Per-traversal counters, merged into m_stats once at the end (traversals run concurrently).
    struct TraverseStats
    {
        int cellsTested = 0;
        int cellsFullyInside = 0;
        int entityTests = 0;
        int emitted = 0;
    };
    // A subtree root for the parallel fan-out, already classified (active 0 = fully inside).
    struct FrontierCell
    {
        uint64 key;
        const CellRecord* rec; // stable: the cell maps only mutate in commitFrame
        glm::vec3 cellMin;     // relative to the traversal's refPos
        uint32 level;
        uint32 active;
    };

    // One entry's 8 stamps as 16-bit lanes vs the 8 current generations; passBits packs to SpatialPassBit_*.
    __m128i stampLanes(uint32 idx) const { return _mm_load_si128(reinterpret_cast<const __m128i*>(&m_pool.stamps[idx])); }
    __m128i currentLanes(__m128i stamps) const
    {
        return _mm_cmpeq_epi16(stamps, _mm_load_si128(reinterpret_cast<const __m128i*>(m_visibleQueryId)));
    }
    static uint32 passBits(__m128i lanes) { return uint32(_mm_movemask_epi8(_mm_packs_epi16(lanes, _mm_setzero_si128()))); }

    // Commit-only bookkeeping (see BlockStore).
    uint32 entryLevel(float radius); // levelForRadius clamped to the level count; tracks the oversize radius
    void insertLane(uint32 idx);
    void retireLane(uint32 idx);
    void compactCell(uint32 level, CellRecord& rec);
    void setOccupancyBits(uint32 level, uint64 key);
    void sweepEmptyCells();

    // -- Traversal -- `active`: the tester's still-undecided bounds; 0 = the cell is fully inside.
    void forEachInSphereImpl(const glm::dvec3& center, float radius, uint32 layerMask, uint32 skipStampedIn, void* ctx, void (*emit)(void*, uint64)) const;
    void forEachInFrustumImpl(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                              IOcclusionTester* occlusion, void* ctx, void (*emit)(void*, uint64)) const;

    template <typename Tester, typename EmitFunc>
    void traverse(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask, const EmitFunc& emit) const;

    // A cell that passed its test: emits its entries, then recurses into the children that pass.
    template <typename Tester, typename EmitFunc>
    void traverseCell(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                      uint64 key, uint32 level, const CellRecord& rec, const glm::vec3& cellMin, uint32 active,
                      TraverseStats& stats, const EmitFunc& emit) const;

    // Returns the cell's new active mask, or UINT32_MAX when it is culled.
    template <typename Tester>
    uint32 testCell(const Tester& tester, const glm::vec3& cellMin, float halfCell, uint32 level,
                    uint32 active, TraverseStats& stats) const;

    // onChild(childKey, childRecord, childMin, childActive) for each child that passes.
    template <typename Tester, typename ChildFunc>
    void forEachChild(const Tester& tester, const glm::dvec3& refPos, uint64 key, uint32 level,
                      const CellRecord& rec, const glm::vec3& cellMin, uint32 active,
                      TraverseStats& stats, const ChildFunc& onChild) const;

    // emit(poolIdx, posRelRef, layerMask) per entry of the cell, 8 lanes at a time.
    template <typename Tester, typename EmitFunc>
    void emitCellEntries(const Tester& tester, const glm::vec3& cellMin, uint32 layerMask,
                         const CellRecord& rec, uint32 level, uint32 active,
                         TraverseStats& stats, const EmitFunc& emit) const;

    // Serial frontier expansion, then a parallelFor over the roots. emit must be thread-safe; a chunk-aware
    // emit gets prepareChunks(n) first. Uses m_frontier: one call at a time.
    template <typename Tester, typename EmitFunc, typename PrepareFunc>
    void traverseParallel(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                          TraverseStats& stats, const EmitFunc& emit, const PrepareFunc& prepareChunks);

    // -- Storage --
    oc::array<CellMap, Morton::MaxLevels> m_levels;
    oc::array<BlockStore, Morton::MaxLevels> m_blocks;
    RecordPool m_pool;
    uint32 m_levelEntityCount[Morton::MaxLevels] = {};
    uint32 m_numLevels = Morton::MaxLevels;
    oc::atomic<float> m_topLevelMaxRadius = 0.0f; // largest clamped-oversize radius (CAS-max)
    bool m_initialized = false;

    // -- Commit --
    // PerWorker on purpose: pushed from any job with no chunk identity, unbounded volume.
    PerWorker<oc::vector<PendingOp>> m_pendingOps;
    oc::vector<EmptyCandidate> m_emptyCandidates;
    oc::vector<CompactEntry> m_compactScratch;

    // -- Stamps --
    uint32 m_frameId = 1;
    alignas(16) SpatialStamp m_visibleQueryId[uint32(ESpatialPass::Count)] = {}; // generation per pass, 0 = never; one __m128i

    // -- Traversal scratch (main thread only) --
    oc::vector<FrontierCell> m_frontier;
    oc::vector<FrontierCell> m_frontierNext;
    oc::vector<oc::vector<uint64>> m_tierHitChunks;

    // -- Visible set hand-over -- chunk list 0 = the serial expansion, 1 + i = fan-out chunk i; merged per slot.
    uint32 m_visibleCollectLayers[NumVisibleCollectSlots] = {};
    ECollect m_visibleCollectWhat[NumVisibleCollectSlots] = {};
    oc::vector<oc::vector<SpatialHandle>> m_visibleCollectChunks[NumVisibleCollectSlots];
    oc::vector<SpatialHandle> m_visibleCollected[NumVisibleCollectSlots];
    oc::vector<oc::vector<uint64>> m_visibleCollectUserDataChunks[NumVisibleCollectSlots];
    oc::vector<uint64> m_visibleCollectedUserData[NumVisibleCollectSlots];
    uint32 m_visibleCollectGeneration = 0;

    // -- The update job -- its copy of the view (the job outlives the caller's stack).
    Camera m_updateJobCamera;
    Frustum m_updateJobFrustum;
    glm::mat4 m_updateJobViewProj = glm::mat4(1.0f);
    glm::vec3 m_updateJobSunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    JobCounter m_updateJobCounter;
    bool m_updateJobKicked = false; // false = the view was invalid, join has nothing to wait on
    // Near-ball requery hysteresis.
    glm::dvec3 m_lastNearQueryPos = glm::dvec3(1e30);
    uint32     m_framesSinceNearQuery = 0;

    // -- Settings --
    SpatialCullingConfig& m_culling = Globals::settings.spatial.culling;
    SpatialStats& m_stats = Globals::settings.spatial.stats;
};

export namespace Globals
{
    SpatialIndex spatialIndex;
}
