module;

#include <immintrin.h>

export module Spatial:CellMap;

import Core;
import :Morton;

// One record per occupied cell of one level: open addressing, linear probing, backward-shift deletion. A slot holds the key next to
// its record (32 bytes, two per cache line, never split), so a hit reads one line. Fibonacci hashing: one multiply, the top bits.
// Grows at half full: a miss (the direct cell path probes empty cells) stays ~2.5 slots, a hit ~1.5.

struct CellRecord
{
    uint64 childMask = 0;      // occupied 4x4x4 child cells one level down
    uint32 head = UINT32_MAX;  // first CellBlock of the cell's chain, UINT32_MAX = none
    uint32 count = 0;          // entries owning a lane here (blocks free as soon as they empty, so 0 <=> no chain)
};

struct alignas(32) CellSlot
{
    uint64 key = Morton::InvalidKey;
    CellRecord rec;
};
static_assert(sizeof(CellSlot) == 32);

class CellMap final
{
public:

    void initialize(uint32 capacity)
    {
        setCapacity(oc::bitCeil(capacity < 64 ? 64u : capacity));
        m_slots.assign(m_capacity, CellSlot());
        m_size = 0;
    }

    uint32 size() const { return m_size; }

    CellRecord* find(uint64 key)
    {
        const uint32 mask = m_capacity - 1;
        for (uint32 slot = homeSlot(key);; slot = (slot + 1) & mask)
        {
            CellSlot& s = m_slots[slot];
            if (s.key == key)
                return &s.rec;
            if (s.key == Morton::InvalidKey)
                return nullptr;
        }
    }

    const CellRecord* find(uint64 key) const { return const_cast<CellMap*>(this)->find(key); }

    // The key's home slot into L1: issued for several keys before their finds, the misses overlap.
    void prefetch(uint64 key) const { _mm_prefetch(reinterpret_cast<const char*>(&m_slots[homeSlot(key)]), _MM_HINT_T0); }

    CellRecord& getOrCreate(uint64 key, bool& created) // growth invalidates previously returned pointers
    {
        if (m_size * 2 >= m_capacity)
            grow();
        const uint32 mask = m_capacity - 1;
        for (uint32 slot = homeSlot(key);; slot = (slot + 1) & mask)
        {
            CellSlot& s = m_slots[slot];
            if (s.key == key)
            {
                created = false;
                return s.rec;
            }
            if (s.key == Morton::InvalidKey)
            {
                s.key = key;
                s.rec = CellRecord();
                ++m_size;
                created = true;
                return s.rec;
            }
        }
    }

    CellRecord& getOrCreate(uint64 key)
    {
        bool created;
        return getOrCreate(key, created);
    }

    void erase(uint64 key)
    {
        const uint32 mask = m_capacity - 1;
        uint32 slot = homeSlot(key);
        while (true)
        {
            if (m_slots[slot].key == key)
                break;
            if (m_slots[slot].key == Morton::InvalidKey)
                return;
            slot = (slot + 1) & mask;
        }
        uint32 hole = slot;
        while (true) // backward-shift: pull later probe-chain entries into the hole
        {
            slot = (slot + 1) & mask;
            if (m_slots[slot].key == Morton::InvalidKey)
                break;
            const uint32 home = homeSlot(m_slots[slot].key);
            if (((slot - home) & mask) >= ((slot - hole) & mask))
            {
                m_slots[hole] = m_slots[slot];
                hole = slot;
            }
        }
        m_slots[hole] = CellSlot();
        --m_size;
    }

    template <typename Func>
    void forEachCell(Func&& func) const // func(uint64 key, const CellRecord&)
    {
        for (const CellSlot& s : m_slots)
            if (s.key != Morton::InvalidKey)
                func(s.key, s.rec);
    }

    template <typename Func>
    void forEachCellMutable(Func&& func) // func(uint64 key, CellRecord&); must not insert/erase
    {
        for (CellSlot& s : m_slots)
            if (s.key != Morton::InvalidKey)
                func(s.key, s.rec);
    }

private:

    uint32 homeSlot(uint64 key) const { return uint32((key * 0x9E37'79B9'7F4A'7C15ull) >> m_shift); }

    void setCapacity(uint32 capacity)
    {
        m_capacity = capacity;
        m_shift = 64u - uint32(oc::tzcnt(uint64(capacity)));
    }

    void grow()
    {
        oc::vector<CellSlot> old = oc::move(m_slots);
        setCapacity(m_capacity * 2);
        m_slots.assign(m_capacity, CellSlot());
        const uint32 mask = m_capacity - 1;
        for (const CellSlot& s : old)
        {
            if (s.key == Morton::InvalidKey)
                continue;
            uint32 slot = homeSlot(s.key);
            while (m_slots[slot].key != Morton::InvalidKey)
                slot = (slot + 1) & mask;
            m_slots[slot] = s;
        }
    }

    oc::vector<CellSlot> m_slots;
    uint32 m_capacity = 0;
    uint32 m_shift = 64;
    uint32 m_size = 0;
};
