export module Core.BitRangeAllocator;

import Core;

// ThreadSafe: acquire* / release* may run concurrently (atomic bit words, lock-free). resize() and isBitSet()
// are NOT concurrent-safe: resize() replaces the bit array, so the owner must exclude every other call around it
// (MeshDataManager: its grow lock).
export OC_FWD_ATTACH template<bool ThreadSafe = false>
class BitRangeAllocator final
{
public:

    BitRangeAllocator(uint32 size)
    {
        m_size = size;
        m_pBits = oc::make_unique<uint64[]>((size + 63) / 64);
        reserveTail();
    }
    ~BitRangeAllocator() {};
    BitRangeAllocator(const BitRangeAllocator&) = delete;

    void resize(uint32 size)
    {
        if (size < m_size)
            return;
        const uint32 oldNumInts = (m_size + 63) / 64;
        auto newBits = oc::make_unique<uint64[]>((size + 63) / 64); // value-init: grown tail starts free
        memcpy(newBits.get(), m_pBits.get(), oldNumInts * sizeof(uint64));
        if (m_size % 64 != 0)
            newBits[m_size / 64] &= ~(~0ull << (m_size % 64)); // the old tail padding becomes real slots
        m_pBits = oc::move(newBits);
        m_size = size;
        reserveTail();
    }

