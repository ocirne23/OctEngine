module;

#include <immintrin.h>

export module Spatial:SpatialIndexState;

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

export class SpatialIndex; // declared before their friend lines / self()
export class SpatialEntry;
export class SpatialStressTest;

// Everything of SpatialIndex but its public API (SpatialIndex.ixx): the storage, the commit, the traversals, the stamps, the cull job and the hand-over
// lists. A helper reaches the API through self().
export class SpatialIndexState
{
protected:

    friend class SpatialEntry;      // unregisterEntry
    friend class SpatialStressTest; // unregisterEntry, the synthetic load

    template<typename S = SpatialIndex> S& self() { return static_cast<S&>(*this); }
    template<typename S = SpatialIndex> const S& self() const { return static_cast<const S&>(*this); }

    void unregisterEntry(SpatialHandle handle); // neutralized immediately, unlinked at commit (SpatialEntry::reset)

    // The per-frame visibility pass: commit, then the Main, Near and Shadow stamps. sunDirection points at the sun; viewProjRelCamera is reversed-Z.
    void update(const Camera& camera, const Frustum& frustum, const glm::mat4& viewProjRelCamera, const glm::vec3& sunDirection);
    // Each mark call starts a new stamp generation for that pass.
    void markVisibleSet(ESpatialPass pass, const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist,
                        uint32 layerMask, IOcclusionTester* occlusion = nullptr);
    void markVisibleSphere(ESpatialPass pass, const glm::dvec3& center, float radius, uint32 layerMask);

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

    // ---- Traversal ---- `active`: the tester's still-undecided bounds; 0 = the cell is fully inside.

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

    // Serial frontier expansion, then a parallelFor over the roots. emit must be thread-safe; a chunk-aware emit gets prepareChunks(n) first. Uses
    // m_frontier: one call at a time.
    template <typename Tester, typename EmitFunc, typename PrepareFunc>
    void traverseParallel(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                          TraverseStats& stats, const EmitFunc& emit, const PrepareFunc& prepareChunks);

    // ---- Storage ----

    oc::array<CellMap, Morton::MaxLevels> m_levels;
    oc::array<BlockStore, Morton::MaxLevels> m_blocks;
    RecordPool m_pool;
    uint32 m_levelEntityCount[Morton::MaxLevels] = {};
    uint32 m_numLevels = Morton::MaxLevels;
    oc::atomic<float> m_topLevelMaxRadius = 0.0f; // largest clamped-oversize radius (CAS-max)
    bool m_initialized = false;

    // ---- Commit ----

    // PerWorker on purpose: pushed from any job with no chunk identity, unbounded volume.
    PerWorker<oc::vector<PendingOp>> m_pendingOps;
    oc::vector<EmptyCandidate> m_emptyCandidates;
    oc::vector<CompactEntry> m_compactScratch;

    // ---- Stamps ----

    uint32 m_frameId = 1;
    alignas(16) SpatialStamp m_visibleQueryId[uint32(ESpatialPass::Count)] = {}; // generation per pass, 0 = never; one __m128i

    // ---- Traversal scratch (main thread only) ----

    oc::vector<FrontierCell> m_frontier;
    oc::vector<FrontierCell> m_frontierNext;
    oc::vector<oc::vector<uint64>> m_tierHitChunks;

    // ---- Visible set hand-over ---- chunk list 0 = the serial expansion, 1 + i = fan-out chunk i; merged per slot.

    static constexpr uint32 NumVisibleCollectSlots = 2; // public as SpatialIndex::NumVisibleCollectSlots
    enum class ECollect : uint8 { Handles, UserData };  // public as SpatialIndex::ECollect
    uint32 m_visibleCollectLayers[NumVisibleCollectSlots] = {};
    ECollect m_visibleCollectWhat[NumVisibleCollectSlots] = {};
    oc::vector<oc::vector<SpatialHandle>> m_visibleCollectChunks[NumVisibleCollectSlots];
    oc::vector<SpatialHandle> m_visibleCollected[NumVisibleCollectSlots];
    oc::vector<oc::vector<uint64>> m_visibleCollectUserDataChunks[NumVisibleCollectSlots];
    oc::vector<uint64> m_visibleCollectedUserData[NumVisibleCollectSlots];
    uint32 m_visibleCollectGeneration = 0;

    // ---- The update job ---- its copy of the view (the job outlives the caller's stack).

    Camera m_updateJobCamera;
    Frustum m_updateJobFrustum;
    glm::mat4 m_updateJobViewProj = glm::mat4(1.0f);
    glm::vec3 m_updateJobSunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
    JobCounter m_updateJobCounter;
    bool m_updateJobKicked = false; // false = the view was invalid, join has nothing to wait on
    // Near-ball requery hysteresis.
    glm::dvec3 m_lastNearQueryPos = glm::dvec3(1e30);
    uint32     m_framesSinceNearQuery = 0;

    // ---- Settings ----

    SpatialCullingConfig& m_culling = Globals::settings.spatial.culling;
    SpatialStats& m_stats = Globals::settings.spatial.stats;
};
