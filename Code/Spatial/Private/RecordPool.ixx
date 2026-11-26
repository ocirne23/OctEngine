export module Spatial:RecordPool;

import Core;
import Core.VirtualArray;
import :Types;

constexpr uint32 NumSpatialPasses = uint32(ESpatialPass::Count);

// One entry's stamps, together: a pass mask is one 16-byte load.
struct alignas(16) PassStamps
{
    SpatialStamp pass[NumSpatialPasses];
};
static_assert(sizeof(PassStamps) == 16, "getPassMask loads the stamps as one __m128i");

enum ERecordFlag : uint8
{
    RecordFlag_Alive        = 1,
    RecordFlag_Unindexed    = 2,  // registered but not yet in its cell: no lane, no query finds it (pre-commit)
    RecordFlag_PendingFree  = 4,  // unregistered, unindex + slot release happen at commit
    RecordFlag_PendingMove  = 8,  // a queued Move op holds newer data than the lane (or indexes the Unindexed entry)
    RecordFlag_NoSpawnGuard = 64, // registerEntry(spawnVisible = false)
};

// SoA storage for every registered entry, by handle slot: what a handle needs and a query test does not. Position, radius and
// layer mask live only in the entry's CellBlock lane (before it is indexed, in its pending Index / Move op). Rows are VirtualArrays
// (reserved up front, never move; read through the packed base pointers below), so acquire is lock-free: a tagged free stack, else
// a bump of the high-water mark. release() at commit only.
class RecordPool final
{
public:
    static constexpr uint32 MAX_ENTRIES = 1u << 24; // address space only (~740 MB over all rows); commit follows use

    void initialize(uint32 capacity)
    {
        forEachRow([](auto& row) { row.reserve(MAX_ENTRIES, "Spatial record pool", EProfileCategory::Spatial); });
        m_next.reserve(MAX_ENTRIES, "Spatial record pool", EProfileCategory::Spatial);
        cellKey = m_cellKeyRow.data();
        userData = m_userDataRow.data();
        stamps = m_stampsRow.data();
        gen = m_genRow.data();
        storeIdx = m_storeIdxRow.data();
        level = m_levelRow.data();
        flags = m_flagsRow.data();
        commitRows(oc::max(capacity, 64u));
    }

    // Any thread, lock-free.
    uint32 acquire()
    {
        m_numAlive.fetch_add(1, oc::memory_order_relaxed);
        uint64 head = m_freeHead.load(oc::memory_order_acquire);
        while ((uint32)head != EMPTY)
        {
            // A stale link (the slot was popped and pushed again meanwhile) fails the CAS: the tag moved.
            const uint32 idx = (uint32)head;
            const uint64 popped = bumpTag(head) | oc::atomic_ref<uint32>(m_next[idx]).load(oc::memory_order_relaxed);
            if (m_freeHead.compare_exchange_weak(head, popped, oc::memory_order_acquire, oc::memory_order_acquire))
                return idx;
        }
        // Commit before the bump: everything below capacity() stays readable.
        uint32 idx = m_count.load(oc::memory_order_relaxed);
        do
        {
            assert(idx < MAX_ENTRIES && "spatial record pool exhausted");
            if (idx >= m_committed.load(oc::memory_order_acquire))
                commitRows(idx + 1);
        } while (!m_count.compare_exchange_weak(idx, idx + 1, oc::memory_order_release, oc::memory_order_relaxed));
        return idx;
    }

    // commitFrame only.
    void release(uint32 idx)
    {
        ++gen[idx];
        flags[idx] = 0;
        uint64 head = m_freeHead.load(oc::memory_order_relaxed);
        uint64 pushed;
        do
        {
            oc::atomic_ref<uint32>(m_next[idx]).store((uint32)head, oc::memory_order_relaxed);
            pushed = bumpTag(head) | idx;
        } while (!m_freeHead.compare_exchange_weak(head, pushed, oc::memory_order_release, oc::memory_order_relaxed));
        m_numAlive.fetch_sub(1, oc::memory_order_relaxed);
    }

    bool isValidAlive(SpatialHandle handle) const
    {
        return handle.idx < capacity() && gen[handle.idx] == handle.gen && (flags[handle.idx] & RecordFlag_Alive);
    }

    // The high-water mark: every slot ever handed out (live or free) lies below it.
    uint32 capacity() const { return m_count.load(oc::memory_order_acquire); }
    uint32 numAlive() const { return m_numAlive.load(oc::memory_order_relaxed); }

    // The row base pointers, together in ONE cache line: a VirtualArray is ~640 B (most of it its commit-step table), so through the
    // VirtualArrays themselves every row of an updateEntry / getPassMask was a different line. Set by initialize; a row never moves.
    alignas(64) uint64* cellKey = nullptr; // current cell Morton key at `level`
    uint64* userData = nullptr;
    PassStamps* stamps = nullptr;          // stamp generation per pass (see SpatialStamp)
    uint32* gen = nullptr;                 // handle generation: 32-bit so a stale handle can never match a reused slot
    uint32* storeIdx = nullptr;            // indexed: block * 8 + lane in the level's BlockStore; UINT32_MAX while Unindexed
    uint8* level = nullptr;
    uint8* flags = nullptr;

private:
    static constexpr uint32 EMPTY = UINT32_MAX;
    static uint64 bumpTag(uint64 head) { return ((head >> 32) + 1) << 32; }

    template<typename Fn>
    void forEachRow(Fn&& fn)
    {
        fn(m_cellKeyRow); fn(m_userDataRow); fn(m_stampsRow); fn(m_genRow); fn(m_storeIdxRow); fn(m_levelRow); fn(m_flagsRow);
    }
    // Doubles the committed prefix past `needed`; m_committed is published after every row covers it.
    void commitRows(uint32 needed)
    {
        const std::lock_guard lock(m_commitMutex);
        uint32 target = oc::max(m_committed.load(oc::memory_order_relaxed), 64u);
        while (target < needed)
            target *= 2;
        target = oc::min(target, MAX_ENTRIES);
        if (target <= m_committed.load(oc::memory_order_relaxed))
            return;
        forEachRow([&](auto& row) { row.commit(target); });
        m_next.commit(target);
        m_committed.store(target, oc::memory_order_release);
    }

    VirtualArray<uint64> m_cellKeyRow, m_userDataRow;
    VirtualArray<PassStamps> m_stampsRow;
    VirtualArray<uint32> m_genRow, m_storeIdxRow;
    VirtualArray<uint8> m_levelRow, m_flagsRow;
    VirtualArray<uint32> m_next;           // free-stack links
    oc::atomic<uint64> m_freeHead{ EMPTY };
    oc::atomic<uint32> m_count{ 0 };       // the bump cursor (high-water mark)
    oc::atomic<uint32> m_committed{ 0 };   // every row is committed below this
    oc::atomic<uint32> m_numAlive{ 0 };
    std::mutex m_commitMutex;
};
