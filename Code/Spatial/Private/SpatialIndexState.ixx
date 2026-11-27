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

// A traversal's reference position: the fine lattice cell holding it and its offset inside that cell (x, y, z, 0 each). A cell's min
// relative to it is (cellMinFine - fine) * FineCellSize - frac: an exact integer subtract, an exact scale, one rounding (relMin).
struct TraversalRef
{
    __m128i fine; // not clamped to the world bound: a query volume may reach past it
    __m128 frac;
};

// Everything of SpatialIndex but its public API (SpatialIndex.ixx): the storage, the commit, the traversals, the stamps, the cull job and the hand-over
// lists. A helper reaches the API through self().
export class SpatialIndexState
{
protected:

    friend class SpatialEntry;      // unregisterEntry
    friend class SpatialStressTest; // unregisterEntry, the synthetic load

    template<typename S = SpatialIndex> S& self() { return static_cast<S&>(*this); }
    template<typename S = SpatialIndex> const S& self() const { return static_cast<const S&>(*this); }

    void unregisterEntry(SpatialHandle handle); // neutralized immediately, unindexed at commit (SpatialEntry::reset)

    // The per-frame visibility pass: commit, then the Main + Shadow stamps (one traversal), then Near. sunDirection points at the sun;
    // viewProjRelCamera is reversed-Z.
    void update(const Camera& camera, const Frustum& frustum, const glm::mat4& viewProjRelCamera, const glm::vec3& sunDirection);
    // Each mark call starts a new stamp generation for its passes. markVisibleSet stamps Main (and fills the collect lists); with a
    // shadowFrustum (which must contain mainFrustum) it stamps Shadow in the same traversal.
    void markVisibleSet(const Frustum& mainFrustum, const glm::dvec3& cameraPos, float mainMaxDist, uint32 mainLayers, IOcclusionTester* occlusion,
                        const Frustum* shadowFrustum, float shadowMaxDist, uint32 shadowLayers);
    void markVisibleSphere(ESpatialPass pass, const glm::dvec3& center, float radius, uint32 layerMask);

    struct PendingOp
    {
        enum EType : uint8 { Index, Move, Unindex };
        uint64 newKey;
        LaneCode newCode; // the lane encoding at newLevel (BlockStore)
        uint32 idx;
        uint32 gen;
        EType type;
        uint8 newLevel;
        uint8 layerMask; // Index only: the lane's SpatialLayer_* bits (the pool keeps none; a Move takes them from the old lane)
    };
    static_assert(sizeof(PendingOp) == 32);
    struct EmptyCandidate
    {
        uint64 key;
        uint32 level;
    };
    struct CompactEntry { LaneCode code; uint64 userData; uint32 poolIdx; uint8 layer; };
    // Per-traversal counters, merged into m_stats once at the end (traversals run concurrently). Counted only when SpatialStatsEnabled.
    struct TraverseStats
    {
        int cellsTested = 0;
        int cellsFullyInside = 0;
        int entityTests = 0;
        int emitted = 0;
        int emittedShadow = 0; // the second pass of a two-pass tester (Main + Shadow)
    };
    // A subtree root for the parallel fan-out, already classified (active 0 = fully inside).
    struct FrontierCell
    {
        __m128i minFine;       // the cell's min corner on the fine lattice (x, y, z, 0)
        uint64 key;
        const CellRecord* rec; // stable: the cell maps only mutate in commitFrame
        glm::vec3 cellMin;     // relative to the traversal's reference position
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

    // ---- Register / update ---- updateEntry's same-cell path has no call: the body and the key math are force-inlined, the cold
    // parts (the top level, the op push) out of line, so their register needs stay out of its prologue.

    // levelForRadius clamped to the level count; the top level (clamped, maybe oversize: tracks the radius) is out of line.
    OC_FORCEINLINE uint32 entryLevel(float radius)
    {
        const uint32 level = Morton::levelForRadius(radius);
        if (level + 1 < m_numLevels) [[likely]]
            return level;
        return entryLevelTop(radius);
    }
    uint32 entryLevelTop(float radius);
    void queueOp(const PendingOp& op);

