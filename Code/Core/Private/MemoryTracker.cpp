module;

#include <intrin.h> // _mm_pause for the shard/node spinlocks

module Core.MemoryTracker;

import Core;
import Core.Allocator;

// Reentrancy guard: the hooks never allocate through the engine paths themselves (fixed pools),
// but this keeps any future slip from recursing into the allocator.
static thread_local bool t_inMemoryHook = false;

namespace
{
    uint64 hashPtr(const void* ptr)
    {
        uint64 h = (uint64)ptr >> 4; // allocations are >= 16-byte aligned
        h *= 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
        return h;
    }

    void spinLock(oc::atomic<uint32>& lock)
    {
        while (lock.exchange(1, oc::memory_order_acquire) != 0)
            _mm_pause();
    }
    void spinUnlock(oc::atomic<uint32>& lock)
    {
        lock.store(0, oc::memory_order_release);
    }

    void memoryAllocThunk(void* ptr, size_t size) { Globals::memoryTracker.onAlloc(ptr, (uint64)size); }
    void memoryFreeThunk(void* ptr) { Globals::memoryTracker.onFree(ptr); }
}

MemoryTracker::MemoryTracker()
{
    initialize();
}

void MemoryTracker::initialize()
{
    assert(!m_initialized && "MemoryTracker initialized twice");
    // Pool allocations run BEFORE the hooks install, so they are untracked (and deliberately never
    // freed - frees from other globals' destructors outlive this object).
    m_nodePool = new MemScopeNode[MAX_NODES];
    for (MapShard& shard : m_shards)
        shard.entries = new MapEntry[SHARD_CAPACITY];

    m_root = &m_nodePool[0];
    m_root->name = "Total";
    m_root->category = (uint8)EProfileCategory::Other;
    m_numNodes.store(1, oc::memory_order_relaxed);
    m_initialized = true;

    g_memoryAllocHook.store(&memoryAllocThunk, oc::memory_order_release);
    g_memoryFreeHook.store(&memoryFreeThunk, oc::memory_order_release);
}

MemoryTracker::~MemoryTracker()
{
    g_memoryAllocHook.store(nullptr, oc::memory_order_release);
    g_memoryFreeHook.store(nullptr, oc::memory_order_release);
}

void MemoryTracker::onAlloc(void* ptr, uint64 size)
{
    if (ptr == nullptr || !m_enabled.load(oc::memory_order_relaxed) || t_inMemoryHook)
        return;
    t_inMemoryHook = true;

    MemScopeNode* node = resolveCurrentNode();
    if (node == nullptr || !mapInsert(ptr, size, node))
    {
        m_droppedAllocs.fetch_add(1, oc::memory_order_relaxed);
        t_inMemoryHook = false;
        return;
    }
    node->selfBytes.fetch_add((int64)size, oc::memory_order_relaxed);
    node->selfCount.fetch_add(1, oc::memory_order_relaxed);
    node->totalAllocBytes.fetch_add(size, oc::memory_order_relaxed);
    node->totalAllocCount.fetch_add(1, oc::memory_order_relaxed);
    m_trackedBytes.fetch_add((int64)size, oc::memory_order_relaxed);
    m_trackedCount.fetch_add(1, oc::memory_order_relaxed);

    t_inMemoryHook = false;
}

void MemoryTracker::onFree(void* ptr)
{
    // NOT gated on m_enabled: blocks tracked while enabled must still untrack after a disable,
    // or their counts would leak.
    if (ptr == nullptr || t_inMemoryHook)
        return;
    t_inMemoryHook = true;

    MapEntry entry;
    if (mapErase(ptr, entry)) // miss = allocated before tracking (or dropped) - ignore
    {
        entry.node->selfBytes.fetch_sub((int64)entry.size, oc::memory_order_relaxed);
        entry.node->selfCount.fetch_sub(1, oc::memory_order_relaxed);
        m_trackedBytes.fetch_sub((int64)entry.size, oc::memory_order_relaxed);
        m_trackedCount.fetch_sub(1, oc::memory_order_relaxed);
    }

    t_inMemoryHook = false;
}

