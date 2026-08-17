module;

#include <cstdio>

// The C VMA header lives in the global module fragment (no VMA_IMPLEMENTATION — that is compiled once in
// Vma.cpp). Its declarations attach to the global module so they link against that implementation.
#pragma warning(push, 0)
#include "vma/vk_mem_alloc.h"
#pragma warning(pop)

module RendererVK;

import Core;
import :VK;
import :Instance;
import :Device;

Allocator::Allocator() {}
Allocator::~Allocator()
{
    destroy();
}

bool Allocator::initialize()
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

void Allocator::destroy()
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
    }
}

bool Allocator::createImage(const vk::ImageCreateInfo& info, vk::Image& outImage, VmaAllocation& outAllocation,
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
        // VMA's allocation name is leak-report only; give the VkImage itself a real debug-utils object name
        // so validation layer messages (which report bare image handles) can be matched back to source.
        static PFN_vkSetDebugUtilsObjectNameEXT setObjectName =
            (PFN_vkSetDebugUtilsObjectNameEXT)Globals::instance.getInstance().getProcAddr("vkSetDebugUtilsObjectNameEXT");
        if (setObjectName)
        {
            const VkDebugUtilsObjectNameInfoEXT nameInfo{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .objectType = VK_OBJECT_TYPE_IMAGE,
                .objectHandle = (uint64_t)image,
                .pObjectName = debugName,
            };
            setObjectName((VkDevice)Globals::device.getDevice(), &nameInfo);
        }
    }
    outImage = vk::Image(image);
    return true;
}

void Allocator::destroyImage(vk::Image image, VmaAllocation allocation)
{
    if (image)
        vmaDestroyImage(m_allocator, (VkImage)image, allocation);
}

bool Allocator::createBuffer(const vk::BufferCreateInfo& info, vk::MemoryPropertyFlags properties,
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
        static PFN_vkSetDebugUtilsObjectNameEXT setObjectName =
            (PFN_vkSetDebugUtilsObjectNameEXT)Globals::instance.getInstance().getProcAddr("vkSetDebugUtilsObjectNameEXT");
        if (setObjectName)
        {
            const VkDebugUtilsObjectNameInfoEXT nameInfo{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .objectType = VK_OBJECT_TYPE_BUFFER,
                .objectHandle = (uint64_t)buffer,
                .pObjectName = debugName,
            };
            setObjectName((VkDevice)Globals::device.getDevice(), &nameInfo);
        }
    }
    outBuffer = vk::Buffer(buffer);
    outMappedData = allocInfo.pMappedData;
    return true;
}

void Allocator::destroyBuffer(vk::Buffer buffer, VmaAllocation allocation)
{
    if (buffer)
        vmaDestroyBuffer(m_allocator, (VkBuffer)buffer, allocation);
}

void Allocator::flushAllocation(VmaAllocation allocation, vk::DeviceSize offset, vk::DeviceSize size)
{
    if (allocation)
        (void)vmaFlushAllocation(m_allocator, allocation, offset, size);
}

uint64 Allocator::getAllocationSize(VmaAllocation allocation) const
{
    if (!allocation)
        return 0;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_allocator, allocation, &info);
    return info.size;
}

Allocator::MemoryUsage Allocator::getMemoryUsage() const
{
    MemoryUsage usage{};
    if (!m_allocator)
        return usage;

    const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
    vmaGetMemoryProperties(m_allocator, &memProps);

    // One budget entry per memory heap (cheap; backed by VK_EXT_memory_budget where available).
    oc::vector<VmaBudget> budgets(memProps->memoryHeapCount);
    vmaGetHeapBudgets(m_allocator, budgets.data());
    for (uint32 i = 0; i < memProps->memoryHeapCount; i++)
    {
        usage.usedBytes += budgets[i].statistics.allocationBytes;
        usage.reservedBytes += budgets[i].statistics.blockBytes;
        if (memProps->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage.budgetBytes += budgets[i].budget;
    }
    return usage;
}
