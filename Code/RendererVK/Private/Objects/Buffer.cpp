module RendererVK;

import :VK;
import :Device;
import :Allocator;
import :StagingManager;

Buffer::Buffer() {}
Buffer::~Buffer()
{
    destroy();
}

void Buffer::destroy()
{
    if (m_buffer)
    {
        Globals::gpuAllocator.destroyBuffer(m_buffer, m_allocation);
        m_buffer = VK_NULL_HANDLE;
        m_allocation = nullptr;
        m_mappedData = nullptr;
    }
}

bool Buffer::initialize(vk::DeviceSize size, vk::BufferUsageFlags2 usage, vk::MemoryPropertyFlags properties, bool useBackingStore, const char* debugName, BufferHostAccess hostAccess)
{
    if (m_buffer)
        destroy();

    m_size = size;
    m_usage = usage;
    m_properties = properties;
    m_debugName = debugName;
    m_hostAccess = hostAccess;
    m_hasBackingStore = useBackingStore;
    m_uploadOffset = 0;
    // BufferUsageFlags2 is carried in the pNext chain (maintenance5). The device-address allocation flag
    // is added automatically by VMA for buffers that request shaderDeviceAddress usage.
    vk::BufferUsageFlags2CreateInfo usageInfo{ .usage = usage };
    vk::BufferCreateInfo bufferInfo = {};
    bufferInfo.pNext = &usageInfo;
    bufferInfo.size = size;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    if (!Globals::gpuAllocator.createBuffer(bufferInfo, properties, m_buffer, m_allocation, m_mappedData, m_hostAccess, m_debugName))
        return false;

    if (m_hasBackingStore)
        return uploadBackingStore();
    return true;
}

bool Buffer::resize(vk::DeviceSize newSize)
{
    assert(m_hasBackingStore && "resize() requires a buffer initialized with useBackingStore = true");
    if (!m_hasBackingStore)
        return false;
    const vk::DeviceSize uploadOffset = m_uploadOffset;
    const bool result = initialize(newSize, m_usage, m_properties, true, m_debugName, m_hostAccess);
    m_uploadOffset = uploadOffset;
    return result;
}

bool Buffer::upload(vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset)
{
    if (dataSize == 0)
        return true;
    assert(dstOffset + dataSize <= m_size);
    if (m_properties & vk::MemoryPropertyFlagBits::eHostVisible)
    {
        oc::span<uint8> mapped = mapMemory(dstOffset, dataSize);
        if (mapped.empty())
            return false;
        memcpy(mapped.data(), data, (size_t)dataSize);
        flushMappedMemory((size_t)dataSize, (size_t)dstOffset);
        unmapMemory();
    }
    else
    {
        Globals::stagingManager.upload(m_buffer, dataSize, data, dstOffset);
    }
    return true;
}

vk::DeviceSize Buffer::appendUpload(vk::DeviceSize dataSize, const void* data)
{
    const vk::DeviceSize offset = m_uploadOffset;
    if (m_hasBackingStore)
    {
        const uint8* bytes = (const uint8*)data;
        m_backingStore.insert(m_backingStore.end(), bytes, bytes + dataSize);
    }
    upload(dataSize, data, offset);
    m_uploadOffset += dataSize;
    return offset;
}

bool Buffer::uploadBackingStore()
{
    assert(m_hasBackingStore && "uploadBackingStore() requires a buffer initialized with useBackingStore = true");
    if (!m_hasBackingStore || m_backingStore.empty())
        return true;
    return upload(oc::min((size_t)m_size, m_backingStore.size()), m_backingStore.data());
}

vk::DeviceAddress Buffer::getDeviceAddress() const
{
    return Globals::device.getDevice().getBufferAddress(vk::BufferDeviceAddressInfo{ .buffer = m_buffer });
}

oc::span<uint8> Buffer::mapMemory(uint64 offset, uint64 size)
{
    // Host-visible allocations are persistently mapped by VMA, so this just hands back a view into the
    // existing mapping. Device-local buffers have no mapped pointer.
    if (!m_mappedData)
    {
        assert(false && "Buffer is not host-visible / not mapped");
        return {};
    }
    return oc::span<uint8>((uint8*)m_mappedData + offset, (size_t)oc::min(size, m_size - offset));
}

void Buffer::unmapMemory()
{
    // No-op: the allocation stays persistently mapped for the buffer's lifetime.
}

void Buffer::flushMappedMemory(size_t size, size_t offset)
{
    // VMA handles non-coherent atom alignment internally and treats vk::WholeSize natively; it is a no-op
    // when the underlying memory is host-coherent.
    Globals::gpuAllocator.flushAllocation(m_allocation, offset, (size == (size_t)vk::WholeSize) ? vk::WholeSize : size);
}