export module RendererVK:TextureStreamer;

import Core;
import :VK;
import :Layout;
import :Allocator;

// One mip level's byte range in the source .dds file plus its dimensions.
export struct TextureMipRange
{
    uint64 fileOffset = 0;
    uint32 byteSize = 0;
    uint16 width = 0;
    uint16 height = 0;
};

// Streaming metadata a Texture builds while parsing its DDS header. TextureManager::upload moves it
// into the TextureStreamer (only the manager knows the texture's index). Textures that never produce
// one (procedural, embedded, non-DDS, cubemap/array/3D) stay pinned at full residency.
export struct StreamedTextureMeta
{
    oc::string filePath;              // resolved source path; mip byte ranges are re-read from it
    oc::vector<TextureMipRange> mips; // [0..numMips), mip 0 = full resolution
    vk::Format format = vk::Format::eUndefined; // sRGB-resolved, matches the live image
    uint16 fullWidth = 0;
    uint16 fullHeight = 0;
    uint8 initialResidentTop = 0;      // first mip the load-time image spans (tail-only load > 0)
};

// The always-resident low-res tail = the smallest mips (maxDim <= tailMaxDim, at least the last one).
// Shared by the load path (tail-only initial upload) and the streamer's registration.
export inline uint8 computeStreamTailTop(const StreamedTextureMeta& meta, uint32 tailMaxDim)
{
    uint8 tailTop = (uint8)(meta.mips.size() - 1);
    while (tailTop > 0)
    {
        const TextureMipRange& mip = meta.mips[tailTop - 1];
        if ((uint32)oc::max(mip.width, mip.height) > tailMaxDim)
            break;
        tailTop--;
    }
    return tailTop;
}

// Streams texture mips in/out of VRAM against a fixed memory budget, prioritized by camera distance.
// Residency = "top K mips dropped": a residency change creates a new image spanning [residentTop..numMips),
// re-uploads its mips from the source file (read on the worker thread), rewrites the texture's slot in the
// bindless descriptor arrays (update-after-bind), and retires the old image after NUM_FRAMES_IN_FLIGHT
// frames. The low-res mip tail (maxDim <= tailMaxDim) is always resident so a texture never disappears.
export class TextureStreamer final
{
public:

    bool initialize();
    ~TextureStreamer();
    // Stops the disk worker and destroys any images still awaiting retirement (waits the GPU if needed).
    // Called from Renderer's teardown (after its waitIdle); the destructor is an idempotent fallback.
    void shutdown();
    // The GPU is known idle (capacity growth, shader reload, swapchain recreate): retire immediately.
    void onGpuIdle();

    // Called by TextureManager::upload for textures qualified for streaming. meta.initialResidentTop =
    // highest-resolution mip currently in VRAM, residentBytes = the live image's VMA allocation size.
    void registerTexture(uint16 texIdx, StreamedTextureMeta&& meta, uint64 residentBytes);
    // Unstreamable textures report their allocation size for stats/budget accounting only.
    void notePinned(uint64 allocatedBytes);
    // Called by TextureManager::free before the texture is destroyed: drops the slot's streaming state
    // and accounting, and orphans any in-flight op (its completion is discarded). allocatedBytes is the
    // live image's allocation size, used to reverse notePinned for unstreamable textures.
    void unregisterTexture(uint16 texIdx, uint64 allocatedBytes);

    // Whether new DDS textures should load tail-only and stream up (checked at Texture load time).
    bool isStreamingEnabled() const { return m_enabled; }
    uint32 tailMaxDim() const { return (uint32)m_tailMaxDim; }

    // Priority pass: a rendered instance using texIdx reports how many texels it can display this frame
    // (log2 of its projected screen size in pixels). Folds into a per-texture "wanted top mip" via an
    // atomic min, so it is callable from renderNodeThreadSafe. Cheap enough for one call per material
    // texture per instance per frame.
    void noteUse(uint16 texIdx, float log2TexelsAvailable);

    // Per-frame streaming step, called from Renderer::present() BEFORE StagingManager::update() (so
    // stream-in uploads join this frame's staging batch). Folds the frame's noteUse accumulators into
    // hysteresis-filtered desired mips, then (M4+) issues/completes streaming ops against the budget.
    void update();

    // Queues a bindless-slot rewrite of texIdx into every frame-in-flight's descriptor sets (after an
    // image swap). The CURRENT view is fetched at apply time, so entries survive descriptor set
    // re-allocation (texture capacity growth), shader reloads, and re-swaps of the same texture.
    // The Renderer drains one frame slot per recordCommandBuffers (its fence has been waited there).
    void queueDescriptorWrite(uint16 texIdx);
    oc::span<const uint16> getPendingDescriptorWrites(uint32 frameIdx) const { return m_pendingDescriptorWrites[frameIdx]; }
    void clearPendingDescriptorWrites(uint32 frameIdx) { m_pendingDescriptorWrites[frameIdx].clear(); }
    bool debugRewriteAllSlots() const { return m_debugRewriteAllSlots; }

    struct StreamerStats
    {
        uint64 budgetBytes = 0;
        uint64 residentBytes = 0;  // live allocations of streamable textures
        uint64 pinnedBytes = 0;    // unstreamable textures, always fully resident
        uint64 desiredBytes = 0;   // what the priority pass wants resident
        uint64 tailBytes = 0;      // the always-resident mip tails (part of residentBytes)
        uint32 numStreamable = 0;
        uint32 numOpsInFlight = 0;
    };
    StreamerStats getStats() const;

private:

