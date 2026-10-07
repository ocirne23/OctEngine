module RendererVK;

import Core;
import Core.glm;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import Settings;

namespace
{
    constexpr vk::Format RAIN_FORMAT = vk::Format::eR32Uint; // the packed texel, rain_occlusion.cs.glsl
}

void RainOcclusionPipeline::registerPushFields()
{
    m_block.lockable(m_push.layerBlock, Globals::settings.particles.rainOcclusionFoliageBlock);
}

void RainOcclusionPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/rain_occlusion.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.pushDeclaration = m_block.declaration();
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = m_block.size() });
}

RainOcclusionPipeline::~RainOcclusionPipeline()
{
    destroyImages();
    if (m_sampler)
        Globals::device.getDevice().destroySampler(m_sampler);
}

void RainOcclusionPipeline::destroyImages()
{
    if (m_view)
        Globals::device.getDevice().destroyImageView(m_view);
    Globals::gpuAllocator.destroyImage(m_image, m_memory);
    m_view = nullptr;
    m_image = nullptr;
    m_memory = nullptr;
}

// The image, cleared to open sky (every bit set: solid and foliage depth 1, pass 15/15) and left in GENERAL - the
// particle sim's descriptor names it before the first trace.
void RainOcclusionPipeline::createImages(uint32 resolution)
{
    vk::Device vkDevice = Globals::device.getDevice();
    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = RAIN_FORMAT,
        .extent = { resolution, resolution, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, m_image, m_memory, "RainOcclusionMap");
    const vk::ImageViewCreateInfo viewInfo{
        .image = m_image,
        .viewType = vk::ImageViewType::e2D,
        .format = RAIN_FORMAT,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
    };
    auto viewResult = vkDevice.createImageView(viewInfo);
    assert(viewResult.result == vk::Result::eSuccess);
    m_view = viewResult.value;
    Globals::device.setDebugName(m_view, "RainOcclusionMap");

    const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    vk::ClearColorValue sky;
    for (uint32& c : sky.uint32)
        c = 0xFFFFFFFFu;
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "RainOcclusion.init");
    vk::CommandBuffer cmd = init.begin(true);
    const vk::ImageMemoryBarrier2 toGeneral{
        .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = m_image,
        .subresourceRange = range,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toGeneral });
    cmd.clearColorImage(m_image, vk::ImageLayout::eGeneral, &sky, 1, &range);
    const vk::MemoryBarrier2 toSim{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toSim });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void RainOcclusionPipeline::initialize()
{
    // Nearest, clamped: the sim bounds-checks its uv itself (outside = open sky), so no border colour is read.
    const vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = vk::False,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .unnormalizedCoordinates = vk::False,
    };
    auto samplerResult = Globals::device.getDevice().createSampler(samplerInfo);
    assert(samplerResult.result == vk::Result::eSuccess);
    m_sampler = samplerResult.value;
    Globals::device.setDebugName(m_sampler, "RainOcclusion");
    createImages(1);
}

void RainOcclusionPipeline::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    destroyImages();
    if (!active)
    {
        createImages(1);
        return;
    }
    if (!m_pipelineBuilt)
    {
        ComputePipelineLayout layout;
        buildLayout(layout);
        m_pipeline.initialize(layout);
        for (DescriptorSet& set : m_sets)
            set.initialize(m_pipeline.getDescriptorSetLayout(), "RainOcclusion");
        m_pipelineBuilt = true;
    }
    createImages(RendererVKLayout::RAIN_OCCLUSION_RESOLUTION);
    // The storage image changes only here; the TLAS (its handle can change) is written per record.
    const vk::DescriptorImageInfo imageInfo{ .imageView = m_view, .imageLayout = vk::ImageLayout::eGeneral };
    for (DescriptorSet& set : m_sets)
    {
        const vk::WriteDescriptorSet write{ .dstSet = set.getDescriptorSet(), .dstBinding = 1, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &imageInfo };
        Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
    }
}

void RainOcclusionPipeline::reloadShaders()
{
    if (!m_pipelineBuilt)
        return; // built from the current file at the first activation
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("RainOcclusionPipeline: shader reload failed, keeping previous pipeline\n");
}

void RainOcclusionPipeline::record(vk::CommandBuffer cmd, uint32 frameIdx, vk::AccelerationStructureKHR tlas, const glm::mat4& viewProj)
{
    assert(m_active && "RainOcclusionPipeline::record while inactive");
    // The whole image is rewritten: its old contents are discarded. The source scope (every earlier compute read on the
    // queue) covers the previous frame's particle sim, which is what lets ONE image serve both frame slots.
    const vk::ImageMemoryBarrier2 toWrite{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = m_image,
        .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toWrite });

    const vk::DescriptorSet set = m_sets[frameIdx].getDescriptorSet();
    const vk::WriteDescriptorSetAccelerationStructureKHR asInfo{ .accelerationStructureCount = 1, .pAccelerationStructures = &tlas };
    const vk::WriteDescriptorSet asWrite{ .pNext = &asInfo, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR };
    Globals::device.getDevice().updateDescriptorSets(1, &asWrite, 0, nullptr);

    const uint32 res = RendererVKLayout::RAIN_OCCLUSION_RESOLUTION;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    PushData pc(m_block);
    pc.set(m_push.invViewProj, glm::inverse(viewProj));
    pc.set(m_push.resolution, res);
    cmd.pushConstants(m_pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, pc.size(), pc.data());
    cmd.dispatch((res + 7) / 8, (res + 7) / 8, 1);

    const vk::MemoryBarrier2 toSim{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toSim });
}
