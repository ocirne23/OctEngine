export module RendererVK:MeshDataManager;

import Core;
import Core.BitRangeAllocator;
import :VK;
import :Layout;
import :Buffer;

export class MeshDataManager final
{
public:

    MeshDataManager();
    ~MeshDataManager();
    MeshDataManager(const MeshDataManager&) = delete;

    bool initialize(size_t vertexBufSize, size_t indexBufSize);

    Buffer& getVertexBuffer() { return m_vertex.buffer; }
    Buffer& getIndexBuffer() { return m_index.buffer; }
    Buffer& getSkinningBuffer() { return m_skinning.buffer; }

    size_t getVertexBufSize() const { return m_vertex.bufSize; }
    size_t getIndexBufSize() const { return m_index.bufSize; }

    size_t getVertexBufUsed() const { return m_vertex.bytesAllocated.load(oc::memory_order_relaxed); }
    size_t getIndexBufUsed() const { return m_index.bytesAllocated.load(oc::memory_order_relaxed); }

    // Bumped whenever a mega-buffer is reallocated (capacity growth). The Renderer compares this each
    // frame and re-records its command buffers when it changes (they bind the buffers by handle).
    uint32 getGeneration() const { return m_generation.load(oc::memory_order_acquire); }

    // Keeps every mega-buffer (handle, size, contents) as it is while alive: a grow on another thread waits.
    // present() holds one from its generation check through the submit, so the check, the recording and the
    // submit see ONE set of buffers. The holding thread must not allocate or free mesh data meanwhile - not even
    // through a job it HELPS inside a JobSystem::wait (a grow would wait on itself; a nested shared acquire can
    // block behind a waiting grow): so no helping wait inside the hold. allocate / release assert it.
    class BufferHold
    {
    public:
        explicit BufferHold(MeshDataManager& owner) : m_owner(&owner), m_lock(owner.m_growMutex)
        {
            owner.m_holder.store(std::this_thread::get_id(), oc::memory_order_relaxed);
        }
        ~BufferHold() { release(); }
        BufferHold(const BufferHold&) = delete;
        BufferHold& operator=(const BufferHold&) = delete;
        void release()
        {
            if (!m_lock.owns_lock())
                return;
            m_owner->m_holder.store(std::thread::id(), oc::memory_order_relaxed);
            m_lock.unlock();
        }
    private:
        MeshDataManager* m_owner;
        std::shared_lock<std::shared_mutex> m_lock;
    };

private:

    friend class ObjectContainer;
    friend class MeshStreamer;
    friend class Renderer; // container teardown frees ranges recorded by the container/its skinned bundles
    size_t uploadVertexData(const void* pData, size_t size);
    size_t uploadIndexData(const void* pData, size_t size);
    size_t uploadSkinningData(const void* pData, size_t size);
    // Reserves (uninitialized) space in the vertex mega-buffer for a per-instance skinned output region;
    // the skinning compute fills it each frame. Returns the byte offset (caller divides by sizeof(MeshVertex)).
    size_t reserveVertexData(size_t size);

    // Returns a range to the free list (mesh streaming eviction / ObjectContainer teardown). offset/size
    // must exactly match a prior upload*/reserve* call. The caller is responsible for GPU-lifetime
    // safety: nothing in flight may still read the range (the MeshStreamer defers frees by
    // NUM_FRAMES_IN_FLIGHT; container teardown forces the pre-flush GPU drain in present()).
    void freeVertexData(size_t offset, size_t size);
    void freeIndexData(size_t offset, size_t size);
    void freeSkinningData(size_t offset, size_t size);

    // One mega-buffer and its range allocator.
    struct Pool
    {
        Buffer buffer;
        size_t bufSize = 0;
        BitRangeAllocator<true> allocator{ 0 };
        oc::atomic<size_t> bytesAllocated{ 0 }; // bucket-rounded
    };

    // Doubles the buffer until neededSize fits, GPU-copying the used range into the new allocation.
    // Under m_growMutex EXCLUSIVE.
    void growBuffer(Buffer& buffer, size_t& bufSize, size_t usedSize, size_t neededSize, vk::BufferUsageFlags2 usage);

    // Range allocation in fixed element-aligned buckets: offsets stay exact multiples of the element
    // size (MeshInfo stores offsets in elements), holes left by frees are reused by later allocations.
    // Copies `size` bytes from pData into the range when pData is set (nullptr = reserve only). Returns the
    // byte offset, SIZE_MAX on failure.
    size_t allocate(Pool& pool, size_t bucketBytes, vk::BufferUsageFlags2 usage, size_t size, const void* pData);
    void release(Pool& pool, size_t bucketBytes, size_t offset, size_t size);

private:

    // Bucket sizes must be a multiple of the element size (48-byte MeshVertex / 4-byte MeshIndex /
    // 32-byte SkinningVertex).
    static constexpr size_t VERTEX_BUCKET_BYTES = 64 * sizeof(RendererVKLayout::MeshVertex); // 3 KB
    static constexpr size_t INDEX_BUCKET_BYTES = 256 * sizeof(RendererVKLayout::MeshIndex);  // 1 KB
    static constexpr size_t SKINNING_BUCKET_BYTES = 64 * sizeof(RendererVKLayout::SkinningVertex); // 2 KB

    Pool m_vertex;
    Pool m_index;
    Pool m_skinning;
    oc::atomic<uint32> m_generation{ 0 };

    // Allocation and free are LOCK-FREE (BitRangeAllocator<true>): parallel entity spawning (spawnSkinnedNode
    // reserves output regions from spawn jobs) and job-side createMesh (the terrain's upload job). This lock only
    // fences a GROW: every allocate / upload / release holds it SHARED - over the staging copy too, which names the
    // buffer by handle - and growBuffer, which replaces the buffer AND the allocator's bit array, holds it
    // EXCLUSIVE. Lock order: grow -> staging -> queue.
    std::shared_mutex m_growMutex;
    oc::atomic<std::thread::id> m_holder{}; // the thread inside a BufferHold (one at a time: present)
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DATA)
    MeshDataManager meshDataManager;
}
