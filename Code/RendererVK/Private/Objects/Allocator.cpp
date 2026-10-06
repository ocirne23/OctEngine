module;

#include <cstdio>

// The C VMA header lives in the global module fragment (no VMA_IMPLEMENTATION - that is compiled once in
// Vma.cpp). Its declarations attach to the global module so they link against that implementation.
#pragma warning(push, 0)
#include "vma/vk_mem_alloc.h"
#pragma warning(pop)

module RendererVK;

import Core;
import :VK;
import :Instance;
import :Device;

GpuAllocator::GpuAllocator() {}
GpuAllocator::~GpuAllocator()
{
    destroy();
}

bool GpuAllocator::initialize()
{
    VmaAllocatorCreateInfo createInfo{};
    createInfo.instance = Globals::instance.getInstance();
    createInfo.physicalDevice = Globals::device.getPhysicalDevice();
    createInfo.device = Globals::device.getDevice();
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    // BUFFER_DEVICE_ADDRESS: the device enables bufferDeviceAddress and many buffers request it; this lets
    // VMA add the device-address allocation flag automatically so getDeviceAddress works on those buffers.
    // KHR_MAINTENANCE5: buffers pass their usage via VkBufferUsageFlags2CreateInfo, which VMA only reads
    // (needed by VMA_MEMORY_USAGE_AUTO) when maintenance5 is enabled. The device enables it.
    createInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

    if (vmaCreateAllocator(&createInfo, &m_allocator) != VK_SUCCESS)
    {
        assert(false && "Failed to create VMA allocator");
        return false;
    }
    return true;
}

void GpuAllocator::destroy()
{
    if (m_allocator)
    {
#ifndef NDEBUG
        // Before tearing down, list anything still allocated. vmaBuildStatsString includes each
        // allocation's debug name, so this names leaks regardless of whether they are block or dedicated
        // allocations (VMA's own per-block leak log only covers the former).
        VmaTotalStatistics totalStats{};
        vmaCalculateStatistics(m_allocator, &totalStats);
        if (totalStats.total.statistics.allocationCount > 0)
        {
            char* statsString = nullptr;
            vmaBuildStatsString(m_allocator, &statsString, VK_TRUE);
            fprintf(stderr, "VMA: %u allocation(s) still alive at allocator destruction:\n%s\n",
                totalStats.total.statistics.allocationCount, statsString ? statsString : "(no stats)");
            fflush(stderr);
            vmaFreeStatsString(m_allocator, statsString);
        }
#endif
        vmaDestroyAllocator(m_allocator);
        m_allocator = nullptr;
        std::lock_guard lock(m_liveMutex);
        m_live.clear();
    }
}

void GpuAllocator::registerAllocation(VmaAllocation allocation, const GpuAllocationInfo& desc)
{
    std::lock_guard lock(m_liveMutex);
    vmaSetAllocationUserData(m_allocator, allocation, (void*)(uintptr_t)m_live.size());
    m_live.push_back(LiveAllocation{ allocation, desc });
}

void GpuAllocator::unregisterAllocation(VmaAllocation allocation)
{
    std::lock_guard lock(m_liveMutex);
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_allocator, allocation, &info);
    const size_t index = (size_t)(uintptr_t)info.pUserData;
    assert(index < m_live.size() && m_live[index].allocation == allocation);
    if (index + 1 != m_live.size())
    {
        m_live[index] = m_live.back();
        vmaSetAllocationUserData(m_allocator, m_live[index].allocation, (void*)(uintptr_t)index);
    }
    m_live.pop_back();
}

