module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    constexpr vk::Format TAA_FORMAT = vk::Format::eR16G16B16A16Sfloat;

    struct TaaPC
    {
        uint32 width;
        uint32 height;
        float  feedback;
        uint32 viewIndex;
        float  oceanFeedback; // history weight cap on ocean pixels (waves animate without motion vectors)
    };

    auto imgInfoGeneral(vk::ImageView view) { return vk::DescriptorImageInfo{ .imageView = view, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledGeneral(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledRO(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal }; }
}

void TaaPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/taa.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 i = 1; i <= 4; ++i)
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = i, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 5, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 6, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute }); // normals (.a = ocean flag)
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(TaaPC) });
}

TaaPipeline::~TaaPipeline()
{
    destroyImageSet(m_resolved);
    if (m_sampler)
        Globals::device.getDevice().destroySampler(m_sampler);
}

void TaaPipeline::destroyImageSet(ImageSet& set)
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < SLOTS; ++i)
    {
        if (set.view[i])   vkDevice.destroyImageView(set.view[i]);
        Globals::gpuAllocator.destroyImage(set.image[i], set.memory[i]);
        set.view[i] = nullptr; set.image[i] = nullptr; set.memory[i] = nullptr;
        set.initialized[i] = false;
    }
}

void TaaPipeline::createImageSet(ImageSet& set)
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
    {
        const uint32 i = slot(f, e);
        vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = TAA_FORMAT,
            .extent = { m_width, m_height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        (void)Globals::gpuAllocator.createImage(info, set.image[i], set.memory[i], "Taa.resolved");

        vk::ImageViewCreateInfo viewInfo{
            .image = set.image[i],
            .viewType = vk::ImageViewType::e2D,
            .format = TAA_FORMAT,
            .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
        };
        auto viewResult = vkDevice.createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        set.view[i] = viewResult.value;
    }
}

void TaaPipeline::recreateImages(uint32 width, uint32 height)
{
    destroyImageSet(m_resolved);
    m_width = width;
    m_height = height;
    createImageSet(m_resolved);

    // One-time UNDEFINED -> GENERAL so a never-yet-written resolved image (sampled as history on the very
    // first frame) is in the layout its descriptor was written with.
    oc::vector<vk::ImageMemoryBarrier2> bars;
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
    {
        const uint32 i = slot(f, e);
        bars.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = m_resolved.image[i],
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        });
        m_resolved.initialized[i] = true;
    }
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer cmd = init.begin(true);
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
    init.end();
    init.submitGraphics();
    (void)Globals::device.getGraphicsQueue().waitIdle();
}

void TaaPipeline::initialize(uint32 width, uint32 height, uint32 viewCount)
{
    m_viewCount = viewCount;
    ComputePipelineLayout layout; buildLayout(layout); m_pipeline.initialize(layout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
        m_sets[slot(f, e)].initialize(m_pipeline.getDescriptorSetLayout());

    recreateImages(width, height);

    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = vk::False,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = vk::BorderColor::eFloatOpaqueBlack,
        .unnormalizedCoordinates = vk::False,
    };
    auto samplerResult = Globals::device.getDevice().createSampler(samplerInfo);
    assert(samplerResult.result == vk::Result::eSuccess);
    m_sampler = samplerResult.value;
}

void TaaPipeline::reloadShaders()
{
    ComputePipelineLayout layout; buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("TaaPipeline: shader reload failed, keeping previous pipeline\n");
}

void TaaPipeline::transitionToGeneral(vk::CommandBuffer cmd, ImageSet& set, uint32 slotIdx, bool forWrite)
{
    vk::ImageMemoryBarrier2 bar{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = forWrite ? vk::AccessFlagBits2::eShaderStorageWrite : vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = set.initialized[slotIdx] ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = set.image[slotIdx],
        .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar });
    set.initialized[slotIdx] = true;
}

void TaaPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    // eye (0/1) selects the per-eye history; viewIndex selects the UBO matrices (0 = centre/desktop, 1/2 = eyes).
    const uint32 viewIndex = RendererVKLayout::eyeToViewIndex(eye, m_viewCount);
    const uint32 prevFrame = (frameIdx + RendererVKLayout::NUM_FRAMES_IN_FLIGHT - 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    const uint32 cur = slot(frameIdx, eye);
    const uint32 prevIdx = slot(prevFrame, eye); // same eye's resolved image last frame (history)
    vk::PipelineLayout pipelineLayout = m_pipeline.getPipelineLayout();
    const uint32 gx = (m_width + 7) / 8;
    const uint32 gy = (m_height + 7) / 8;

    transitionToGeneral(cmd, m_resolved, cur, true);

    auto uboInfo = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };
    DescriptorSet& set = m_sets[cur];
    vk::DescriptorSet vkSet = set.getDescriptorSet();
    oc::array<DescriptorSetUpdateInfo, 7> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.currentColorSampler, params.currentColorView) } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_sampler, m_resolved.view[prevIdx]) } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.gbufferDepthView) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.prevGbufferDepthView) } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_resolved.view[cur]) } },
        DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.gbufferNormalView) } },
    };
    commandBuffer.cmdUpdateDescriptorSets(pipelineLayout, vk::PipelineBindPoint::eCompute, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipelineLayout, 0, 1, &vkSet, 0, nullptr);
    TaaPC pc{ .width = m_width, .height = m_height, .feedback = params.feedback, .viewIndex = viewIndex, .oceanFeedback = params.oceanFeedback };
    cmd.pushConstants(pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
    cmd.dispatch(gx, gy, 1);

    // Resolved storage write -> composite fragment sampled read.
    vk::MemoryBarrier2 bar{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
}
