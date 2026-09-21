export module RendererVK:SharedTable;

import Core;
import :Buffer;
import :MeshLodRegistry; // IndexRangeFreeList
import :StagingManager;

// An APPEND-ONLY device-local scene table with slot recycling: the mesh infos, the materials and the
// mesh instance offsets are all this shape. The Buffer keeps a CPU backing store (the mirror every
// grow re-uploads), the free list hands back ranges destroyed containers released, and holes are never
// compacted - a freed range is reused by a later container or stays neutralized.
//
// Growth doubles the capacity and re-uploads the whole mirror, so it must also re-record: that, and
// whatever else the capacity change means for the pipelines, is the `onGrown` callback - the only way
// out of this class.
//
// NOT internally locked: every mutator runs under the Renderer's spawn mutex (parallel entity spawning).
export template<typename T>
class SharedTable final
{
public:
    // onGpuIdle runs BEFORE the buffer is re-created (the old one may still be in flight); onGrown
    // afterwards, to resize whatever else the capacity feeds and to re-record.
    void initialize(const char* debugName, uint32 capacity, uint32 limit, vk::BufferUsageFlags2 usage,
        oc::function<void()> onGpuIdle, oc::function<void(uint32)> onGrown)
    {
        m_capacity = capacity;
        m_limit = limit;
        m_onGpuIdle = oc::move(onGpuIdle);
        m_onGrown = oc::move(onGrown);
        m_buffer.initialize((vk::DeviceSize)capacity * sizeof(T), usage,
            vk::MemoryPropertyFlagBits::eDeviceLocal, true, debugName);
    }

    // Claims `items.size()` slots - a recycled range when one fits, appended otherwise - writes the CPU
    // mirror, runs onAssigned(base, reused) so the caller can fill its own parallel side tables, then
    // uploads. An append past the capacity grows first (which re-uploads the whole mirror instead).
    template<typename Fn>
    uint32 add(oc::span<const T> items, Fn&& onAssigned)
    {
        const uint32 count = (uint32)items.size();
        if (const uint32 reusedBase = m_freeSlots.allocate(count); reusedBase != UINT32_MAX)
        {
            memcpy(m_buffer.template getBackingStoreAs<T>().data() + reusedBase, items.data(), count * sizeof(T));
            onAssigned(reusedBase, true);
            upload(reusedBase, count);
            return reusedBase;
        }
        const uint32 base = m_counter;
        m_counter += count;
        assert(m_counter < m_limit);
        m_buffer.template appendToBackingStore<T>(items);
        onAssigned(base, false);
        if (m_counter > m_capacity)
            grow(m_counter);   // re-uploads the full mirror; re-records
        else
            upload(base, count);
        return base;
    }
    uint32 add(oc::span<const T> items) { return add(items, [](uint32, bool) {}); }

    void release(uint32 base, uint32 count) { m_freeSlots.release(base, count); }
    // One slot's mirror entry, for callers that edit in place (mesh streaming, neutralizing a range).
    oc::span<T> items() { return m_buffer.template getBackingStoreAs<T>(); }
    void upload(uint32 base, uint32 count)
    {
        Globals::stagingManager.ensureDrainedForSharedWrite();
        m_buffer.upload((vk::DeviceSize)count * sizeof(T), m_buffer.template getBackingStoreAs<T>().data() + base,
            (vk::DeviceSize)base * sizeof(T));
    }

    Buffer& getBuffer() { return m_buffer; }
    uint32 count() const { return m_counter; }
    uint32 capacity() const { return m_capacity; }

private:
    void grow(uint32 needed)
    {
        uint64 capacity = m_capacity;
        while (capacity < needed)
            capacity *= 2;
        m_capacity = (uint32)oc::min<uint64>(capacity, m_limit);
        m_onGpuIdle();
        m_buffer.resize((vk::DeviceSize)m_capacity * sizeof(T)); // preserves + re-uploads the whole mirror
        m_onGrown(m_capacity);
    }

    Buffer m_buffer;
    IndexRangeFreeList m_freeSlots;
    oc::function<void()> m_onGpuIdle;
    oc::function<void(uint32)> m_onGrown;
    uint32 m_counter = 0;
    uint32 m_capacity = 0;
    uint32 m_limit = UINT32_MAX;
};
