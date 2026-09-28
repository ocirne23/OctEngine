module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    constexpr vk::Format DLSS_MVEC_FORMAT = vk::Format::eR16G16Sfloat;
    constexpr vk::ImageUsageFlags DLSS_MVEC_USAGE = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    // Not R8_UNORM: SL's Vulkan format table (sl.chi Vulkan::getFormat) has no entry for it ("Cannot have undefined
    // format"). R16_SFLOAT is in it.
    constexpr vk::Format DLSS_BIAS_FORMAT = vk::Format::eR16Sfloat;
    // TaaPipeline's resolved format: the copy needs the same one.
    constexpr vk::Format DLSS_OUTPUT_FORMAT = TaaPipeline::RESOLVED_FORMAT;
    constexpr vk::ImageUsageFlags DLSS_OUTPUT_USAGE = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;

    // Must match dlss_mvec.cs.glsl's push constant block.
    struct MvecPC
    {
        glm::ivec2 base;
        glm::ivec2 origin;
        glm::ivec2 size;
        float oceanBias;
        uint32 mbEnabled;
        float mbShutter;
        float mbMaxRadius;
        float mbCameraScale;
    };

    vk::DescriptorSetLayoutBinding binding(uint32 idx, vk::DescriptorType type)
    {
        return vk::DescriptorSetLayoutBinding{ .binding = idx, .descriptorType = type, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute };
    }
}

DlssPipeline::~DlssPipeline()
{
    destroyImage(m_motion);
    destroyImage(m_bias);
    destroyImage(m_output);
    if (m_sampler)
        Globals::device.getDevice().destroySampler(m_sampler);
}

void DlssPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/dlss_mvec.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.descriptorSetLayoutBindings = {
        binding(0, vk::DescriptorType::eUniformBuffer),
        binding(1, vk::DescriptorType::eCombinedImageSampler), // depth
        binding(2, vk::DescriptorType::eCombinedImageSampler), // motion target
        binding(3, vk::DescriptorType::eStorageImage),         // motion vectors
        binding(4, vk::DescriptorType::eCombinedImageSampler), // scene colour (the ocean flag)
        binding(5, vk::DescriptorType::eStorageImage),         // bias mask
        binding(6, vk::DescriptorType::eStorageImage),         // motion blur velocity
        binding(7, vk::DescriptorType::eStorageImage),         // motion blur sub-tiles
    };
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(MvecPC) });
}

void DlssPipeline::initialize(uint32 renderWidth, uint32 renderHeight, uint32 outputWidth, uint32 outputHeight)
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    m_pipeline.initialize(layout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
        m_sets[f].initialize(m_pipeline.getDescriptorSetLayout(), "Dlss.mvec");

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
    Globals::device.setDebugName(m_sampler, "Dlss");

    recreateImages(renderWidth, renderHeight);
    recreateOutputImage(outputWidth, outputHeight);
}

void DlssPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("DlssPipeline: shader reload failed, keeping previous pipeline\n");
}

void DlssPipeline::destroyImage(Image& image)
{
    if (image.view)
        Globals::device.getDevice().destroyImageView(image.view);
    Globals::gpuAllocator.destroyImage(image.image, image.memory);
    image = Image{};
}

