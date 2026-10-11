export module RendererVK:StagingManager;

import RendererVK.fwd;
import Core;
import :VK;
import :Buffer;
import :CommandBuffer;

class SwapChain;

// THREAD-SAFE: every public entry point serializes on one internal mutex, so upload*() may be called
// from jobs/worker threads (the beginFrame UBO upload, the terrain's chunk uploads). The mutex covers only
// the ring claim: upload*() memcpys into its claimed range after the unlock, so concurrent uploads copy
// in parallel, and a flush waits for every writer of the buffer it submits. A ring overflow inside
// upload*() implicitly submits the batch to the graphics queue - legal from any thread because ALL
// queue calls go through Device's graphics-queue mutex (CommandBuffer::submitGraphics,
// SwapChain::present, Device::graphicsQueueWaitIdle). Lock order is staging mutex -> queue mutex,
// never the reverse. update() waits fences while holding the mutex, so concurrent uploads block for
// that duration - inherent to the shared ring, keep bulk uploads off latency-critical threads.
export class StagingManager final
{
public:

    StagingManager();
    ~StagingManager();
    StagingManager(const StagingManager&) = delete;

    bool initialize();

    vk::Semaphore upload(vk::Buffer dstBuffer, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset = 0);
    // upload() into a buffer that is SHARED (not per-frame-in-flight) and read every frame by the draw passes, GI / RTAO
    // or the BLAS builds (the mesh mega-buffers, MeshInfos, MaterialInfos, InstanceOffsets, the LOD tables): a copy may
    // rewrite bytes a frame in flight still reads, and a ring overflow can submit the batch the moment it fills, long
    // before the frame's own flush. So the batch it lands in opens with a barrier against EVERYTHING submitted before it
    // on the queue - the GPU orders the copy after those reads, the CPU never waits. (Until 2026-10-11 the first such
    // upload of a frame drained the queue on the CPU: graphicsQueueWaitIdle, 11 ms on main in a terrain upload.)
    vk::Semaphore uploadShared(vk::Buffer dstBuffer, vk::DeviceSize dataSize, const void* data, vk::DeviceSize dstOffset = 0);
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
    void flushPending();

private:

    vk::Semaphore updateNoLock(); // the body of update(); callers hold m_mutex
    vk::DeviceSize claimNoLock(vk::DeviceSize dataSize, vk::DeviceSize alignment); // the ring range to copy into
    vk::Semaphore copyAndUnlock(std::unique_lock<std::mutex>& lock, vk::DeviceSize offset, vk::DeviceSize dataSize, const void* data);

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

    std::mutex m_mutex;
    oc::atomic<uint32> m_numWriters[NUM_STAGING_BUFFERS]{}; // copies in flight outside m_mutex, per buffer
    bool m_sharedWrite = false; // the current batch holds an uploadShared copy (under m_mutex)
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DATA)
    StagingManager stagingManager;
}