    // pos -> its cell key at `level` (returned) and its lane position in that cell (`code` x, y, z; the radius is not touched), all
    // three axes 4-wide. The lattice offset is added in double, so floor(lattice / cell) is the fine coordinate and no integer offset
    // or constant has to stay in a register; the cell min is the fine coordinate with its low 2 * level bits cleared. pos + 2^23 m
    // rounds at ~2e-9 m: a position that close to a cell boundary may land in the neighbour cell, which the half-cell loose bounds
    // cover. lattice - cellMin is exact and the quantum scale a power of two (8 m fine cells), so the truncation is the exact floor.
    static OC_FORCEINLINE uint64 locate(const glm::dvec3& pos, uint32 level, LaneCode& code)
    {
        const __m256d p = _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_loadu_pd(&pos.x)), _mm_load_sd(&pos.z), 1); // x, y, z, 0
        const __m256d lattice = _mm256_add_pd(p, _mm256_set1_pd(double(Morton::FineOffset) * Morton::FineCellSize));
        const __m256d cell = _mm256_floor_pd(_mm256_mul_pd(lattice, _mm256_set1_pd(Morton::InvFineCellSize)));
        assert(_mm256_movemask_pd(_mm256_or_pd(_mm256_cmp_pd(cell, _mm256_setzero_pd(), _CMP_LT_OQ),
                                               _mm256_cmp_pd(cell, _mm256_set1_pd(double(Morton::FineCoordMax)), _CMP_GT_OQ))) == 0
               && "outside the +/-8389km world bound (see SPATIAL_FINEST_CELL_SIZE)");
        // max(x, 0) returns its 2nd operand for a NaN: a NaN position clamps to 0, like an out-of-range one.
        const __m128i fine = _mm256_cvttpd_epi32(_mm256_min_pd(_mm256_max_pd(cell, _mm256_setzero_pd()),
                                                               _mm256_set1_pd(double(Morton::FineCoordMax))));
        const uint64 key = Morton::keyAtLevel(Morton::encode({ uint32(_mm_cvtsi128_si32(fine)), uint32(_mm_extract_epi32(fine, 1)),
                                                               uint32(_mm_extract_epi32(fine, 2)) }), level);
        const __m128i cellMin = _mm_and_si128(fine, _mm_set1_epi32(int32(~((1u << (2 * level)) - 1u))));
        const __m256d rel = _mm256_fnmadd_pd(_mm256_cvtepi32_pd(cellMin), _mm256_set1_pd(Morton::FineCellSize), lattice);
        const __m128i quanta = _mm256_cvttpd_epi32(_mm256_mul_pd(rel, _mm256_set1_pd(g_laneScales.level[level].posScale)));
        const uint64 packed = uint64(_mm_cvtsi128_si64(_mm_packus_epi32(quanta, quanta))); // saturated to 0..65535: x, y, z, 0
        code.x = uint16(packed);
        code.y = uint16(packed >> 16);
        code.z = uint16(packed >> 32);
        return key;
    }
    // The lane radius: rounded up, saturated at LaneRadiusMax (a NaN saturates too: minss returns its 2nd operand).
    static OC_FORCEINLINE int16 quantizeRadius(float radius, uint32 level)
    {
        const __m128 scaled = _mm_min_ss(_mm_set_ss(radius * g_laneScales.level[level].radiusScale), _mm_set_ss(float(LaneRadiusMax)));
        return int16(_mm_cvtss_si32(_mm_round_ss(scaled, scaled, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC)));
    }

    // updateEntry after the handle check, at a known level: inlined into updateEntry (every level below the top) and into the
    // out-of-line updateEntryTop.
    OC_FORCEINLINE void updateEntryAt(SpatialHandle handle, const glm::dvec3& pos, float radius, uint32 level)
    {
        const uint32 idx = handle.idx;
        LaneCode code;
        const uint64 key = locate(pos, level, code);
        code.radius = quantizeRadius(radius, level);
        const uint8 flags = m_pool.flags[idx];
        if (!(flags & (RecordFlag_PendingMove | RecordFlag_Unindexed)) && key == m_pool.cellKey[idx] && level == m_pool.level[idx])
        {
            // Same cell, indexed: the lane is the only copy. Its four rows share the block's first cache line.
            const uint32 slot = m_pool.storeIdx[idx];
            CellBlock& block = m_blocks[level].blocks[BlockStore::blockOf(slot)];
            const uint32 lane = BlockStore::laneOf(slot);
            if (code.x == block.posX[lane] && code.y == block.posY[lane] && code.z == block.posZ[lane] && code.radius == block.radius[lane])
                return; // untouched (no move past a quantum): no store into a line the queries read
            block.posX[lane] = code.x;
            block.posY[lane] = code.y;
            block.posZ[lane] = code.z;
            block.radius[lane] = code.radius;
            return;
        }
        // A cell change, or no lane yet (Unindexed): a Move op, applied at commit.
        m_pool.flags[idx] = uint8(flags | RecordFlag_PendingMove);
        queueOp({ .newKey = key, .newCode = code, .idx = idx, .gen = handle.gen, .type = PendingOp::Move, .newLevel = uint8(level) });
    }
    void updateEntryTop(SpatialHandle handle, const glm::dvec3& pos, float radius);

    // ---- Commit bookkeeping (see BlockStore) ----

    void indexEntry(const PendingOp& op); // an Index op: the lane and the spawn-guard stamps
    void insertLane(uint32 idx, const LaneCode& code, uint32 layerMask);
    uint32 retireLane(uint32 idx);        // returns the lane's layer mask (a Move re-inserts it)
    void compactCell(uint32 level, CellRecord& rec);
    void setOccupancyBits(uint32 level, uint64 key);
    void sweepEmptyCells();

    // ---- Traversal ---- `active`: the tester's still-undecided bounds; 0 = the cell is fully inside.

    // emit returns true to go on, false to stop the traversal.
    void forEachInSphereImpl(const glm::dvec3& center, float radius, uint32 layerMask, uint32 skipStampedIn, void* ctx, bool (*emit)(void*, uint64)) const;
    void forEachInFrustumImpl(const Frustum& frustumRelCamera, const glm::dvec3& cameraPos, float maxDist, uint32 layerMask,
                              IOcclusionTester* occlusion, void* ctx, bool (*emit)(void*, uint64)) const;

    // EARLY STOP: an emit that returns bool stops the traversal with false; a void emit never does (its checks fold away). The
    // traversal functions below return false once stopped. traverseParallel ignores it: its emits (the stamps) never stop.
    template <typename Tester, typename EmitFunc>
    void traverse(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask, const EmitFunc& emit) const;

    // A cell that passed its test: emits its entries, then recurses into the children that pass. minFine: the cell's min corner on the
    // fine lattice; cellMin: the same corner relative to the reference.
    template <typename Tester, typename EmitFunc>
    bool traverseCell(const Tester& tester, const TraversalRef& ref, uint32 layerMask,
                      uint64 key, uint32 level, const CellRecord& rec, const __m128i& minFine, const glm::vec3& cellMin, uint32 active,
                      TraverseStats& stats, const EmitFunc& emit) const;

    // Returns the cell's new active mask, or UINT32_MAX when it is culled.
    template <typename Tester>
    uint32 testCell(const Tester& tester, const glm::vec3& cellMin, float halfCell, uint32 level,
                    uint32 active, TraverseStats& stats) const;

    // onChild(childKey, childRecord, childMinFine, childMin, childActive) for each child that passes; a bool onChild stops it with false.
    template <typename Tester, typename ChildFunc>
    bool forEachChild(const Tester& tester, const TraversalRef& ref, uint64 key, uint32 level,
                      const CellRecord& rec, const __m128i& minFine, const glm::vec3& cellMin, uint32 active,
                      TraverseStats& stats, const ChildFunc& onChild) const;

    // emit(block, lane, decode, passes) per hit of the cell, 8 lanes at a time: the emit reads what it needs from the lane (userData,
    // poolIdx, layer; `decode` turns the lane's position and radius into metres relative to the reference, for the emits that need a
    // distance). passes: bit 0 = the tester's first pass, bit 1 = its second (a two-pass tester; a one-pass one always passes 1).
    template <typename Tester, typename EmitFunc>
    bool emitCellEntries(const Tester& tester, const glm::vec3& cellMin, uint32 layerMask,
                         const CellRecord& rec, uint32 level, uint32 active,
                         TraverseStats& stats, const EmitFunc& emit) const;

    // Serial frontier expansion, then a parallelFor over the roots. emit must be thread-safe; a chunk-aware emit (one more uint32 chunk
    // argument) gets prepareChunks(n) first. Uses m_frontier: one call at a time.
    template <typename Tester, typename EmitFunc, typename PrepareFunc>
    void traverseParallel(const Tester& tester, const glm::dvec3& refPos, uint32 layerMask,
                          TraverseStats& stats, const EmitFunc& emit, const PrepareFunc& prepareChunks);

    // markVisibleSet's traversal: the Main (+ Shadow) stamps, and with `collect` the hand-over lists of the Main hits.
    template <typename Tester>
    void stampFrustum(const Tester& tester, const glm::dvec3& cameraPos, uint32 layerMask, bool collect, TraverseStats& stats);

    // ---- Storage ----
    //
    // A LEVEL is one grid of cubic cells. Level 0 cells are SPATIAL_FINEST_CELL_SIZE (8 m); each level up is 4x larger per axis, so one cell
    // holds 4x4x4 = 64 cells of the level below (11 levels, a top cell is ~8389 km). One axis of one level-2 cell:
    //
    //   level 2  |                             128 m                             |
    //   level 1  |     32 m      |     32 m      |     32 m      |     32 m      |
    //   level 0  |8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|8 m|
    //
    // An entry lives at exactly ONE level: the smallest whose cell fits its sphere (cellSize >= 2 * radius), in the cell holding its centre, so
    // it reaches at most half a cell out of it. A unit (radius 0.5 m) is level 0, a 20 m house level 1, a 100 m crag level 2.
    // A Morton key names a cell: the parent's key is key >> 6, and the low 6 bits pick which of its 64 children it is.
    //
    // Per level, a CellMap finds a cell and a BlockStore holds its entries:
    //
    //   m_levels[L]  CellMap: Morton key -> CellRecord { childMask, head, count }   (the key sits next to its record: one line per hit)
    //                                                               |
    //                              +--------------------------------+
    //                              v
    //   m_blocks[L]  BlockStore:  [CellBlock] --next--> [CellBlock] --next--> UINT32_MAX   (the cell's CHAIN, newest block first)
    //
    // childMask: one bit per occupied child cell one level down, so a descent visits only those. count: the entries with a lane in the chain.
    //
    // A CellBlock (192 B, three cache lines) is 8 LANES in SIMD layout: lane i is index i of every row, and one lane is one entry.
    //
    //                   lane:  0   1   2   3   4   5   6   7
    //  line 0  posX[8]       | x | x | x | x | x | x | x | x |  -+
    //          posY[8]       | y | y | y | y | y | y | y | y |   |  test8: one 16-byte load per row, 8 entries at once; fixed point (the
    //          posZ[8]       | z | z | z | z | z | z | z | z |   |  lane encoding, BlockStore): uint16 quanta of cellSize / 65536 from the
    //          radius[8]     | r | r | r | - | r | r | - | - |  -+  cell's min corner, the radius int16. - = tombstone (negative)
    //  line 1  poolIdx[8]    | the entry's m_pool slot  -----+---->  m_pool row: gen, flags, stamps, userData, cellKey, level, storeIdx
    //          layer[8]      | SpatialLayer_* bits per lane, one byte each                                                       |
    //          next, count, live, chainBlocks (head block only)                                                                  |
    //  line 2  userData[8]   | a copy of the pool row's userData (it never changes): what a query hands out                      |
    //                                                                                                                            |
    //   storeIdx = block * 8 + lane: the way back from the pool row to the lane (UINT32_MAX while Unindexed) <-------------------+
    //
    // A SpatialEntry (SpatialTypes.ixx) is how an OWNER - an Entity, a terrain chunk, an ocean sector - holds its entry:
    //
    //   owner --holds--> SpatialEntry (RAII, move-only)
    //                      m_handle: SpatialHandle { idx, gen } --idx--> m_pool row --userData--> the owner again (Entity*, Resident*, ...)
    //
    //   * registerEntry returns the handle; the entry is Unindexed until the next commit.
    //   * gen: the row's gen goes up when its slot is freed, so a handle to a freed and reused slot finds nothing (isValidAlive).
    //   * ~SpatialEntry / reset() unregisters: the lane (if any) is tombstoned at once, and the Unindex op frees the slot at commit.
    //
    // The two ways through the storage:
    //
    //   owner moves:  owner -> SpatialEntry -> handle.idx -> m_pool row -> storeIdx -> lane      updateEntry writes the lane
    //   a query:      cell -> chain -> CellBlock -> lane -> userData -> owner                    the query hands out the lane's userData
    //   a stamp:      cell -> chain -> CellBlock -> lane -> poolIdx -> m_pool row (stamps)       the visibility passes
    //
    // So a LANE is one entry, a CellBlock is 8 entries in a SIMD layout, a CHAIN is all the entries of one cell, a BlockStore is all the chains
    // of one level, and the CellMap finds a cell's chain by its key. m_pool holds what a handle needs (and the stamps), and a SpatialEntry is
    // the owner's handle to its m_pool row.

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
    oc::vector<PendingOp> m_deferredMoves; // commit scratch: Moves that reached their entry before its Index op
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
