export module Spatial:CellMap;

import Core;
import :Morton;

// Module-internal flat hash map holding one record per occupied grid cell of one level. Open
// addressing over a plain key array (8 keys per cache line), power-of-two capacity, linear
// probing, backward-shift deletion instead of tombstones (erases only happen in the
// single-threaded commit phase). Nothing here is exported.

struct CellRecord
{
    uint32 dynHead = UINT32_MAX; // head of the cell's intrusive entry list (RecordPool index)
    uint32 dynCount = 0;
    uint64 childMask = 0;        // occupied 4x4x4 child cells one level down
    uint32 staticStart = 0;      // reserved for the compacted static tier
    uint32 staticCount = 0;
};

class CellMap final
{
public:

    void initialize(uint32 capacity)
    {
        m_capacity = std::bit_ceil(capacity < 64 ? 64u : capacity);
        m_keys.assign(m_capacity, Morton::InvalidKey);
        m_records.assign(m_capacity, CellRecord());
        m_size = 0;
    }

    uint32 size() const { return m_size; }

    CellRecord* find(uint64 key)
    {
        const uint32 mask = m_capacity - 1;
        for (uint32 slot = homeSlot(key);; slot = (slot + 1) & mask)
        {
            if (m_keys[slot] == key)
                return &m_records[slot];
            if (m_keys[slot] == Morton::InvalidKey)
                return nullptr;
        }
    }

    const CellRecord* find(uint64 key) const { return const_cast<CellMap*>(this)->find(key); }

    CellRecord& getOrCreate(uint64 key) // growth invalidates previously returned pointers
    {
        if (m_size * 10 >= m_capacity * 7)
            grow();
        const uint32 mask = m_capacity - 1;
        for (uint32 slot = homeSlot(key);; slot = (slot + 1) & mask)
        {
            if (m_keys[slot] == key)
                return m_records[slot];
            if (m_keys[slot] == Morton::InvalidKey)
            {
                m_keys[slot] = key;
                m_records[slot] = CellRecord();
                ++m_size;
                return m_records[slot];
            }
        }
    }

    void erase(uint64 key)
    {
        const uint32 mask = m_capacity - 1;
        uint32 slot = homeSlot(key);
        while (true)
        {
            if (m_keys[slot] == key)
                break;
            if (m_keys[slot] == Morton::InvalidKey)
                return;
            slot = (slot + 1) & mask;
        }
        uint32 hole = slot;
        while (true) // backward-shift: pull later probe-chain entries into the hole
        {
            slot = (slot + 1) & mask;
            if (m_keys[slot] == Morton::InvalidKey)
                break;
            const uint32 home = homeSlot(m_keys[slot]);
            if (((slot - home) & mask) >= ((slot - hole) & mask))
            {
                m_keys[hole] = m_keys[slot];
                m_records[hole] = m_records[slot];
                hole = slot;
            }
        }
        m_keys[hole] = Morton::InvalidKey;
        m_records[hole] = CellRecord();
        --m_size;
    }

    template <typename Func>
    void forEachCell(Func&& func) const // func(uint64 key, const CellRecord&)
    {
        for (uint32 slot = 0; slot < m_capacity; ++slot)
            if (m_keys[slot] != Morton::InvalidKey)
                func(m_keys[slot], m_records[slot]);
    }

    template <typename Func>
    void forEachCellMutable(Func&& func) // func(uint64 key, CellRecord&); must not insert/erase
    {
        for (uint32 slot = 0; slot < m_capacity; ++slot)
            if (m_keys[slot] != Morton::InvalidKey)
                func(m_keys[slot], m_records[slot]);
    }

private:

    static uint64 hashKey(uint64 x) // splitmix64 finalizer
    {
        x ^= x >> 30; x *= 0xbf58'476d'1ce4'e5b9ull;
        x ^= x >> 27; x *= 0x94d0'49bb'1331'11ebull;
        return x ^ (x >> 31);
    }

    uint32 homeSlot(uint64 key) const { return uint32(hashKey(key)) & (m_capacity - 1); }

    void grow()
    {
        oc::vector<uint64> oldKeys = oc::move(m_keys);
        oc::vector<CellRecord> oldRecords = oc::move(m_records);
        const uint32 oldCapacity = m_capacity;
        m_capacity *= 2;
        m_keys.assign(m_capacity, Morton::InvalidKey);
        m_records.assign(m_capacity, CellRecord());
        const uint32 mask = m_capacity - 1;
        for (uint32 i = 0; i < oldCapacity; ++i)
        {
            if (oldKeys[i] == Morton::InvalidKey)
                continue;
            uint32 slot = homeSlot(oldKeys[i]);
            while (m_keys[slot] != Morton::InvalidKey)
                slot = (slot + 1) & mask;
            m_keys[slot] = oldKeys[i];
            m_records[slot] = oldRecords[i];
        }
    }

    oc::vector<uint64> m_keys;
    oc::vector<CellRecord> m_records;
    uint32 m_capacity = 0;
    uint32 m_size = 0;
};
