export module Spatial:SpatialIndex;

import Core;
import Core.glm;
import Core.Camera;
import Core.Frustum;
import Threading;
import :Morton;
import :Types;
import :CellMap;
import :RecordPool;
import :BlockStore;

// Spatial index independent of the entity parent/child hierarchy: an implicit 64-ary hierarchy
// over per-level hashed grids. Entries live at exactly one level (the one matching their bounding
// radius); every cell knows which of its 4x4x4 children are occupied, so queries descend the
// hierarchy with bit scans and never visit empty space. Mutations that change a cell are queued
// and applied in commitFrame(); same-cell position updates write in place. Queries are read-only
// and valid between commits.
//
// Threading contract: updateEntry is callable from any job during the parallel entity pass -
// same-cell updates write only that entry's SoA slots, and cell-changing ops stage into per-worker
// pending lists. registerEntry/unregisterEntry are callable from any thread in the spawn window
// (parallel entity spawning): both take m_registerMutex exclusively - pool growth reallocates the
// SoA the query traversals read, so the query* entry points take it SHARED (a spawning worker's
// script OnSpawn may query while another worker registers). setLayerMask/commitFrame stay
// single-threaded (main, outside the pass); the markVisible* traversals stay lock-free (the
// kick/join window forbids registration by contract).
export class SpatialIndex final
{
public:

    void initialize(const SpatialIndexDesc& desc = {});

    // spawnVisible: whether the entry counts as visible in every pass until its first real stamp.
    // Entities want true (a fresh spawn must not flash invisible during its link+stamp latency);
    // streamed geometry (terrain chunks) passes false - appearing one frame late is invisible for
    // something that didn't exist before, while the guard would leak never-stamped off-screen entries
    // into the main pass (permanently while the culling is frozen).
    SpatialHandle registerEntry(const glm::dvec3& pos, float radius, uint64 userData, uint32 layerMask = 1, bool spawnVisible = true);
    void unregisterEntry(SpatialHandle handle); // neutralized immediately, unlinked at commit
    void updateEntry(SpatialHandle handle, const glm::dvec3& pos, float radius);
    void setLayerMask(SpatialHandle handle, uint32 layerMask);
    void commitFrame();

    uint32 querySphere(const glm::dvec3& center, float radius, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    // CALLBACK forms: `emit(userData)` per hit, straight out of the traversal - no result buffer,
    // so a caller on a job fiber needs no scratch at all (a thread_local one would follow the
    // THREAD across a park, not the job). Zero-allocation type erasure: the functor stays on the
    // caller's stack. Do NOT wait inside emit (the index's shared lock is held).
    template<typename Func>
    void forEachInSphere(const glm::dvec3& center, float radius, uint32 layerMask, Func&& emit) const
    {
        forEachInSphereImpl(center, radius, layerMask, &emit,
            [](void* ctx, uint64 userData) { (*static_cast<std::remove_reference_t<Func>*>(ctx))(userData); });
    }
    template<typename Func>
    void forEachInFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                          Func&& emit, IOcclusionTester* occlusion = nullptr) const
    {
        forEachInFrustumImpl(frustumRelCamera, cameraPos, maxDist, layerMask, occlusion, &emit,
            [](void* ctx, uint64 userData) { (*static_cast<std::remove_reference_t<Func>*>(ctx))(userData); });
    }
    uint32 queryAABB(const glm::dvec3& boxMin, const glm::dvec3& boxMax, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    uint32 queryFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                        oc::vector<uint64>& outUserData, IOcclusionTester* occlusion = nullptr) const;
    uint32 queryRay(const glm::dvec3& origin, const glm::dvec3& dir, double maxDist, uint32 layerMask,
                    oc::vector<uint64>& outUserData) const; // broadphase: entries whose bounds cross the segment
    uint64 queryNearest(const glm::dvec3& pos, float maxRadius, uint32 layerMask, uint64 excludeUserData = 0) const;
    void forEachInSphereImpl(const glm::dvec3& center, float radius, uint32 layerMask, void* ctx, void (*emit)(void*, uint64)) const;
    void forEachInFrustumImpl(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                              IOcclusionTester* occlusion, void* ctx, void (*emit)(void*, uint64)) const;

