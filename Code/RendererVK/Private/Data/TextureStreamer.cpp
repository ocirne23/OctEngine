module;

#include <cstdio> // fopen_s/_fseeki64/SEEK_SET for the disk worker

module RendererVK;

import Core;
import Core.Tweaks;
import :TextureStreamer;
import :Device;
import :Allocator;
import :StagingManager;
import :TextureManager;
import :Texture;
import :Layout;

// StreamRequest carries an owned, null-terminated copy of the source path (unique_ptr<const char[]>)
// instead of referencing the meta's oc::string across the worker thread.
static oc::unique_ptr<const char[]> copyPathString(const oc::string& path)
{
    oc::unique_ptr<char[]> buffer = oc::make_unique<char[]>(path.size() + 1);
    memcpy(buffer.get(), path.c_str(), path.size() + 1);
    return buffer;
}

bool TextureStreamer::initialize()
{
    Tweak::intVar("Texture Streaming", "Budget (MB)", &m_budgetMB, 64, 8192);
    Tweak::boolean("Texture Streaming", "Enabled", &m_enabled);
    Tweak::intVar("Texture Streaming", "Tail max dim", &m_tailMaxDim, 32, 512);
    Tweak::floatVar("Texture Streaming", "Mip bias", &m_mipBias, -4.0f, 4.0f);
    Tweak::floatVar("Texture Streaming", "Texel ratio", &m_texelRatio, 0.25f, 4.0f);
    Tweak::intVar("Texture Streaming", "Max ops in flight", &m_maxOpsInFlight, 0, 32);
    Tweak::floatVar("Texture Streaming", "Max MB/frame", &m_maxMBPerFrame, 1.0f, 64.0f);
    Tweak::boolean("Texture Streaming", "GPU mip copies", &m_gpuMipCopies);
    Tweak::intVar("Texture Streaming", "Demote hysteresis frames", &m_demoteHysteresisFrames, 1, 600);
    Tweak::intVar("Texture Streaming", "Decay frames", &m_decayFrames, 1, 3600);
    Tweak::boolean("Texture Streaming", "Debug rewrite all slots", &m_debugRewriteAllSlots);

    m_worker = std::jthread([this](std::stop_token stopToken) { workerRun(stopToken); });
    return true;
}

TextureStreamer::~TextureStreamer()
{
    shutdown();
}

void TextureStreamer::shutdown()
{
    if (m_worker.joinable())
    {
        m_worker.request_stop();
        m_requestCv.notify_all();
        m_worker.join();
    }
    m_requests.clear();
    m_completions.clear();
    if (!m_retiredImages.empty())
    {
        // Fallback path (Renderer teardown normally calls shutdown after its own waitIdle).
        (void)Globals::device.getGraphicsQueue().waitIdle();
        onGpuIdle();
    }
}

void TextureStreamer::onGpuIdle()
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (RetiredImage& retired : m_retiredImages)
    {
        if (retired.view)
            vkDevice.destroyImageView(retired.view);
        Globals::gpuAllocator.destroyImage(retired.image, retired.memory);
    }
    m_retiredImages.clear();
}

void TextureStreamer::workerRun(std::stop_token stopToken)
{
    Globals::profiler.registerThread("TexStream", Profiler::SORT_KEY_BACKGROUND + 0);
    //ProfileScope profileScope("TextureStreamer::workerRun", EProfileCategory::Renderer);

    for (;;)
    {
        StreamRequest request;
        {
            std::unique_lock lock(m_requestMutex);
            if (!m_requestCv.wait(lock, stopToken, [&] { return !m_requests.empty(); }))
                return; // stop requested
            request = oc::move(m_requests.front());
            m_requests.pop_front();
        }
        ProfileScope profileScope("Tex mip read", EProfileCategory::Renderer); // after the cv wait: measure the work, not the idle

        StreamCompletion completion;
        completion.texIdx = request.texIdx;
        completion.targetTop = request.targetTop;
        completion.dataMipEnd = request.dataMipEnd;
        completion.opId = request.opId;
        completion.byteCount = request.byteCount;
        completion.data = oc::unique_ptr<uint8[]>(new uint8[request.byteCount]);

        FILE* pFile = nullptr;
        fopen_s(&pFile, request.filePath.get(), "rb");
        if (pFile)
        {
            completion.ok = _fseeki64(pFile, (int64)request.fileOffset, SEEK_SET) == 0
                && fread(completion.data.get(), 1, request.byteCount, pFile) == request.byteCount;
            fclose(pFile);
        }

        {
            std::scoped_lock lock(m_completionMutex);
            m_completions.push_back(oc::move(completion));
        }
    }
}

