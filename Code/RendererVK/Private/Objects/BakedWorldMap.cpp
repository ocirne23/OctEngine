module RendererVK;

import Core;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    // Every stage that may sample one of these maps (ocean shore: VS/FS; fog terrain: CS + VS/FS via the
    // ocean fallback). Using the union for both keeps the barriers simple; extra stages are harmless.
    constexpr vk::PipelineStageFlags2 SAMPLE_STAGES = vk::PipelineStageFlagBits2::eVertexShader
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

void BakedWorldMap::initialize(uint32 resolution, uint32 numLayers, uint32 channels, const char* debugName)
{
    assert(channels == 1 || channels == 2 || channels == 4); // no 3: RGB32F isn't a sampled-image format on most GPUs
    m_resolution = resolution;
    m_numLayers = numLayers;
    m_channels = channels;
    const vk::Format format = channels == 4 ? vk::Format::eR32G32B32A32Sfloat
        : channels == 2 ? vk::Format::eR32G32Sfloat : vk::Format::eR32Sfloat;
    vk::Device vkDevice = Globals::device.getDevice();
    const vk::ImageSubresourceRange allLayers{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, numLayers };

    for (uint32 i = 0; i < 2; ++i)
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
    }
    m_sampler.initialize(vk::SamplerAddressMode::eClampToEdge);

    // One host-visible staging buffer per frame slot: upload() writes the slot whose fence beginFrame
    // just waited, recordUpload() copies it in that frame's primary CB.
    const vk::DeviceSize bytes = (vk::DeviceSize)numLayers * resolution * resolution * channels * sizeof(float);
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        (void)m_staging[i].initialize(bytes, vk::BufferUsageFlagBits2::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible, false, debugName, BufferHostAccess::eSequentialWrite);
        m_stagingMapped[i] = m_staging[i].mapMemory();
    }

    // One-time UNDEFINED -> SHADER_READ_ONLY: the images are statically bound from the first frame but
    // never sampled until the first upload sets the consumers' UBO 1/size flags.
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer cmd = init.begin(true);
    oc::array<vk::ImageMemoryBarrier2, 2> bars;
    for (uint32 i = 0; i < 2; ++i)
        bars[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = SAMPLE_STAGES,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = m_image[i],
            .subresourceRange = allLayers,
        };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
    init.end();
    init.submitGraphics();
    (void)Globals::device.getGraphicsQueue().waitIdle();
}

void BakedWorldMap::upload(oc::span<const float> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam, uint32 frameIdx)
{
    assert(texels.size() == (size_t)m_numLayers * m_resolution * m_resolution * m_channels);
    memcpy(m_stagingMapped[frameIdx].data(), texels.data(), texels.size_bytes());
    m_staging[frameIdx].flushMappedMemory(texels.size_bytes());
    m_uploadSlot = (int)frameIdx;
    m_pendingCenter = centerXZ;
    m_pendingSizes = worldSizes;
    m_pendingUserParam = userParam;
}

void BakedWorldMap::recordUpload(CommandBuffer& commandBuffer)
{
    if (m_uploadSlot < 0)
        return;
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const vk::Image dst = m_image[m_active ^ 1u];
    const vk::ImageSubresourceRange allLayers{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, m_numLayers };

    // The inactive image was still sampled by older submissions the last time it was active — the
    // discard transition must execution-depend on those reads (WRITE_AFTER_READ).
    vk::ImageMemoryBarrier2 toDst{
        .srcStageMask = SAMPLE_STAGES,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined, // full replace: previous contents don't matter
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = dst,
        .subresourceRange = allLayers,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toDst });

    const vk::BufferImageCopy2 region{
        .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, m_numLayers },
        .imageExtent = { m_resolution, m_resolution, 1 },
    };
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
    m_flipPending = true; // activates at the next updateUBO, together with the new center/sizes
}

void BakedWorldMap::flipIfPending()
{
    if (!m_flipPending)
        return;
    m_active ^= 1u;
    ++m_generation;
    m_center = m_pendingCenter;
    m_worldSizes = m_pendingSizes;
    m_userParam = m_pendingUserParam;
    m_flipPending = false;
}