    // Per-pass visibility stamps consumed by the render gate; each call invalidates that pass's
    // previous stamp generation (single consumer per pass by design). Main is the camera frustum
    // (occlusion-testable); Near is the camera ball that keeps off-screen shadow casters and
    // ray-traced geometry alive.
    void markVisibleSet(ESpatialPass pass, const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist,
                        uint32 layerMask, IOcclusionTester* occlusion = nullptr);
    void markVisibleSphere(ESpatialPass pass, const glm::dvec3& center, float radius, uint32 layerMask);
    // SIM LOD selection (the World's selection job - NOT the cull job: it is update logic, and
    // not frame-critical). advanceUpdateTiers opens a new stamp generation for the three
    // UpdateTier passes (once per selection, before its traversals; nothing else stamps them, so
    // the stamps stay current until the next selection). queryUpdateTiers is ONE traversal of the
    // ball (center, queryRadius): every hit is emitted AND stamped in each tier whose radius it
    // falls inside (nested balls: tierRadius[t] > distance, <= 0 = that tier is not stamped by
    // this ball; horizontal = XZ distance). Stamps are pure stores - several calls may run
    // concurrently on jobs; the hit list is the caller's.
    void advanceUpdateTiers(); // the three tiers + UpdateRoot
    void advanceStamp(ESpatialPass pass); // one pass's generation (the World: VisibleRoot, every pass)
    // Root DEDUPE: stamps the entry current in `pass` and reports whether it was NOT current
    // before - an ATOMIC exchange, so of several jobs reaching one root exactly one gets true.
    bool stampCurrentOnce(SpatialHandle handle, ESpatialPass pass)
    {
        if (!m_pool.isValidAlive(handle))
            return false;
        const SpatialStamp id = m_visibleQueryId[uint32(pass)];
        return oc::atomic_ref<SpatialStamp>(m_pool.lastVisible[uint32(pass)][handle.idx]).exchange(id, oc::memory_order_relaxed) != id;
    }
    // The VISIBLE set for the World's update selection, straight from the cull job's Main
    // frustum pass - no second traversal: the Main stamp also appends the HANDLE of every hit
    // carrying one of these layers (per-chunk lists, merged once inside the job). Valid from
    // joinUpdateJob until the next kick; empty when nothing collects (headless, layers 0).
    // Handles, not userData: entities may die between the join and the consumer (the destroy
    // windows sit there) - userData(handle) reads 0 for a dead one.
    void setVisibleCollect(uint32 layerMask) { m_visibleCollectLayers = layerMask; }
    const oc::vector<SpatialHandle>& visibleHandles() const { return m_visibleCollected; }
    uint64 userData(SpatialHandle handle) const { return m_pool.isValidAlive(handle) ? m_pool.userData[handle.idx] : 0; }
    // PARALLEL (traverseParallel, so one call at a time - the World's selection runs its spheres
    // in sequence): the hits come back as owner-sliced lists, one per traversal chunk (some
    // empty), valid until the next queryUpdateTiers. Callable from a job; its chunks take the
    // register lock themselves (a spawn may register meanwhile).
    const oc::vector<oc::vector<uint64>>& queryUpdateTiers(const glm::dvec3& center, float queryRadius, const float tierRadius[3],
                                                           bool horizontal, uint32 layerMask);

    // Exact-compare variants (NO spawn guard: a never-stamped entry reads as in no pass) for the
    // World's update selection. isStampedCurrent/stampCurrent are its single-entry accessors (the
    // selection job stamps the ancestors of a selected entity; pure stores, job-safe).
    // isAlive lets the World check that a root its post-update selection job found still exists
    // when the next pass uses it (the generation in the handle rules out slot reuse).
    bool isAlive(SpatialHandle handle) const { return m_pool.isValidAlive(handle); }
    uint32 getPassMaskExact(SpatialHandle handle) const
    {
        if (!m_pool.isValidAlive(handle))
            return 0;
        uint32 mask = 0;
        for (uint32 p = 0; p < uint32(ESpatialPass::Count); ++p)
            if (m_pool.lastVisible[p][handle.idx] == m_visibleQueryId[p])
                mask |= 1u << p;
        return mask;
    }
    // Whether a stamp generation was EVER written in `pass` (neither the spawn-guard 0 nor the
    // link-time SpatialStamp_Linked): for the tier passes, "the selection job placed this entry
    // at some point" - else the World derives the tier from the distance.
    bool hasStamp(SpatialHandle handle, ESpatialPass pass) const
    {
        if (!m_pool.isValidAlive(handle))
            return false;
        const SpatialStamp stamp = m_pool.lastVisible[uint32(pass)][handle.idx];
        return stamp != 0 && stamp != SpatialStamp_Linked;
    }
    bool isStampedCurrent(SpatialHandle handle, ESpatialPass pass) const
    {
        return m_pool.isValidAlive(handle) && m_pool.lastVisible[uint32(pass)][handle.idx] == m_visibleQueryId[uint32(pass)];
    }
    void stampCurrent(SpatialHandle handle, ESpatialPass pass)
    {
        if (m_pool.isValidAlive(handle))
            m_pool.lastVisible[uint32(pass)][handle.idx] = m_visibleQueryId[uint32(pass)];
    }

