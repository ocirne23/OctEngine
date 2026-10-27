export module Threading:FreeStack;

import Core;

// Lock-free LIFO freelist over a fixed index range (Treiber stack, ABA tag in the head). Not a ring:
// a pre-empted thread can never make it look empty.
export class TaggedIndexStack final
{
public:

    static constexpr uint32 Invalid = 0xffffffffu;

    void initialize(uint32 capacity)
    {
        m_next = oc::make_unique<oc::atomic<uint32>[]>(capacity);
        m_head.store(uint64(Invalid), oc::memory_order_relaxed);
    }

    void push(uint32 index)
    {
        uint64 head = m_head.load(oc::memory_order_relaxed);
        for (;;)
        {
            m_next[index].store(uint32(head), oc::memory_order_relaxed);
            const uint64 newHead = ((head + 0x1'0000'0000ull) & 0xffffffff'00000000ull) | index;
            if (m_head.compare_exchange_weak(head, newHead, oc::memory_order_release, oc::memory_order_relaxed))
                return;
        }
    }

    bool pop(uint32& outIndex)
    {
        uint64 head = m_head.load(oc::memory_order_acquire);
        for (;;)
        {
            const uint32 index = uint32(head);
            if (index == Invalid)
                return false;
            const uint32 next = m_next[index].load(oc::memory_order_relaxed);
            const uint64 newHead = ((head + 0x1'0000'0000ull) & 0xffffffff'00000000ull) | next;
            if (m_head.compare_exchange_weak(head, newHead, oc::memory_order_acquire, oc::memory_order_acquire))
            {
                outIndex = index;
                return true;
            }
        }
    }

    bool wasEmpty() const { return uint32(m_head.load(oc::memory_order_relaxed)) == Invalid; }

private:

    oc::unique_ptr<oc::atomic<uint32>[]> m_next;
    alignas(64) oc::atomic<uint64> m_head = uint64(Invalid);
};
