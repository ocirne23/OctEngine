module;

#pragma warning(push)
#pragma warning(disable: 5202)
// Forward-declare VMA's opaque handle types in the global module fragment so they name the same global
// entities that vk_mem_alloc.h defines (a forward declaration in module purview would instead create
// distinct module-owned types that wouldn't match the C API in the implementation unit).
struct VmaAllocator_T;
struct VmaAllocation_T;
#pragma warning(pop)

export module RendererVK:Allocator;

import Core;
import :VK;

// Opaque VMA handles re-exported as aliases, so vk_mem_alloc.h stays out of this interface (and out of
// every module that imports it). The definitions live in Vma.cpp via VMA_IMPLEMENTATION.
export using VmaAllocator = VmaAllocator_T*;
export using VmaAllocation = VmaAllocation_T*;

// How the CPU touches a host-visible buffer's mapping. Device-local buffers ignore this.
export enum class BufferHostAccess
{
    // CPU reads the mapping (e.g. readback / resize copy). VMA keeps it in cached host memory so reads
    // are fast. This is the safe default.
    eRandom,
    // CPU only ever writes the mapping (uploads, per-frame UBO/SSBO fills). Lets VMA place it in
    // write-combined (uncached) - or ReBAR device-local-host-visible - memory, which is faster for the
    // GPU to read and for sequential CPU writes. Never read such a mapping on the CPU: WC reads are slow.
    eSequentialWrite,
};

// Thin wrapper around the VulkanMemoryAllocator. Owns a single VmaAllocator for the whole renderer and
// is the one place GPU buffer/image memory is allocated; everything else goes through Buffer / the image
// helpers which call into here.
export class Allocator final
{
public:
    Allocator();
    ~Allocator();
    Allocator(const Allocator&) = delete;

    bool initialize();
    void destroy();

    // Device-local image. The optional debugName is attached to the allocation for leak reporting.
    // Returns false on failure.
    bool createImage(const vk::ImageCreateInfo& info, vk::Image& outImage, VmaAllocation& outAllocation,
        const char* debugName = nullptr);
    void destroyImage(vk::Image image, VmaAllocation allocation);

    // Buffer allocated to satisfy the given memory properties. Host-visible allocations are persistently
    // mapped; outMappedData receives the base pointer (nullptr for device-local). The optional debugName
    // is attached to the allocation for leak reporting. Returns false on failure.
    bool createBuffer(const vk::BufferCreateInfo& info, vk::MemoryPropertyFlags properties,
        vk::Buffer& outBuffer, VmaAllocation& outAllocation, void*& outMappedData,
        BufferHostAccess hostAccess = BufferHostAccess::eRandom, const char* debugName = nullptr);
    void destroyBuffer(vk::Buffer buffer, VmaAllocation allocation);

    // Flush a host-visible (possibly non-coherent) allocation. Atom-size alignment is handled internally;
    // a no-op when the underlying memory is host-coherent. Pass vk::WholeSize to flush to the end.
    void flushAllocation(VmaAllocation allocation, vk::DeviceSize offset, vk::DeviceSize size);

    struct MemoryUsage
    {
        uint64 usedBytes;     // sum of live allocations (the bytes our resources actually occupy)
        uint64 reservedBytes; // memory VMA has reserved in blocks (>= usedBytes, includes free space)
        uint64 budgetBytes;   // how many bytes the process may use across device-local heaps
    };
    MemoryUsage getMemoryUsage() const;

    // Size of a single live allocation (0 for null).
    uint64 getAllocationSize(VmaAllocation allocation) const;

private:
    VmaAllocator m_allocator = nullptr;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DEVICE)
    Allocator gpuAllocator;
}