MemScopeNode* MemoryTracker::resolveCurrentNode()
{
    ProfileTrack* track = Globals::profiler.threadTrack();
    if (track == nullptr) // unregistered thread (transient helpers, foreign threads using engine new)
        return findOrAddChild(m_root, "<other threads>", (uint8)EProfileCategory::Other);

    const uint32 depth = oc::min(track->m_openDepth, ProfileTrack::MAX_OPEN_DEPTH);
    if (depth == 0)
        return findOrAddChild(m_root, "<unscoped>", (uint8)EProfileCategory::Other);

    MemScopeNode* node = m_root;
    for (uint32 i = 0; i < depth && node != nullptr; ++i)
        node = findOrAddChild(node, track->m_openNames[i], track->m_openCategories[i]);
    return node;
}

MemScopeNode* MemoryTracker::findOrAddChild(MemScopeNode* parent, const char* name, uint8 category)
{
    // Lock-free fast path: identity is the name LITERAL pointer (the same scope from two TUs can in
    // principle make two nodes; harmless duplicates in the view).
    for (MemScopeNode* child = parent->firstChild.load(oc::memory_order_acquire); child != nullptr;
         child = child->nextSibling.load(oc::memory_order_relaxed))
        if (child->name == name)
            return child;

    spinLock(m_nodeLock);
    for (MemScopeNode* child = parent->firstChild.load(oc::memory_order_relaxed); child != nullptr;
         child = child->nextSibling.load(oc::memory_order_relaxed))
    {
        if (child->name == name)
        {
            spinUnlock(m_nodeLock);
            return child;
        }
    }
    const uint32 idx = m_numNodes.load(oc::memory_order_relaxed);
    if (idx >= MAX_NODES)
    {
        spinUnlock(m_nodeLock);
        return nullptr;
    }
    MemScopeNode* node = &m_nodePool[idx];
    node->name = name;
    node->parent = parent;
    node->category = category;
    node->nextSibling.store(parent->firstChild.load(oc::memory_order_relaxed), oc::memory_order_relaxed);
    m_numNodes.store(idx + 1, oc::memory_order_relaxed);
    parent->firstChild.store(node, oc::memory_order_release); // publish fully-initialized
    spinUnlock(m_nodeLock);
    return node;
}

bool MemoryTracker::mapInsert(void* ptr, uint64 size, MemScopeNode* node)
{
    const uint64 hash = hashPtr(ptr);
    MapShard& shard = m_shards[hash & (NUM_SHARDS - 1)];
    constexpr uint32 mask = SHARD_CAPACITY - 1;
    uint32 slot = (uint32)(hash >> 6) & mask;

    spinLock(shard.lock);
    if (shard.count >= SHARD_CAPACITY - (SHARD_CAPACITY >> 2)) // keep probes short: cap at 75%
    {
        spinUnlock(shard.lock);
        return false;
    }
    while (shard.entries[slot].ptr != nullptr)
        slot = (slot + 1) & mask;
    shard.entries[slot] = MapEntry{ .ptr = ptr, .size = size, .node = node };
    shard.count++;
    spinUnlock(shard.lock);
    return true;
}

bool MemoryTracker::mapErase(void* ptr, MapEntry& out)
{
    const uint64 hash = hashPtr(ptr);
    MapShard& shard = m_shards[hash & (NUM_SHARDS - 1)];
    constexpr uint32 mask = SHARD_CAPACITY - 1;
    const uint32 home = (uint32)(hash >> 6) & mask;

    spinLock(shard.lock);
    uint32 i = home;
    while (shard.entries[i].ptr != ptr)
    {
        if (shard.entries[i].ptr == nullptr)
        {
            spinUnlock(shard.lock);
            return false;
        }
        i = (i + 1) & mask;
        if (i == home) // full lap (can't happen below the 75% cap, but stay safe)
        {
            spinUnlock(shard.lock);
            return false;
        }
    }
    out = shard.entries[i];

    // Backward-shift deletion (no tombstones, probe chains stay minimal): pull every later entry
    // of the chain into the hole when its home slot allows it.
    uint32 hole = i;
    uint32 j = i;
    while (true)
    {
        j = (j + 1) & mask;
        const MapEntry& entry = shard.entries[j];
        if (entry.ptr == nullptr)
            break;
        const uint32 entryHome = (uint32)(hashPtr(entry.ptr) >> 6) & mask;
        if (((j - entryHome) & mask) >= ((j - hole) & mask)) // entry may move back to the hole without passing its home
        {
            shard.entries[hole] = entry;
            hole = j;
        }
    }
    shard.entries[hole].ptr = nullptr;
    shard.count--;
    spinUnlock(shard.lock);
    return true;
}
