export module RendererVK:StagingManager;

import RendererVK.fwd;
import Core;
import :VK;
import :Buffer;
import :CommandBuffer;

class SwapChain;

export class StagingManager final
{
public:

    StagingManager();
    ~StagingManager();
    StagingManager(const StagingManager&) = delete;

    bool initialize();

    vk::Semaphore upload(vk::Buffer dstBuffer, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset = 0);
    vk::Semaphore uploadImage(vk::Image dstImage, uint32 imageWidth, uint32 imageHeight, vk::DeviceSize dataSize, const void* data, uint32 mipLevel, vk::DeviceSize dstOffset = 0);
    vk::Semaphore uploadImageAndGenerateMipMaps(vk::Image image, uint32 imageWidth, uint32 imageHeight, uint32 numMipLevels, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset = 0);
    // GPU image->image mip copies (texture streaming residency swaps): src is a retired streamed image
    // whose mips are all eShaderReadOnlyOptimal and which nothing samples after this frame's swap; the
    // dst mips [dstBaseMip..dstBaseMip + regions.size()) are freshly created (eUndefined) and end up
    // eShaderReadOnlyOptimal. Executed in update()'s batch alongside the staged uploads.
    vk::Semaphore copyImageMips(vk::Image srcImage, vk::Image dstImage, uint32 dstBaseMip, oc::vector<vk::ImageCopy>&& regions);
    vk::Semaphore update();
    // Submits any queued copies now so their destination buffers can be destroyed (capacity growth).
    // The signal semaphore is kept as m_nextUpdateSemaphore so the next update() consumes it.
    void flushPending() { m_nextUpdateSemaphore = update(); }

    // Call before queuing an upload() into a buffer that is shared (not per-frame-in-flight) and read
    // full-range every frame by GI/RTAO compute or the draw passes (mesh mega-buffers, MeshInfos,
    // MaterialInfos, InstanceOffsets). upload()'s ring buffer can implicitly submit (via update()) the
    // moment it overflows, regardless of caller - so a copy into one of these buffers can go to the GPU
    // far earlier than whatever call site eventually flushes it, racing an in-flight frame's read with no
    // synchronization. Draining here, once per frame (cheap no-op on every later call this frame - nothing
    // new can have been submitted since, until the next resetSharedWriteGate), closes that window
    // regardless of when the eventual flush happens. Reset once per frame right after that frame's
    // primary command buffer is submitted (Renderer::present()).
    void ensureDrainedForSharedWrite();
    void resetSharedWriteGate() { m_drainedForSharedWrite = false; }

private:

    SwapChain* m_swapChain = nullptr;

    static constexpr int NUM_STAGING_BUFFERS = 2;
    Buffer m_stagingBuffers[NUM_STAGING_BUFFERS];
    oc::span<uint8> m_mappedStagingBuffers[NUM_STAGING_BUFFERS];
    vk::Fence m_fences[NUM_STAGING_BUFFERS];
    vk::Semaphore m_semaphores[NUM_STAGING_BUFFERS];
    CommandBuffer m_commandBuffers[NUM_STAGING_BUFFERS];

    vk::Semaphore m_nextUpdateSemaphore = VK_NULL_HANDLE;

    int m_currentBuffer = 0;
    vk::DeviceSize m_currentBufferOffset = 0;
    oc::vector<oc::pair<vk::Buffer, vk::BufferCopy>> m_bufferCopyRegions;
    oc::vector<oc::pair<vk::Image, vk::BufferImageCopy>> m_imageCopyRegions;
    struct UploadAndMip
    {
        vk::Image image;
        vk::BufferImageCopy bufferCopy;
        uint32 width, height, numMips;
    };
    oc::vector<UploadAndMip> m_imageCopyAndMipList;
    struct ImageMipCopy
    {
        vk::Image srcImage;
        vk::Image dstImage;
        uint32 dstBaseMip;
        oc::vector<vk::ImageCopy> regions; // contiguous dst mips starting at dstBaseMip
    };
    oc::vector<ImageMipCopy> m_imageMipCopyList;
    oc::span<uint8> m_mappedMemory;

    bool m_drainedForSharedWrite = false;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DATA)
    StagingManager stagingManager;
}