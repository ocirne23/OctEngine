export module RendererVK:SlotTable;

import Core;
import :Layout;
import :SlotAlloc;

// The CPU side of a per-frame GPU slot table with DEFERRED recycling - the shape every emitter/query
// registry the outside creates and destroys needs. `create` hands out a slot index, `retire` only
// MARKS one dead, and a retired slot comes back for reuse once every frame that could still read it
// has drained: the GPU readbacks and the kill/active flags are slot-indexed, so a new owner landing on
// a slot too early would read another owner's in-flight results. The caller clears/sets the payload's
// own flag around retire() - that part differs per table.
//
// Capacity is fixed (create returns INVALID_SLOT when full). The storage is sized to it ONCE, so a slot
// never moves; `slots()` is the dense prefix up to the high-water mark that the uploader walks.
//
// LOCK-FREE (parallel entity spawning): create / retire run on any worker. The free slots are a SlotStack;
// a retire goes into the bucket of its frame (frame % RETIRE_BUCKETS), and recycle() - main thread, once
// per frame - moves the oldest bucket, whose frames have drained, onto the free stack.
export template<typename T>
class RecycledSlotTable final
{
public:
    static constexpr uint32 INVALID_SLOT = UINT32_MAX;

    void initialize(uint32 capacity)
    {
        m_capacity = capacity;
        m_slots.resize(capacity);
        m_links = oc::make_unique<oc::atomic<uint32>[]>(capacity);
    }

    // INVALID_SLOT = the table is full.
    uint32 create()
    {
        if (const uint32 slot = m_free.pop(link()); slot != SlotStack::EMPTY)
            return slot;
        uint32 count = m_count.load(oc::memory_order_relaxed);
        do
        {
            if (count >= m_capacity)
                return INVALID_SLOT;
        } while (!m_count.compare_exchange_weak(count, count + 1, oc::memory_order_relaxed));
        return count;
    }
    // The slot stays allocated (and keeps its payload) until recycle() finds its frame drained.
    void retire(uint32 slot, uint32 frameCounter) { m_retired[frameCounter % RETIRE_BUCKETS].push(slot, link()); }
    // Main thread, once per frame (a skipped call only delays): the bucket recycled at frame f holds the retires
    // of frame f + 1 - RETIRE_BUCKETS, i.e. NUM_FRAMES_IN_FLIGHT + 3 frames ago and older.
    void recycle(uint32 frameCounter)
    {
        uint32 slot = m_retired[(frameCounter + 1) % RETIRE_BUCKETS].takeAll();
        while (slot != SlotStack::EMPTY)
        {
            const uint32 next = m_links[slot].load(oc::memory_order_relaxed);
            m_free.push(slot, link());
            slot = next;
        }
    }

    T& operator[](uint32 slot) { return m_slots[slot]; }
    const T& operator[](uint32 slot) const { return m_slots[slot]; }
    bool isValid(uint32 slot) const { return slot < size(); }
    uint32 size() const { return m_count.load(oc::memory_order_acquire); }
    oc::span<const T> slots() const { return oc::span<const T>(m_slots.data(), size()); }

private:
    static constexpr uint32 RETIRE_BUCKETS = RendererVKLayout::NUM_FRAMES_IN_FLIGHT + 4;

    auto link() { return [this](uint32 s) -> oc::atomic<uint32>& { return m_links[s]; }; }

    uint32 m_capacity = 0;
    oc::vector<T> m_slots;                        // sized to the capacity at initialize, never reallocated
    oc::unique_ptr<oc::atomic<uint32>[]> m_links; // per slot: its free / retired stack link
    oc::atomic<uint32> m_count{ 0 };              // the high-water mark
    SlotStack m_free;
    oc::array<SlotStack, RETIRE_BUCKETS> m_retired;
};