    struct StreamState
    {
        StreamedTextureMeta meta;
        oc::vector<uint64> bytesFromMip; // [k] = file bytes of mips [k..numMips); size numMips + 1
        float log2FullDim = 0.0f;  // log2(max(fullWidth, fullHeight))
        uint8 numMips = 0;         // 0 = slot not streamable
        uint8 tailTop = 0;         // first mip of the always-resident tail
        uint8 residentTop = 0;
        uint8 desiredTop = 0;      // hysteresis-filtered priority-pass output
        uint8 wantTopThisFrame = 255; // this frame's noteUse atomic-min accumulator (255 = unseen)
        uint16 demoteCounter = 0;  // consecutive frames wanting less resolution than desiredTop
        uint32 lastSeenFrame = 0;
        uint64 residentBytes = 0;  // VMA allocation size of the live image
        uint64 tailBytes = 0;      // file byte size of mips [tailTop..numMips)
        uint8 targetTop = 0;       // residency the solver wants; ops are issued while != residentTop
        uint8 opTargetTop = 0;     // residency the in-flight op will produce (committed-bytes ledger)
        bool opInFlight = false;
        bool failed = false;       // disk read failed: pinned at current residency, never retried
        uint32 opId = 0;           // id of the in-flight op (stale completions are dropped)
    };

    // A disk-read request for mips [targetTop..dataMipEnd) of one texture (contiguous in the file), and
    // its result. With GPU mip copies enabled, promotions read only the mips the live image lacks
    // (dataMipEnd = residentTop at issue time); the rest is copied out of the old image on the GPU.
    // The worker thread only does CRT file IO into a plain buffer; every Vulkan/allocator interaction
    // stays on the main thread in update().
    struct StreamRequest
    {
        uint16 texIdx = 0;
        uint8 targetTop = 0;
        uint8 dataMipEnd = 0;
        uint32 opId = 0;
        uint64 fileOffset = 0;
        uint32 byteCount = 0;
        oc::unique_ptr<const char[]> filePath; // owned copy (null-terminated); the meta's string must not be referenced cross-thread
    };
    struct StreamCompletion
    {
        uint16 texIdx = 0;
        uint8 targetTop = 0;
        uint8 dataMipEnd = 0;
        uint32 opId = 0;
        uint32 byteCount = 0;
        oc::unique_ptr<uint8[]> data;
        bool ok = false;
    };
    struct RetiredImage
    {
        vk::Image image;
        VmaAllocation memory = nullptr;
        vk::ImageView view;
        uint32 swapFrame = 0; // streamer frame the swap happened; safe to destroy NUM_FRAMES_IN_FLIGHT later
    };

    void workerRun(std::stop_token stopToken);
    void applyCompletion(StreamCompletion&& completion);
    // Replaces the live image with one spanning [targetTop..numMips): uploads mips [targetTop..dataMipEnd)
    // from pMipData, GPU-copies [dataMipEnd..numMips) out of the old image, swaps, queues the descriptor
    // rewrite, and parks the old image for deferred destruction. Demotions pass pMipData == nullptr with
    // dataMipEnd == targetTop (pure GPU copy, no disk data).
    bool swapResidency(uint16 texIdx, StreamState& state, uint8 targetTop, const uint8* pMipData, uint8 dataMipEnd);
    void issueOps();

    oc::vector<StreamState> m_states; // indexed by texture idx; numMips == 0 => not streamed

    std::jthread m_worker;
    std::mutex m_requestMutex;
    std::condition_variable_any m_requestCv;
    oc::deque<StreamRequest> m_requests;      // guarded by m_requestMutex
    std::mutex m_completionMutex;
    oc::deque<StreamCompletion> m_completions; // guarded by m_completionMutex

    oc::vector<RetiredImage> m_retiredImages;
    uint32 m_opCounter = 0;
    uint32 m_numOpsInFlight = 0;

    oc::vector<uint16> m_pendingDescriptorWrites[RendererVKLayout::NUM_FRAMES_IN_FLIGHT];

    uint64 m_residentBytes = 0;        // streamable textures only (VMA allocation sizes)
    uint64 m_pinnedBytes = 0;
    uint64 m_tailBytes = 0;
    uint64 m_desiredBytes = 0;
    uint64 m_committedBytes = 0;       // file-byte estimate of resident + in-flight promotions (ledger)
    uint32 m_numStreamable = 0;
    uint32 m_frameCounter = 0;         // incremented once per update()

    int  m_budgetMB = 512;
    bool m_enabled = true;
    int  m_tailMaxDim = 128;
    int  m_maxOpsInFlight = 4;
    float m_maxMBPerFrame = 24.0f; // issued read volume per frame; keeps stream-ins well under the staging buffer
    bool m_gpuMipCopies = true;    // copy surviving mips old->new on the GPU (demotions skip the disk entirely)
    bool m_debugRewriteAllSlots = false;
    float m_mipBias = 0.0f;            // global quality knob: +1 = one mip level coarser everywhere
    float m_texelRatio = 1.0f;         // texels wanted per projected pixel (tiling textures want > 1)
    int  m_demoteHysteresisFrames = 60;
    int  m_decayFrames = 120;          // unseen for this long -> desire only the tail
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DATA)
    TextureStreamer textureStreamer;
}
