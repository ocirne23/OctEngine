export module Threading:StealDeque;

import Core;
import :Types;

// Chase-Lev work-stealing deque (memory orderings per Le et al., "Correct and Efficient
// Work-Stealing for Weak Memory Models"). The owning worker pushes/pops LIFO at the bottom
// (cache-hot continuations); thieves steal FIFO from the top. Fixed capacity instead of the
// paper's growable buffer: a full deque overflows to the global queues, which keeps memory
// bounded and sidesteps buffer-reclamation. The no-overwrite argument still holds: push refuses
// when bottom - top >= capacity, so a slot a pending steal may read is never rewritten while its
// top CAS can still succeed.
export class StealDeque final
{
public:

    void initialize(uint32 capacity)
    {
        capacity = std::bit_ceil(capacity);
        m_capacity = capacity;
        m_mask = capacity - 1;
        m_buffer = oc::make_unique<oc::atomic<Job*>[]>(capacity);
        m_top.store(0, oc::memory_order_relaxed);
        m_bottom.store(0, oc::memory_order_relaxed);
    }

    bool push(Job* job) // owner thread only
    {
        const int64 b = m_bottom.load(oc::memory_order_relaxed);
        const int64 t = m_top.load(oc::memory_order_acquire);
        if (b - t >= int64(m_capacity))
            return false;
        m_buffer[uint64(b) & m_mask].store(job, oc::memory_order_relaxed);
        m_bottom.store(b + 1, oc::memory_order_release);
        return true;
    }

    Job* pop() // owner thread only
    {
        const int64 b = m_bottom.load(oc::memory_order_relaxed) - 1;
        m_bottom.store(b, oc::memory_order_relaxed);
        oc::atomic_thread_fence(oc::memory_order_seq_cst);
        int64 t = m_top.load(oc::memory_order_relaxed);
        if (t <= b)
        {
            Job* job = m_buffer[uint64(b) & m_mask].load(oc::memory_order_relaxed);
            if (t == b)
            {
                // last element: race the thieves for it
                if (!m_top.compare_exchange_strong(t, t + 1, oc::memory_order_seq_cst, oc::memory_order_relaxed))
                    job = nullptr;
                m_bottom.store(b + 1, oc::memory_order_relaxed);
            }
            return job;
        }
        m_bottom.store(b + 1, oc::memory_order_relaxed);
        return nullptr;
    }

    Job* steal() // any other thread
    {
        int64 t = m_top.load(oc::memory_order_acquire);
        oc::atomic_thread_fence(oc::memory_order_seq_cst);
        const int64 b = m_bottom.load(oc::memory_order_acquire);
        if (t < b)
        {
            Job* job = m_buffer[uint64(t) & m_mask].load(oc::memory_order_relaxed);
            if (!m_top.compare_exchange_strong(t, t + 1, oc::memory_order_seq_cst, oc::memory_order_relaxed))
                return nullptr; // lost to the owner or another thief
            return job;
        }
        return nullptr;
    }

    bool maybeNonEmpty() const
    {
        return m_bottom.load(oc::memory_order_relaxed) > m_top.load(oc::memory_order_relaxed);
    }

private:

    oc::unique_ptr<oc::atomic<Job*>[]> m_buffer;
    uint64 m_mask = 0;
    uint32 m_capacity = 0;
    alignas(64) oc::atomic<int64> m_top = 0;
    alignas(64) oc::atomic<int64> m_bottom = 0;
};
