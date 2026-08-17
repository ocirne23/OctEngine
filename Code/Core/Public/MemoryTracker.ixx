export module Core.MemoryTracker;

import Core;
import Core.Allocator;

// One node per unique profile-scope PATH that has allocated: the attribution tree the Memory panel
// renders as nested boxes. Nodes are created on first use and never destroyed (pointers are stable;
// readers traverse the child lists lock-free). selfBytes is the LIVE memory allocated at exactly
// this path - inclusive sums are computed by the reader.
export struct MemScopeNode
{
    const char* name = nullptr;                 // scope name literal ("<unscoped>" / "<other threads>" at the root)
    MemScopeNode* parent = nullptr;
    oc::atomic<MemScopeNode*> firstChild = nullptr;
    oc::atomic<MemScopeNode*> nextSibling = nullptr;
    oc::atomic<int64> selfBytes = 0;           // live bytes allocated at exactly this path
    oc::atomic<int64> selfCount = 0;           // live allocations at exactly this path
    oc::atomic<uint64> totalAllocBytes = 0;    // cumulative since tracking started (churn)
    oc::atomic<uint64> totalAllocCount = 0;
    uint8 category = 0;                         // EProfileCategory of the innermost scope
};

// Attributes every engine allocation to the profile-scope stack open on the allocating thread
// (Core's allocator hooks -> ProfileTrack's open-name stack -> a path in the node tree), and
// untracks it on free via a sharded pointer map. Everything is preallocated at initialize (the
// hooks themselves never allocate, so they can run inside the allocator); the pools deliberately
// leak at exit - the destructor only uninstalls the hooks, because other globals' destructors
// still free tracked memory afterwards.
export class MemoryTracker final
{
public:
    static constexpr uint32 MAX_NODES = 4096;
    static constexpr uint32 NUM_SHARDS = 64;         // pointer-map shards (lock striping)
    static constexpr uint32 SHARD_CAPACITY = 1 << 13; // entries per shard, power of two (512K total, 24B each)

    // Constructed at STATIC INIT (init_seg below: after the allocator and the profiler): allocates
    // the pools, then installs the Core allocator hooks - so allocations inside later globals'
    // static initializers are already tracked (attributed via the profiler's "Main" track).
    MemoryTracker();
    ~MemoryTracker();  // uninstalls the hooks; pools stay (see above)

    // New allocations are only attributed while enabled; frees of already-tracked blocks always
    // untrack regardless, so toggling can never corrupt the counts.
    void setEnabled(bool enabled) { m_enabled.store(enabled, oc::memory_order_relaxed); }
    bool isEnabled() const { return m_enabled.load(oc::memory_order_relaxed); }

    // ---- reading (any thread; the panel uses main) ----
    const MemScopeNode* getRoot() const { return m_root; }
    uint64 getTrackedBytes() const { return (uint64)m_trackedBytes.load(oc::memory_order_relaxed); }
    uint32 getTrackedCount() const { return (uint32)m_trackedCount.load(oc::memory_order_relaxed); }
    uint32 getNumNodes() const { return m_numNodes.load(oc::memory_order_relaxed); }
    // Allocations dropped because a map shard or the node pool was full - nonzero means the
    // treemap under-reports by whatever those allocations held.
    uint64 getDroppedAllocs() const { return m_droppedAllocs.load(oc::memory_order_relaxed); }

    void onAlloc(void* ptr, uint64 size); // hook bodies (public for the free-function thunks)
    void onFree(void* ptr);

private:

    void initialize();

    struct MapEntry
    {
        void* ptr = nullptr; // null = empty slot
        uint64 size = 0;
        MemScopeNode* node = nullptr;
    };
    struct alignas(64) MapShard
    {
        oc::atomic<uint32> lock = 0;
        uint32 count = 0;
        MapEntry* entries = nullptr; // SHARD_CAPACITY, allocated at initialize, never freed
    };

    MemScopeNode* resolveCurrentNode();
    MemScopeNode* findOrAddChild(MemScopeNode* parent, const char* name, uint8 category);
    bool mapInsert(void* ptr, uint64 size, MemScopeNode* node);
    bool mapErase(void* ptr, MapEntry& out);

    MemScopeNode* m_nodePool = nullptr; // MAX_NODES, bump-allocated under m_nodeLock, never freed
    MemScopeNode* m_root = nullptr;
    oc::atomic<uint32> m_numNodes = 0;
    oc::atomic<uint32> m_nodeLock = 0;

    MapShard m_shards[NUM_SHARDS];

    oc::atomic<int64> m_trackedBytes = 0;
    oc::atomic<int64> m_trackedCount = 0;
    oc::atomic<uint64> m_droppedAllocs = 0;
    oc::atomic<bool> m_enabled = true;
    bool m_initialized = false;
};

// After the allocator (.CRT$XCA) and the profiler (XCA2); before every ordinary global, whose
// static-init allocations then flow through the installed hooks. Constructed-early also means
// destructed-LAST, so the hooks stay valid through the other globals' teardown frees.
OC_INIT_SEG(OC_SEG_CORE_MEMORY_TRACKER)
export namespace Globals
{
    MemoryTracker memoryTracker;
}
