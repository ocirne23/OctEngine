export module RendererVK:Buffer;

import Core;
import :VK;
import :Allocator;

export class Buffer final
{
public:
    Buffer();
    ~Buffer();
    Buffer(Buffer&& move)
    {
        m_size = move.m_size;
        m_buffer = move.getBuffer();
        m_allocation = move.m_allocation;
        m_mappedData = move.m_mappedData;
        m_usage = move.m_usage;
        m_properties = move.m_properties;
        m_debugName = move.m_debugName;
        m_hostAccess = move.m_hostAccess;
        m_backingStore = oc::move(move.m_backingStore);
        m_uploadOffset = move.m_uploadOffset;
        m_hasBackingStore = move.m_hasBackingStore;
        move.m_size = 0;
        move.m_buffer = nullptr;
        move.m_allocation = nullptr;
        move.m_mappedData = nullptr;
        move.m_hasBackingStore = false;
    }
    Buffer& operator=(Buffer&& move)
    {
        if (this != &move)
        {
            destroy(); // release the current buffer before adopting the moved-in one
            m_size = move.m_size;
            m_buffer = move.m_buffer;
            m_allocation = move.m_allocation;
            m_mappedData = move.m_mappedData;
            m_usage = move.m_usage;
            m_properties = move.m_properties;
            m_debugName = move.m_debugName;
            m_hostAccess = move.m_hostAccess;
            m_backingStore = oc::move(move.m_backingStore);
            m_uploadOffset = move.m_uploadOffset;
            m_hasBackingStore = move.m_hasBackingStore;
            move.m_size = 0;
            move.m_buffer = nullptr;
            move.m_allocation = nullptr;
            move.m_mappedData = nullptr;
            move.m_hasBackingStore = false;
        }
        return *this;
    }
    Buffer(const Buffer&) = delete;

    // Requesting a shader-device-address usage allocates the memory with the device-address flag so
    // getDeviceAddress works.
    // debugName (optional) is attached to the underlying VMA allocation to identify it in leak reports.
    // hostAccess selects the memory type for host-visible buffers (see BufferHostAccess); pass
    // eSequentialWrite for buffers the CPU only writes (never reads) to allow faster write-combined memory.
    bool initialize(vk::DeviceSize size, vk::BufferUsageFlags2 usage, vk::MemoryPropertyFlags properties, bool useBackingStore = false, const char* debugName = nullptr, BufferHostAccess hostAccess = BufferHostAccess::eRandom);
    void destroy();

    bool resize(vk::DeviceSize newSize);

    oc::vector<uint8>& getBackingStore() { return m_backingStore; }
    template<typename T>
    oc::span<T> getBackingStoreAs() { return oc::span<T>((T*)m_backingStore.data(), m_backingStore.size() / sizeof(T)); }
    template<typename T>
    void appendToBackingStore(oc::span<const T> items)
    {
        const uint8* bytes = (const uint8*)items.data();
        m_backingStore.insert(m_backingStore.end(), bytes, bytes + items.size_bytes());
    }
    bool upload(vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset = 0);
    template<typename T>
    bool upload(oc::span<const T> items, vk::DeviceSize dstOffset = 0)
    {
        return upload(items.size_bytes(), items.data(), dstOffset);
    }

    vk::DeviceSize appendUpload(vk::DeviceSize dataSize, const void* data);
    template<typename T>
    vk::DeviceSize appendUpload(oc::span<const T> items)
    {
        return appendUpload(items.size_bytes(), items.data());
    }

    bool uploadBackingStore();

    vk::DeviceAddress getDeviceAddress() const;

    oc::span<uint8> mapMemory(uint64 offset = 0, uint64 size = vk::WholeSize);
    template<typename T>
    oc::span<T> mapMemory(uint64 offsetBytes = 0, uint64 sizeBytes = vk::WholeSize)
    {
        oc::span<uint8> spanBytes = mapMemory(offsetBytes, sizeBytes);
        return oc::span<T>((T*)spanBytes.data(), spanBytes.size() / sizeof(T));
    }
    void unmapMemory();
    void flushMappedMemory(size_t size, size_t offset = 0);

    vk::Buffer getBuffer() const       { return m_buffer; }
	size_t getSize() const             { return (size_t)m_size; }
    const char* getDebugName() const   { return m_debugName; }

private:

    vk::DeviceSize m_size = 0;
    vk::Buffer m_buffer;
    VmaAllocation m_allocation = nullptr;
    void* m_mappedData = nullptr;

    vk::BufferUsageFlags2 m_usage;
    vk::MemoryPropertyFlags m_properties;
    const char* m_debugName = nullptr;
    BufferHostAccess m_hostAccess = BufferHostAccess::eRandom;
    oc::vector<uint8> m_backingStore;
    vk::DeviceSize m_uploadOffset = 0;
    bool m_hasBackingStore = false;
};