void TextureStreamer::registerTexture(uint16 texIdx, StreamedTextureMeta&& meta, uint64 residentBytes)
{
    if (texIdx >= m_states.size())
        m_states.resize(texIdx + 1);

    StreamState& state = m_states[texIdx];
    assert(state.numMips == 0 && "Texture registered twice");
    state.meta = oc::move(meta);
    state.numMips = (uint8)state.meta.mips.size();
    state.residentTop = state.meta.initialResidentTop;
    state.residentBytes = residentBytes;
    state.log2FullDim = std::log2((float)oc::max(state.meta.fullWidth, state.meta.fullHeight));
    state.lastSeenFrame = m_frameCounter;

    // The tail = the smallest mips (maxDim <= tailMaxDim), always resident so the texture never
    // disappears from the descriptor array. At least the last mip always qualifies.
    state.tailTop = computeStreamTailTop(state.meta, (uint32)m_tailMaxDim);
    assert(state.residentTop == 0 || state.residentTop == state.tailTop);
    state.desiredTop = state.tailTop;
    state.targetTop = state.residentTop;

    state.bytesFromMip.resize(state.numMips + 1, 0);
    for (int32 i = state.numMips - 1; i >= 0; i--)
        state.bytesFromMip[i] = state.bytesFromMip[i + 1] + state.meta.mips[i].byteSize;
    state.tailBytes = state.bytesFromMip[state.tailTop];

    m_residentBytes += residentBytes;
    m_tailBytes += state.tailBytes;
    m_numStreamable++;
}

void TextureStreamer::notePinned(uint64 allocatedBytes)
{
    m_pinnedBytes += allocatedBytes;
}

void TextureStreamer::unregisterTexture(uint16 texIdx, uint64 allocatedBytes)
{
    if (texIdx < m_states.size() && m_states[texIdx].numMips != 0)
    {
        StreamState& state = m_states[texIdx];
        if (state.opInFlight)
            m_numOpsInFlight--; // the completion is dropped by applyCompletion (opInFlight false / slot reset)
        m_residentBytes -= state.residentBytes;
        m_tailBytes -= state.tailBytes;
        m_numStreamable--;
        state = StreamState{}; // numMips 0 = slot unused; a recycled slot re-registers cleanly
        // m_desiredBytes/m_committedBytes are recomputed from the states every update().
    }
    else
    {
        m_pinnedBytes -= oc::min(m_pinnedBytes, allocatedBytes);
    }
}

void TextureStreamer::noteUse(uint16 texIdx, float log2TexelsAvailable)
{
    if (texIdx >= m_states.size())
        return;
    StreamState& state = m_states[texIdx];
    if (state.numMips == 0)
        return;
    const int32 want = (int32)std::ceil(state.log2FullDim - log2TexelsAvailable - std::log2(m_texelRatio) + m_mipBias);
    const uint8 wantTop = (uint8)oc::clamp(want, 0, (int32)state.tailTop);

    oc::atomic_ref<uint8> accum(state.wantTopThisFrame);
    uint8 cur = accum.load(oc::memory_order_relaxed);
    while (wantTop < cur && !accum.compare_exchange_weak(cur, wantTop, oc::memory_order_relaxed)) {}
}

