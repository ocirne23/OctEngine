export module Threading:MPMCQueue;

import Core;

// Dmitry Vyukov's bounded MPMC queue: each cell carries a sequence number that encodes whether
// it is ready to be written (seq == pos) or read (seq == pos + 1); producers and consumers claim
// positions with a CAS on their own counter and never contend with the other side except through
// the cell sequences. One CAS per operation, no spurious failure modes, FIFO per producer.
export template<typename T>
class MPMCQueue final
{
public:

    void initialize(uint32 capacity)
    {
        capacity = std::bit_ceil(capacity);
        m_mask = capacity - 1;
        m_cells = oc::make_unique<Cell[]>(capacity);
        for (uint32 i = 0; i < capacity; ++i)
            m_cells[i].seq.store(i, oc::memory_order_relaxed);
        m_enqueuePos.store(0, oc::memory_order_relaxed);
        m_dequeuePos.store(0, oc::memory_order_relaxed);
    }

    bool push(const T& value)
    {
        uint64 pos = m_enqueuePos.load(oc::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = m_cells[pos & m_mask];
            const uint64 seq = cell.seq.load(oc::memory_order_acquire);
            const int64 dif = int64(seq) - int64(pos);
            if (dif == 0)
            {
                if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, oc::memory_order_relaxed))
                {
                    cell.data = value;
                    cell.seq.store(pos + 1, oc::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0)
                return false; // full
            else
                pos = m_enqueuePos.load(oc::memory_order_relaxed);
        }
    }

    bool pop(T& outValue)
    {
        uint64 pos = m_dequeuePos.load(oc::memory_order_relaxed);
        for (;;)
        {
            Cell& cell = m_cells[pos & m_mask];
            const uint64 seq = cell.seq.load(oc::memory_order_acquire);
            const int64 dif = int64(seq) - int64(pos + 1);
            if (dif == 0)
            {
                if (m_dequeuePos.compare_exchange_weak(pos, pos + 1, oc::memory_order_relaxed))
                {
                    outValue = cell.data;
                    cell.seq.store(pos + m_mask + 1, oc::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0)
                return false; // empty
            else
                pos = m_dequeuePos.load(oc::memory_order_relaxed);
        }
    }

    // Approximate (racy) emptiness, good enough for the idle-recheck / wake heuristics.
    bool wasEmpty() const
    {
        return m_dequeuePos.load(oc::memory_order_relaxed) >= m_enqueuePos.load(oc::memory_order_relaxed);
    }

private:

    struct Cell
    {
        oc::atomic<uint64> seq;
        T data;
    };

    oc::unique_ptr<Cell[]> m_cells;
    uint64 m_mask = 0;
    alignas(64) oc::atomic<uint64> m_enqueuePos = 0;
    alignas(64) oc::atomic<uint64> m_dequeuePos = 0;
};
