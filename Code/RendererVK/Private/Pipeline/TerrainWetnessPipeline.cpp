module RendererVK;

import Core;
import Core.Tweaks;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import :Layout;

namespace
{
    constexpr vk::Format WET_FORMAT = vk::Format::eR16Sfloat;
    constexpr uint32 WET_LAYERS = 2; // ping/pong: the pass reads last frame's layer (3x3 diffusion tent) and writes the other
    constexpr vk::ImageSubresourceRange WET_RANGE{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, WET_LAYERS };
}

TerrainWetnessPipeline::~TerrainWetnessPipeline()
{
    destroyImage();
}

void TerrainWetnessPipeline::destroyImage()
{
    if (m_view) Globals::device.getDevice().destroyImageView(m_view);
    Globals::gpuAllocator.destroyImage(m_image, m_memory);
    m_view = nullptr; m_image = nullptr; m_memory = nullptr;
}

void TerrainWetnessPipeline::buildLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/terrain_wetness.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    layout.defines.push_back({ "WET_DIFFUSION", m_diffusion ? "1" : "0" });
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    // Binding 2 (terrain-data cascades) is a ping-pong pair rewritten per frame by updateTerrainDescriptor.
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags[2] = vk::DescriptorBindingFlagBits::eUpdateAfterBind;
}

void TerrainWetnessPipeline::createImage()
{
    vk::Device vkDevice = Globals::device.getDevice();
    vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = WET_FORMAT,
        .extent = { RES, RES, 1 },
        .mipLevels = 1,
        .arrayLayers = WET_LAYERS,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, m_image, m_memory, "TerrainWetness");
    vk::ImageViewCreateInfo viewInfo{
        .image = m_image,
        .viewType = vk::ImageViewType::e2DArray,
        .format = WET_FORMAT,
        .subresourceRange = WET_RANGE,
    };
    auto viewResult = vkDevice.createImageView(viewInfo);
    assert(viewResult.result == vk::Result::eSuccess);
    m_view = viewResult.value;

    // One-time init: clear to dry, then GENERAL for its whole life (compute read/write in place, fragment
    // sampled read) - no per-frame layout churn.
    {
        CommandBuffer init;
        init.initialize(vk::CommandBufferLevel::ePrimary);
        vk::CommandBuffer cmd = init.begin(true);
        vk::ImageMemoryBarrier2 toClear{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = m_image,
            .subresourceRange = WET_RANGE,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toClear });
        const vk::ClearColorValue zero{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }; // vk::ClearColorValue is spelled in std::array
        cmd.clearColorImage(m_image, vk::ImageLayout::eTransferDstOptimal, &zero, 1, &WET_RANGE);
        vk::ImageMemoryBarrier2 toGeneral{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = m_image,
            .subresourceRange = WET_RANGE,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toGeneral });
        init.end();
        init.submitGraphics();
        (void)Globals::device.graphicsQueueWaitIdle();
    }
}

void TerrainWetnessPipeline::initialize(oc::function<void()> onDefinesChanged)
{
    Tweak::boolean("Terrain/Wetness", "Diffusion", &m_diffusion, onDefinesChanged);

    m_sampler.initialize(vk::SamplerAddressMode::eClampToEdge);
    createImage();

    ComputePipelineLayout layout;
    buildLayout(layout);
    m_pipeline.initialize(layout);
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
        m_sets[i].initialize(m_pipeline.getDescriptorSetLayout());
}

void TerrainWetnessPipeline::reloadShaders()
{
    ComputePipelineLayout layout;
    buildLayout(layout);
    if (!m_pipeline.reloadShaders(layout))
        printf("TerrainWetnessPipeline: shader reload failed, keeping previous pipeline\n");
}

void TerrainWetnessPipeline::updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler)
{
    vk::DescriptorImageInfo imageInfo{ .sampler = terrainSampler, .imageView = terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{ .dstSet = m_sets[frameIdx].getDescriptorSet(), .dstBinding = 2, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
}

void TerrainWetnessPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();

    // Last frame's forward pass sampled the image and last frame's dispatch wrote it; this dispatch
    // reads and writes it in place. The image never leaves GENERAL, so a global barrier is enough.
    vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &before });

    oc::array<DescriptorSetUpdateInfo, 4> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer,
            .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = params.ubo->getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage,
            .imageInfos = { vk::DescriptorImageInfo{ .imageView = m_view, .imageLayout = vk::ImageLayout::eGeneral } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.terrainSampler, .imageView = params.terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler,
            .imageInfos = { vk::DescriptorImageInfo{ .sampler = params.oceanMapsSampler, .imageView = params.oceanMapsView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal } } },
    };
    vk::DescriptorSet set = m_sets[frameIdx].getDescriptorSet();
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline.getPipeline());
    commandBuffer.cmdUpdateDescriptorSets(m_pipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, set, updates);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline.getPipelineLayout(), 0, 1, &set, 0, nullptr);
    cmd.dispatch(RES / 8, RES / 8, 1);

    // Hand the result to the scene forward pass (sampled in the terrain fragment shader).
    vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &after });
}
