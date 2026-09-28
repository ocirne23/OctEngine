module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    // Must match the three shaders' push constant blocks (the neighbour pass reads the first two words as its
    // tile counts, so it gets its own).
    struct MotionBlurPC
    {
        uint32 width;
        uint32 height;
        float  shutter;
        float  maxRadius;
        float  cameraScale;
        uint32 samples;
    };
    struct NeighborPC
    {
        uint32 tilesX;
        uint32 tilesY;
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

void MotionBlurPipeline::buildLayouts(ComputePipelineLayout& tiles, ComputePipelineLayout& neighbor, ComputePipelineLayout& gather)
{
    const vk::PushConstantRange pcRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(MotionBlurPC) };

    tiles.computeShaderDebugFilePath = "Shaders/motion_blur_tiles.cs.glsl";
    tiles.computeShaderText = FileSystem::readFileStr(tiles.computeShaderDebugFilePath);
    tiles.descriptorSetLayoutBindings = {
        binding(0, vk::DescriptorType::eUniformBuffer),
        binding(1, vk::DescriptorType::eCombinedImageSampler), // depth
        binding(2, vk::DescriptorType::eCombinedImageSampler), // motion target
        binding(3, vk::DescriptorType::eStorageImage),         // velocity
        binding(4, vk::DescriptorType::eStorageImage),         // tile max
    };
    tiles.pushConstantRanges.push_back(pcRange);

    neighbor.computeShaderDebugFilePath = "Shaders/motion_blur_neighbor.cs.glsl";
    neighbor.computeShaderText = FileSystem::readFileStr(neighbor.computeShaderDebugFilePath);
    neighbor.descriptorSetLayoutBindings = {
        binding(0, vk::DescriptorType::eCombinedImageSampler), // tile max
        binding(1, vk::DescriptorType::eStorageImage),         // neighbour max
    };
    neighbor.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(NeighborPC) });

    gather.computeShaderDebugFilePath = "Shaders/motion_blur_gather.cs.glsl";
    gather.computeShaderText = FileSystem::readFileStr(gather.computeShaderDebugFilePath);
    gather.descriptorSetLayoutBindings = {
        binding(0, vk::DescriptorType::eUniformBuffer),
        binding(1, vk::DescriptorType::eCombinedImageSampler), // colour
        binding(2, vk::DescriptorType::eCombinedImageSampler), // velocity
        binding(3, vk::DescriptorType::eCombinedImageSampler), // neighbour max
        binding(4, vk::DescriptorType::eStorageImage),         // out
    };
    gather.pushConstantRanges.push_back(pcRange);
}

void MotionBlurPipeline::initialize(uint32 width, uint32 height)
{
    ComputePipelineLayout tiles, neighbor, gather;
    buildLayouts(tiles, neighbor, gather);
    m_tilesPipeline.initialize(tiles);
    m_neighborPipeline.initialize(neighbor);
    m_gatherPipeline.initialize(gather);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        m_tilesSets[f].initialize(m_tilesPipeline.getDescriptorSetLayout(), "MotionBlur.tiles");
        m_neighborSets[f].initialize(m_neighborPipeline.getDescriptorSetLayout(), "MotionBlur.neighbor");
        m_gatherSets[f].initialize(m_gatherPipeline.getDescriptorSetLayout(), "MotionBlur.gather");
    }

    // Linear + clamp: the composite filters the output; the passes themselves only texelFetch.
    const vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
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
    ComputePipelineLayout tiles, neighbor, gather;
    buildLayouts(tiles, neighbor, gather);
    bool ok = m_tilesPipeline.reloadShaders(tiles);
    ok = m_neighborPipeline.reloadShaders(neighbor) && ok;
    ok = m_gatherPipeline.reloadShaders(gather) && ok;
    if (!ok)
        printf("MotionBlurPipeline: shader reload failed, keeping previous pipelines\n");
}

