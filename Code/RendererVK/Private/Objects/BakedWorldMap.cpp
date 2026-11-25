module RendererVK;

import Core;
import :Device;
import :Allocator;
import :CommandBuffer;
import :StagingManager;

namespace
{
    // Every stage that may sample one of these maps: VS / FS (the ocean, the terrain, clutter, particles), CS (the fog, the
    // grass and clutter culls, the snow, the tree volume) and the TESSELLATED TERRAIN's TCS / TES (the splat's height
    // composite reads the climate: snow.inc.glsl). A one-image map's copy and its readers are in one frame, so a stage
    // missing here reads a half-copied map. Using the union for both maps keeps the barriers simple.
    constexpr vk::PipelineStageFlags2 SAMPLE_STAGES = vk::PipelineStageFlagBits2::eVertexShader
        | vk::PipelineStageFlagBits2::eTessellationControlShader | vk::PipelineStageFlagBits2::eTessellationEvaluationShader
        | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader;

    glm::ivec4 joinRect(const glm::ivec4& a, const glm::ivec4& b)
    {
        if (a.z < a.x) return b;
        if (b.z < b.x) return a;
        return glm::ivec4(oc::min(a.x, b.x), oc::min(a.y, b.y), oc::max(a.z, b.z), oc::max(a.w, b.w));
    }
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
    requestFullCopies();

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

void BakedWorldMap::stage(const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam)
{
    m_pendingCenter = centerXZ;
    m_pendingSizes = worldSizes;
    m_pendingUserParam = userParam;
    m_flipPending = true; // the copy runs before this frame's primary: the region (and a pair's image) goes live at the next UBO build
}

void BakedWorldMap::upload(oc::span<const float> texels, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam)
{
    assert(m_texelBytes == m_channels * sizeof(float) && texels.size() == (size_t)m_numLayers * m_resolution * m_resolution * m_channels);
    const StagingManager::ImageRegion region{
        .image = m_image[uploadImage()],
        .layerCount = m_numLayers,
        .extent = { m_resolution, m_resolution, 1 },
        .keepContents = false, // a full replace
        .readStages = SAMPLE_STAGES,
    };
    (void)Globals::stagingManager.uploadImageRegion(region, texels.size_bytes(),
        [&texels](oc::span<uint8> dst) { memcpy(dst.data(), texels.data(), texels.size_bytes()); });
    stage(centerXZ, worldSizes, userParam);
}

void BakedWorldMap::uploadBytesRect(const uint8* image, const glm::ivec4& rectIn, const glm::vec2& centerXZ, const glm::vec2& worldSizes, float userParam,
    const uint8* lut)
{
    assert(m_texelBytes == 1 && m_numLayers == 1);
    const int last = (int)m_resolution - 1;
    const glm::ivec4 own(oc::clamp(rectIn.x, 0, last), oc::clamp(rectIn.y, 0, last), oc::clamp(rectIn.z, 0, last), oc::clamp(rectIn.w, 0, last));
    const glm::ivec4 whole(0, 0, last, last);
    const uint32 dst = uploadImage();
    const glm::ivec4 rect = m_needsFull[dst] ? whole : joinRect(own, m_missed[dst]);
    m_needsFull[dst] = false;
    m_missed[dst] = EMPTY_RECT;
    // A PAIR: the other image misses this call's texels until its own next upload (unless it is owed a whole copy anyway).
    if (m_numImages == 2 && !m_needsFull[dst ^ 1u])
        m_missed[dst ^ 1u] = joinRect(m_missed[dst ^ 1u], own);

    // The rectangle's rows, tightly packed in the ring (the copy's bufferRowLength 0), read from the WHOLE image.
    const uint32 width = (uint32)(rect.z - rect.x + 1), height = (uint32)(rect.w - rect.y + 1);
    const StagingManager::ImageRegion region{
        .image = m_image[dst],
        .offset = vk::Offset3D{ rect.x, rect.y, 0 },
        .extent = vk::Extent3D{ width, height, 1 },
        .keepContents = rect != whole, // the rest of the image is KEPT
        .readStages = SAMPLE_STAGES,
    };
    const uint32 resolution = m_resolution;
    (void)Globals::stagingManager.uploadImageRegion(region, (vk::DeviceSize)width * height,
        [image, lut, rect, width, height, resolution](oc::span<uint8> out)
        {
            for (uint32 y = 0; y < height; ++y)
            {
                const uint8* src = image + (size_t)(rect.y + (int)y) * resolution + (size_t)rect.x;
                uint8* row = out.data() + (size_t)y * width;
                if (!lut)
                    memcpy(row, src, width);
                else
                    for (uint32 x = 0; x < width; ++x)
                        row[x] = lut[src[x]];
            }
        });
    stage(centerXZ, worldSizes, userParam);
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
