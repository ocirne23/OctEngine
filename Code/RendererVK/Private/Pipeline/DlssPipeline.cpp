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
    // TaaPipeline's resolved format: the copy needs the same one.
    constexpr vk::Format DLSS_OUTPUT_FORMAT = TaaPipeline::RESOLVED_FORMAT;
    constexpr vk::ImageUsageFlags DLSS_OUTPUT_USAGE = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;

    struct MvecPC
    {
        glm::ivec2 origin;
        glm::ivec2 size;
    };

    vk::DescriptorSetLayoutBinding binding(uint32 idx, vk::DescriptorType type)
    {
        return vk::DescriptorSetLayoutBinding{ .binding = idx, .descriptorType = type, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute };
    }
}

DlssPipeline::~DlssPipeline()
{
    destroyImage(m_motion);
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
}

void DlssPipeline::recreateOutputImage(uint32 width, uint32 height)
{
    createImage(m_output, width, height, DLSS_OUTPUT_FORMAT, DLSS_OUTPUT_USAGE, "Dlss.output");
}

void DlssPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();

    // Last frame's upscale read (compute) -> this write.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &before });

    vk::DescriptorSet vkSet = m_sets[frameIdx].getDescriptorSet();
    oc::array<DescriptorSetUpdateInfo, 4> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { vk::DescriptorImageInfo{ .sampler = m_sampler, .imageView = params.sceneDepthView, .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { vk::DescriptorImageInfo{ .sampler = m_sampler, .imageView = params.motionView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { vk::DescriptorImageInfo{ .imageView = m_motion.view, .imageLayout = vk::ImageLayout::eGeneral } } },
    };
    const vk::PipelineLayout layout = m_pipeline.getPipelineLayout();
    commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
    const MvecPC pc{ .origin = params.renderOrigin, .size = params.renderSize };
    cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
    cmd.dispatch((uint32)(params.renderSize.x + 7) / 8, (uint32)(params.renderSize.y + 7) / 8, 1);

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