void TextureStreamer::applyCompletion(StreamCompletion&& completion)
{
    if (completion.texIdx >= m_states.size())
        return;
    StreamState& state = m_states[completion.texIdx];
    if (state.numMips == 0 || !state.opInFlight || state.opId != completion.opId)
        return; // stale op (superseded or from before a reset)
    state.opInFlight = false;
    m_numOpsInFlight--;
    if (!completion.ok)
    {
        // Missing/truncated file: freeze this texture at its current residency instead of retry-looping.
        printf("TextureStreamer: read failed for '%s', pinning at mip %u\n", state.meta.filePath.c_str(), state.residentTop);
        state.failed = true;
        return;
    }
    const uint8 targetTop = completion.targetTop;
    if (targetTop == state.residentTop)
        return;

    if (!swapResidency(completion.texIdx, state, targetTop, completion.data.get(), completion.dataMipEnd))
        state.failed = true;
}

bool TextureStreamer::swapResidency(uint16 texIdx, StreamState& state, uint8 targetTop, const uint8* pMipData, uint8 dataMipEnd)
{
    assert(targetTop != state.residentTop);
    assert(dataMipEnd >= state.residentTop && "GPU-copied mips must exist in the old image");

    // Build the replacement image spanning mips [targetTop..numMips). Freshly read mips join this
    // frame's staging batch (the per-mip upload path transitions each level Undefined -> TransferDst ->
    // ShaderReadOnly); the rest is GPU-copied out of the old image in the same batch. The primary CB
    // waits the staging semaphore, so this frame's rendering samples a fully populated image.
    const uint32 numMips = (uint32)state.numMips - targetTop;
    const vk::ImageCreateInfo imageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = state.meta.format,
        .extent = { oc::max(1u, (uint32)state.meta.fullWidth >> targetTop), oc::max(1u, (uint32)state.meta.fullHeight >> targetTop), 1 },
        .mipLevels = numMips,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    vk::Image image;
    VmaAllocation memory = nullptr;
    if (!Globals::gpuAllocator.createImage(imageCreateInfo, image, memory, "TextureStreamed"))
        return false;
    const vk::ImageViewCreateInfo viewInfo{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = state.meta.format,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0,
            .levelCount = vk::RemainingMipLevels, .baseArrayLayer = 0, .layerCount = 1 },
    };
    auto viewResult = Globals::device.getDevice().createImageView(viewInfo);
    if (viewResult.result != vk::Result::eSuccess)
    {
        Globals::gpuAllocator.destroyImage(image, memory);
        return false;
    }

    uint64 bufferOffset = 0;
    for (uint32 i = targetTop; i < dataMipEnd; i++)
    {
        const TextureMipRange& mip = state.meta.mips[i];
        Globals::stagingManager.uploadImage(image, mip.width, mip.height, mip.byteSize, pMipData + bufferOffset, i - targetTop);
        bufferOffset += mip.byteSize;
    }

    Texture& texture = Globals::textureManager.getTextureMutable(texIdx);
    if (dataMipEnd < state.numMips)
    {
        oc::vector<vk::ImageCopy> regions;
        regions.reserve(state.numMips - dataMipEnd);
        for (uint32 i = dataMipEnd; i < state.numMips; i++)
        {
            const TextureMipRange& mip = state.meta.mips[i];
            regions.push_back(vk::ImageCopy{
                .srcSubresource = { vk::ImageAspectFlagBits::eColor, i - state.residentTop, 0, 1 },
                .dstSubresource = { vk::ImageAspectFlagBits::eColor, i - targetTop, 0, 1 },
                .extent = { mip.width, mip.height, 1 },
            });
        }
        Globals::stagingManager.copyImageMips(texture.getImage(), image, (uint32)(dataMipEnd - targetTop), oc::move(regions));
    }

    // Swap into the live Texture; the old image keeps servicing the frames still in flight and is
    // destroyed NUM_FRAMES_IN_FLIGHT streamer frames later.
    RetiredImage retired{ .swapFrame = m_frameCounter };
    texture.adoptStreamedImage(image, memory, viewResult.value, numMips, retired.image, retired.memory, retired.view);
    m_retiredImages.push_back(retired);

    const uint64 newResidentBytes = Globals::gpuAllocator.getAllocationSize(memory);
    m_residentBytes = m_residentBytes - state.residentBytes + newResidentBytes;
    state.residentBytes = newResidentBytes;
    state.residentTop = targetTop;

    queueDescriptorWrite(texIdx);
    return true;
}

