export module Core.Allocator;

#if 0 //defined(_DEBUG)
#define CHECK_BOUNDS
#endif

#if 0 //defined(_DEBUG)
#define CHECK_FULL_MEMORY
#endif

#if defined(_DEBUG)
#define TRACK_ALLOCATION_SIZE
#endif

#if 0 //defined(_DEBUG)
#define CHECK_DIFFERENT_THREAD_ACCESS_FOR_NON_THREADSAFE
#endif

import Core;

oc::atomic<size_t> g_alignedAllocMemory = 0;
export size_t getAlignedAllocatedSize() { return g_alignedAllocMemory.load(); }

// Allocation tracking hooks (installed by the Memory library's MemoryTracker; Core stays
// dependency-free). Called on EVERY allocate/deallocate that flows through the engine allocators
// and the aligned operator new/delete paths - null until installed, one relaxed load + branch of
// overhead. Contract: the alloc hook fires AFTER a successful allocation, the free hook BEFORE the
// memory is actually released (so a recycled address can never race its own map entry). Hook
// implementations must never allocate through these paths themselves.
export using MemoryAllocHook = void(*)(void* ptr, size_t size);
export using MemoryFreeHook = void(*)(void* ptr);
export oc::atomic<MemoryAllocHook> g_memoryAllocHook = nullptr;
export oc::atomic<MemoryFreeHook> g_memoryFreeHook = nullptr;

// Per-thread hook suppression for tracking INFRASTRUCTURE allocations (a profiler track + its
// ring is allocated on a freshly registered thread BEFORE that thread's TLS track pointer exists,
// so tracking it would misfile permanent memory under "<other threads>"/"<unscoped>"). Only for
// allocations that are never freed - a suppressed alloc that later frees unsuppressed is a
// harmless map miss, but a tracked alloc freed under suppression would leak its counts.
export inline thread_local int32 g_memoryHookSuppress = 0;
export struct MemoryHookSuppress final
{
    MemoryHookSuppress() { ++g_memoryHookSuppress; }
    ~MemoryHookSuppress() { --g_memoryHookSuppress; }
};

export inline void callMemoryAllocHook(void* ptr, size_t size)
{
    if (MemoryAllocHook hook = g_memoryAllocHook.load(oc::memory_order_relaxed)) [[unlikely]]
        if (g_memoryHookSuppress == 0)
            hook(ptr, size);
}
export inline void callMemoryFreeHook(void* ptr)
{
    if (MemoryFreeHook hook = g_memoryFreeHook.load(oc::memory_order_relaxed)) [[unlikely]]
        if (g_memoryHookSuppress == 0)
            hook(ptr);
}

export template<typename T, typename Alloc>
class STLAllocator final
{
public:
    using value_type = T;

    STLAllocator(Alloc& allocator) : m_allocator(allocator) {}
    template<typename U>
    STLAllocator(const STLAllocator<U, Alloc>& other) noexcept : m_allocator(other.m_allocator) {}
    ~STLAllocator() {}

