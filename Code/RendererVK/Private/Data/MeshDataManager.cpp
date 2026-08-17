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
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
    if (waitResult != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for device idle in MeshDataManager::~MeshDataManager");
    }
}

bool MeshDataManager::initialize(size_t vertexBufSize, size_t indexBufSize)
{
    // Round to whole buckets so the allocators and byte sizes stay in lockstep.
    m_vertexBufSize = (vertexBufSize + VERTEX_BUCKET_BYTES - 1) / VERTEX_BUCKET_BYTES * VERTEX_BUCKET_BYTES;
    m_indexBufSize = (indexBufSize + INDEX_BUCKET_BYTES - 1) / INDEX_BUCKET_BYTES * INDEX_BUCKET_BYTES;
    m_vertexAllocator.resize(uint32(m_vertexBufSize / VERTEX_BUCKET_BYTES));
    m_indexAllocator.resize(uint32(m_indexBufSize / INDEX_BUCKET_BYTES));
    m_skinningAllocator.resize(uint32(RendererVKLayout::INITIAL_SKINNING_DATA / SKINNING_BUCKET_BYTES));

    // Extra usage beyond vertex/index: the GI ray tracer builds BLASes directly from these mega-buffers
    // (eAccelerationStructureBuildInputReadOnlyKHR + eShaderDeviceAddress) and fetches hit-triangle
    // attributes from them in the probe-trace compute shader (eStorageBuffer). eTransferSrc enables the
    // GPU copy that carries contents over on capacity growth.
    m_vertexBuffer.initialize(m_vertexBufSize, vertexBufferUsage(), vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshVertex");
    m_indexBuffer.initialize(m_indexBufSize, indexBufferUsage(), vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshIndex");

    // Skinning influences for skeletal meshes; sized lazily on first skinned upload (grows on demand).
    m_skinningBufSize = RendererVKLayout::INITIAL_SKINNING_DATA;
    m_skinningBuffer.initialize(m_skinningBufSize, skinningBufferUsage(), vk::MemoryPropertyFlagBits::eDeviceLocal, false, "MeshSkinning");

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
    auto waitResult = Globals::device.getGraphicsQueue().waitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle in MeshDataManager::growBuffer");
    // Now safe to submit those queued copies (they target the buffer about to be moved-from/destroyed
    // below) - then drain again so that submission itself completes before the destroy, avoiding
    // "buffer currently in use by command buffer" at vkDestroyBuffer.
    Globals::stagingManager.flushPending();
    waitResult = Globals::device.getGraphicsQueue().waitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle after staging flush in MeshDataManager::growBuffer");

    Buffer oldBuffer = oc::move(buffer);
    buffer.initialize(newSize, usage, vk::MemoryPropertyFlagBits::eDeviceLocal, false, oldBuffer.getDebugName());
    if (usedSize > 0)
    {
        CommandBuffer copyCommandBuffer;
        copyCommandBuffer.initialize(vk::CommandBufferLevel::ePrimary);
        vk::CommandBuffer vkCmd = copyCommandBuffer.begin(true);
        const vk::BufferCopy region{ .srcOffset = 0, .dstOffset = 0, .size = usedSize };
        vkCmd.copyBuffer(oldBuffer.getBuffer(), buffer.getBuffer(), 1, &region);
        copyCommandBuffer.end();
        copyCommandBuffer.submitGraphics();
        auto copyWaitResult = Globals::device.getGraphicsQueue().waitIdle();
        assert(copyWaitResult == vk::Result::eSuccess && "Failed to wait for grow copy in MeshDataManager::growBuffer");
    }
    bufSize = newSize;
    m_generation++;
    printf("MeshDataManager: grew buffer to %zu bytes\n", newSize);
}

size_t MeshDataManager::allocRange(BitRangeAllocator<false>& allocator, size_t& bufSize, size_t bucketBytes, size_t size,
    Buffer& buffer, vk::BufferUsageFlags2 usage, size_t& allocatedBytes)
{
    const uint32 numBuckets = uint32((size + bucketBytes - 1) / bucketBytes);
    int bucketStart = allocator.acquireRange(numBuckets);
    if (bucketStart < 0)
    {
        // No contiguous free run: grow the buffer (the whole old range is copied over — it may be
        // fragmented, so everything allocated must survive) and retry in the fresh tail.
        growBuffer(buffer, bufSize, bufSize, bufSize + size, usage);
        allocator.resize(uint32(bufSize / bucketBytes));
        bucketStart = allocator.acquireRange(numBuckets);
        assert(bucketStart >= 0 && "Mesh data allocation failed after growth");
        if (bucketStart < 0)
            return SIZE_MAX;
    }
    allocatedBytes += (size_t)numBuckets * bucketBytes;
    return (size_t)bucketStart * bucketBytes;
}

size_t MeshDataManager::uploadVertexData(const void* pData, size_t size)
{
    const size_t offset = allocRange(m_vertexAllocator, m_vertexBufSize, VERTEX_BUCKET_BYTES, size,
        m_vertexBuffer, vertexBufferUsage(), m_vertexBytesAllocated);
    // Shared (not per-frame-in-flight), read full-range every frame by GI probe trace / RTAO / BLAS builds.
    // See StagingManager::ensureDrainedForSharedWrite: without this, upload()'s ring buffer can implicitly
    // submit the copy the moment it overflows, racing an in-flight frame's read with no synchronization.
    Globals::stagingManager.ensureDrainedForSharedWrite();
    Globals::stagingManager.upload(m_vertexBuffer.getBuffer(), size, pData, offset);
    return offset;
}

size_t MeshDataManager::uploadIndexData(const void* pData, size_t size)
{
    const size_t offset = allocRange(m_indexAllocator, m_indexBufSize, INDEX_BUCKET_BYTES, size,
        m_indexBuffer, indexBufferUsage(), m_indexBytesAllocated);
    Globals::stagingManager.ensureDrainedForSharedWrite(); // see uploadVertexData
    Globals::stagingManager.upload(m_indexBuffer.getBuffer(), size, pData, offset);
    return offset;
}

size_t MeshDataManager::reserveVertexData(size_t size)
{
    return allocRange(m_vertexAllocator, m_vertexBufSize, VERTEX_BUCKET_BYTES, size,
        m_vertexBuffer, vertexBufferUsage(), m_vertexBytesAllocated);
}

void MeshDataManager::freeVertexData(size_t offset, size_t size)
{
    const uint32 numBuckets = uint32((size + VERTEX_BUCKET_BYTES - 1) / VERTEX_BUCKET_BYTES);
    m_vertexAllocator.releaseRange(int(offset / VERTEX_BUCKET_BYTES), numBuckets);
    m_vertexBytesAllocated -= (size_t)numBuckets * VERTEX_BUCKET_BYTES;
}

void MeshDataManager::freeIndexData(size_t offset, size_t size)
{
    const uint32 numBuckets = uint32((size + INDEX_BUCKET_BYTES - 1) / INDEX_BUCKET_BYTES);
    m_indexAllocator.releaseRange(int(offset / INDEX_BUCKET_BYTES), numBuckets);
    m_indexBytesAllocated -= (size_t)numBuckets * INDEX_BUCKET_BYTES;
}

size_t MeshDataManager::uploadSkinningData(const void* pData, size_t size)
{
    const size_t offset = allocRange(m_skinningAllocator, m_skinningBufSize, SKINNING_BUCKET_BYTES, size,
        m_skinningBuffer, skinningBufferUsage(), m_skinningBytesAllocated);
    Globals::stagingManager.ensureDrainedForSharedWrite(); // see uploadVertexData
    Globals::stagingManager.upload(m_skinningBuffer.getBuffer(), size, pData, offset);
    return offset;
}

void MeshDataManager::freeSkinningData(size_t offset, size_t size)
{
    const uint32 numBuckets = uint32((size + SKINNING_BUCKET_BYTES - 1) / SKINNING_BUCKET_BYTES);
    m_skinningAllocator.releaseRange(int(offset / SKINNING_BUCKET_BYTES), numBuckets);
    m_skinningBytesAllocated -= (size_t)numBuckets * SKINNING_BUCKET_BYTES;
}
