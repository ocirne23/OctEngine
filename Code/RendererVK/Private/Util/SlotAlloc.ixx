export module RendererVK:SlotAlloc;

import Core;

// The LOCK-FREE slot primitives behind the spawn path's allocators (transform slots, LOD state ranges, the
// emitter / query slot tables): parallel entity spawning claims and frees slots on any worker without a mutex.

// A stack of uint32 slot indices (a Treiber stack). The links live in a caller-owned array of atomics indexed by
// slot; the head's high 32 bits are a tag that every push and pop bumps, so a pop that read a stale link (the slot
// was popped and pushed again meanwhile) fails its CAS (ABA).
export class SlotStack final
{
public:
    static constexpr uint32 EMPTY = UINT32_MAX;

    // link(slot) -> oc::atomic<uint32>&
    template<typename LinkFn>
    void push(uint32 slot, LinkFn&& link)
    {
        oc::atomic<uint32>& next = link(slot);
        uint64 head = m_head.load(oc::memory_order_relaxed);
        uint64 pushed;
        do
        {
            next.store((uint32)head, oc::memory_order_relaxed);
            pushed = bumpTag(head) | slot;
        } while (!m_head.compare_exchange_weak(head, pushed, oc::memory_order_release, oc::memory_order_relaxed));
    }

    template<typename LinkFn>
    uint32 pop(LinkFn&& link)
    {
        uint64 head = m_head.load(oc::memory_order_acquire);
        while ((uint32)head != EMPTY)
        {
            const uint64 popped = bumpTag(head) | link((uint32)head).load(oc::memory_order_relaxed);
            if (m_head.compare_exchange_weak(head, popped, oc::memory_order_acquire, oc::memory_order_acquire))
                return (uint32)head;
        }
        return EMPTY;
    }

    // Detaches the whole stack: the caller walks it through the links (nobody else can reach those slots now).
    uint32 takeAll()
    {
        uint64 head = m_head.load(oc::memory_order_acquire);
        while ((uint32)head != EMPTY
            && !m_head.compare_exchange_weak(head, bumpTag(head) | EMPTY, oc::memory_order_acquire, oc::memory_order_acquire)) {}
        return (uint32)head;
    }

private:
    static uint64 bumpTag(uint64 head) { return ((head >> 32) + 1) << 32; }

    oc::atomic<uint64> m_head{ EMPTY };
};

// A growable array whose elements NEVER move: fixed-size pages behind a fixed page table. A reader on one worker
// never sees a reallocation another worker's growth caused. Only a new page takes a mutex.
export template<typename T, uint32 PAGE_SHIFT = 12, uint32 MAX_PAGES = 4096>
class PagedArray final
{
public:
    static constexpr uint32 PAGE_SIZE = 1u << PAGE_SHIFT;
    static constexpr uint32 CAPACITY = PAGE_SIZE * MAX_PAGES;

    PagedArray()
    {
        for (oc::atomic<Page*>& page : m_pageTable)
            page.store(nullptr, oc::memory_order_relaxed);
    }
    PagedArray(const PagedArray&) = delete;
    PagedArray& operator=(const PagedArray&) = delete;

    // `idx` must lie in a page ensure() made.
    T& operator[](uint32 idx) { return m_pageTable[idx >> PAGE_SHIFT].load(oc::memory_order_acquire)->items[idx & (PAGE_SIZE - 1)]; }
    const T& operator[](uint32 idx) const { return m_pageTable[idx >> PAGE_SHIFT].load(oc::memory_order_acquire)->items[idx & (PAGE_SIZE - 1)]; }

    T& ensure(uint32 idx)
    {
        assert(idx < CAPACITY);
        oc::atomic<Page*>& slot = m_pageTable[idx >> PAGE_SHIFT];
        Page* page = slot.load(oc::memory_order_acquire);
        if (page == nullptr)
        {
            const std::lock_guard lock(m_pageMutex);
            page = slot.load(oc::memory_order_relaxed);
            if (page == nullptr)
            {
                page = m_pages.emplace_back(oc::make_unique<Page>()).get();
                slot.store(page, oc::memory_order_release);
            }
        }
        return page->items[idx & (PAGE_SIZE - 1)];
    }

private:
    struct Page
    {
        T items[PAGE_SIZE];
    };

    oc::array<oc::atomic<Page*>, MAX_PAGES> m_pageTable;
    oc::vector<oc::unique_ptr<Page>> m_pages; // the owners (m_pageMutex)
    std::mutex m_pageMutex;
};

// Slot RANGES [base, base + count) from a bump cursor, recycled per range SIZE: one lock-free SlotStack per count
// up to MAX_POOLED, a mutex-guarded list above it (rare). Freed ranges are never split or merged - a respawn of the
// same model asks for the same count again. The high-water mark is what a backing GPU buffer must hold.
export class SlotRangeAllocator final
{
public:
    static constexpr uint32 MAX_POOLED = 64;

    uint32 allocate(uint32 count)
    {
        assert(count > 0);
        uint32 base = SlotStack::EMPTY;
        if (count <= MAX_POOLED)
            base = m_pooled[count - 1].pop([this](uint32 s) -> oc::atomic<uint32>& { return m_links[s]; });
        else
        {
            const std::lock_guard lock(m_largeMutex);
            if (const auto it = m_large.find(count); it != m_large.end() && !it->second.empty())
            {
                base = it->second.back();
                it->second.pop_back();
            }
        }
        if (base != SlotStack::EMPTY)
        {
            m_numFree.fetch_sub(count, oc::memory_order_relaxed);
            return base;
        }
        base = m_count.fetch_add(count, oc::memory_order_relaxed);
        assert(base + count <= decltype(m_links)::CAPACITY && "slot range allocator exhausted");
        m_links.ensure(base);
        return base;
    }

    void release(uint32 base, uint32 count)
    {
        m_numFree.fetch_add(count, oc::memory_order_relaxed);
        if (count <= MAX_POOLED)
            m_pooled[count - 1].push(base, [this](uint32 s) -> oc::atomic<uint32>& { return m_links[s]; });
        else
        {
            const std::lock_guard lock(m_largeMutex);
            m_large[count].push_back(base);
        }
    }

    // Every slot ever handed out (live or free).
    uint32 highWater() const { return m_count.load(oc::memory_order_relaxed); }
    uint32 numFree() const { return m_numFree.load(oc::memory_order_relaxed); } // stats: may lag

private:
    oc::array<SlotStack, MAX_POOLED> m_pooled;
    PagedArray<oc::atomic<uint32>> m_links; // per range BASE: its free-stack link
    oc::atomic<uint32> m_count{ 0 };
    oc::atomic<uint32> m_numFree{ 0 };
    std::mutex m_largeMutex;
    oc::unordered_map<uint32, oc::vector<uint32>> m_large; // count -> free bases (m_largeMutex)
};
