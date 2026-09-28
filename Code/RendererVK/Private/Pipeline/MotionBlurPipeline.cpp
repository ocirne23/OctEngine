module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    constexpr vk::Format MOTION_BLUR_FORMAT = vk::Format::eR16G16Sfloat;

    // Must match the shaders' push constant blocks.
    struct TilesPC
    {
        uint32 width;
        uint32 height;
        float  shutter;
        float  maxRadius;
        float  cameraScale;
    };
    struct NeighborPC
    {
        uint32 tilesX;
        uint32 tilesY;
        uint32 subTilesX;
        uint32 subTilesY;
    };

    auto sampled(vk::Sampler s, vk::ImageView v, vk::ImageLayout layout) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = layout }; }
    auto storage(vk::ImageView v) { return vk::DescriptorImageInfo{ .imageView = v, .imageLayout = vk::ImageLayout::eGeneral }; }

    vk::DescriptorSetLayoutBinding binding(uint32 idx, vk::DescriptorType type)
    {
        return vk::DescriptorSetLayoutBinding{ .binding = idx, .descriptorType = type, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute };
    }

    void computeBarrier(vk::CommandBuffer cmd, vk::PipelineStageFlags2 srcStages, vk::AccessFlags2 srcAccess, vk::PipelineStageFlags2 dstStages, vk::AccessFlags2 dstAccess)
    {
        const vk::MemoryBarrier2 barrier{ .srcStageMask = srcStages, .srcAccessMask = srcAccess, .dstStageMask = dstStages, .dstAccessMask = dstAccess };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    }
}

MotionBlurPipeline::~MotionBlurPipeline()
{
    destroyImages();
    if (m_sampler)
        Globals::device.getDevice().destroySampler(m_sampler);
}

void MotionBlurPipeline::buildLayouts(ComputePipelineLayout& tiles, ComputePipelineLayout& neighbor)
{
    tiles.computeShaderDebugFilePath = "Shaders/motion_blur_tiles.cs.glsl";
    tiles.computeShaderText = FileSystem::readFileStr(tiles.computeShaderDebugFilePath);
    tiles.descriptorSetLayoutBindings = {
        binding(0, vk::DescriptorType::eUniformBuffer),
        binding(1, vk::DescriptorType::eCombinedImageSampler), // depth
        binding(2, vk::DescriptorType::eCombinedImageSampler), // motion target
        binding(3, vk::DescriptorType::eStorageImage),         // velocity
        binding(4, vk::DescriptorType::eStorageImage),         // sub-tiles
    };
    tiles.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(TilesPC) });

    neighbor.computeShaderDebugFilePath = "Shaders/motion_blur_neighbor.cs.glsl";
    neighbor.computeShaderText = FileSystem::readFileStr(neighbor.computeShaderDebugFilePath);
    neighbor.descriptorSetLayoutBindings = {
        binding(0, vk::DescriptorType::eCombinedImageSampler), // sub-tiles
        binding(1, vk::DescriptorType::eStorageImage),         // neighbour max
    };
    neighbor.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(NeighborPC) });
}

void MotionBlurPipeline::initialize(uint32 width, uint32 height)
{
    ComputePipelineLayout tiles, neighbor;
    buildLayouts(tiles, neighbor);
    m_tilesPipeline.initialize(tiles);
    m_neighborPipeline.initialize(neighbor);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        m_tilesSets[f].initialize(m_tilesPipeline.getDescriptorSetLayout(), "MotionBlur.tiles");
        m_neighborSets[f].initialize(m_neighborPipeline.getDescriptorSetLayout(), "MotionBlur.neighbor");
    }

    // Every read is a texelFetch; nearest + clamp.
    const vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    };
    auto samplerResult = Globals::device.getDevice().createSampler(samplerInfo);
    assert(samplerResult.result == vk::Result::eSuccess);
    m_sampler = samplerResult.value;
    Globals::device.setDebugName(m_sampler, "MotionBlur");

    recreateImages(width, height);
}

void MotionBlurPipeline::reloadShaders()
{
    ComputePipelineLayout tiles, neighbor;
    buildLayouts(tiles, neighbor);
    bool ok = m_tilesPipeline.reloadShaders(tiles);
    ok = m_neighborPipeline.reloadShaders(neighbor) && ok;
    if (!ok)
        printf("MotionBlurPipeline: shader reload failed, keeping previous pipelines\n");
}

