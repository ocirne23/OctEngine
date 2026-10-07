module RendererVK;

import Core;
import Core.glm;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    constexpr vk::Format BLOOM_FORMAT = vk::Format::eB10G11R11UfloatPack32;

    // Must match both shaders' push constant block (the downsample declares only the first three).
    struct BloomPC
    {
        glm::ivec2 srcRegion;
        glm::ivec2 dstRegion;
        glm::vec2  srcInvSize;
        float dstWeight;
        float srcWeight;
    };

    void computeBarrier(vk::CommandBuffer cmd, vk::PipelineStageFlags2 srcStages, vk::AccessFlags2 srcAccess, vk::PipelineStageFlags2 dstStages, vk::AccessFlags2 dstAccess)
    {
        const vk::MemoryBarrier2 barrier{ .srcStageMask = srcStages, .srcAccessMask = srcAccess, .dstStageMask = dstStages, .dstAccessMask = dstAccess };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &barrier });
    }
}

BloomPipeline::~BloomPipeline()
{
    destroyImages();
    if (m_sampler)
        Globals::device.getDevice().destroySampler(m_sampler);
}

void BloomPipeline::buildLayouts(ComputePipelineLayout& down, ComputePipelineLayout& up)
{
    const auto fill = [](ComputePipelineLayout& layout, const char* path)
    {
        layout.computeShaderDebugFilePath = path;
        layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
        layout.descriptorSetLayoutBindings = {
            vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute },
            vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute },
        };
        layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(BloomPC) });
    };
    fill(down, "Shaders/PostProcess/bloom_downsample.cs.glsl");
    fill(up, "Shaders/PostProcess/bloom_upsample.cs.glsl");
}

void BloomPipeline::initialize(uint32 width, uint32 height)
{
    ComputePipelineLayout down, up;
    buildLayouts(down, up);
    m_downPipeline.initialize(down);
    m_upPipeline.initialize(up);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
        for (uint32 i = 0; i < MAX_LEVELS - 1; ++i)
        {
            m_downSets[f][i].initialize(m_downPipeline.getDescriptorSetLayout(), "Bloom.down");
            m_upSets[f][i].initialize(m_upPipeline.getDescriptorSetLayout(), "Bloom.up");
        }

    // Linear + clamp: the filters are built on bilinear taps; the composite samples level 0 with it too.
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
    Globals::device.setDebugName(m_sampler, "Bloom");

    recreateImages(width, height);
}

void BloomPipeline::reloadShaders()
{
    ComputePipelineLayout down, up;
    buildLayouts(down, up);
    bool ok = m_downPipeline.reloadShaders(down);
    ok = m_upPipeline.reloadShaders(up) && ok;
    if (!ok)
        printf("BloomPipeline: shader reload failed, keeping previous pipelines\n");
}

void BloomPipeline::destroyImages()
{
    for (vk::ImageView& view : m_levelViews)
    {
        if (view)
            Globals::device.getDevice().destroyImageView(view);
        view = nullptr;
    }
    Globals::gpuAllocator.destroyImage(m_image, m_memory);
    m_image = nullptr;
    m_memory = nullptr;
}

glm::ivec2 BloomPipeline::levelSize(uint32 level) const
{
    const glm::ivec2 level0((int32)((m_width + 1) / 2), (int32)((m_height + 1) / 2));
    return glm::max(glm::ivec2(1), glm::ivec2(level0.x >> level, level0.y >> level));
}

