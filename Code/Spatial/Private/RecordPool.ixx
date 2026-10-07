export module Spatial:RecordPool;

import Core;
import Core.VirtualArray;
import :Types;

constexpr uint32 NumSpatialPasses = uint32(ESpatialPass::Count);

// One entry's stamp generation per pass, together: a pass mask is one 16-byte load (one cache line)
// instead of a read per pass array, and SpatialIndex::getPassMask compares all passes at once.
struct alignas(16) PassStamps
{
    SpatialStamp pass[NumSpatialPasses];
};
static_assert(sizeof(PassStamps) == 16, "getPassMask loads the stamps as one __m128i");

// Module-internal SoA storage for every registered entry. Positions are stored relative to the owning
// cell's min corner (bounded by the cell size, so float keeps full precision at planet-scale
// coordinates). The pool copy is the authoritative one (getPosition, the same-cell compare in
// updateEntry, the Link op); the cell's CellBlock lane carries the copy queries read. Nothing here is
// exported.
//
// LOCK-FREE ACQUIRE (parallel entity spawning): every row is a VirtualArray - reserved for MAX_ENTRIES up
// front, committed as the pool grows - so a growth never moves a row under a concurrent register, unregister
// or query, and a row access stays a plain base + index. A slot comes from a tagged free stack (the head's
// high 32 bits bump on every push and pop: ABA) or, when that is empty, a bump of the high-water mark.
// release() runs at commit only.

enum ERecordFlag : uint8
{
    RecordFlag_Alive       = 1,
    RecordFlag_Unlinked    = 2, // registered but not yet linked into its cell (pre-commit)
    RecordFlag_PendingFree   = 4,  // unregistered, unlink + slot release happen at commit
    RecordFlag_PendingMove   = 8,  // a queued Move op holds newer data than the in-place SoA fields
    RecordFlag_NoSpawnGuard  = 64, // registerEntry(spawnVisible = false): the entry is NEVER treated as
                                   // visible before its first real stamp - streamed terrain wants this
                                   // (chunks materialize off-screen constantly; the guard pinned them in
                                   // the main pass until first entering the frustum, forever with Freeze)
};

class RecordPool final
{
public:
    static constexpr uint32 MAX_ENTRIES = 1u << 24; // address space only (~1 GB over all rows); commit follows use

    void initialize(uint32 capacity)
    {
        forEachRow([](auto& row) { row.reserve(MAX_ENTRIES, "Spatial record pool", EProfileCategory::Spatial); });
        m_next.reserve(MAX_ENTRIES, "Spatial record pool", EProfileCategory::Spatial);
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
        // Commit BEFORE the bump: the high-water mark never passes the committed rows, so everything below
        // capacity() is readable (the stamp sweep walks it while spawns register).
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

    VirtualArray<float> posX, posY, posZ; // relative to the owning cell's min corner
    VirtualArray<float> radius;           // negative = neutralized (pending free), fails every test
    VirtualArray<uint64> cellKey;         // current cell Morton key at `level`
    VirtualArray<uint64> userData;
    VirtualArray<uint8> layerMask;        // SpatialLayer_* bits (4 in use; static_assert in Types)
    VirtualArray<uint32> gen;             // handle generation: 32-bit so a stale handle can never match a reused slot
    VirtualArray<PassStamps> stamps;      // stamp generation per pass (see SpatialStamp)
    VirtualArray<uint32> storeIdx;        // linked: block * 8 + lane in the level's BlockStore; UINT32_MAX while Unlinked
    VirtualArray<uint8> level;
    VirtualArray<uint8> flags;

private:
    static constexpr uint32 EMPTY = UINT32_MAX;
    static uint64 bumpTag(uint64 head) { return ((head >> 32) + 1) << 32; }

    template<typename Fn>
    void forEachRow(Fn&& fn)
    {
        fn(posX); fn(posY); fn(posZ); fn(radius); fn(cellKey); fn(userData); fn(layerMask);
        fn(gen); fn(stamps); fn(storeIdx); fn(level); fn(flags);
    }
    // Doubles the committed prefix past `needed` (each row commits under its own mutex; m_committed is
    // published after every row covers it).
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

    VirtualArray<uint32> m_next;           // free-stack links
    oc::atomic<uint64> m_freeHead{ EMPTY };
    oc::atomic<uint32> m_count{ 0 };       // the bump cursor (high-water mark)
    oc::atomic<uint32> m_committed{ 0 };   // every row is committed below this
    oc::atomic<uint32> m_numAlive{ 0 };
    std::mutex m_commitMutex;
};