void TextureStreamer::issueOps()
{
    if (!m_enabled)
        return;
    const uint64 budgetBytes = (uint64)m_budgetMB * 1024ull * 1024ull;
    const uint64 availBytes = budgetBytes > m_pinnedBytes ? budgetBytes - m_pinnedBytes : 0;
    uint64 bytesLeft = (uint64)((double)m_maxMBPerFrame * 1024.0 * 1024.0);
    bool issuedAny = false;

    // dataMipEnd = end of the disk-read mip range [targetTop..dataMipEnd); the rest comes from the old
    // image via GPU copies (see swapResidency).
    auto tryIssue = [&](uint32 texIdx, StreamState& state, uint8 dataMipEnd) -> bool
    {
        if (m_numOpsInFlight >= (uint32)oc::max(0, m_maxOpsInFlight))
            return false;
        const uint64 readBytes = state.bytesFromMip[state.targetTop] - state.bytesFromMip[dataMipEnd];
        if (readBytes > bytesLeft && issuedAny)
            return false; // over this frame's IO volume; the first op of a frame may always go
        bytesLeft -= oc::min(bytesLeft, readBytes);
        issuedAny = true;

        state.opId = ++m_opCounter;
        state.opTargetTop = state.targetTop;
        state.opInFlight = true;
        m_numOpsInFlight++;
        {
            std::scoped_lock lock(m_requestMutex);
            m_requests.push_back(StreamRequest{
                .texIdx = (uint16)texIdx,
                .targetTop = state.targetTop,
                .dataMipEnd = dataMipEnd,
                .opId = state.opId,
                .fileOffset = state.meta.mips[state.targetTop].fileOffset,
                .byteCount = (uint32)readBytes,
                .filePath = copyPathString(state.meta.filePath),
            });
        }
        return true;
    };

    // Demotions first: they free memory and never need budget headroom. With GPU copies they skip the
    // disk entirely — the surviving mips are copied out of the old image right here, synchronously.
    uint32 demotionsLeft = (uint32)oc::max(0, m_maxOpsInFlight);
    for (uint32 texIdx = 0; texIdx < (uint32)m_states.size() && demotionsLeft > 0; ++texIdx)
    {
        StreamState& state = m_states[texIdx];
        if (state.numMips == 0 || state.opInFlight || state.failed || state.targetTop <= state.residentTop)
            continue;
        if (m_gpuMipCopies)
        {
            demotionsLeft--;
            if (!swapResidency((uint16)texIdx, state, state.targetTop, nullptr, state.targetTop))
                state.failed = true;
        }
        else
            tryIssue(texIdx, state, state.numMips);
    }
    // Promotions ordered by how much resolution the priority pass wants (lowest desiredTop = biggest on
    // screen sharpens first, cheaper reads breaking ties), and only while the committed ledger (resident
    // + in-flight growth) stays under budget, so a burst of issued reads can't land us above it.
    oc::vector<uint32> promotions;
    for (uint32 texIdx = 0; texIdx < (uint32)m_states.size(); ++texIdx)
    {
        const StreamState& state = m_states[texIdx];
        if (state.numMips != 0 && !state.opInFlight && !state.failed && state.targetTop < state.residentTop)
            promotions.push_back(texIdx);
    }
    oc::sort(promotions.begin(), promotions.end(), [&](uint32 a, uint32 b)
    {
        if (m_states[a].desiredTop != m_states[b].desiredTop)
            return m_states[a].desiredTop < m_states[b].desiredTop;
        return m_states[a].bytesFromMip[m_states[a].targetTop] < m_states[b].bytesFromMip[m_states[b].targetTop];
    });
    for (const uint32 texIdx : promotions)
    {
        StreamState& state = m_states[texIdx];
        const uint64 growBytes = state.bytesFromMip[state.targetTop] - state.bytesFromMip[state.residentTop];
        if (m_committedBytes + growBytes > availBytes)
            continue;
        // With GPU copies, read only the mips the live image lacks; the rest is copied at apply time.
        if (tryIssue(texIdx, state, m_gpuMipCopies ? state.residentTop : state.numMips))
            m_committedBytes += growBytes;
    }

    if (issuedAny)
        m_requestCv.notify_one();
}

