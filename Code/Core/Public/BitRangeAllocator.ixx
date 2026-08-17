export module Core.BitRangeAllocator;

import Core;

export template<bool ThreadSafe = false>
class BitRangeAllocator final
{
public:

    BitRangeAllocator(uint32 size)
    {
        const uint32 numInts = (size + 63) / 64;
        m_size = numInts * 64;
        m_pBits = oc::make_unique<uint64[]>(numInts);
    }
    ~BitRangeAllocator() {};
    BitRangeAllocator(const BitRangeAllocator&) = delete;

    void resize(uint32 size)
    {
        const uint32 numInts = (size + 63) / 64;
        const uint32 newSize = numInts * 64;
        if (m_size <= newSize)
        {
            const uint32 oldNumInts = (m_size + 63) / 64;
            m_size = newSize;
            auto newBits = oc::make_unique<uint64[]>(numInts); // value-init: grown tail starts free
            memcpy(newBits.get(), m_pBits.get(), oldNumInts * sizeof(uint64));
            m_pBits = oc::move(newBits);
        }
    }

    int acquireOne()
    {
        const int numInts = (m_size + 63) / 64;
        int startSlot;
        if constexpr (ThreadSafe)
            startSlot = m_lastAcquiredIdx + 1; // In a multithreaded environment we spread out different allocations to minimize contention
        else
            startSlot = m_lastAcquiredIdx; // In a single threaded environment we try to make allocations as linear as possible

        for (int i = 0; i < numInts; ++i)
        {
            const int intIdx = (i + startSlot) % numInts;
            const uint64 usedBits = m_pBits[intIdx];
            if (usedBits != ~0ull)
            {
#pragma warning(disable: 4102) // unreferenced label
            retry:
                const int bitIdx = (int)_tzcnt_u64(~usedBits);
                const int idx = intIdx * 64 + bitIdx;
                if constexpr (ThreadSafe)
                {
                    const uint64 old = oc::atomic_ref<uint64>(m_pBits[intIdx]).fetch_or(1ull << bitIdx, oc::memory_order_relaxed);
                    if ((old & (1ull << bitIdx)) == 0)
                    {
                        m_lastAcquiredIdx = intIdx;
                        return idx;
                    }
                    else if (old == ~0ull)
                        continue;
                    else
                        goto retry;
                }
                else
                {
                    m_pBits[intIdx] |= 1ull << bitIdx;
                    m_lastAcquiredIdx = intIdx;
                    return idx;
                }
            }
#pragma warning(default: 4102) // unreferenced label
        }
        return -1;
    }

    void releaseOne(int idx)
    {
        const int intIdx = idx / 64;
        const int bitIdx = idx % 64;
        if constexpr (ThreadSafe)
        {
            do
            {
                const uint64 old = oc::atomic_ref<uint64>(m_pBits[intIdx]).fetch_and(~(1ull << bitIdx), oc::memory_order_relaxed);
                if ((old & (1ull << bitIdx)) == 0)
                    return;
            } while (true);
        }
        else
        {
            assert((m_pBits[intIdx] & (1ull << bitIdx)) != 0);
            m_pBits[intIdx] &= ~(1ull << bitIdx);
        }
        m_lastAcquiredIdx = intIdx;
    }

    int acquireRange(uint32 size)
    {
        const int numBucketsWanted = size;
        int numWantedBucketsRemaining = numBucketsWanted;
        int continuousBitStart = -1;
        int startSlot;
        const int numInts = (m_size + 63) / 64;

        if constexpr (ThreadSafe)
        {	// In a multithreaded environment we spread out different thread allocations to minimize contention
            const int lastUsed = oc::atomic_ref<uint32>(m_lastAcquiredIdx).load(oc::memory_order_relaxed);
            int emptiestSlotBits = 64;
            startSlot = 0;
            for (int i = 0; i < numInts; ++i)
            {
                const int intIdx = (i + lastUsed + 1) % numInts;
                const int numSetBits = (int)__popcnt64(m_pBits[intIdx].load(oc::memory_order_relaxed));
                if (numSetBits + numBucketsWanted * 2 < 64) // If we find a slot that can comfortably fit the allocation try to use it
                {
                    startSlot = intIdx;
                    break;
                }
                if (numSetBits < emptiestSlotBits) // otherwise find the emptiest slot
                {
                    startSlot = intIdx;
                    emptiestSlotBits = numSetBits;
                }
            }
            oc::atomic_ref<uint32>(m_lastAcquiredIdx).store(startSlot, oc::memory_order_relaxed);
        }
        else
        {
            startSlot = m_lastAcquiredIdx; // In a single threaded environment we try to make allocations as linear as possible
        }

        for (int i = 0; i < numInts; ++i)
        {
            const int intIdx = (i + startSlot) % numInts;
            if (intIdx == 0) // If we looped around make sure to break the continuous range
            {
                continuousBitStart = -1;
                numWantedBucketsRemaining = numBucketsWanted;
            }
            uint64_t usedBits;
            if constexpr (ThreadSafe) usedBits = oc::atomic_ref<uint64>(m_pBits[i]).load(oc::memory_order_relaxed);
            else                      usedBits = m_pBits[intIdx];

            const int numSetBits = (int)__popcnt64(usedBits);
            if (numSetBits == 0) // optimize for empty buckets
            {
                continuousBitStart = continuousBitStart == -1 ? intIdx * 64 : continuousBitStart;
                numWantedBucketsRemaining -= 64;
            }
            else
            {
                int startBitIdx = (int)_tzcnt_u64(~usedBits); // std::countr_xxx functions have bad performance
                if (startBitIdx != 0)
                {
                    continuousBitStart = -1;
                    numWantedBucketsRemaining = numBucketsWanted;
                }
                while (startBitIdx < 64)
                {
                    const uint64_t ignoreMask = startBitIdx ? ~((1ull << (64 - startBitIdx)) - 1) : 0;
                    const int numZeroes = (int)_tzcnt_u64((usedBits >> startBitIdx) | ignoreMask);
                    numWantedBucketsRemaining -= numZeroes;
                    if (numWantedBucketsRemaining <= 0 || startBitIdx + numZeroes == 64) // Fits completely or to the end
                    {
                        continuousBitStart = continuousBitStart == -1 ? intIdx * 64 + startBitIdx : continuousBitStart;
                        break;
                    }
                    else // Does not fit to the end, find next start pos
                    {
                        startBitIdx += (int)_tzcnt_u64(~(usedBits >> (startBitIdx + numZeroes))) + numZeroes;
                        continuousBitStart = -1;
                        numWantedBucketsRemaining = numBucketsWanted;
                    }
                }
            }

            if (numWantedBucketsRemaining <= 0) // Fully fitted
            {
                m_lastAcquiredIdx = intIdx; // Write unsynchronized
                if (!setBitRange(continuousBitStart, continuousBitStart + numBucketsWanted))
                {	// Someone else set bits in the range, try find new range from the current position
                    continuousBitStart = -1;
                    numWantedBucketsRemaining = numBucketsWanted;
                    startSlot = i;
                    i = -1;
                    continue;
                }
                return continuousBitStart;
            }
        }
        return -1;
    }

