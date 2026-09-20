export module RendererVK:SlotTable;

import Core;
import :Layout;

// The CPU side of a per-frame GPU slot table with DEFERRED recycling - the shape every emitter/query
// registry the outside creates and destroys needs. `create` hands out a slot index, `retire` only
// MARKS one dead, and a retired slot comes back for reuse once every frame that could still read it
// has drained: the GPU readbacks and the kill/active flags are slot-indexed, so a new owner landing on
// a slot too early would read another owner's in-flight results. The caller clears/sets the payload's
// own flag around retire() - that part differs per table.
//
// Capacity is fixed (create returns INVALID_SLOT when full) and the storage only ever grows to the
// high-water mark, so `slots()` stays a dense span the uploader walks.
//
// NOT thread-safe by itself: every caller holds the Renderer's spawn mutex (parallel entity spawning).
export template<typename T>
class RecycledSlotTable final
{
public:
    static constexpr uint32 INVALID_SLOT = UINT32_MAX;

    void initialize(uint32 capacity) { m_capacity = capacity; }

    // Recycles what has drained, then claims a slot. INVALID_SLOT = the table is full.
    uint32 create(uint32 frameCounter)
    {
        for (size_t i = 0; i < m_retired.size();)
        {
            if (frameCounter - m_retired[i].second > RendererVKLayout::NUM_FRAMES_IN_FLIGHT + 2)
            {
                m_free.push_back(m_retired[i].first);
                m_retired.erase(m_retired.begin() + i);
            }
            else
                ++i;
        }
        if (!m_free.empty())
        {
            const uint32 slot = m_free.back();
            m_free.pop_back();
            return slot;
        }
        if (m_slots.size() >= m_capacity)
            return INVALID_SLOT;
        m_slots.emplace_back();
        return (uint32)m_slots.size() - 1;
    }
    // The slot stays allocated (and keeps its payload) until create() finds it drained.
    void retire(uint32 slot, uint32 frameCounter) { m_retired.emplace_back(slot, frameCounter); }

    T& operator[](uint32 slot) { return m_slots[slot]; }
    const T& operator[](uint32 slot) const { return m_slots[slot]; }
    bool isValid(uint32 slot) const { return slot < m_slots.size(); }
    uint32 size() const { return (uint32)m_slots.size(); }
    oc::span<const T> slots() const { return m_slots; }

private:
    uint32 m_capacity = 0;
    oc::vector<T> m_slots;
    oc::vector<uint32> m_free;
    oc::vector<oc::pair<uint32, uint32>> m_retired; // slot, retire frame
};