    // THE per-frame visibility pass, in one call (the App's only spatial step): commits the cell moves
    // queued during last frame's entity updates, tracks the render camera's far plane, then stamps the
    // Main frustum set - rasterizing the CPU occlusion buffer first when it is enabled - and the Near
    // ball. The Near ball is requeried only once the camera has moved past its slack, so its hysteresis
    // state belongs here rather than at the call site. Then the Shadow pass: the same frustum swept
    // toward the sun (sunDirection points AT the sun) on the horizontal plane by shadowReach, so the
    // off-screen casters whose shadows fall into the view keep their shadow pass.
    // viewProjRelCamera maps camera-relative world positions to clip space in the renderer's REVERSED-Z
    // convention; it is flipped back to standard z here, for the occlusion rasterizer only.
    void update(const Camera& camera, const Frustum& frustum, const glm::mat4& viewProjRelCamera, const glm::vec3& sunDirection);

    // update() as the High "Spatial cull" job: kick copies the view into members (the job outlives
    // the caller's stack) and submits; join waits, helping. An INVALID view (the first VR frame - see
    // Renderer::getCullView) skips the whole update for that frame: stamps stay a frame stale (the
    // spawn guard keeps fresh entries visible) and the commit's pending ops just wait one frame. The
    // index must stay QUIESCENT between kick and join - no registers, commits, queries or traversals
    // (see main.cpp's window comment).
    void kickUpdateJob(const CullView& view);
    void joinUpdateJob();

    // Main pass; a never-stamped entry counts as visible unless it registered with spawnVisible = false
    // (see registerEntry).
    bool isVisible(SpatialHandle handle) const
    {
        if (!m_pool.isValidAlive(handle))
            return false;
        const SpatialStamp stamp = m_pool.lastVisible[uint32(ESpatialPass::Main)][handle.idx];
        return stamp == m_visibleQueryId[uint32(ESpatialPass::Main)]
            || (stamp == 0 && !(m_pool.flags[handle.idx] & RecordFlag_NoSpawnGuard));
    }

    uint32 getPassMask(SpatialHandle handle) const // SpatialPassBit_* bits; spawn guard as in isVisible
    {
        if (!m_pool.isValidAlive(handle))
            return 0;
        const bool spawnGuard = !(m_pool.flags[handle.idx] & RecordFlag_NoSpawnGuard);
        uint32 mask = 0;
        for (uint32 p = 0; p < uint32(ESpatialPass::Count); ++p)
        {
            const SpatialStamp stamp = m_pool.lastVisible[p][handle.idx];
            if (stamp == m_visibleQueryId[p] || (spawnGuard && stamp == 0))
                mask |= 1u << p;
        }
        return mask;
    }

    glm::dvec3 getPosition(SpatialHandle handle) const;
    float getRadius(SpatialHandle handle) const;

    const SpatialStats& getStats() const { return m_stats; }
    const SpatialCullingConfig& getCullingConfig() const { return m_culling; }
    // The main-pass cull distance tracks the render camera's far plane (driven per frame by the App),
    // so terrain/entities stream to exactly the view distance rather than a fixed cap.
    void setCullMaxDist(float maxDist) { m_culling.maxDist = maxDist; }

private:

    // kickUpdateJob storage: the job reads these, so they only change while no job is in flight.
    Camera m_updateJobCamera;
    Frustum m_updateJobFrustum;
    glm::mat4 m_updateJobViewProj = glm::mat4(1.0f);
    glm::vec3 m_updateJobSunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    JobCounter m_updateJobCounter;
    bool m_updateJobKicked = false; // false = the view was invalid, join has nothing to wait on

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

    // Per-traversal query counters, accumulated locally and merged into m_stats once at the end:
    // traversals run concurrently (script queries on workers, the parallel markVisible* fan-out),
    // and racing increments on the shared stats would lose counts on the hottest path.
    struct TraverseStats
    {
        int cellsTested = 0;
        int cellsFullyInside = 0;
        int entityTests = 0;
        int emitted = 0; // emit() calls = accepted entries; the markVisible* visible counts
    };

    // A subtree root the parallel markVisible* fan-out hands to a worker (see traverseParallel).
    struct FrontierCell
    {
        uint64 key;
        const CellRecord* rec; // stable: the cell maps only mutate in commitFrame
        uint32 level;
        bool fullyInside;
    };

    uint32 entryLevel(float radius); // levelForRadius clamped to the level count; tracks the oversize radius
    // Commit-only lane bookkeeping (see BlockStore): insertLane appends the entry into its cell's
    // chain from the pool copy; retireLane tombstones its lane, frees an emptied block, and compacts
    // the cell once a block's worth of lanes is dead.
    void insertLane(uint32 idx);
    void retireLane(uint32 idx);
    void compactCell(uint32 level, CellRecord& rec);
    void setOccupancyBits(uint32 level, uint64 key);
    void sweepEmptyCells();