    [[nodiscard]] T* allocate(std::size_t n)
    {
        if constexpr (alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return reinterpret_cast<T*>(m_allocator.allocate(n));
        else
        {
            g_alignedAllocMemory += n;
            T* p = reinterpret_cast<T*>(_aligned_malloc(n, alignof(T)));
            callMemoryAllocHook(p, n);
            return p;
        }
    }
    void deallocate(T* p, [[maybe_unused]] std::size_t n = 0)
    {
        if constexpr (alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            m_allocator.deallocate((void*)p);
        else
        {
            callMemoryFreeHook(p);
            g_alignedAllocMemory -= _aligned_msize(p, alignof(T), 0);
            _aligned_free(p);
        }
    }

    Alloc& m_allocator;
};

struct AllocationHeader
{
    static constexpr uint32 CHECK = 0x69696969;
    void* pAllocator;
    uint32 size;
    uint32 checkVal;
};
static_assert(sizeof(AllocationHeader) == __STDCPP_DEFAULT_NEW_ALIGNMENT__);

#ifdef CHECK_BOUNDS
struct AllocationTail
{
    static constexpr uint32 CHECK = 0x78787878;
    uint32 checkVal;
};
#endif

export constexpr size_t getStackAllocatorOverhead()
{
    return 0 +
#ifdef CHECK_BOUNDS
        sizeof(AllocationTail) +
#endif
        sizeof(AllocationHeader);
}

export class Allocator
{
public:

    virtual ~Allocator() = default;

    [[nodiscard]]
    virtual void* allocate(size_t size)
    {
        const size_t sizeWithHeader = size
#ifdef CHECK_BOUNDS
            + sizeof(AllocationTail)
#endif
            + sizeof(AllocationHeader);
        AllocationHeader* pAllocation = static_cast<AllocationHeader*>(std::malloc(sizeWithHeader));
        pAllocation->pAllocator = this;
        pAllocation->size = (uint32)sizeWithHeader;
#ifdef TRACK_ALLOCATION_SIZE
        m_usedSize += size
#ifdef CHECK_BOUNDS
            + sizeof(AllocationTail)
#endif
            + sizeof(AllocationHeader);
        m_maxUsedSize = m_usedSize > m_maxUsedSize ? (size_t)m_usedSize : m_maxUsedSize;
#endif

#ifdef CHECK_BOUNDS
        pAllocation->checkVal = AllocationHeader::CHECK;
        AllocationTail* pTail = (AllocationTail*)((uint8_t*)(pAllocation)+sizeWithHeader - sizeof(AllocationTail));
        pTail->checkVal = AllocationTail::CHECK;
#endif
        callMemoryAllocHook(pAllocation + 1, size);
        return pAllocation + 1;
    }

    virtual void deallocate(void* ptr)
    {
        callMemoryFreeHook(ptr); // before the release: a recycled address must not race its own tracking entry
        AllocationHeader* pAllocation = static_cast<AllocationHeader*>(ptr) - 1;
#ifdef TRACK_ALLOCATION_SIZE
        m_usedSize -= pAllocation->size;
#endif
#ifdef CHECK_BOUNDS
        AllocationTail* pTail = (AllocationTail*)(((uint8_t*)pAllocation) + pAllocation->size - sizeof(AllocationTail));
        if (pAllocation->checkVal != AllocationHeader::CHECK) { assert(false && "Buffer underflow detected!"); __debugbreak(); }
        if (pTail->checkVal != AllocationTail::CHECK) { assert(false && "Buffer overflow detected!"); __debugbreak(); }
#endif
        std::free(pAllocation);
    }

    inline static void globalFree(void* ptr)
    {
        AllocationHeader* pAllocation = reinterpret_cast<AllocationHeader*>(ptr) - 1;
        Allocator* pAllocator = static_cast<Allocator*>(pAllocation->pAllocator);
        pAllocator->deallocate(ptr);
    }

    template<typename T>
    using toStd = STLAllocator<T, Allocator>;

    constexpr size_t getCapacity() const { return 0; }
    constexpr size_t getBucketSize() const { return 0; }
    constexpr bool isThreadSafe() const { return false; }

    inline size_t getUsedSize() const { return m_usedSize; }
    inline size_t getMaxUsedSize() const { return m_maxUsedSize; }

    oc::atomic<size_t> m_usedSize = 0;
    size_t m_maxUsedSize = 0;
};

export template<size_t Capacity, size_t BucketSize, bool ThreadSafe = false>
class StackAllocator final : public Allocator
{
public:
    StackAllocator()
    {   // Set the last bits to 1 in case capacity is not a multiple of bucket size
        const int remainder = NUM_BUCKETS % 64;
        m_usedBits[NUM_BUCKET_INTS - 1] = remainder ? ~((1ull << remainder) - 1) : 0;
#ifdef CHECK_FULL_MEMORY
        memset(m_buffer, 'f', Capacity);
#endif
    }
    virtual ~StackAllocator() {}

    static void globalFree(void* ptr)
    {
        AllocationHeader* pAllocation = reinterpret_cast<AllocationHeader*>(ptr) - 1;
        StackAllocator* pAllocator = static_cast<StackAllocator*>(pAllocation->pAllocator);
        pAllocator->deallocate(ptr);
    }

    [[nodiscard]]
    virtual void* allocate(size_t size) override
    {
        const size_t sizeWithHeader = size
#ifdef CHECK_BOUNDS
            + sizeof(AllocationTail)
#endif
            + sizeof(AllocationHeader);

        const int start = acquireRange(sizeWithHeader);
        AllocationHeader* pAllocation;
        if (start != -1)
        {
            pAllocation = reinterpret_cast<AllocationHeader*>(m_buffer + start * BucketSize);
#ifdef TRACK_ALLOCATION_SIZE
            if constexpr (ThreadSafe) m_usedStackSize.fetch_add(sizeWithHeader, oc::memory_order_relaxed);
            else                      m_usedStackSize += sizeWithHeader;
            m_maxUsedStackSize = m_usedStackSize > m_maxUsedStackSize ? (size_t)m_usedStackSize : (size_t)m_maxUsedStackSize;
#endif
        }
        else
        {
            pAllocation = reinterpret_cast<AllocationHeader*>(std::malloc(sizeWithHeader));
#ifdef TRACK_ALLOCATION_SIZE
            if constexpr (ThreadSafe) m_usedFallbackSize.fetch_add(sizeWithHeader, oc::memory_order_relaxed);
            else                      m_usedFallbackSize += sizeWithHeader;
            m_maxUsedFallbackSize = m_usedFallbackSize > m_maxUsedFallbackSize ? (size_t)m_usedFallbackSize : (size_t)m_maxUsedFallbackSize;
#endif
        }
#ifdef CHECK_FULL_MEMORY
        for (int i = start * BucketSize; start != -1 && i < start * BucketSize + sizeWithHeader; ++i)
            if (m_buffer[i] != 'f') { __debugbreak(); assert(false); }
#endif
        pAllocation->pAllocator = this;
        pAllocation->size = (uint32)sizeWithHeader;
#ifdef CHECK_BOUNDS
        pAllocation->checkVal = AllocationHeader::CHECK;
        AllocationTail* pTail = (AllocationTail*)((uint8_t*)(pAllocation + 1) + size);
        pTail->checkVal = AllocationTail::CHECK;
#endif
        callMemoryAllocHook(pAllocation + 1, size);
        return reinterpret_cast<void*>(pAllocation + 1);
    }

    bool validateBuffer(void* ptr)
    {
        AllocationHeader* pAllocation = reinterpret_cast<AllocationHeader*>(ptr) - 1;
        if (pAllocation->checkVal != AllocationHeader::CHECK) { return false; }
#ifdef CHECK_BOUNDS
        AllocationTail* pTail = (AllocationTail*)((uint8_t*)(pAllocation)+pAllocation->size - sizeof(AllocationTail));
        if (pTail->checkVal != AllocationTail::CHECK) { return false; }
#endif
        return true;
    }

    virtual void deallocate(void* ptr) override
    {
        callMemoryFreeHook(ptr); // before the release: a recycled address must not race its own tracking entry
        AllocationHeader* pAllocation = reinterpret_cast<AllocationHeader*>(ptr) - 1;
        const size_t size = pAllocation->size;

#ifdef CHECK_BOUNDS
        AllocationTail* pTail = (AllocationTail*)((uint8_t*)(pAllocation)+size - sizeof(AllocationTail));
        if (pAllocation->checkVal != AllocationHeader::CHECK) { assert(false && "Buffer underflow detected!"); __debugbreak(); }
        if (pTail->checkVal != AllocationTail::CHECK) { assert(false && "Buffer overflow detected!"); __debugbreak(); }
#endif

        uint8_t* pMem = reinterpret_cast<uint8_t*>(pAllocation);
#ifdef CHECK_FULL_MEMORY
        memset(pMem, 'f', size);
#endif
        if (pMem >= m_buffer && pMem < m_buffer + Capacity)
        {
            assert(pMem + size <= m_buffer + Capacity);
#ifdef TRACK_ALLOCATION_SIZE
            if constexpr (ThreadSafe) m_usedStackSize.fetch_sub(size, oc::memory_order_relaxed);
            else                      m_usedStackSize -= size;
#endif
            releaseRange(pMem, size);
        }
        else
        {
#ifdef TRACK_ALLOCATION_SIZE
            if constexpr (ThreadSafe) m_usedFallbackSize.fetch_sub(size, oc::memory_order_relaxed);
            else                      m_usedFallbackSize -= size;
#endif
            std::free(pMem);
        }
    }

    template<typename T>
    using toStd = STLAllocator<T, StackAllocator<Capacity, BucketSize, ThreadSafe>>;

    constexpr size_t getCapacity() const { return Capacity; }
    constexpr size_t getBucketSize() const { return BucketSize; }
    constexpr bool isThreadSafe() const { return ThreadSafe; }
#ifdef TRACK_ALLOCATION_SIZE
    inline size_t getUsedStackSize() const { return m_usedStackSize; }
    inline size_t getUsedFallbackSize() const { return m_usedFallbackSize; }
    inline size_t getUsedSize() const { return m_usedStackSize + m_usedFallbackSize; }
    inline size_t getMaxUsedStackSize() const { return m_maxUsedStackSize; }
    inline size_t getMaxUsedFallbackSize() const { return m_maxUsedFallbackSize; }
#else
    inline size_t getUsedStackSize() const
    {
        size_t numUsedBits = 0;
        for (int i = 0; i < NUM_BUCKET_INTS; ++i)
            numUsedBits += __popcnt64(m_usedBits[i]);
        return numUsedBits * BucketSize;
    }
#endif

    void printUsedBits() const
    {
        for (int i = 0; i < NUM_BUCKET_INTS; ++i)
            printf("%s\n", oc::bitset<64>(m_usedBits[i]).to_string().c_str());
    }

private:

    int acquireRange(size_t size)
    {
#ifdef CHECK_DIFFERENT_THREAD_ACCESS_FOR_NON_THREADSAFE
        if constexpr (!ThreadSafe)
        {
            if (checkDifferentThreadAccess())
            {
                __debugbreak();
                assert(false && "Detected different thread allocation in non-threadsafe allocator!");
                return -1;
            }
        }
#endif
        const int numBucketsWanted = (int)(size / BucketSize + (size % BucketSize != 0));
        int numWantedBucketsRemaining = numBucketsWanted;
        int continuousBitStart = -1;
        int startSlot;

        if constexpr (ThreadSafe)
        {	// In a multithreaded environment we spread out different thread allocations to minimize contention
            const int lastUsed = m_lastUsedIdx.load(oc::memory_order_relaxed);
            int emptiestSlotBits = 64;
            startSlot = 0;
            for (int i = 0; i < NUM_BUCKET_INTS; ++i)
            {
                const int intIdx = (i + lastUsed + 1) % (NUM_BUCKET_INTS);
                const int numSetBits = (int)__popcnt64(m_usedBits[intIdx].load(oc::memory_order_relaxed));
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
            m_lastUsedIdx.store(startSlot, oc::memory_order_relaxed);
        }
        else
        {
            startSlot = m_lastUsedIdx; // In a single threaded environment we try to make allocations as linear as possible
        }

        for (int i = 0; i < NUM_BUCKET_INTS; ++i)
        {
            const int intIdx = (i + startSlot) % (NUM_BUCKET_INTS);
            if (intIdx == 0) // If we looped around make sure to break the continuous range
            {
                continuousBitStart = -1;
                numWantedBucketsRemaining = numBucketsWanted;
            }
            uint64_t usedBits;
            if constexpr (ThreadSafe) usedBits = m_usedBits[intIdx].load(oc::memory_order_relaxed);
            else                      usedBits = m_usedBits[intIdx];

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
                reinterpret_cast<int&>(m_lastUsedIdx) = intIdx; // Write unsynchronized
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

    void releaseRange(uint8_t* pMem, size_t size)
    {
        const int offset = (int)(pMem - m_buffer);
        const int start = offset / BucketSize;
        const int end = start + (int)((size / BucketSize) + (size % BucketSize != 0));
        clearBitRange(start, end);
    }

    bool setBitRange(int start, int end)
    {
        const int intStart = start / 64;
        const int intEnd = end / 64 + (end % 64 != 0);
        int remaining = end - start;
        int bitStart = start % 64;
        // prepare masks to minimize time between atomic operations
        uint64_t bitMasks[NUM_BUCKET_INTS];
        for (int i = intStart; i < intEnd; ++i)
        {
            const int bitEnd = oc::min(bitStart + remaining, 64);
            const int bitRange = bitEnd - bitStart;
            remaining -= bitRange;
            bitMasks[i] = bitRange == 64 ? uint64_t(~0) : ((1ull << bitRange) - 1) << bitStart;
            bitStart = 0;
        }

        if constexpr (ThreadSafe)
        {
            for (int i = intStart; i < intEnd; ++i)
            {
                // With extremely high contention a compare_exchange_weak loop can be slightly faster than fetch_or
                const uint64_t old = m_usedBits[i].fetch_or(bitMasks[i], oc::memory_order_relaxed);
                const uint64_t oldBitMask = old & bitMasks[i];
                if (oldBitMask != 0)
                {	// Undo bit sets because someone else has set bits in the range
                    if (oldBitMask != bitMasks[i]) // if we set any incorrectly in the current int, undo
                        m_usedBits[i].fetch_and(~(bitMasks[i] & ~oldBitMask), oc::memory_order_relaxed);
                    for (int j = i - 1; j >= intStart; --j) // if we set any previous ints, undo those also
                        m_usedBits[j].fetch_and(~bitMasks[j], oc::memory_order_relaxed);
                    return false;
                }
            }
        }
        else
        {
            for (int i = intStart; i < intEnd; ++i)
            {
                assert((m_usedBits[i] & bitMasks[i]) == 0);
                m_usedBits[i] |= bitMasks[i];
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
        uint64_t bitMasks[NUM_BUCKET_INTS];
        for (int i = intStart; i < intEnd; ++i)
        {
            const int bitEnd = oc::min(bitStart + remaining, 64);
            const int bitRange = bitEnd - bitStart;
            remaining -= bitRange;
            bitMasks[i] = bitRange == 64 ? uint64_t(~0) : ((1ull << bitRange) - 1) << bitStart;
            bitStart = 0;
        }
        for (int i = intStart; i < intEnd; ++i)
        {
            if constexpr (ThreadSafe)
            {
                uint64_t old = m_usedBits[i].fetch_and(~bitMasks[i], oc::memory_order_relaxed);
                assert((old & bitMasks[i]) == bitMasks[i]);
            }
            else
            {
                assert((m_usedBits[i] & bitMasks[i]) == bitMasks[i]);
                m_usedBits[i] &= ~bitMasks[i];
            }
        }
    }

    inline bool checkDifferentThreadAccess()
    {
#ifndef CHECK_DIFFERENT_THREAD_ACCESS_FOR_NON_THREADSAFE
        if constexpr (!ThreadSafe)
            return false;
#endif
        const std::thread::id currentThreadId = std::this_thread::get_id();
        const std::thread::id lastThreadId = m_lastThreadId;
        m_lastThreadId = currentThreadId;
        if (lastThreadId != std::thread::id() && currentThreadId != lastThreadId)
            return true;
        return false;
    }

private:

    static constexpr int NUM_BUCKETS = Capacity / BucketSize + (Capacity % BucketSize != 0);
    static constexpr int NUM_BUCKET_INTS = NUM_BUCKETS / 64 + (NUM_BUCKETS % 64 != 0);

    uint8_t m_buffer[Capacity];
    std::conditional<ThreadSafe, oc::atomic<uint64_t>, uint64_t>::type m_usedBits[NUM_BUCKET_INTS] = {};
    std::conditional<ThreadSafe, oc::atomic<int>, int>::type m_lastUsedIdx = 0;
    std::thread::id m_lastThreadId;
#ifdef TRACK_ALLOCATION_SIZE
    std::conditional<ThreadSafe, oc::atomic<size_t>, size_t>::type m_usedStackSize = 0;
    std::conditional<ThreadSafe, oc::atomic<size_t>, size_t>::type m_usedFallbackSize = 0;
    size_t m_maxUsedStackSize = 0;
    size_t m_maxUsedFallbackSize = 0;
#endif
};

/* for extremely high contention this is slightly faster than fetch_or
uint64_t expected = m_usedBits[i].load(oc::memory_order_relaxed) & ~bitMasks[i];
while (!m_usedBits[i].compare_exchange_weak(expected, expected | bitMasks[i], oc::memory_order_relaxed, oc::memory_order_relaxed))
{
    if ((expected & bitMasks[i]) != 0)
    {   // Undo bit sets because someone else has set bits in the range
        for (int j = i - 1; j >= intStart; --j)
            m_usedBits[j].fetch_and(~bitMasks[j], oc::memory_order_relaxed);
        return false;
    }
    expected &= ~bitMasks[i];
}
*/

// Earliest possible static initialization for allocator
OC_INIT_SEG(OC_SEG_CORE_ALLOCATOR)

export namespace Globals
{
#if 1
    Allocator allocator;
#else
    constexpr size_t stackSize = 1024 * 48;
    StackAllocator<stackSize, 32, false> allocator;
#endif
}


extern "C++"
{

void* operator new(std::size_t n)
{
    return Globals::allocator.allocate(n);
}

void* operator new(std::size_t n, std::align_val_t align)
{
    if ((size_t)align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        return Globals::allocator.allocate(n);
    else
    {
        g_alignedAllocMemory += n;
        void* p = _aligned_malloc(n, (size_t)align);
        callMemoryAllocHook(p, n);
        return p;
    }
}

void* operator new[](std::size_t n)
{
    return Globals::allocator.allocate(n);
}

void* operator new[](std::size_t n, std::align_val_t align)
{
    if ((size_t)align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        return Globals::allocator.allocate(n);
    else
    {
        g_alignedAllocMemory += n;
        void* p = _aligned_malloc(n, (size_t)align);
        callMemoryAllocHook(p, n);
        return p;
    }
}

void operator delete(void* p)
{
    if (p)
        Globals::allocator.deallocate(p);
}

void operator delete(void* p, std::align_val_t align)
{
    if (p)
    {
        if ((size_t)align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return Globals::allocator.deallocate(p);
        else
        {
            callMemoryFreeHook(p);
            g_alignedAllocMemory -= _aligned_msize(p, (size_t)align, 0);
            return _aligned_free(p);
        }
    }
}

void operator delete[](void* p)
{
    if (p)
        Globals::allocator.deallocate(p);
}

void operator delete[](void* p, std::align_val_t align)
{
    if (p)
    {
        if ((size_t)align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return Globals::allocator.deallocate(p);
        else
        {
            callMemoryFreeHook(p);
            g_alignedAllocMemory -= _aligned_msize(p, (size_t)align, 0);
            return _aligned_free(p);
        }
    }
}

}