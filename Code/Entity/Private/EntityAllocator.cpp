module;

#include <intrin.h>

module Entity;

import Core;
import Core.Allocator;

static constexpr uint32 alignUp(uint32 value, uint32 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

EntityAllocator::~EntityAllocator()
{
    for (Chunk* chunk : m_chunks)
        Globals::allocator.deallocate(chunk); // header lives at the chunk base
    m_chunks.clear();
}

// Called with m_chunkLock held. Re-checks the active chunk so racing losers don't each add one.
EntityAllocator::Chunk* EntityAllocator::addChunkLocked(uint32 minSize, Chunk* expectedActive)
{
    Chunk* current = m_activeChunk.load(oc::memory_order_acquire);
    if (current != expectedActive)
        return current; // another thread already replaced it

    constexpr uint32 headerSize = alignUp(sizeof(Chunk), ALIGNMENT);
    const uint32 payloadSize = minSize > CHUNK_SIZE ? minSize : CHUNK_SIZE;
    uint8* memory = static_cast<uint8*>(Globals::allocator.allocate(headerSize + payloadSize)); // 16-byte aligned
    Chunk* chunk = new (memory) Chunk{ .offset = 0, .size = payloadSize, .base = memory + headerSize };

    m_chunks.push_back(chunk);
    m_numChunks.store(m_chunks.size(), oc::memory_order_relaxed);
    m_activeChunk.store(chunk, oc::memory_order_release);
    return chunk;
}

void* EntityAllocator::allocate(uint32 size)
{
    assert(size > 0 && size <= MAX_BLOCK_SIZE);
    size = alignUp(size, ALIGNMENT);

    // recycled block of the exact size: lock-free tagged pop (tag defeats ABA on concurrent pop/push)
    oc::atomic<uint64>& freeList = m_freeLists[size / ALIGNMENT - 1];
    uint64 head = freeList.load(oc::memory_order_acquire);
    while (headPtr(head))
    {
        FreeBlock* block = headPtr(head);
        if (freeList.compare_exchange_weak(head, packHead(block->next, (head >> 48) + 1),
                                           oc::memory_order_acquire, oc::memory_order_acquire))
            return block;
    }

    for (;;)
    {
        Chunk* chunk = m_activeChunk.load(oc::memory_order_acquire);
        if (chunk)
        {
            const uint32 offset = chunk->offset.fetch_add(size, oc::memory_order_relaxed);
            if (offset + size <= chunk->size)
                return chunk->base + offset;
            // chunk exhausted (tail bytes wasted, bounded by one claim per racing thread)
        }

        while (m_chunkLock.test_and_set(oc::memory_order_acquire))
            _mm_pause();
        addChunkLocked(size, chunk);
        m_chunkLock.clear(oc::memory_order_release);
    }
}

void EntityAllocator::deallocate(void* ptr, uint32 size)
{
    assert(size > 0 && size <= MAX_BLOCK_SIZE);
    size = alignUp(size, ALIGNMENT);

    // lock-free tagged push
    oc::atomic<uint64>& freeList = m_freeLists[size / ALIGNMENT - 1];
    FreeBlock* block = static_cast<FreeBlock*>(ptr);
    uint64 head = freeList.load(oc::memory_order_relaxed);
    do
    {
        block->next = headPtr(head);
    } while (!freeList.compare_exchange_weak(head, packHead(block, (head >> 48) + 1),
                                             oc::memory_order_release, oc::memory_order_relaxed));
}
