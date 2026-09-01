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

    Buffer& getVertexBuffer() { return m_vertexBuffer; }
    Buffer& getIndexBuffer() { return m_indexBuffer; }
    Buffer& getSkinningBuffer() { return m_skinningBuffer; }

    size_t getVertexBufSize() const { return m_vertexBufSize; }
    size_t getIndexBufSize() const { return m_indexBufSize; }

    size_t getVertexBufUsed() const { return m_vertexBytesAllocated; }
    size_t getIndexBufUsed() const { return m_indexBytesAllocated; }

    // Bumped whenever a mega-buffer is reallocated (capacity growth). The Renderer compares this each
    // frame and re-records its command buffers when it changes (they bind the buffers by handle).
    uint32 getGeneration() const { return m_generation; }

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

    // Doubles the buffer until neededSize fits, GPU-copying the used range into the new allocation.
    void growBuffer(Buffer& buffer, size_t& bufSize, size_t usedSize, size_t neededSize, vk::BufferUsageFlags2 usage);

    // Range allocation in fixed element-aligned buckets: offsets stay exact multiples of the element
    // size (MeshInfo stores offsets in elements), holes left by frees are reused by later allocations.
    size_t allocRange(BitRangeAllocator<false>& allocator, size_t& bufSize, size_t bucketBytes, size_t size,
        Buffer& buffer, vk::BufferUsageFlags2 usage, size_t& allocatedBytes);

private:

    // Bucket sizes must be a multiple of the element size (48-byte MeshVertex / 4-byte MeshIndex /
    // 32-byte SkinningVertex).
    static constexpr size_t VERTEX_BUCKET_BYTES = 64 * sizeof(RendererVKLayout::MeshVertex); // 3 KB
    static constexpr size_t INDEX_BUCKET_BYTES = 256 * sizeof(RendererVKLayout::MeshIndex);  // 1 KB
    static constexpr size_t SKINNING_BUCKET_BYTES = 64 * sizeof(RendererVKLayout::SkinningVertex); // 2 KB

    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;
    Buffer m_skinningBuffer;
    uint32 m_generation = 0;

    size_t m_vertexBufSize = 0;
    size_t m_indexBufSize = 0;
    size_t m_skinningBufSize = 0;

    // Parallel entity spawning: spawnSkinnedNode reserves output regions from spawn jobs, so the
    // range allocators serialize here (locked in allocRange and every free*).
    std::mutex m_allocMutex;

    BitRangeAllocator<false> m_vertexAllocator{ 0 };
    BitRangeAllocator<false> m_indexAllocator{ 0 };
    BitRangeAllocator<false> m_skinningAllocator{ 0 };
    size_t m_vertexBytesAllocated = 0; // bucket-rounded
    size_t m_indexBytesAllocated = 0;
    size_t m_skinningBytesAllocated = 0;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DATA)
    MeshDataManager meshDataManager;
}
