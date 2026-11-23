module RendererVK;

import Core;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    // Every stage that may sample one of these maps: VS / FS (the ocean, the terrain, clutter, particles), CS (the fog, the
    // grass and clutter culls, the snow, the tree volume) and the TESSELLATED TERRAIN's TCS / TES (the splat's height
    // composite reads the climate: snow.inc.glsl). A one-image map's copy and its readers are in one frame, so a stage
    // missing here reads a half-copied map. Using the union for both maps keeps the barriers simple.
    constexpr vk::PipelineStageFlags2 SAMPLE_STAGES = vk::PipelineStageFlagBits2::eVertexShader
        | vk::PipelineStageFlagBits2::eTessellationControlShader | vk::PipelineStageFlagBits2::eTessellationEvaluationShader
        | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader;
}

BakedWorldMap::~BakedWorldMap()
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < 2; ++i)
    {
        if (m_view[i]) vkDevice.destroyImageView(m_view[i]);
        Globals::gpuAllocator.destroyImage(m_image[i], m_memory[i]);
        m_view[i] = nullptr; m_image[i] = nullptr; m_memory[i] = nullptr;
    }
}

void BakedWorldMap::initialize(uint32 resolution, uint32 numLayers, uint32 channels, bool pair, const char* debugName)
{
    assert(channels == 1 || channels == 2 || channels == 4); // no 3: RGB32F isn't a sampled-image format on most GPUs
    m_resolution = resolution;
    m_numLayers = numLayers;
    m_channels = channels;
    m_texelBytes = channels * (uint32)sizeof(float);
    m_numImages = pair ? 2u : 1u;
    create(channels == 4 ? vk::Format::eR32G32B32A32Sfloat : channels == 2 ? vk::Format::eR32G32Sfloat : vk::Format::eR32Sfloat, debugName);
}

void BakedWorldMap::initializeBytes(uint32 resolution, uint32 numLayers, bool pair, const char* debugName)
{
    m_resolution = resolution;
    m_numLayers = numLayers;
    m_channels = 1;
    m_texelBytes = 1;
    m_numImages = pair ? 2u : 1u;
    create(vk::Format::eR8Unorm, debugName);
}

void BakedWorldMap::create(vk::Format format, const char* debugName)
{
    const uint32 resolution = m_resolution, numLayers = m_numLayers;
    vk::Device vkDevice = Globals::device.getDevice();
    const vk::ImageSubresourceRange allLayers{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, numLayers };
    m_fullCopiesLeft = m_numImages;

    for (uint32 i = 0; i < m_numImages; ++i)
    {
        vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = { resolution, resolution, 1 },
            .mipLevels = 1,
            .arrayLayers = numLayers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        (void)Globals::gpuAllocator.createImage(info, m_image[i], m_memory[i], debugName);
        vk::ImageViewCreateInfo viewInfo{
            .image = m_image[i],
            .viewType = numLayers > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = allLayers,
        };
        auto viewResult = vkDevice.createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        m_view[i] = viewResult.value;
        Globals::device.setDebugName(m_view[i], debugName);
    }
    m_sampler.initialize(vk::SamplerAddressMode::eClampToEdge);

    // One host-visible staging buffer per frame slot: upload() writes the slot whose fence beginFrame
    // just waited, recordUpload() copies it in that frame's primary CB.
    const vk::DeviceSize bytes = (vk::DeviceSize)numLayers * resolution * resolution * m_texelBytes;
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        (void)m_staging[i].initialize(bytes, vk::BufferUsageFlagBits2::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible, false, debugName, BufferHostAccess::eSequentialWrite);
        m_stagingMapped[i] = m_staging[i].mapMemory();
    }

    // One-time UNDEFINED -> SHADER_READ_ONLY: the images are statically bound from the first frame but
    // never sampled until the first upload sets the consumers' UBO 1/size flags.
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "BakedWorldMap.init");
    vk::CommandBuffer cmd = init.begin(true);
    oc::array<vk::ImageMemoryBarrier2, 2> bars;
    for (uint32 i = 0; i < m_numImages; ++i)
        bars[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = SAMPLE_STAGES,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = m_image[i],
            .subresourceRange = allLayers,
        };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = m_numImages, .pImageMemoryBarriers = bars.data() });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void BakedWorldMap::stage(uint32 frameIdx, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam)
{
    m_uploadSlot = (int)frameIdx;
    m_pendingCenter = centerXZ;
    m_pendingSizes = worldSizes;
    m_pendingUserParam = userParam;
}

void BakedWorldMap::upload(oc::span<const float> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx)
{
    assert(m_texelBytes == m_channels * sizeof(float) && texels.size() == (size_t)m_numLayers * m_resolution * m_resolution * m_channels);
    memcpy(m_stagingMapped[frameIdx].data(), texels.data(), texels.size_bytes());
    m_staging[frameIdx].flushMappedMemory(texels.size_bytes());
    m_copyRect = glm::ivec4(0, 0, -1, -1); // the whole image
    stage(frameIdx, centerXZ, worldSizes, userParam);
}