void DlssPipeline::createImage(Image& image, uint32 width, uint32 height, vk::Format format, vk::ImageUsageFlags usage, const char* name)
{
    destroyImage(image);
    image.width = width;
    image.height = height;
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
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

    // GENERAL for life (the storage write, DLSS's read, the copy all use it).
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "Dlss.init");
    vk::CommandBuffer cmd = init.begin(true);
    const vk::ImageMemoryBarrier2 bar{
        .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image.image,
        .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void DlssPipeline::recreateImages(uint32 width, uint32 height)
{
    createImage(m_motion, width, height, DLSS_MVEC_FORMAT, DLSS_MVEC_USAGE, "Dlss.mvec");
    createImage(m_bias, width, height, DLSS_BIAS_FORMAT, DLSS_MVEC_USAGE, "Dlss.bias");
}

void DlssPipeline::recreateOutputImage(uint32 width, uint32 height)
{
    createImage(m_output, width, height, DLSS_OUTPUT_FORMAT, DLSS_OUTPUT_USAGE, "Dlss.output");
}

void DlssPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();

    // Last frame's reads -> this write: the upscale (compute), and the motion blur images' neighbour pass
    // (compute) and composite gather (fragment).
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &before });

    vk::DescriptorSet vkSet = m_sets[frameIdx].getDescriptorSet();
    oc::array<DescriptorSetUpdateInfo, 8> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { vk::DescriptorImageInfo{ .sampler = m_sampler, .imageView = params.sceneDepthView, .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { vk::DescriptorImageInfo{ .sampler = m_sampler, .imageView = params.motionView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { vk::DescriptorImageInfo{ .imageView = m_motion.view, .imageLayout = vk::ImageLayout::eGeneral } } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { vk::DescriptorImageInfo{ .sampler = m_sampler, .imageView = params.sceneColorView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { vk::DescriptorImageInfo{ .imageView = m_bias.view, .imageLayout = vk::ImageLayout::eGeneral } } },
        DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageImage, .imageInfos = { vk::DescriptorImageInfo{ .imageView = params.mbVelocityView, .imageLayout = vk::ImageLayout::eGeneral } } },
        DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eStorageImage, .imageInfos = { vk::DescriptorImageInfo{ .imageView = params.mbSubTileView, .imageLayout = vk::ImageLayout::eGeneral } } },
    };
    const vk::PipelineLayout layout = m_pipeline.getPipelineLayout();
    commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
    // The motion blur's sub-tile grid starts at pixel 0: with it, the dispatch covers the whole target (as TAA's).
    const glm::ivec2 base = params.mbEnabled ? glm::ivec2(0) : params.renderOrigin;
    const glm::ivec2 extent = params.mbEnabled ? glm::ivec2((int)m_motion.width, (int)m_motion.height) : params.renderSize;
    const MvecPC pc{ .base = base, .origin = params.renderOrigin, .size = params.renderSize, .oceanBias = params.oceanBias,
        .mbEnabled = params.mbEnabled ? 1u : 0u, .mbShutter = params.mbShutter, .mbMaxRadius = params.mbMaxRadius, .mbCameraScale = params.mbCameraScale };
    cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
    constexpr uint32 group = RendererVKLayout::MOTION_BLUR_SUBTILE; // the shader's workgroup
    cmd.dispatch(((uint32)extent.x + group - 1) / group, ((uint32)extent.y + group - 1) / group, 1);

    // The motion vectors -> the upscale's compute read.
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &after });
}

Streamline::Image DlssPipeline::getMotionImage() const
{
    return Streamline::Image{ .image = m_motion.image, .view = m_motion.view, .layout = vk::ImageLayout::eGeneral, .format = DLSS_MVEC_FORMAT,
        .size = glm::uvec2(m_motion.width, m_motion.height), .usage = DLSS_MVEC_USAGE };
}

Streamline::Image DlssPipeline::getBiasImage() const
{
    return Streamline::Image{ .image = m_bias.image, .view = m_bias.view, .layout = vk::ImageLayout::eGeneral, .format = DLSS_BIAS_FORMAT,
        .size = glm::uvec2(m_bias.width, m_bias.height), .usage = DLSS_MVEC_USAGE };
}

Streamline::Image DlssPipeline::getOutputImage() const
{
    return Streamline::Image{ .image = m_output.image, .view = m_output.view, .layout = vk::ImageLayout::eGeneral, .format = DLSS_OUTPUT_FORMAT,
        .size = glm::uvec2(m_output.width, m_output.height), .usage = DLSS_OUTPUT_USAGE };
}

void DlssPipeline::beginOutputWrite(vk::CommandBuffer cmd) const
{
    const vk::MemoryBarrier2 bar{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
}

void DlssPipeline::recordOutputCopy(vk::CommandBuffer cmd, vk::Image dst, glm::ivec2 dstOffset, glm::uvec2 size) const
{
    // DLSS's compute write -> the copy read.
    const vk::MemoryBarrier2 bar{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
    const vk::ImageCopy region{
        .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .dstOffset = { dstOffset.x, dstOffset.y, 0 },
        .extent = { size.x, size.y, 1 },
    };
    cmd.copyImage(m_output.image, vk::ImageLayout::eGeneral, dst, vk::ImageLayout::eGeneral, 1, &region);
}