    template <typename Tester, typename EmitFunc>
    void traverse(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask, const EmitFunc& emit) const;

    template <typename Tester, typename EmitFunc>
    void traverseCell(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                      uint64 key, uint32 level, const CellRecord& rec, bool fullyInside,
                      TraverseStats& stats, const EmitFunc& emit) const;

    // The cell-level classification split out of traverseCell so the parallel frontier expansion
    // shares it. Returns false when the cell is culled; flips fullyInside when the tester contains
    // the cell's loose bounds outright.
    template <typename Tester>
    bool testCell(const Tester& tester, const glm::vec3& cellMin, float halfCell, uint32 level,
                  bool& fullyInside, TraverseStats& stats) const;

    // The cell's own entry emission (its block chain, 8 lanes at a time) split out of traverseCell,
    // shared by the frontier expansion for the upper-level cells it consumes while splitting.
    template <typename Tester, typename EmitFunc>
    void emitCellEntries(const Tester& tester, const glm::vec3& cellMin, uint32 layerMask,
                         const CellRecord& rec, uint32 level, bool fullyInside,
                         TraverseStats& stats, const EmitFunc& emit) const;

    // Multithreaded traversal for the markVisible* stamps and the update-tier query: expands a
    // frontier of subtree roots serially (emitting the upper cells' own entries as it goes), then
    // parallelFors traverseCell over the roots. emit must be thread-safe; the stamps are (each
    // entry lives in exactly ONE cell, so no two roots ever emit the same index). A chunk-aware
    // emit (idx, pos, chunk) gets prepareChunks(n) called before any emit with chunk < n, so an
    // owner-sliced list per chunk can be sized. registerLock: see the definition. Uses the
    // m_frontier scratch: ONE traverseParallel at a time (the cull job, or the post-update
    // selection - never both in flight).
    template <typename Tester, typename EmitFunc, typename PrepareFunc>
    void traverseParallel(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                          TraverseStats& stats, const EmitFunc& emit, const PrepareFunc& prepareChunks,
                          bool registerLock);

    oc::array<CellMap, Morton::MaxLevels> m_levels;
    oc::array<BlockStore, Morton::MaxLevels> m_blocks;
    RecordPool m_pool;
    // Staged per scheduler context, drained FIFO per slot in commitFrame. Deliberately a PerWorker
    // (not owner-sliced or a shared queue): the pushes come from ANY job - the entity pass's
    // continuation batches, spawn jobs - with no chunk identity to slice by, the per-frame volume
    // is unbounded (every moved entity, several ops per entry possible) so a bounded queue cannot
    // drop, and thousands of pushes a frame on one shared atomic cursor would contend.
    PerWorker<oc::vector<PendingOp>> m_pendingOps;
    oc::vector<EmptyCandidate> m_emptyCandidates;
    struct CompactEntry { float x, y, z, radius; uint32 layer, poolIdx; };
    oc::vector<CompactEntry> m_compactScratch; // compactCell's owned-lane gather (kept: sized by the largest cell)
    uint32 m_levelEntityCount[Morton::MaxLevels] = {};
    uint32 m_numLevels = Morton::MaxLevels;
    uint32 m_frameId = 1;
    SpatialStamp m_visibleQueryId[uint32(ESpatialPass::Count)] = {}; // stamp generation per pass, 0 = never stamped (advanceStamp: wrap sweep)
    oc::atomic<float> m_topLevelMaxRadius = 0.0f; // largest clamped-oversize radius, inflates top-level tests (CAS-max: updateEntry runs on jobs)
    // Parallel spawning: exclusive over registerEntry/unregisterEntry (slot acquire/release + SoA
    // growth), shared over queries - see the threading contract above.
    mutable std::shared_mutex m_registerMutex;
    SpatialCullingConfig m_culling;
    mutable SpatialStats m_stats;
    oc::vector<FrontierCell> m_frontier;     // traverseParallel scratch (main thread only)
    oc::vector<FrontierCell> m_frontierNext;
    // setVisibleCollect: slot 0 = the serial frontier expansion, slot 1 + i = fan-out chunk i
    // (owner-sliced: a chunk appends to its own list only), merged into m_visibleCollected.
    uint32 m_visibleCollectLayers = 0;
    oc::vector<oc::vector<SpatialHandle>> m_visibleCollectChunks;
    oc::vector<SpatialHandle> m_visibleCollected;
    oc::vector<oc::vector<uint64>> m_tierHitChunks; // queryUpdateTiers' owner-sliced hits (kept: one query per selection sphere)

    // Near-ball requery hysteresis, see update.
    glm::dvec3 m_lastNearQueryPos = glm::dvec3(1e30);
    uint32     m_framesSinceNearQuery = 0;
    bool m_initialized = false;
};

export namespace Globals
{
    SpatialIndex spatialIndex;
}