void BakedWorldMap::uploadBytes(oc::span<const uint8> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx)
{
    assert(m_texelBytes == 1 && texels.size() == (size_t)m_numLayers * m_resolution * m_resolution);
    memcpy(m_stagingMapped[frameIdx].data(), texels.data(), texels.size_bytes());
    m_staging[frameIdx].flushMappedMemory(texels.size_bytes());
    m_copyRect = glm::ivec4(0, 0, -1, -1); // the whole image
    stage(frameIdx, centerXZ, worldSizes, userParam);
}

void BakedWorldMap::uploadBytesRect(const uint8* image, const glm::ivec4& rectIn, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam,
    uint32 frameIdx, const uint8* lut)
{
    assert(m_texelBytes == 1 && m_numLayers == 1);
    const int last = (int)m_resolution - 1;
    const glm::ivec4 own(oc::clamp(rectIn.x, 0, last), oc::clamp(rectIn.y, 0, last), oc::clamp(rectIn.z, 0, last), oc::clamp(rectIn.w, 0, last));
    const auto join = [](const glm::ivec4& a, const glm::ivec4& b)
    {
        return glm::ivec4(oc::min(a.x, b.x), oc::min(a.y, b.y), oc::max(a.z, b.z), oc::max(a.w, b.w));
    };
    glm::ivec4 rect = own;
    if (m_fullCopiesLeft > 0)
    {
        --m_fullCopiesLeft;
        rect = glm::ivec4(0, 0, last, last);
    }
    else
    {
        // A PAIR: the image this lands in (the inactive one) missed the last call - join its rect.
        if (m_numImages == 2 && m_lastRect.z >= m_lastRect.x)
            rect = join(rect, m_lastRect);
        // An upload not yet copied (two calls before one present): copy both.
        if (m_uploadSlot >= 0)
            rect = m_copyRect.z < m_copyRect.x ? glm::ivec4(0, 0, last, last) : join(rect, m_copyRect);
    }
    m_lastRect = own;
    // The staging buffer has the image's own layout (the copy's bufferRowLength = the resolution): only the rect's rows
    // are written - every row of a joined rect again, from the WHOLE image.
    const size_t width = (size_t)(rect.z - rect.x + 1);
    for (int y = rect.y; y <= rect.w; ++y)
    {
        const size_t offset = (size_t)y * m_resolution + (size_t)rect.x;
        uint8* dst = m_stagingMapped[frameIdx].data() + offset;
        if (!lut)
            memcpy(dst, image + offset, width);
        else
            for (size_t x = 0; x < width; ++x)
                dst[x] = lut[image[offset + x]];
    }
    m_staging[frameIdx].flushMappedMemory(vk::WholeSize);
    m_copyRect = rect;
    stage(frameIdx, centerXZ, worldSizes, userParam);
}

void BakedWorldMap::recordUpload(CommandBuffer& commandBuffer)
{
    if (m_uploadSlot < 0)
        return;
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const vk::Image dst = m_image[m_numImages == 2 ? m_active ^ 1u : 0u];
    const vk::ImageSubresourceRange allLayers{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, m_numLayers };
    const bool partial = m_copyRect.z >= m_copyRect.x; // uploadBytesRect: the rest of the image is KEPT

    // The destination was sampled by older submissions (a pair's inactive image the last time it was active; a single
    // image by the previous frame, perhaps still running) - the transition must execution-depend on those reads
    // (WRITE_AFTER_READ). Fences order the CPU, not the GPU.
    vk::ImageMemoryBarrier2 toDst{
        .srcStageMask = SAMPLE_STAGES,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        // A full replace: the previous contents don't matter. A partial one keeps them (the image rests in SHADER_READ_ONLY).
        .oldLayout = partial ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = dst,
        .subresourceRange = allLayers,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toDst });

    vk::BufferImageCopy2 region{
        .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, m_numLayers },
        .imageExtent = { m_resolution, m_resolution, 1 },
    };
    if (partial)
    {
        region.bufferOffset = (vk::DeviceSize)m_copyRect.y * m_resolution + (vk::DeviceSize)m_copyRect.x;
        region.bufferRowLength = m_resolution;
        region.imageOffset = vk::Offset3D{ m_copyRect.x, m_copyRect.y, 0 };
        region.imageExtent = vk::Extent3D{ (uint32)(m_copyRect.z - m_copyRect.x + 1), (uint32)(m_copyRect.w - m_copyRect.y + 1), 1 };
    }
    m_copyRect = glm::ivec4(0, 0, -1, -1);
    const vk::CopyBufferToImageInfo2 copyInfo{
        .srcBuffer = m_staging[m_uploadSlot].getBuffer(),
        .dstImage = dst,
        .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
        .regionCount = 1,
        .pRegions = &region,
    };
    cmd.copyBufferToImage2(copyInfo);

    vk::ImageMemoryBarrier2 toSampled{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = SAMPLE_STAGES,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = dst,
        .subresourceRange = allLayers,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toSampled });

    m_uploadSlot = -1;
    m_flipPending = true; // the region (and a pair's image) goes live at the next UBO build
}

void BakedWorldMap::flipIfPending()
{
    if (!m_flipPending)
        return;
    if (m_numImages == 2)
    {
        m_active ^= 1u;
        ++m_generation;
    }
    m_center = m_pendingCenter;
    m_worldSizes = m_pendingSizes;
    m_userParam = m_pendingUserParam;
    m_flipPending = false;
}