    int acquireOne()
    {
        const int numInts = (m_size + 63) / 64;
        int startSlot;
        if constexpr (ThreadSafe)
            startSlot = (int)lastAcquired() + 1; // In a multithreaded environment we spread out different allocations to minimize contention
        else
            startSlot = m_lastAcquiredIdx; // In a single threaded environment we try to make allocations as linear as possible

        for (int i = 0; i < numInts; ++i)
        {
            const int intIdx = (i + startSlot) % numInts;
            const uint64 usedBits = loadBits(intIdx);
            if (usedBits != ~0ull)
            {
#pragma warning(disable: 4102) // unreferenced label
            retry:
                const int bitIdx = (int)oc::tzcnt(~usedBits);
                const int idx = intIdx * 64 + bitIdx;
                if constexpr (ThreadSafe)
                {
                    const uint64 old = oc::atomic_ref<uint64>(m_pBits[intIdx]).fetch_or(1ull << bitIdx, oc::memory_order_relaxed);
                    if ((old & (1ull << bitIdx)) == 0)
                    {
                        setLastAcquired(intIdx);
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
        setLastAcquired(intIdx);
    }

    int acquireRange(uint32 size)
    {
        const int numBucketsWanted = size;
        int numWantedBucketsRemaining = numBucketsWanted;
        int continuousBitStart = -1;
        int startSlot;
        const int numInts = (m_size + 63) / 64;

        if constexpr (ThreadSafe)
        {
            // WHERE TO START: a SMALL range (under a word) goes into a word that is already PARTLY used and holds a free run of
            // its length - empty words stay whole for the ranges that span words. Only without one: the first empty word, else
            // the emptiest. The search begins past the last acquired word, so threads still spread out. (Any word with
            // "comfortable" room, empty ones included, sprinkled the small ranges over every word: MeshDataManager's vertex
            // pool grew at 244 of 384 MB used, its longest free run one word.)
            const int lastUsed = (int)lastAcquired();
            int partlyUsed = -1, firstEmpty = -1, emptiest = 0, emptiestBits = 65;
            for (int i = 0; i < numInts; ++i)
            {
                const int intIdx = (i + lastUsed + 1) % numInts;
                const uint64 usedBits = loadBits(intIdx);
                const int numSetBits = (int)oc::popcnt(usedBits);
                if (numSetBits == 0)
                {
                    if (firstEmpty < 0)
                        firstEmpty = intIdx;
                    if (numBucketsWanted >= 64)
                        break; // a large range starts at the first empty word
                    continue;
                }
                if (numBucketsWanted < 64 && 64 - numSetBits >= numBucketsWanted && hasFreeRun(usedBits, numBucketsWanted))
                {
                    partlyUsed = intIdx;
                    break;
                }
                if (numSetBits < emptiestBits)
                {
                    emptiestBits = numSetBits;
                    emptiest = intIdx;
                }
            }
            // A large range starts one word BEFORE the first empty one: a free tail there joins the run (from the empty word,
            // a run that began in the word before was cut short - a 65-bucket range failed in a free run of 127).
            startSlot = partlyUsed >= 0 ? partlyUsed : firstEmpty > 0 && numBucketsWanted >= 64 ? firstEmpty - 1 : firstEmpty >= 0 ? firstEmpty : emptiest;
            setLastAcquired(startSlot);
        }
        else
        {
            startSlot = m_lastAcquiredIdx; // In a single threaded environment we try to make allocations as linear as possible
        }

        // Past one full lap by the words a range spans: a free run that holds the start word, but began before it, is then
        // seen whole when the search comes round again.
        const int numSteps = numInts + (numBucketsWanted + 63) / 64 + 1;
        for (int i = 0; i < numSteps; ++i)
        {
            const int intIdx = (i + startSlot) % numInts;
            if (intIdx == 0) // If we looped around make sure to break the continuous range
            {
                continuousBitStart = -1;
                numWantedBucketsRemaining = numBucketsWanted;
            }
            const uint64_t usedBits = loadBits(intIdx);

            const int numSetBits = (int)oc::popcnt(usedBits);
            if (numSetBits == 0) // optimize for empty buckets
            {
                continuousBitStart = continuousBitStart == -1 ? intIdx * 64 : continuousBitStart;
                numWantedBucketsRemaining -= 64;
            }
            else
            {
                int startBitIdx = (int)oc::tzcnt(~usedBits); // oc::tzcnt, not std::countr_zero: no CPU dispatch
                if (startBitIdx != 0)
                {
                    continuousBitStart = -1;
                    numWantedBucketsRemaining = numBucketsWanted;
                }
                while (startBitIdx < 64)
                {
                    const uint64_t ignoreMask = startBitIdx ? ~((1ull << (64 - startBitIdx)) - 1) : 0;
                    const int numZeroes = (int)oc::tzcnt((usedBits >> startBitIdx) | ignoreMask);
                    numWantedBucketsRemaining -= numZeroes;
                    if (numWantedBucketsRemaining <= 0 || startBitIdx + numZeroes == 64) // Fits completely or to the end
                    {
                        continuousBitStart = continuousBitStart == -1 ? intIdx * 64 + startBitIdx : continuousBitStart;
                        break;
                    }
                    else // Does not fit to the end, find next start pos
                    {
                        startBitIdx += (int)oc::tzcnt(~(usedBits >> (startBitIdx + numZeroes))) + numZeroes;
                        continuousBitStart = -1;
                        numWantedBucketsRemaining = numBucketsWanted;
                    }
                }
            }

            if (numWantedBucketsRemaining <= 0) // Fully fitted
            {
                setLastAcquired(intIdx);
                if (!setBitRange(continuousBitStart, continuousBitStart + numBucketsWanted))
                {	// Someone else set bits in the range, try find new range from the current position
                    continuousBitStart = -1;
                    numWantedBucketsRemaining = numBucketsWanted;
                    startSlot = intIdx; // the WORD index: `i` counts from startSlot, so restarting at `i` jumped elsewhere
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
        return (loadBits(intIdx) & (1ull << bitIdx)) != 0;
    }

    void releaseRange(int idx, uint32 size)
    {
        clearBitRange(idx, idx + size);
        setLastAcquired((uint32)(idx / 64));
    }

private:

    // The bits past m_size in the last word stay SET, so the word-granular searches never hand them out: the owner's
    // storage ends at exactly m_size slots (rounding the size up to whole words gave MeshDataManager ranges past the
    // end of its buffer).
    void reserveTail()
    {
        if (m_size % 64 != 0)
            m_pBits[m_size / 64] |= ~0ull << (m_size % 64);
    }

    // Whether a word's FREE bits hold a run of n (1..63) consecutive ones: the free mask ANDed with itself shifted, the
    // run lengths doubling (log2(n) steps).
    static bool hasFreeRun(uint64 usedBits, int n)
    {
        uint64 m = ~usedBits;
        int have = 1;
        while (have < n && m != 0)
        {
            const int step = oc::min(have, n - have);
            m &= m >> step;
            have += step;
        }
        return m != 0;
    }

    uint64 loadBits(int intIdx) const
    {
        if constexpr (ThreadSafe)
            return oc::atomic_ref<uint64>(m_pBits[intIdx]).load(oc::memory_order_relaxed);
        else
            return m_pBits[intIdx];
    }
    // A search hint only: relaxed is enough, but concurrent plain accesses would be a data race.
    uint32 lastAcquired() const
    {
        if constexpr (ThreadSafe)
            return oc::atomic_ref<uint32>(const_cast<uint32&>(m_lastAcquiredIdx)).load(oc::memory_order_relaxed);
        else
            return m_lastAcquiredIdx;
    }
    void setLastAcquired(uint32 intIdx)
    {
        if constexpr (ThreadSafe)
            oc::atomic_ref<uint32>(m_lastAcquiredIdx).store(intIdx, oc::memory_order_relaxed);
        else
            m_lastAcquiredIdx = intIdx;
    }

    // The masks of words [intStart, intEnd), indexed from intStart: sized by the RANGE, not the whole bitmap (a
    // large mesh buffer's bitmap is tens of KB - on a 64 KB-commit fiber stack, per allocation).
    static void buildMasks(int start, int end, int intStart, int intEnd, uint64* bitMasks)
    {
        int remaining = end - start;
        int bitStart = start % 64;
        for (int i = intStart; i < intEnd; ++i)
        {
            const int bitEnd = oc::min(bitStart + remaining, 64);
            const int bitRange = bitEnd - bitStart;
            remaining -= bitRange;
            bitMasks[i - intStart] = bitRange == 64 ? uint64(~0) : ((1ull << bitRange) - 1) << bitStart;
            bitStart = 0;
        }
    }

    bool setBitRange(int start, int end)
    {
        const int intStart = start / 64;
        const int intEnd = end / 64 + (end % 64 != 0);
        uint64* bitMasks = (uint64*)_alloca((size_t)(intEnd - intStart) * sizeof(uint64));
        buildMasks(start, end, intStart, intEnd, bitMasks);

        if constexpr (ThreadSafe)
        {
            for (int i = intStart; i < intEnd; ++i)
            {
                const uint64 mask = bitMasks[i - intStart];
                // With extremely high contention a compare_exchange_weak loop can be slightly faster than fetch_or
                const uint64 old = oc::atomic_ref<uint64>(m_pBits[i]).fetch_or(mask, oc::memory_order_relaxed);
                const uint64 oldBitMask = old & mask;
                if (oldBitMask != 0)
                {	// Undo bit sets because someone else has set bits in the range
                    if (oldBitMask != mask) // if we set any incorrectly in the current int, undo
                        oc::atomic_ref<uint64>(m_pBits[i]).fetch_and(~(mask & ~oldBitMask), oc::memory_order_relaxed);
                    for (int j = i - 1; j >= intStart; --j) // if we set any previous ints, undo those also
                        oc::atomic_ref<uint64>(m_pBits[j]).fetch_and(~bitMasks[j - intStart], oc::memory_order_relaxed);
                    return false;
                }
            }
        }
        else
        {
            for (int i = intStart; i < intEnd; ++i)
            {
                assert((m_pBits[i] & bitMasks[i - intStart]) == 0);
                m_pBits[i] |= bitMasks[i - intStart];
            }
        }
        return true;
    }

    void clearBitRange(int start, int end)
    {
        const int intStart = start / 64;
        const int intEnd = end / 64 + ((end % 64) != 0);
        // prepare masks to minimize time between CAS operations
        uint64* bitMasks = (uint64*)_alloca((size_t)(intEnd - intStart) * sizeof(uint64));
        buildMasks(start, end, intStart, intEnd, bitMasks);
        for (int i = intStart; i < intEnd; ++i)
        {
            const uint64 mask = bitMasks[i - intStart];
            if constexpr (ThreadSafe)
            {
                uint64_t old = oc::atomic_ref<uint64>(m_pBits[i]).fetch_and(~mask, oc::memory_order_relaxed);
                assert((old & mask) == mask);
            }
            else
            {
                assert((m_pBits[i] & mask) == mask);
                m_pBits[i] &= ~mask;
            }
        }
    }

    oc::unique_ptr<uint64[]> m_pBits;
    uint32 m_size = 0;
    uint32 m_lastAcquiredIdx = 0;
};