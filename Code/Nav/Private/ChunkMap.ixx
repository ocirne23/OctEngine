export module Nav:ChunkMap;

import Core;
import :Grid;

// Sparse chunk storage: an open-addressed uint64 -> unique_ptr<T> map (the Spatial CellMap
// pattern — splitmix64 hash, power-of-two capacity, linear probing, backward-shift deletion). The
// values are heap-owned so growth never moves a chunk: readers on workers may hold a chunk
// pointer for a whole frame while nothing inserts/erases (all mutation is main-thread or inside
// the build job that owns the map). find() is the hot path — units resolve their chunk every tick.
export namespace Nav
{
    template<typename T>
    class ChunkMap final
    {
    public:

        ChunkMap() { reset(64); }

        // Chunks are POOLED: reset and erase park the values in m_pool and getOrCreate re-initializes
        // one from there, so a rebuild (the TeamField resets every 0.25 s) or an evict/regrow cycle
        // allocates nothing once the peak chunk count has been reached. reset also keeps the table
        // capacity, so the rebuild does not regrow through the doublings again.
        void reset(uint32 capacity)
        {
            for (oc::unique_ptr<T>& value : m_values)
                releaseChunk(value);
            m_capacity = oc::bitCeil(oc::max(capacity < 64 ? 64u : capacity, m_capacity));
            m_keys.assign(m_capacity, InvalidChunkKey);
            m_values.clear();
            m_values.resize(m_capacity);
            m_size = 0;
        }

        uint32 size() const { return m_size; }
        uint32 capacity() const { return m_capacity; }

        T* find(uint64 key)
        {
            const uint32 mask = m_capacity - 1;
            for (uint32 slot = homeSlot(key);; slot = (slot + 1) & mask)
            {
                if (m_keys[slot] == key)
                    return m_values[slot].get();
                if (m_keys[slot] == InvalidChunkKey)
                    return nullptr;
            }
        }
        const T* find(uint64 key) const { return const_cast<ChunkMap*>(this)->find(key); }

        T& getOrCreate(uint64 key)
        {
            if (m_size * 10 >= m_capacity * 7)
                grow();
            const uint32 mask = m_capacity - 1;
            for (uint32 slot = homeSlot(key);; slot = (slot + 1) & mask)
            {
                if (m_keys[slot] == key)
                    return *m_values[slot];
                if (m_keys[slot] == InvalidChunkKey)
                {
                    m_keys[slot] = key;
                    m_values[slot] = takeChunk();
                    ++m_size;
                    return *m_values[slot];
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
                if (m_keys[slot] == InvalidChunkKey)
                    return;
                slot = (slot + 1) & mask;
            }
            releaseChunk(m_values[slot]);
            uint32 hole = slot;
            while (true) // backward-shift: pull later probe-chain entries into the hole
            {
                slot = (slot + 1) & mask;
                if (m_keys[slot] == InvalidChunkKey)
                    break;
                const uint32 home = homeSlot(m_keys[slot]);
                if (((slot - home) & mask) >= ((slot - hole) & mask))
                {
                    m_keys[hole] = m_keys[slot];
                    m_values[hole] = oc::move(m_values[slot]);
                    hole = slot;
                }
            }
            m_keys[hole] = InvalidChunkKey;
            m_values[hole].reset(); // moved-from by the shift (or the released slot itself): already null
            --m_size;
        }

        template<typename Func>
        void forEach(Func&& func) const // func(uint64 key, const T&)
        {
            for (uint32 slot = 0; slot < m_capacity; ++slot)
                if (m_keys[slot] != InvalidChunkKey)
                    func(m_keys[slot], *m_values[slot]);
        }
        template<typename Func>
        void forEachMutable(Func&& func) // func(uint64 key, T&); must not insert/erase
        {
            for (uint32 slot = 0; slot < m_capacity; ++slot)
                if (m_keys[slot] != InvalidChunkKey)
                    func(m_keys[slot], *m_values[slot]);
        }
        // Slot-indexed access for parallel sweeps (nullptr = empty slot).
        T* slotValue(uint32 slot) { return m_values[slot].get(); }
        uint64 slotKey(uint32 slot) const { return m_keys[slot]; }
        template<typename Pred>
        void eraseIf(Pred&& pred) // pred(uint64 key, T&) -> bool
        {
            m_doomed.clear(); // kept scratch: no allocation on the per-step eviction sweep
            for (uint32 slot = 0; slot < m_capacity; ++slot)
                if (m_keys[slot] != InvalidChunkKey && pred(m_keys[slot], *m_values[slot]))
                    m_doomed.push_back(m_keys[slot]);
            for (const uint64 key : m_doomed)
                erase(key);
        }

    private:

        static uint64 hashKey(uint64 x) // splitmix64 finalizer
        {
            x ^= x >> 30; x *= 0xbf58'476d'1ce4'e5b9ull;
            x ^= x >> 27; x *= 0x94d0'49bb'1331'11ebull;
            return x ^ (x >> 31);
        }
        uint32 homeSlot(uint64 key) const { return uint32(hashKey(key)) & (m_capacity - 1); }

        oc::unique_ptr<T> takeChunk() // a pooled chunk re-initialized in place, else a fresh one
        {
            if (m_pool.empty())
                return oc::make_unique<T>();
            oc::unique_ptr<T> chunk = oc::move(m_pool.back());
            m_pool.pop_back();
            *chunk = T();
            return chunk;
        }
        void releaseChunk(oc::unique_ptr<T>& chunk)
        {
            if (chunk)
                m_pool.push_back(oc::move(chunk));
        }

        void grow()
        {
            oc::vector<uint64> oldKeys = oc::move(m_keys);
            oc::vector<oc::unique_ptr<T>> oldValues = oc::move(m_values);
            const uint32 oldCapacity = m_capacity;
            m_capacity *= 2;
            m_keys.assign(m_capacity, InvalidChunkKey);
            m_values.clear();
            m_values.resize(m_capacity);
            const uint32 mask = m_capacity - 1;
            for (uint32 i = 0; i < oldCapacity; ++i)
            {
                if (oldKeys[i] == InvalidChunkKey)
                    continue;
                uint32 slot = homeSlot(oldKeys[i]);
                while (m_keys[slot] != InvalidChunkKey)
                    slot = (slot + 1) & mask;
                m_keys[slot] = oldKeys[i];
                m_values[slot] = oc::move(oldValues[i]);
            }
        }

        oc::vector<uint64> m_keys;
        oc::vector<oc::unique_ptr<T>> m_values;
        oc::vector<uint64> m_doomed; // eraseIf scratch
        oc::vector<oc::unique_ptr<T>> m_pool; // released chunks, reused by getOrCreate
        uint32 m_capacity = 0;
        uint32 m_size = 0;
    };
}
