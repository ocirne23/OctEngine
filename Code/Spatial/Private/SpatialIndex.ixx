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
import :StaticStore;

// Spatial index independent of the entity parent/child hierarchy: an implicit 64-ary hierarchy
// over per-level hashed grids. Entries live at exactly one level (the one matching their bounding
// radius); every cell knows which of its 4x4x4 children are occupied, so queries descend the
// hierarchy with bit scans and never visit empty space. Mutations that change a cell are queued
// and applied in commitFrame(); same-cell position updates write in place. Queries are read-only
// and valid between commits.
//
// Threading contract: updateEntry is callable from any job during the parallel entity pass -
// same-cell updates write only that entry's SoA slots, and cell-changing ops stage into per-worker
// pending lists. registerEntry/unregisterEntry/setLayerMask/commitFrame stay single-threaded
// (main, outside the pass), as do queries relative to commits.
export class SpatialIndex final
{
public:

    void initialize(const SpatialIndexDesc& desc = {});

    // spawnVisible: whether the entry counts as visible in every pass until its first real stamp.
    // Entities want true (a fresh spawn must not flash invisible during its link+stamp latency);
    // streamed geometry (terrain chunks) passes false — appearing one frame late is invisible for
    // something that didn't exist before, while the guard would leak never-stamped off-screen entries
    // into the main pass (permanently while the culling is frozen).
    SpatialHandle registerEntry(const glm::dvec3& pos, float radius, uint64 userData, uint32 layerMask = 1, bool spawnVisible = true);
    void unregisterEntry(SpatialHandle handle); // neutralized immediately, unlinked at commit
    void updateEntry(SpatialHandle handle, const glm::dvec3& pos, float radius);
    void setLayerMask(SpatialHandle handle, uint32 layerMask);
    void commitFrame();

    uint32 querySphere(const glm::dvec3& center, float radius, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    uint32 queryAABB(const glm::dvec3& boxMin, const glm::dvec3& boxMax, uint32 layerMask, oc::vector<uint64>& outUserData) const;
    uint32 queryFrustum(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                        oc::vector<uint64>& outUserData, IOcclusionTester* occlusion = nullptr) const;
    uint32 queryRay(const glm::dvec3& origin, const glm::dvec3& dir, double maxDist, uint32 layerMask,
                    oc::vector<uint64>& outUserData) const; // broadphase: entries whose bounds cross the segment
    uint64 queryNearest(const glm::dvec3& pos, float maxRadius, uint32 layerMask, uint64 excludeUserData = 0) const;

    // Per-pass visibility stamps consumed by the render gate; each call invalidates that pass's
    // previous stamp generation (single consumer per pass by design). Main is the camera frustum
    // (occlusion-testable); Near is the camera ball that keeps off-screen shadow casters and
    // ray-traced geometry alive.
    void markVisibleSet(ESpatialPass pass, const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist,
                        uint32 layerMask, IOcclusionTester* occlusion = nullptr);
    void markVisibleSphere(ESpatialPass pass, const glm::dvec3& center, float radius, uint32 layerMask);

    // THE per-frame visibility pass, in one call (the App's only spatial step): commits the cell moves
    // queued during last frame's entity updates, tracks the render camera's far plane, then stamps the
    // Main frustum set - rasterizing the CPU occlusion buffer first when it is enabled - and the Near
    // ball. The Near ball is requeried only once the camera has moved past its slack, so its hysteresis
    // state belongs here rather than at the call site.
    // viewProjRelCamera maps camera-relative world positions to clip space in the renderer's REVERSED-Z
    // convention; it is flipped back to standard z here, for the occlusion rasterizer only.
    void update(const Camera& camera, const Frustum& frustum, const glm::mat4& viewProjRelCamera);

    // Main pass; a never-stamped entry counts as visible unless it registered with spawnVisible = false
    // (see registerEntry).
    bool isVisible(SpatialHandle handle) const
    {
        if (!m_pool.isValidAlive(handle))
            return false;
        const uint32 stamp = m_pool.lastVisible[uint32(ESpatialPass::Main)][handle.idx];
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
            const uint32 stamp = m_pool.lastVisible[p][handle.idx];
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

    void linkIntoCell(uint32 idx);
    void unlinkFromCell(uint32 idx);
    void setOccupancyBits(uint32 level, uint64 key);
    void demoteOrUnlink(uint32 idx); // pulls a StaticTier entry back to dynamic, else plain unlink
    void sweepEmptyCells();
    void promotionScan();
    void rebuildStaticLevel(uint32 level);

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

    // The cell's own entry emission (dynamic list + static ranges) split out of traverseCell,
    // shared by the frontier expansion for the upper-level cells it consumes while splitting.
    template <typename Tester, typename EmitFunc>
    void emitCellEntries(const Tester& tester, const glm::vec3& cellMin, uint32 layerMask,
                         const CellRecord& rec, uint32 level, bool fullyInside,
                         TraverseStats& stats, const EmitFunc& emit) const;

    // Multithreaded traversal for the markVisible* stamps: expands a frontier of subtree roots
    // serially (emitting the upper cells' own entries as it goes), then parallelFors traverseCell
    // over the roots. emit must be thread-safe; the stamps are (each entry lives in exactly ONE
    // cell, so no two roots ever emit the same index).
    template <typename Tester, typename EmitFunc>
    void traverseParallel(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                          TraverseStats& stats, const EmitFunc& emit);

    oc::array<CellMap, Morton::MaxLevels> m_levels;
    oc::array<StaticStore, Morton::MaxLevels> m_static;
    RecordPool m_pool;
    PerWorker<oc::vector<PendingOp>> m_pendingOps; // staged per scheduler context, drained FIFO per slot in commitFrame
    oc::vector<EmptyCandidate> m_emptyCandidates;
    uint32 m_levelEntityCount[Morton::MaxLevels] = {};
    uint32 m_numLevels = Morton::MaxLevels;
    uint32 m_frameId = 1;
    uint32 m_visibleQueryId[uint32(ESpatialPass::Count)] = {}; // stamp generation per pass, 0 = never stamped
    uint32 m_promoteCursor = 0;  // round-robin pool scan position for static promotion
    bool m_staticEnabled = true;
    int m_promoteAfterFrames = 60;
    int m_staticScanBudget = 65536;  // pool slots inspected per commit
    int m_staticRebuildBatch = 1024; // pending promotions that force a level rebuild
    oc::atomic<float> m_topLevelMaxRadius = 0.0f; // largest clamped-oversize radius, inflates top-level tests (CAS-max: updateEntry runs on jobs)
    SpatialCullingConfig m_culling;
    mutable SpatialStats m_stats;
    oc::vector<FrontierCell> m_frontier;     // traverseParallel scratch (main thread only)
    oc::vector<FrontierCell> m_frontierNext;

    // Near-ball requery hysteresis, see update.
    glm::dvec3 m_lastNearQueryPos = glm::dvec3(1e30);
    uint32     m_framesSinceNearQuery = 0;
    bool m_initialized = false;
};

export namespace Globals
{
    SpatialIndex spatialIndex;
}