void TextureStreamer::update()
{
    // 1. Apply finished disk reads: create + upload the replacement images, swap them into the live
    //    textures, queue their descriptor rewrites, park the old images for deferred destruction.
    {
        oc::deque<StreamCompletion> completions;
        {
            std::scoped_lock lock(m_completionMutex);
            completions.swap(m_completions);
        }
        for (StreamCompletion& completion : completions)
            applyCompletion(oc::move(completion));
    }

    // 2. Fold this frame's want accumulators into the hysteresis-filtered desired mips: promotions apply
    //    immediately (a texture filling the screen should sharpen now), demotions only after the want has
    //    been consistently lower, and textures no rendered instance touched decay to their tail. Also
    //    refresh the committed-bytes ledger (file-byte estimate of what is / is about to be resident:
    //    in-flight promotions count at their target so a burst of issues can't overshoot the budget).
    uint64 desiredBytes = 0;
    uint64 committedBytes = 0;
    for (StreamState& state : m_states)
    {
        if (state.numMips == 0)
            continue;
        const uint8 want = state.wantTopThisFrame;
        state.wantTopThisFrame = 255;
        if (want != 255)
        {
            state.lastSeenFrame = m_frameCounter;
            if (want < state.desiredTop)
            {
                state.desiredTop = want;
                state.demoteCounter = 0;
            }
            else if (want > state.desiredTop)
            {
                if (++state.demoteCounter >= (uint16)oc::max(1, m_demoteHysteresisFrames))
                {
                    state.desiredTop = want;
                    state.demoteCounter = 0;
                }
            }
            else
                state.demoteCounter = 0;
        }
        else if (m_frameCounter - state.lastSeenFrame > (uint32)m_decayFrames)
        {
            state.desiredTop = state.tailTop;
            state.demoteCounter = 0;
        }
        desiredBytes += state.bytesFromMip[state.desiredTop];
        committedBytes += state.bytesFromMip[state.opInFlight ? oc::min(state.opTargetTop, state.residentTop) : state.residentTop];
    }
    m_desiredBytes = desiredBytes;
    m_committedBytes = committedBytes;

    // 3. Solve residency targets under the budget (all sizes are file-byte estimates; VMA alignment
    //    overhead makes the true footprint a hair larger). Every texture is owed its tail; the remaining
    //    headroom is granted one mip level at a time to the largest desire deficit (cheapest grant first
    //    on ties, then lowest index — a deterministic order, so targets stay stable while desires do).
    {
        const uint64 budgetBytes = (uint64)m_budgetMB * 1024ull * 1024ull;
        const uint64 availBytes = budgetBytes > m_pinnedBytes ? budgetBytes - m_pinnedBytes : 0;

        struct Grant { uint8 deficit; uint32 cost; uint32 texIdx; };
        auto lowerPriority = [](const Grant& a, const Grant& b)
        {
            if (a.deficit != b.deficit) return a.deficit < b.deficit;
            if (a.cost != b.cost) return a.cost > b.cost;
            return a.texIdx > b.texIdx;
        };
        oc::priority_queue<Grant, oc::vector<Grant>, decltype(lowerPriority)> grants(lowerPriority);

        uint64 usedBytes = 0;
        for (uint32 texIdx = 0; texIdx < (uint32)m_states.size(); ++texIdx)
        {
            StreamState& state = m_states[texIdx];
            if (state.numMips == 0)
                continue;
            state.targetTop = state.tailTop;
            usedBytes += state.tailBytes;
            if (state.desiredTop < state.tailTop)
                grants.push(Grant{ (uint8)(state.tailTop - state.desiredTop), state.meta.mips[state.tailTop - 1].byteSize, texIdx });
        }
        while (!grants.empty() && usedBytes < availBytes)
        {
            const Grant grant = grants.top();
            grants.pop();
            StreamState& state = m_states[grant.texIdx];
            if (usedBytes + grant.cost > availBytes)
                continue; // too big; cheaper grants may still fit
            usedBytes += grant.cost;
            state.targetTop--;
            if (state.targetTop > state.desiredTop)
                grants.push(Grant{ (uint8)(state.targetTop - state.desiredTop), state.meta.mips[state.targetTop - 1].byteSize, grant.texIdx });
        }

        // Retention phase: after every need is met, keep already-resident mips that still fit instead of
        // evicting them (a camera spin must not re-stream everything behind you). Most recently seen
        // textures retain first, so under real budget pressure eviction is oldest-first.
        oc::vector<uint32> retainOrder;
        for (uint32 texIdx = 0; texIdx < (uint32)m_states.size(); ++texIdx)
        {
            const StreamState& state = m_states[texIdx];
            if (state.numMips != 0 && state.residentTop < state.targetTop)
                retainOrder.push_back(texIdx);
        }
        oc::sort(retainOrder.begin(), retainOrder.end(), [&](uint32 a, uint32 b)
        {
            if (m_states[a].lastSeenFrame != m_states[b].lastSeenFrame)
                return m_states[a].lastSeenFrame > m_states[b].lastSeenFrame;
            return a < b;
        });
        for (const uint32 texIdx : retainOrder)
        {
            StreamState& state = m_states[texIdx];
            while (state.targetTop > state.residentTop)
            {
                const uint32 cost = state.meta.mips[state.targetTop - 1].byteSize;
                if (usedBytes + cost > availBytes)
                    break;
                usedBytes += cost;
                state.targetTop--;
            }
        }
    }

    // 4. Issue disk reads for textures whose target differs from what is resident.
    issueOps();

    // 5. Retire replaced images once no frame in flight can still be sampling them. A swap at streamer
    //    frame N is last sampled by frame N-1 (frame N's sets were rewritten at record time), and frame
    //    N+1's swapchain acquire fence-waits frame N-1, so destruction at N+2 is safe.
    oc::erase_if(m_retiredImages, [&](RetiredImage& retired)
    {
        if (m_frameCounter - retired.swapFrame < RendererVKLayout::NUM_FRAMES_IN_FLIGHT)
            return false;
        if (retired.view)
            Globals::device.getDevice().destroyImageView(retired.view);
        Globals::gpuAllocator.destroyImage(retired.image, retired.memory);
        return true;
    });

    m_frameCounter++;
}

void TextureStreamer::queueDescriptorWrite(uint16 texIdx)
{
    for (oc::vector<uint16>& pending : m_pendingDescriptorWrites)
    {
        if (oc::find(pending.begin(), pending.end(), texIdx) == pending.end())
            pending.push_back(texIdx);
    }
}

TextureStreamer::StreamerStats TextureStreamer::getStats() const
{
    StreamerStats stats;
    stats.budgetBytes = (uint64)m_budgetMB * 1024ull * 1024ull;
    stats.residentBytes = m_residentBytes;
    stats.pinnedBytes = m_pinnedBytes;
    stats.tailBytes = m_tailBytes;
    stats.desiredBytes = m_desiredBytes;
    stats.numStreamable = m_numStreamable;
    stats.numOpsInFlight = m_numOpsInFlight;
    return stats;
}