void MotionBlurPipeline::createImage(Image& image, uint32 width, uint32 height, vk::Format format, const char* name)
{
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, image.image, image.memory, name);
    const vk::ImageViewCreateInfo viewInfo{
        .image = image.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
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
    destroyImage(m_tileMax);
    destroyImage(m_neighborMax);
    destroyImage(m_out);
}

void MotionBlurPipeline::recreateImages(uint32 width, uint32 height)
{
    destroyImages();
    m_width = width;
    m_height = height;
    m_tilesX = (width + RendererVKLayout::MOTION_BLUR_TILE - 1) / RendererVKLayout::MOTION_BLUR_TILE;
    m_tilesY = (height + RendererVKLayout::MOTION_BLUR_TILE - 1) / RendererVKLayout::MOTION_BLUR_TILE;
    createImage(m_velocity, width, height, vk::Format::eR16G16B16A16Sfloat, "MotionBlur.velocity");
    createImage(m_tileMax, m_tilesX, m_tilesY, vk::Format::eR16G16Sfloat, "MotionBlur.tileMax");
    createImage(m_neighborMax, m_tilesX, m_tilesY, vk::Format::eR16G16Sfloat, "MotionBlur.neighborMax");
    createImage(m_out, width, height, vk::Format::eR16G16B16A16Sfloat, "MotionBlur.out");

    // GENERAL for life (the composite's descriptor is written with it before the first record).
    oc::array<vk::ImageMemoryBarrier2, 4> bars;
    const oc::array<vk::Image, 4> images{ m_velocity.image, m_tileMax.image, m_neighborMax.image, m_out.image };
    for (size_t i = 0; i < images.size(); ++i)
        bars[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = images[i],
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "MotionBlur.init");
    vk::CommandBuffer cmd = init.begin(true);
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void MotionBlurPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const auto uboInfo = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };
    // The blur may reach at most one tile, so the 3x3 neighbourhood holds every velocity that can reach a pixel.
    const MotionBlurPC pc{
        .width = m_width, .height = m_height, .shutter = params.shutter,
        .maxRadius = oc::min(params.maxRadius, (float)RendererVKLayout::MOTION_BLUR_TILE),
        .cameraScale = params.cameraScale, .samples = oc::max(params.samples, 1u),
    };

    // Last frame's reads of these images (the gather's, the composite's) -> this frame's writes.
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader, {},
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite);

    { // 1. velocity + tile max
        vk::DescriptorSet vkSet = m_tilesSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 5> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, params.sceneDepthView, SCENE_DEPTH_SAMPLED_LAYOUT) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, params.motionView, vk::ImageLayout::eShaderReadOnlyOptimal) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storage(m_velocity.view) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storage(m_tileMax.view) } },
        };
        const vk::PipelineLayout layout = m_tilesPipeline.getPipelineLayout();
        commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_tilesPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(m_tilesX, m_tilesY, 1); // one workgroup per tile
    }
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead);

    { // 2. neighbour max
        vk::DescriptorSet vkSet = m_neighborSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 2> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, m_tileMax.view, vk::ImageLayout::eGeneral) } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storage(m_neighborMax.view) } },
        };
        const vk::PipelineLayout layout = m_neighborPipeline.getPipelineLayout();
        commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_neighborPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
        const NeighborPC npc{ .tilesX = m_tilesX, .tilesY = m_tilesY };
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(npc), &npc);
        cmd.dispatch((m_tilesX + 7) / 8, (m_tilesY + 7) / 8, 1);
    }
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead);

    { // 3. gather
        vk::DescriptorSet vkSet = m_gatherSets[frameIdx].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 5> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, params.colorView, params.colorLayout) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, m_velocity.view, vk::ImageLayout::eGeneral) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampled(m_sampler, m_neighborMax.view, vk::ImageLayout::eGeneral) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { storage(m_out.view) } },
        };
        const vk::PipelineLayout layout = m_gatherPipeline.getPipelineLayout();
        commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_gatherPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
    }
    // The blurred colour -> the composite's fragment read.
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
}