void GpuAllocator::forEachAllocation(GpuAllocationVisit visit, void* ctx) const
{
    std::lock_guard lock(m_liveMutex);
    if (!m_allocator)
        return;
    const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
    vmaGetMemoryProperties(m_allocator, &memProps);
    for (const LiveAllocation& live : m_live)
    {
        VmaAllocationInfo2 info2{};
        vmaGetAllocationInfo2(m_allocator, live.allocation, &info2);
        const VmaAllocationInfo& info = info2.allocationInfo;
        const VkMemoryType& type = memProps->memoryTypes[info.memoryType];
        GpuAllocationInfo out = live.desc;
        out.name = info.pName;
        out.bytes = info.size;
        out.blockBytes = info2.blockSize;
        out.memoryFlags = type.propertyFlags;
        out.deviceLocal = (memProps->memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        out.dedicated = info2.dedicatedMemory != VK_FALSE;
        out.mapped = info.pMappedData != nullptr;
        visit(ctx, out);
    }
}

void GpuAllocator::enumName(GpuEnum kind, uint64 value, char* out, size_t outSize)
{
    auto copy = [out, outSize](const auto& name) // vk::to_string's std::string
    {
        // The flag sets print as "{ A | B }": drop the braces
        size_t begin = 0, end = name.size();
        if (end >= 2 && name[0] == '{' && name[end - 1] == '}')
        {
            begin = 1;
            --end;
            while (begin < end && name[begin] == ' ') ++begin;
            while (end > begin && name[end - 1] == ' ') --end;
        }
        _snprintf_s(out, outSize, _TRUNCATE, "%.*s", (int)(end - begin), name.c_str() + begin);
    };
    switch (kind)
    {
    case GpuEnum::Format:           copy(vk::to_string((vk::Format)value)); break;
    case GpuEnum::ImageUsage:       copy(vk::to_string(vk::ImageUsageFlags((vk::ImageUsageFlagBits)value))); break;
    case GpuEnum::BufferUsage:      copy(vk::to_string(vk::BufferUsageFlags2((vk::BufferUsageFlagBits2)value))); break;
    case GpuEnum::MemoryProperties: copy(vk::to_string(vk::MemoryPropertyFlags((vk::MemoryPropertyFlagBits)value))); break;
    }
}

bool GpuAllocator::createImage(const vk::ImageCreateInfo& info, vk::Image& outImage, VmaAllocation& outAllocation,
    const char* debugName)
{
    const VkImageCreateInfo ci = info;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage image = VK_NULL_HANDLE;
    if (vmaCreateImage(m_allocator, &ci, &aci, &image, &outAllocation, nullptr) != VK_SUCCESS)
    {
        assert(false && "Failed to create image");
        return false;
    }
    if (debugName)
    {
        vmaSetAllocationName(m_allocator, outAllocation, debugName);
        // VMA's allocation name is leak-report only; the VkImage itself gets the debug-utils object name.
        Globals::device.setDebugName(vk::Image(image), debugName);
    }
    GpuAllocationInfo desc;
    desc.image = true;
    desc.width = info.extent.width;
    desc.height = info.extent.height;
    desc.depth = info.extent.depth;
    desc.mips = (uint16)info.mipLevels;
    desc.layers = (uint16)info.arrayLayers;
    desc.samples = (uint8)(uint32)info.samples;
    desc.format = (uint32)info.format;
    desc.usage = (uint32)info.usage;
    registerAllocation(outAllocation, desc);
    outImage = vk::Image(image);
    return true;
}

void GpuAllocator::destroyImage(vk::Image image, VmaAllocation allocation)
{
    if (image)
    {
        if (allocation)
            unregisterAllocation(allocation);
        vmaDestroyImage(m_allocator, (VkImage)image, allocation);
    }
}

bool GpuAllocator::createBuffer(const vk::BufferCreateInfo& info, vk::MemoryPropertyFlags properties,
    vk::Buffer& outBuffer, VmaAllocation& outAllocation, void*& outMappedData, BufferHostAccess hostAccess,
    const char* debugName)
{
    const VkBufferCreateInfo ci = info;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.requiredFlags = (VkMemoryPropertyFlags)(VkFlags)properties;
    // Host-visible buffers are kept persistently mapped so uploads/readbacks can hit the pointer directly.
    // The access pattern picks the memory type: SEQUENTIAL_WRITE allows fast write-combined placement,
    // RANDOM keeps it cached for CPU reads.
    if (properties & vk::MemoryPropertyFlagBits::eHostVisible)
    {
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.flags |= (hostAccess == BufferHostAccess::eSequentialWrite)
            ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    if (vmaCreateBuffer(m_allocator, &ci, &aci, &buffer, &outAllocation, &allocInfo) != VK_SUCCESS)
    {
        assert(false && "Failed to create buffer");
        return false;
    }
    if (debugName)
    {
        vmaSetAllocationName(m_allocator, outAllocation, debugName);
        Globals::device.setDebugName(vk::Buffer(buffer), debugName);
    }
    GpuAllocationInfo desc;
    desc.bufferSize = info.size;
    desc.usage = (uint32)info.usage;
    // maintenance5: the real usage rides in the pNext chain (and wins over info.usage)
    for (const VkBaseInStructure* next = (const VkBaseInStructure*)ci.pNext; next != nullptr; next = next->pNext)
        if (next->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO)
            desc.usage = ((const VkBufferUsageFlags2CreateInfo*)next)->usage;
    registerAllocation(outAllocation, desc);
    outBuffer = vk::Buffer(buffer);
    outMappedData = allocInfo.pMappedData;
    return true;
}

void GpuAllocator::destroyBuffer(vk::Buffer buffer, VmaAllocation allocation)
{
    if (buffer)
    {
        if (allocation)
            unregisterAllocation(allocation);
        vmaDestroyBuffer(m_allocator, (VkBuffer)buffer, allocation);
    }
}

void GpuAllocator::flushAllocation(VmaAllocation allocation, vk::DeviceSize offset, vk::DeviceSize size)
{
    if (allocation)
        (void)vmaFlushAllocation(m_allocator, allocation, offset, size);
}

uint64 GpuAllocator::getAllocationSize(VmaAllocation allocation) const
{
    if (!allocation)
        return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_allocator, allocation, &info);
    return info.size;
}

GpuAllocator::MemoryUsage GpuAllocator::getMemoryUsage() const
{
    MemoryUsage usage{};
    if (!m_allocator)
        return usage;

    const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
    vmaGetMemoryProperties(m_allocator, &memProps);

    // One budget entry per memory heap (cheap; backed by VK_EXT_memory_budget where available).
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(m_allocator, budgets);
    for (uint32 i = 0; i < memProps->memoryHeapCount; i++)
    {
        usage.usedBytes += budgets[i].statistics.allocationBytes;
        usage.reservedBytes += budgets[i].statistics.blockBytes;
        if (memProps->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            usage.budgetBytes += budgets[i].budget;
            usage.deviceLocalUsageBytes += budgets[i].usage;
        }
    }
    return usage;
}