    bool isBitSet(int idx)
    {
        const int intIdx = idx / 64;
        const int bitIdx = idx % 64;
        return (m_pBits[intIdx] & (1ull << bitIdx)) != 0;
    }

    void releaseRange(int idx, uint32 size)
    {
        clearBitRange(idx, idx + size);
        m_lastAcquiredIdx = idx / 64;
    }

private:

    bool setBitRange(int start, int end)
    {
        const int intStart = start / 64;
        const int intEnd = end / 64 + (end % 64 != 0);
        int remaining = end - start;
        int bitStart = start % 64;
        uint64* bitMasks = (uint64*)_alloca(((m_size + 63) / 64) * sizeof(uint64));
        for (int i = intStart; i < intEnd; ++i)
        {
            const int bitEnd = oc::min(bitStart + remaining, 64);
            const int bitRange = bitEnd - bitStart;
            remaining -= bitRange;
            bitMasks[i] = bitRange == 64 ? uint64(~0) : ((1ull << bitRange) - 1) << bitStart;
            bitStart = 0;
        }

        if constexpr (ThreadSafe)
        {
            for (int i = intStart; i < intEnd; ++i)
            {
                // With extremely high contention a compare_exchange_weak loop can be slightly faster than fetch_or
                const uint64 old = oc::atomic_ref<uint64>(m_pBits[i]).fetch_or(bitMasks[i], oc::memory_order_relaxed);
                const uint64 oldBitMask = old & bitMasks[i];
                if (oldBitMask != 0)
                {	// Undo bit sets because someone else has set bits in the range
                    if (oldBitMask != bitMasks[i]) // if we set any incorrectly in the current int, undo
                        oc::atomic_ref<uint64>(m_pBits[i]).fetch_and(~(bitMasks[i] & ~oldBitMask), oc::memory_order_relaxed);
                    for (int j = i - 1; j >= intStart; --j) // if we set any previous ints, undo those also
                        oc::atomic_ref<uint64>(m_pBits[j]).fetch_and(~bitMasks[j], oc::memory_order_relaxed);
                    return false;
                }
            }
        }
        else
        {
            for (int i = intStart; i < intEnd; ++i)
            {
                assert((m_pBits[i] & bitMasks[i]) == 0);
                m_pBits[i] |= bitMasks[i];
            }
        }
        return true;
    }

    void clearBitRange(int start, int end)
    {
        const int intStart = start / 64;
        const int intEnd = end / 64 + ((end % 64) != 0);
        int remaining = end - start;
        int bitStart = start % 64;
        // prepare masks to minimize time between CAS operations
        uint64* bitMasks = (uint64*)_alloca(((m_size + 63) / 64) * sizeof(uint64));
        for (int i = intStart; i < intEnd; ++i)
        {
            const int bitEnd = oc::min(bitStart + remaining, 64);
            const int bitRange = bitEnd - bitStart;
            remaining -= bitRange;
            bitMasks[i] = bitRange == 64 ? uint64(~0) : ((1ull << bitRange) - 1) << bitStart;
            bitStart = 0;
        }
        for (int i = intStart; i < intEnd; ++i)
        {
            if constexpr (ThreadSafe)
            {
                uint64_t old = oc::atomic_ref<uint64>(m_pBits[i]).fetch_and(~bitMasks[i], oc::memory_order_relaxed);
                assert((old & bitMasks[i]) == bitMasks[i]);
            }
            else
            {
                assert((m_pBits[i] & bitMasks[i]) == bitMasks[i]);
                m_pBits[i] &= ~bitMasks[i];
            }
        }
    }

    oc::unique_ptr<uint64[]> m_pBits;
    uint32 m_size = 0;
    uint32 m_lastAcquiredIdx = 0;
};