void BloomPipeline::recreateImages(uint32 width, uint32 height)
{
    destroyImages();
    m_width = oc::max(width, 2u);
    m_height = oc::max(height, 2u);
    const glm::ivec2 size0 = levelSize(0);
    m_levelCount = 1;
    while (m_levelCount < MAX_LEVELS && oc::min(size0.x, size0.y) >> m_levelCount >= 2)
        ++m_levelCount;

    const vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = BLOOM_FORMAT,
        .extent = { (uint32)size0.x, (uint32)size0.y, 1 },
        .mipLevels = m_levelCount,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        // TRANSFER_DST: the one-time clear below.
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, m_image, m_memory, "Bloom");
    for (uint32 level = 0; level < m_levelCount; ++level)
    {
        const vk::ImageViewCreateInfo viewInfo{
            .image = m_image,
            .viewType = vk::ImageViewType::e2D,
            .format = BLOOM_FORMAT,
            .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = level, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
        };
        auto viewResult = Globals::device.getDevice().createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        m_levelViews[level] = viewResult.value;
        Globals::device.setDebugName(m_levelViews[level], oc::format("Bloom[{}]", level).c_str());
    }

    // GENERAL for life, cleared to black: the composite may sample level 0 before bloom first runs.
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "Bloom.init");
    vk::CommandBuffer cmd = init.begin(true);
    const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, m_levelCount, 0, 1 };
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
    const vk::ClearColorValue black{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } };
    cmd.clearColorImage(m_image, vk::ImageLayout::eGeneral, &black, 1, &range);
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

glm::vec4 BloomPipeline::getUvTransform(glm::ivec2 viewportMin, glm::ivec2 viewportSize) const
{
    // The histogram puts viewport pixel q into level-0 texel q / 2, so level-0 uv = (uv * full - vpMin) / 2 / size0.
    const glm::vec2 size0 = glm::vec2(levelSize(0));
    const glm::vec2 scale = glm::vec2((float)m_width, (float)m_height) * 0.5f / size0;
    return glm::vec4(scale, -glm::vec2(viewportMin) * 0.5f / size0);
}

float BloomPipeline::levelWeight(uint32 level, float radius)
{
    return exp2f((float)level * (2.0f * radius - 1.0f));
}

float BloomPipeline::getNormalize(uint32 levels, float radius) const
{
    levels = clampLevels(levels);
    float sum = 0.0f;
    for (uint32 level = 0; level < levels; ++level)
        sum += levelWeight(level, radius);
    return 1.0f / sum;
}

void BloomPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, glm::ivec2 viewportSize, uint32 levels, float radius)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    levels = clampLevels(levels);

    // The viewport region of every level: level 0 is what the histogram wrote, then half (rounded up) per level,
    // inside the level's own size.
    oc::array<glm::ivec2, MAX_LEVELS> region;
    region[0] = glm::clamp((viewportSize + 1) / 2, glm::ivec2(1), levelSize(0));
    for (uint32 level = 1; level < levels; ++level)
        region[level] = glm::clamp((region[level - 1] + 1) / 2, glm::ivec2(1), levelSize(level));

    const auto step = [&](ComputePipeline& pipeline, DescriptorSet& set, uint32 src, uint32 dst, float dstWeight, float srcWeight)
    {
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 2> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eCombinedImageSampler,
                .imageInfos = { vk::DescriptorImageInfo{ .sampler = m_sampler, .imageView = m_levelViews[src], .imageLayout = vk::ImageLayout::eGeneral } } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage,
                .imageInfos = { vk::DescriptorImageInfo{ .imageView = m_levelViews[dst], .imageLayout = vk::ImageLayout::eGeneral } } },
        };
        const vk::PipelineLayout layout = pipeline.getPipelineLayout();
        commandBuffer.cmdUpdateDescriptorSets(layout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &vkSet, 0, nullptr);
        const BloomPC pc{ .srcRegion = region[src], .dstRegion = region[dst], .srcInvSize = 1.0f / glm::vec2(levelSize(src)),
            .dstWeight = dstWeight, .srcWeight = srcWeight };
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch((uint32)(region[dst].x + 7) / 8, (uint32)(region[dst].y + 7) / 8, 1);
        // This level's writes -> the next step's reads (sampled, or the upsample's own load of its level).
        computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
    };

    // Level 0 (the histogram pass's storage write) -> the first downsample's read.
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead);
    for (uint32 level = 0; level + 1 < levels; ++level)
        step(m_downPipeline, m_downSets[frameIdx][level], level, level + 1, 1.0f, 1.0f);
    // Each level weighs its own blur once (the "Radius" curve); the smallest one on the first step.
    for (uint32 level = levels - 1; level-- > 0;)
        step(m_upPipeline, m_upSets[frameIdx][level], level + 1, level,
            levelWeight(level, radius), level + 2 == levels ? levelWeight(level + 1, radius) : 1.0f);
    // Level 0 -> the composite's fragment read.
    computeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
}
