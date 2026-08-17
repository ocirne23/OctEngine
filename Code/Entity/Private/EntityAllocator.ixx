export module Entity:Allocator;

import Core;

// Thread-safe, lock-free entity allocator.
// - allocate/deallocate are callable from any thread/job concurrently
// - fast path: tag-counted Treiber stack pop per exact size class, else an atomic bump within the active chunk
// - only the rare chunk-add slow path (once per MB) takes a spinlock
// Chunks are never freed until destruction, so Chunk*/block pointers stay valid for the allocator's lifetime.
export class EntityAllocator final
{
public:

    ~EntityAllocator();

    [[nodiscard]] void* allocate(uint32 size);
    void deallocate(void* ptr, uint32 size);

    size_t getNumChunks() const { return m_numChunks.load(oc::memory_order_relaxed); }

private:

    static constexpr uint32 ALIGNMENT = 16;
    static constexpr uint32 CHUNK_SIZE = 1u << 20; // 1 MB
    static constexpr uint32 MAX_BLOCK_SIZE = 65536; // largest recyclable allocation (a whole prefab tree is one block)
    static constexpr uint32 NUM_SIZE_CLASSES = MAX_BLOCK_SIZE / ALIGNMENT;

    struct FreeBlock { FreeBlock* next; };

    struct Chunk
    {
        oc::atomic<uint32> offset;
        uint32 size;
        uint8* base;
    };

    // 48-bit pointer + 16-bit ABA tag packed into one CAS-able word
    static uint64 packHead(FreeBlock* ptr, uint64 tag) { return (tag << 48) | reinterpret_cast<uint64>(ptr); }
    static FreeBlock* headPtr(uint64 head) { return reinterpret_cast<FreeBlock*>(head & 0x0000'FFFF'FFFF'FFFFull); }

    Chunk* addChunkLocked(uint32 minSize, Chunk* expectedActive);

    oc::atomic<Chunk*> m_activeChunk = nullptr;
    oc::atomic_flag m_chunkLock;                       // guards m_chunks growth only (C++20 default-clear)
    oc::vector<Chunk*> m_chunks;                       // chunk headers (allocated in-chunk), freed in the destructor
    oc::atomic<size_t> m_numChunks = 0;

    alignas(64) oc::atomic<uint64> m_freeLists[NUM_SIZE_CLASSES] = {}; // exact size class -> tagged Treiber stack head
};

export namespace Globals
{
    EntityAllocator entityAllocator;
}