void MotionBlurPipeline::createImage(Image& image, uint32 width, uint32 height, const char* name)
{
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = MOTION_BLUR_FORMAT,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        // TRANSFER_DST: the one-time clear (recreateImages).
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, image.image, image.memory, name);
    const vk::ImageViewCreateInfo viewInfo{
        .image = image.image,
        .viewType = vk::ImageViewType::e2D,
        .format = MOTION_BLUR_FORMAT,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
    };
    auto viewResult = Globals::device.getDevice().createImageView(viewInfo);
    assert(viewResult.result == vk::Result::eSuccess);
    image.view = viewResult.value;
    Globals::device.setDebugName(image.view, name);
}

void MotionBlurPipeline::destroyImage(Image& image)
{
    if (image.view)
        Globals::device.getDevice().destroyImageView(image.view);
    Globals::gpuAllocator.destroyImage(image.image, image.memory);
    image = Image{};
}

void MotionBlurPipeline::destroyImages()
{
    destroyImage(m_velocity);
    destroyImage(m_subTiles);
    destroyImage(m_neighborMax);
}

void MotionBlurPipeline::recreateImages(uint32 width, uint32 height)
{
    destroyImages();
    m_width = width;
    m_height = height;
    m_subTilesX = (width + RendererVKLayout::MOTION_BLUR_SUBTILE - 1) / RendererVKLayout::MOTION_BLUR_SUBTILE;
    m_subTilesY = (height + RendererVKLayout::MOTION_BLUR_SUBTILE - 1) / RendererVKLayout::MOTION_BLUR_SUBTILE;
    m_tilesX = (width + RendererVKLayout::MOTION_BLUR_TILE - 1) / RendererVKLayout::MOTION_BLUR_TILE;
    m_tilesY = (height + RendererVKLayout::MOTION_BLUR_TILE - 1) / RendererVKLayout::MOTION_BLUR_TILE;
    createImage(m_velocity, width, height, "MotionBlur.velocity");
    createImage(m_subTiles, m_subTilesX, m_subTilesY, "MotionBlur.subTiles");
    createImage(m_neighborMax, m_tilesX, m_tilesY, "MotionBlur.neighborMax");

    // GENERAL for life, cleared to 0 (no motion): the composite binds them before the first blur runs.
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "MotionBlur.init");
    vk::CommandBuffer cmd = init.begin(true);
    const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    const oc::array<vk::Image, 3> images{ m_velocity.image, m_subTiles.image, m_neighborMax.image };
    oc::array<vk::ImageMemoryBarrier2, 3> bars;
    for (size_t i = 0; i < images.size(); ++i)
        bars[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = images[i],
            .subresourceRange = range,
        };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
    const vk::ClearColorValue zero{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } };
    for (vk::Image image : images)
        cmd.clearColorImage(image, vk::ImageLayout::eGeneral, &zero, 1, &range);
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite);
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void MotionBlurPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();

    if (params.velocityPass) // TAA is off: the velocity + sub-tiles TAA would have written
    {
        // Last frame's reads of the images (the neighbour pass, the composite) -> these writes.
        computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader, {},
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite);
        vk::DescriptorSet vkSet = m_tilesSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 5> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, params.sceneDepthView, SCENE_DEPTH_SAMPLED_LAYOUT) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, params.motionView, vk::ImageLayout::eShaderReadOnlyOptimal) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storage(m_velocity.view) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storage(m_subTiles.view) } },
        };
        const vk::PipelineLayout layout = m_tilesPipeline.getPipelineLayout();
        commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_tilesPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
        const TilesPC pc{ .width = m_width, .height = m_height, .shutter = params.shutter,
            .maxRadius = clampMaxRadius(params.maxRadius), .cameraScale = params.cameraScale };
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(m_subTilesX, m_subTilesY, 1); // one workgroup per sub-tile
    }
    // The velocity + sub-tiles (TAA's or the pass above) -> the neighbour pass. (TAA's own closing barrier covers
    // this too; kept for the TAA-off path.)
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite);

    {
        vk::DescriptorSet vkSet = m_neighborSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 2> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, m_subTiles.view, vk::ImageLayout::eGeneral) } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storage(m_neighborMax.view) } },
        };
        const vk::PipelineLayout layout = m_neighborPipeline.getPipelineLayout();
        commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_neighborPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
        const NeighborPC npc{ .tilesX = m_tilesX, .tilesY = m_tilesY, .subTilesX = m_subTilesX, .subTilesY = m_subTilesY };
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(npc), &npc);
        cmd.dispatch((m_tilesX + 7) / 8, (m_tilesY + 7) / 8, 1);
    }
    // The neighbour max + the velocity -> the composite's gather (fragment).
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
}
