export module Core.Allocator;

// Debug features. Either one puts an AllocationHeader in front of every block; with both off a
// block is exactly what malloc / _aligned_malloc returned.
#if 0 //defined(_DEBUG)
#define CHECK_BOUNDS
#endif

#if 0 //defined(_DEBUG)
#define TRACK_ALLOCATION_SIZE
#endif

#if defined(CHECK_BOUNDS) || defined(TRACK_ALLOCATION_SIZE)
#define ALLOCATION_HEADER
#endif

import Core;

// Allocation tracking hooks (installed by the Memory library's MemoryTracker; Core stays
// dependency-free). Called on EVERY allocate/deallocate that flows through the engine allocator -
// null until installed, one relaxed load + branch of overhead. Contract: the alloc hook fires AFTER
// a successful allocation, the free hook BEFORE the memory is actually released (so a recycled
// address can never race its own map entry). Hook implementations must never allocate through
// these paths themselves.
export using MemoryAllocHook = void(*)(void* ptr, size_t size);
export using MemoryFreeHook = void(*)(void* ptr);
export oc::atomic<MemoryAllocHook> g_memoryAllocHook = nullptr;
export oc::atomic<MemoryFreeHook> g_memoryFreeHook = nullptr;

export inline void callMemoryAllocHook(void* ptr, size_t size)
{
    if (MemoryAllocHook hook = g_memoryAllocHook.load(oc::memory_order_relaxed)) [[unlikely]]
        hook(ptr, size);
}
export inline void callMemoryFreeHook(void* ptr)
{
    if (MemoryFreeHook hook = g_memoryFreeHook.load(oc::memory_order_relaxed)) [[unlikely]]
        hook(ptr);
}

#ifdef ALLOCATION_HEADER
// Sits directly before the user pointer. An over-aligned block puts it there too, with alignPad =
// the bytes between the malloc base and the header, so both free forms work on any block.
struct AllocationHeader
{
    static constexpr uint32 CHECK = 0x69696969;
    uint64 size;     // header + user bytes + tail (not the alignPad)
    uint32 alignPad;
    uint32 checkVal;
};
static_assert(sizeof(AllocationHeader) == __STDCPP_DEFAULT_NEW_ALIGNMENT__);
#endif

#ifdef CHECK_BOUNDS
struct AllocationTail
{
    static constexpr uint32 CHECK = 0x78787878;
    uint32 checkVal;
};
#endif

// The engine heap: malloc / _aligned_malloc plus the memory hooks. A block allocated over-aligned
// (align > 16) MUST free through deallocateAligned with that same align - _aligned_malloc and malloc
// blocks cannot be told apart. Every engine path knows the alignment at the free: the align_val_t
// delete forms, and oc::allocator<T> (Core.OcSTL) for the EASTL containers.
export class Allocator final
{
public:

    [[nodiscard]]
    void* allocate(size_t size)
    {
#ifdef ALLOCATION_HEADER
        return allocateWithHeader(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
#else
        void* ptr = std::malloc(size);
        callMemoryAllocHook(ptr, size);
        return ptr;
#endif
    }

    // Any power-of-two alignment.
    [[nodiscard]]
    void* allocateAligned(size_t size, size_t align)
    {
        if (align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return allocate(size);
#ifdef ALLOCATION_HEADER
        return allocateWithHeader(size, align);
#else
        void* ptr = _aligned_malloc(size, align);
        callMemoryAllocHook(ptr, size);
        return ptr;
#endif
    }

    void deallocate(void* ptr)
    {
        callMemoryFreeHook(ptr); // before the release: a recycled address must not race its own tracking entry
#ifdef ALLOCATION_HEADER
        freeWithHeader(ptr);
#else
        std::free(ptr);
#endif
    }

    void deallocateAligned(void* ptr, size_t align)
    {
        if (align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return deallocate(ptr);
        callMemoryFreeHook(ptr);
#ifdef ALLOCATION_HEADER
        freeWithHeader(ptr);
#else
        _aligned_free(ptr);
#endif
    }

#ifdef TRACK_ALLOCATION_SIZE
    size_t getUsedSize() const { return m_usedSize; }
    size_t getMaxUsedSize() const { return m_maxUsedSize; }
#else
    size_t getUsedSize() const { return 0; }
    size_t getMaxUsedSize() const { return 0; }
#endif

private:

#ifdef ALLOCATION_HEADER
    // malloc returns 16-aligned memory, so a block aligned to `align` needs at most align - 16 bytes
    // of pad in front of its header.
    void* allocateWithHeader(size_t size, size_t align)
    {
        assert((align & (align - 1)) == 0);
        const size_t sizeWithHeader = size
#ifdef CHECK_BOUNDS
            + sizeof(AllocationTail)
#endif
            + sizeof(AllocationHeader);
        const size_t maxPad = align - __STDCPP_DEFAULT_NEW_ALIGNMENT__;
        uint8* base = static_cast<uint8*>(std::malloc(sizeWithHeader + maxPad));
        const uintptr_t user = (reinterpret_cast<uintptr_t>(base) + sizeof(AllocationHeader) + align - 1) & ~uintptr_t(align - 1);
        AllocationHeader* header = reinterpret_cast<AllocationHeader*>(user) - 1;
        header->size = sizeWithHeader;
        header->alignPad = uint32(reinterpret_cast<uint8*>(header) - base);
#ifdef TRACK_ALLOCATION_SIZE
        m_usedSize += sizeWithHeader + header->alignPad;
        m_maxUsedSize = m_usedSize > m_maxUsedSize ? (size_t)m_usedSize : m_maxUsedSize;
#endif
#ifdef CHECK_BOUNDS
        header->checkVal = AllocationHeader::CHECK;
        AllocationTail* tail = reinterpret_cast<AllocationTail*>(reinterpret_cast<uint8*>(header) + sizeWithHeader - sizeof(AllocationTail));
        tail->checkVal = AllocationTail::CHECK;
#endif
        callMemoryAllocHook(header + 1, size);
        return header + 1;
    }

    void freeWithHeader(void* ptr)
    {
        AllocationHeader* header = static_cast<AllocationHeader*>(ptr) - 1;
#ifdef TRACK_ALLOCATION_SIZE
        m_usedSize -= header->size + header->alignPad;
#endif
#ifdef CHECK_BOUNDS
        const AllocationTail* tail = reinterpret_cast<const AllocationTail*>(reinterpret_cast<uint8*>(header) + header->size - sizeof(AllocationTail));
        if (header->checkVal != AllocationHeader::CHECK) { assert(false && "Buffer underflow detected!"); __debugbreak(); }
        if (tail->checkVal != AllocationTail::CHECK) { assert(false && "Buffer overflow detected!"); __debugbreak(); }
#endif
        std::free(reinterpret_cast<uint8*>(header) - header->alignPad);
    }
#endif

#ifdef TRACK_ALLOCATION_SIZE
    oc::atomic<size_t> m_usedSize = 0;
    size_t m_maxUsedSize = 0;
#endif
};

// Earliest possible static initialization for allocator
OC_INIT_SEG(OC_SEG_CORE_ALLOCATOR)

export namespace Globals
{
    Allocator allocator;
}


extern "C++"
{

void* operator new(std::size_t n)
{
    return Globals::allocator.allocate(n);
}

void* operator new(std::size_t n, std::align_val_t align)
{
    return Globals::allocator.allocateAligned(n, (size_t)align);
}

void* operator new[](std::size_t n)
{
    return Globals::allocator.allocate(n);
}

void* operator new[](std::size_t n, std::align_val_t align)
{
    return Globals::allocator.allocateAligned(n, (size_t)align);
}

// EASTL (the oc:: container backing -- see Core.OcSTL) DECLARES these two named/aligned operator
// new[] forms and expects the application to define them; without them the engine does not link.
// Only eastl::allocator reaches them (oc:: containers use oc::allocator<T>), and it frees every
// block with plain delete[] - so an over-aligned block here could never be freed correctly.
void* operator new[](std::size_t n, const char*, int, unsigned, const char*, int)
{
    return Globals::allocator.allocate(n);
}

void* operator new[](std::size_t n, std::size_t align, [[maybe_unused]] std::size_t alignOffset, const char*, int, unsigned, const char*, int)
{
    assert(alignOffset == 0 && "EASTL aligned allocation with an offset: not supported");
    if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__) [[unlikely]]
    {
        assert(false && "over-aligned eastl::allocator block: use an oc:: container / oc::make_shared (oc::allocator<T>)");
        std::abort(); // eastl::allocator::deallocate frees it with plain delete[]: heap corruption otherwise
    }
    return Globals::allocator.allocate(n);
}

void operator delete(void* p)
{
    if (p)
        Globals::allocator.deallocate(p);
}

void operator delete(void* p, std::align_val_t align)
{
    if (p)
        Globals::allocator.deallocateAligned(p, (size_t)align);
}

void operator delete[](void* p)
{
    if (p)
        Globals::allocator.deallocate(p);
}

void operator delete[](void* p, std::align_val_t align)
{
    if (p)
        Globals::allocator.deallocateAligned(p, (size_t)align);
}

}
