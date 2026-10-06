module RendererVK;

import Core;
import Core.BitRangeAllocator;
import :StagingManager;
import :Buffer;
import :CommandBuffer;
import :Device;

namespace
{
    constexpr vk::BufferUsageFlags2 rtUsage()
    {
        return vk::BufferUsageFlagBits2::eShaderDeviceAddress
            | vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR
            | vk::BufferUsageFlagBits2::eStorageBuffer;
    }
    constexpr vk::BufferUsageFlags2 vertexBufferUsage()
    {
        return vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eVertexBuffer | rtUsage();
    }
    constexpr vk::BufferUsageFlags2 indexBufferUsage()
    {
        return vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eIndexBuffer | rtUsage();
    }
    constexpr vk::BufferUsageFlags2 skinningBufferUsage()
    {
        // Read-only storage in the skinning compute; transfer dst/src for upload + grow copy.
        return vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eTransferSrc
            | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
    }
}

MeshDataManager::MeshDataManager()
{
}

MeshDataManager::~MeshDataManager()
{
    auto waitResult = Globals::device.graphicsQueueWaitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in MeshDataManager::~MeshDataManager");
    }
}

bool MeshDataManager::initialize(size_t vertexBufSize, size_t indexBufSize)
{
    // Round to whole buckets so the allocators and byte sizes stay in lockstep.
    m_vertex.bufSize = (vertexBufSize + VERTEX_BUCKET_BYTES - 1) / VERTEX_BUCKET_BYTES * VERTEX_BUCKET_BYTES;
    m_index.bufSize = (indexBufSize + INDEX_BUCKET_BYTES - 1) / INDEX_BUCKET_BYTES * INDEX_BUCKET_BYTES;
    m_vertex.allocator.resize(uint32(m_vertex.bufSize / VERTEX_BUCKET_BYTES));
    m_index.allocator.resize(uint32(m_index.bufSize / INDEX_BUCKET_BYTES));
    m_skinning.allocator.resize(uint32(RendererVKLayout::INITIAL_SKINNING_DATA / SKINNING_BUCKET_BYTES));

    // Extra usage beyond vertex/index: the GI ray tracer builds BLASes directly from these mega-buffers
    // (eAccelerationStructureBuildInputReadOnlyKHR + eShaderDeviceAddress) and fetches hit-triangle
    // attributes from them in the probe-trace compute shader (eStorageBuffer). eTransferSrc enables the
    // GPU copy that carries contents over on capacity growth.
    m_vertex.buffer.initialize(m_vertex.bufSize, vertexBufferUsage(), vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshVertex");
    m_index.buffer.initialize(m_index.bufSize, indexBufferUsage(), vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshIndex");

    // Skinning influences for skeletal meshes; sized lazily on first skinned upload (grows on demand).
    m_skinning.bufSize = RendererVKLayout::INITIAL_SKINNING_DATA;
    m_skinning.buffer.initialize(m_skinning.bufSize, skinningBufferUsage(), vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshSkinning");

    return true;
}

void MeshDataManager::growBuffer(Buffer& buffer, size_t& bufSize, size_t usedSize, size_t neededSize, vk::BufferUsageFlags2 usage)
{
    size_t newSize = bufSize;
    while (newSize < neededSize)
        newSize *= 2;

    // Drain first: this buffer is shared (not per-frame-in-flight) and read full-range every frame by GI
    // probe trace / RTAO / BLAS builds, so an already-submitted dispatch may still be reading it while a
    // queued staging copy (from an unrelated uploadVertexData/uploadIndexData earlier this frame) is about
    // to write into it - WRITE_AFTER_READ, same as Renderer::waitForGpuAndFlushStaging.
    auto waitResult = Globals::device.graphicsQueueWaitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle in MeshDataManager::growBuffer");
    // Now safe to submit those queued copies (they target the buffer about to be moved-from/destroyed
    // below) - then drain again so that submission itself completes before the destroy, avoiding
    // "buffer currently in use by command buffer" at vkDestroyBuffer.
    Globals::stagingManager.flushPending();
    waitResult = Globals::device.graphicsQueueWaitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle after staging flush in MeshDataManager::growBuffer");

    Buffer oldBuffer = oc::move(buffer);
    buffer.initialize(newSize, usage, vk::MemoryPropertyFlagBits::eDeviceLocal, false, oldBuffer.getDebugName());
    if (usedSize > 0)
    {
        CommandBuffer copyCommandBuffer;
        copyCommandBuffer.initialize(vk::CommandBufferLevel::ePrimary, "CB.meshBufferGrow");
        vk::CommandBuffer vkCmd = copyCommandBuffer.begin(true);
        const vk::BufferCopy region{ .srcOffset = 0, .dstOffset = 0, .size = usedSize };
        vkCmd.copyBuffer(oldBuffer.getBuffer(), buffer.getBuffer(), 1, &region);
        copyCommandBuffer.end();
        copyCommandBuffer.submitGraphics();
        auto copyWaitResult = Globals::device.graphicsQueueWaitIdle();
        assert(copyWaitResult == vk::Result::eSuccess && "Failed to wait for grow copy in MeshDataManager::growBuffer");
    }
    bufSize = newSize;
    m_generation.fetch_add(1, oc::memory_order_release);
    printf("MeshDataManager: grew buffer to %zu bytes\n", newSize);
}

// The fast path holds m_growMutex SHARED: the allocator is lock-free, and the shared lock only keeps a grow from
// replacing the buffer (and the allocator's bit array) while this thread allocates and its staging copy names the
// buffer by handle. A full allocator takes it EXCLUSIVE, retries (another thread may have grown it meanwhile), and
// only then grows. Before 2026-10-05 a plain mutex covered the allocation alone: a grow on another thread replaced
// the buffer between an upload's allocation and its staging copy.
size_t MeshDataManager::allocate(Pool& pool, size_t bucketBytes, vk::BufferUsageFlags2 usage, size_t size, const void* pData)
{
    assert(m_holder.load(oc::memory_order_relaxed) != std::this_thread::get_id()
        && "mesh data allocated inside this thread's BufferHold (a job helped inside a wait?) - see MeshDataManager::BufferHold");
    const uint32 numBuckets = uint32((size + bucketBytes - 1) / bucketBytes);
    auto commit = [&](int bucketStart)
    {
        pool.bytesAllocated.fetch_add((size_t)numBuckets * bucketBytes, oc::memory_order_relaxed);
        const size_t offset = (size_t)bucketStart * bucketBytes;
        if (pData)
        {
            // Shared (not per-frame-in-flight), read full-range every frame by GI probe trace / RTAO / BLAS builds.
            // See StagingManager::ensureDrainedForSharedWrite: without this, upload()'s ring buffer can implicitly
            // submit the copy the moment it overflows, racing an in-flight frame's read with no synchronization.
            Globals::stagingManager.ensureDrainedForSharedWrite();
            Globals::stagingManager.upload(pool.buffer.getBuffer(), size, pData, offset);
        }
        return offset;
    };
    {
        const std::shared_lock lock(m_growMutex);
        const int bucketStart = pool.allocator.acquireRange(numBuckets);
        if (bucketStart >= 0)
            return commit(bucketStart);
    }
    const std::unique_lock lock(m_growMutex);
    int bucketStart = pool.allocator.acquireRange(numBuckets);
    if (bucketStart < 0)
    {
        // No contiguous free run: grow the buffer (the whole old range is copied over - it may be
        // fragmented, so everything allocated must survive) and retry in the fresh tail.
        growBuffer(pool.buffer, pool.bufSize, pool.bufSize, pool.bufSize + size, usage);
        pool.allocator.resize(uint32(pool.bufSize / bucketBytes));
        bucketStart = pool.allocator.acquireRange(numBuckets);
        assert(bucketStart >= 0 && "Mesh data allocation failed after growth");
        if (bucketStart < 0)
            return SIZE_MAX;
    }
    return commit(bucketStart);
}

void MeshDataManager::release(Pool& pool, size_t bucketBytes, size_t offset, size_t size)
{
    assert(m_holder.load(oc::memory_order_relaxed) != std::this_thread::get_id()
        && "mesh data freed inside this thread's BufferHold - see MeshDataManager::BufferHold");
    const std::shared_lock lock(m_growMutex); // a grow replaces the bit array
    const uint32 numBuckets = uint32((size + bucketBytes - 1) / bucketBytes);
    pool.allocator.releaseRange(int(offset / bucketBytes), numBuckets);
    pool.bytesAllocated.fetch_sub((size_t)numBuckets * bucketBytes, oc::memory_order_relaxed);
}

size_t MeshDataManager::uploadVertexData(const void* pData, size_t size)
{
    return allocate(m_vertex, VERTEX_BUCKET_BYTES, vertexBufferUsage(), size, pData);
}

size_t MeshDataManager::uploadIndexData(const void* pData, size_t size)
{
    return allocate(m_index, INDEX_BUCKET_BYTES, indexBufferUsage(), size, pData);
}

size_t MeshDataManager::uploadSkinningData(const void* pData, size_t size)
{
    return allocate(m_skinning, SKINNING_BUCKET_BYTES, skinningBufferUsage(), size, pData);
}

size_t MeshDataManager::reserveVertexData(size_t size)
{
    return allocate(m_vertex, VERTEX_BUCKET_BYTES, vertexBufferUsage(), size, nullptr);
}

void MeshDataManager::freeVertexData(size_t offset, size_t size)
{
    release(m_vertex, VERTEX_BUCKET_BYTES, offset, size);
}

void MeshDataManager::freeIndexData(size_t offset, size_t size)
{
    release(m_index, INDEX_BUCKET_BYTES, offset, size);
}

void MeshDataManager::freeSkinningData(size_t offset, size_t size)
{
    release(m_skinning, SKINNING_BUCKET_BYTES, offset, size);
}
