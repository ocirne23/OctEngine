module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import :GIProbePipeline; // the sky map's texel grid (SKY_MAP_WIDTH / HEIGHT)

namespace
{
    constexpr vk::Format NOISE_FORMAT = vk::Format::eR8G8B8A8Unorm;
    // Color: rgb = in-scatter, a = transmittance. Depth: log2 distances (first hit, weighted, limit) + steps.
    constexpr vk::Format CLOUD_FORMAT = vk::Format::eR16G16B16A16Sfloat;
    constexpr uint32 BASE_SIZE = 128;   // keep in sync with CLOUD_BASE_RES (clouds.inc.glsl)
    constexpr uint32 DETAIL_SIZE = 32;  // keep in sync with CLOUD_DETAIL_RES
    constexpr uint32 WEATHER_SIZE = 512;
    constexpr uint32 CURL_SIZE = 128;

    struct NoisePC
    {
        uint32 mode;
        uint32 size;
    };
    struct ShadowPC
    {
        uint32 cascade;
        uint32 resolution;
    };
    // x = the front's along-light coordinate, y = mean extinction, z = whole optical depth (cloud_shadow.inc.glsl).
    constexpr vk::Format SHADOW_FORMAT = vk::Format::eR32G32B32A32Sfloat;
    struct CloudPC
    {
        uint32 viewIndex;
        uint32 width;
        uint32 height;
        uint32 pad;
    };

    auto imgInfoGeneral(vk::ImageView view) { return vk::DescriptorImageInfo{ .imageView = view, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledGeneral(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledDepth(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = SCENE_DEPTH_SAMPLED_LAYOUT }; }

    vk::DescriptorSetLayoutBinding binding(uint32 idx, vk::DescriptorType type, vk::ShaderStageFlags stages = vk::ShaderStageFlagBits::eCompute)
    {
        return vk::DescriptorSetLayoutBinding{ .binding = idx, .descriptorType = type, .descriptorCount = 1, .stageFlags = stages };
    }

    vk::Sampler createSampler(vk::SamplerAddressMode address, float maxLod, const char* debugName)
    {
        vk::SamplerCreateInfo info{
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = address,
            .addressModeV = address,
            .addressModeW = address,
            .anisotropyEnable = vk::False,
            .minLod = 0.0f,
            .maxLod = maxLod,
            .borderColor = vk::BorderColor::eFloatOpaqueWhite,
            .unnormalizedCoordinates = vk::False,
        };
        auto result = Globals::device.getDevice().createSampler(info);
        assert(result.result == vk::Result::eSuccess);
        Globals::device.setDebugName(result.value, debugName);
        return result.value;
    }
}

void CloudPipeline::buildNoiseLayout(ComputePipelineLayout& layout, bool is3D)
{
    layout.computeShaderDebugFilePath = "Shaders/cloud_noise.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    if (is3D)
        layout.defines.push_back({ "NOISE_3D", "1" });
    layout.descriptorSetLayoutBindings.push_back(binding(0, vk::DescriptorType::eStorageImage));
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(NoisePC) });
}

void CloudPipeline::buildShadowLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/cloud_shadow.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    b.push_back(binding(1, vk::DescriptorType::eStorageImage)); // the shadow map (both layers)
    for (uint32 i = 2; i <= 5; ++i)                             // the noise (CLOUD_NOISE_BINDING 2)
        b.push_back(binding(i, vk::DescriptorType::eCombinedImageSampler));
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(ShadowPC) });
}

// The shadow map, cleared to "no cloud" (front at -infinity, no depth): valid for every consumer before the
// first update and while the clouds are off.
void CloudPipeline::createShadowMap()
{
    vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = SHADOW_FORMAT,
        .extent = { SHADOW_RESOLUTION, SHADOW_RESOLUTION, 1 },
        .mipLevels = 1,
        .arrayLayers = SHADOW_CASCADES,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, m_shadow.image, m_shadow.memory, "Clouds.shadowMap");
    vk::ImageViewCreateInfo viewInfo{
        .image = m_shadow.image,
        .viewType = vk::ImageViewType::e2DArray,
        .format = SHADOW_FORMAT,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = SHADOW_CASCADES },
    };
    auto viewResult = Globals::device.getDevice().createImageView(viewInfo);
    assert(viewResult.result == vk::Result::eSuccess);
    m_shadow.view = viewResult.value;
    Globals::device.setDebugName(m_shadow.view, "Clouds.shadowMap");

    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "Clouds.shadowInit");
    vk::CommandBuffer cmd = init.begin(true);
    const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, SHADOW_CASCADES };
    vk::ImageMemoryBarrier2 toGeneral{
        .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = m_shadow.image,
        .subresourceRange = range,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toGeneral });
    cmd.clearColorImage(m_shadow.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ -1e30f, 0.0f, 0.0f, 0.0f } }, { range });
    vk::MemoryBarrier2 clearToRead{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &clearToRead });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void CloudPipeline::buildSkyLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/cloud_sky.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    b.push_back(binding(1, vk::DescriptorType::eStorageImage));         // the sky clouds
    b.push_back(binding(2, vk::DescriptorType::eCombinedImageSampler)); // the sky map (clear layer: ambient)
    b.push_back(binding(3, vk::DescriptorType::eCombinedImageSampler)); // the shadow map (self-shadow)
    for (uint32 i = 4; i <= 7; ++i)                                     // the noise (CLOUD_NOISE_BINDING 4)
        b.push_back(binding(i, vk::DescriptorType::eCombinedImageSampler));
}

// The sky clouds, cleared to "no cloud" (transmittance 1): valid before the first render and while the
// clouds are off (the sky map also gates on the CLOUDS define and u_cloudShape0.w).
void CloudPipeline::createSkyClouds()
{
    vk::ImageCreateInfo info{
        .imageType = vk::ImageType::e2D,
        .format = CLOUD_FORMAT,
        .extent = { GIProbePipeline::SKY_MAP_WIDTH, GIProbePipeline::SKY_MAP_HEIGHT, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, m_skyClouds.image, m_skyClouds.memory, "Clouds.sky");
    vk::ImageViewCreateInfo viewInfo{
        .image = m_skyClouds.image,
        .viewType = vk::ImageViewType::e2D,
        .format = CLOUD_FORMAT,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
    };
    auto viewResult = Globals::device.getDevice().createImageView(viewInfo);
    assert(viewResult.result == vk::Result::eSuccess);
    m_skyClouds.view = viewResult.value;
    Globals::device.setDebugName(m_skyClouds.view, "Clouds.sky");

    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "Clouds.skyInit");
    vk::CommandBuffer cmd = init.begin(true);
    const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    vk::ImageMemoryBarrier2 toGeneral{
        .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = m_skyClouds.image,
        .subresourceRange = range,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toGeneral });
    cmd.clearColorImage(m_skyClouds.image, vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }, { range });
    vk::MemoryBarrier2 clearToRead{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &clearToRead });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void CloudPipeline::buildMarchLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/cloud_march.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    b.push_back(binding(1, vk::DescriptorType::eCombinedImageSampler)); // scene depth
    b.push_back(binding(2, vk::DescriptorType::eCombinedImageSampler)); // sky map
    b.push_back(binding(3, vk::DescriptorType::eStorageImage));         // out color
    b.push_back(binding(4, vk::DescriptorType::eStorageImage));         // out depth
    for (uint32 i = 5; i <= 8; ++i)                                     // the noise (CLOUD_NOISE_BINDING 5)
        b.push_back(binding(i, vk::DescriptorType::eCombinedImageSampler));
    b.push_back(binding(9, vk::DescriptorType::eCombinedImageSampler)); // the shadow map (self-shadow)
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(CloudPC) });
}

void CloudPipeline::buildTemporalLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/cloud_temporal.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer));
    for (uint32 i = 1; i <= 4; ++i) // this frame's march color + depth, the history color + depth
        b.push_back(binding(i, vk::DescriptorType::eCombinedImageSampler));
    b.push_back(binding(5, vk::DescriptorType::eStorageImage));
    b.push_back(binding(6, vk::DescriptorType::eStorageImage));
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(CloudPC) });
}

void CloudPipeline::buildApplyLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/composite.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/cloud_apply.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eNone;
    // out = inScatter + sceneColor * transmittance, like the fog apply.
    layout.blendEnable = true;
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    layout.colorWriteAlpha = false; // scene colour alpha = TAA's ocean flag
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(binding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eFragment));
    for (uint32 i = 1; i <= 3; ++i) // scene depth, cloud color, cloud depth
        b.push_back(binding(i, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment));
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

void CloudPipeline::createNoiseTexture(NoiseTexture& tex, uint32 size, bool is3D, const char* debugName)
{
    vk::Device vkDevice = Globals::device.getDevice();
    tex.size = size;
    tex.is3D = is3D;
    tex.mipLevels = 1;
    for (uint32 s = size; s > 1; s >>= 1)
        ++tex.mipLevels;
    vk::ImageCreateInfo info{
        .imageType = is3D ? vk::ImageType::e3D : vk::ImageType::e2D,
        .format = NOISE_FORMAT,
        .extent = { size, size, is3D ? size : 1u },
        .mipLevels = tex.mipLevels,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    (void)Globals::gpuAllocator.createImage(info, tex.image, tex.memory, debugName);

    const vk::ImageViewType viewType = is3D ? vk::ImageViewType::e3D : vk::ImageViewType::e2D;
    vk::ImageViewCreateInfo viewInfo{
        .image = tex.image,
        .viewType = viewType,
        .format = NOISE_FORMAT,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = tex.mipLevels, .baseArrayLayer = 0, .layerCount = 1 },
    };
    auto viewResult = vkDevice.createImageView(viewInfo);
    assert(viewResult.result == vk::Result::eSuccess);
    tex.view = viewResult.value;
    Globals::device.setDebugName(tex.view, debugName);
    viewInfo.subresourceRange.levelCount = 1;
    auto storageResult = vkDevice.createImageView(viewInfo);
    assert(storageResult.result == vk::Result::eSuccess);
    tex.storageView = storageResult.value;
    Globals::device.setDebugName(tex.storageView, debugName);
}

void CloudPipeline::destroyNoiseTexture(NoiseTexture& tex)
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (tex.view)
        vkDevice.destroyImageView(tex.view);
    if (tex.storageView)
        vkDevice.destroyImageView(tex.storageView);
    Globals::gpuAllocator.destroyImage(tex.image, tex.memory);
    tex = NoiseTexture{};
}

// One-shot: every noise texture to GENERAL, the generator into mip 0, then a blit chain down the mips.
void CloudPipeline::generateNoise()
{
    NoiseTexture* textures[] = { &m_base, &m_detail, &m_weather, &m_curl };
    constexpr uint32 modes[] = { 0, 1, 2, 3 };

    for (uint32 i = 0; i < 4; ++i)
    {
        DescriptorSet& set = m_noiseSets[i];
        if (!set.getDescriptorSet())
            set.initialize((textures[i]->is3D ? m_noise3DPipeline : m_noise2DPipeline).getDescriptorSetLayout(), "Clouds.noise");
        vk::DescriptorImageInfo imageInfo = imgInfoGeneral(textures[i]->storageView);
        vk::WriteDescriptorSet write{ .dstSet = set.getDescriptorSet(), .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &imageInfo };
        Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
    }

    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "Clouds.noise");
    vk::CommandBuffer cmd = init.begin(true);

    oc::vector<vk::ImageMemoryBarrier2> toGeneral;
    for (NoiseTexture* tex : textures)
        toGeneral.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = tex->image,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, tex->mipLevels, 0, 1 },
        });
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)toGeneral.size(), .pImageMemoryBarriers = toGeneral.data() });

    for (uint32 i = 0; i < 4; ++i)
    {
        const NoiseTexture& tex = *textures[i];
        const ComputePipeline& pipeline = tex.is3D ? m_noise3DPipeline : m_noise2DPipeline;
        const vk::DescriptorSet vkSet = m_noiseSets[i].getDescriptorSet();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        const NoisePC pc{ .mode = modes[i], .size = tex.size };
        cmd.pushConstants(pipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        const uint32 groups = (tex.size + 3) / 4;
        cmd.dispatch(groups, groups, tex.is3D ? groups : 1u);
    }

    for (NoiseTexture* tex : textures)
    {
        for (uint32 mip = 1; mip < tex->mipLevels; ++mip)
        {
            // mip - 1 was written (by the generator or the previous blit) -> read by this blit.
            vk::ImageMemoryBarrier2 srcReady{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eBlit,
                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = tex->image,
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, mip - 1, 1, 0, 1 },
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &srcReady });
            const int32 srcSize = (int32)(tex->size >> (mip - 1));
            const int32 dstSize = oc::max((int32)(tex->size >> mip), 1);
            vk::ImageBlit2 blit{
                .srcSubresource = { vk::ImageAspectFlagBits::eColor, mip - 1, 0, 1 },
                .dstSubresource = { vk::ImageAspectFlagBits::eColor, mip, 0, 1 },
            };
            blit.srcOffsets[0] = vk::Offset3D{ 0, 0, 0 };
            blit.srcOffsets[1] = vk::Offset3D{ srcSize, srcSize, tex->is3D ? srcSize : 1 };
            blit.dstOffsets[0] = vk::Offset3D{ 0, 0, 0 };
            blit.dstOffsets[1] = vk::Offset3D{ dstSize, dstSize, tex->is3D ? dstSize : 1 };
            vk::BlitImageInfo2 blitInfo{
                .srcImage = tex->image, .srcImageLayout = vk::ImageLayout::eGeneral,
                .dstImage = tex->image, .dstImageLayout = vk::ImageLayout::eGeneral,
                .regionCount = 1, .pRegions = &blit, .filter = vk::Filter::eLinear,
            };
            cmd.blitImage2(blitInfo);
        }
    }
    vk::MemoryBarrier2 toRead{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eBlit,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toRead });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

void CloudPipeline::createImageSet(ImageSet& set, const char* debugName)
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
    {
        const uint32 i = slot(f, e);
        vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = CLOUD_FORMAT,
            .extent = { m_width, m_height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, // TransferDst: cleared at creation
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        (void)Globals::gpuAllocator.createImage(info, set.image[i], set.memory[i], debugName);
        vk::ImageViewCreateInfo viewInfo{
            .image = set.image[i],
            .viewType = vk::ImageViewType::e2D,
            .format = CLOUD_FORMAT,
            .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
        };
        auto viewResult = vkDevice.createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        set.view[i] = viewResult.value;
        Globals::device.setDebugName(set.view[i], debugName);
    }
}

void CloudPipeline::destroyImageSet(ImageSet& set)
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < SLOTS; ++i)
    {
        if (set.view[i])
            vkDevice.destroyImageView(set.view[i]);
        Globals::gpuAllocator.destroyImage(set.image[i], set.memory[i]);
        set.view[i] = nullptr;
        set.image[i] = nullptr;
        set.memory[i] = nullptr;
    }
}

// The half-res images, cleared to "no cloud" (transmittance 1, far distances): the first frame's history
// reads valid data that the distance test rejects.
void CloudPipeline::recreateImages(uint32 fullWidth, uint32 fullHeight)
{
    destroyImageSet(m_marchColor);
    destroyImageSet(m_marchDepth);
    destroyImageSet(m_accumColor);
    destroyImageSet(m_accumDepth);
    m_width = (fullWidth + 1) / 2;
    m_height = (fullHeight + 1) / 2;
    createImageSet(m_marchColor, "Clouds.marchColor");
    createImageSet(m_marchDepth, "Clouds.marchDepth");
    createImageSet(m_accumColor, "Clouds.accumColor");
    createImageSet(m_accumDepth, "Clouds.accumDepth");

    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary, "Clouds.init");
    vk::CommandBuffer cmd = init.begin(true);
    const ImageSet* sets[] = { &m_marchColor, &m_marchDepth, &m_accumColor, &m_accumDepth };
    oc::vector<vk::ImageMemoryBarrier2> bars;
    for (const ImageSet* s : sets)
        for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
        for (uint32 e = 0; e < m_viewCount; ++e)
            bars.push_back(vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eClear,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = s->image[slot(f, e)],
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
            });
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
    const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    const vk::ClearColorValue noCloud{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } };
    const vk::ClearColorValue farDepth{ std::array<float, 4>{ 30.0f, 30.0f, 30.0f, 0.0f } };
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
    {
        const uint32 i = slot(f, e);
        cmd.clearColorImage(m_marchColor.image[i], vk::ImageLayout::eGeneral, noCloud, { range });
        cmd.clearColorImage(m_accumColor.image[i], vk::ImageLayout::eGeneral, noCloud, { range });
        cmd.clearColorImage(m_marchDepth.image[i], vk::ImageLayout::eGeneral, farDepth, { range });
        cmd.clearColorImage(m_accumDepth.image[i], vk::ImageLayout::eGeneral, farDepth, { range });
    }
    vk::MemoryBarrier2 clearToUse{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &clearToUse });
    init.end();
    init.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();
}

CloudPipeline::~CloudPipeline()
{
    destroyNoiseTexture(m_base);
    destroyNoiseTexture(m_detail);
    destroyNoiseTexture(m_weather);
    destroyNoiseTexture(m_curl);
    destroyImageSet(m_marchColor);
    destroyImageSet(m_marchDepth);
    destroyImageSet(m_accumColor);
    destroyImageSet(m_accumDepth);
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_shadow.view)
        vkDevice.destroyImageView(m_shadow.view);
    Globals::gpuAllocator.destroyImage(m_shadow.image, m_shadow.memory);
    if (m_skyClouds.view)
        vkDevice.destroyImageView(m_skyClouds.view);
    Globals::gpuAllocator.destroyImage(m_skyClouds.image, m_skyClouds.memory);
    if (m_noiseSampler)
        vkDevice.destroySampler(m_noiseSampler);
    if (m_linearSampler)
        vkDevice.destroySampler(m_linearSampler);
}

void CloudPipeline::initialize(uint32 fullWidth, uint32 fullHeight, vk::RenderPass sceneRenderPass, uint32 viewCount)
{
    m_viewCount = viewCount;
    ComputePipelineLayout noise3DLayout;   buildNoiseLayout(noise3DLayout, true);  m_noise3DPipeline.initialize(noise3DLayout);
    ComputePipelineLayout noise2DLayout;   buildNoiseLayout(noise2DLayout, false); m_noise2DPipeline.initialize(noise2DLayout);
    ComputePipelineLayout shadowLayout;    buildShadowLayout(shadowLayout);        m_shadowPipeline.initialize(shadowLayout);
    ComputePipelineLayout marchLayout;     buildMarchLayout(marchLayout);          m_marchPipeline.initialize(marchLayout);
    ComputePipelineLayout skyLayout;       buildSkyLayout(skyLayout);              m_skyPipeline.initialize(skyLayout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        m_shadowSets[f].initialize(m_shadowPipeline.getDescriptorSetLayout(), "Clouds.shadow");
        m_skySets[f].initialize(m_skyPipeline.getDescriptorSetLayout(), "Clouds.sky");
    }
    ComputePipelineLayout temporalLayout;  buildTemporalLayout(temporalLayout);    m_temporalPipeline.initialize(temporalLayout);
    GraphicsPipelineLayout applyLayout;    buildApplyLayout(applyLayout);          m_applyPipeline.initialize(sceneRenderPass, applyLayout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
    {
        const uint32 i = slot(f, e);
        m_marchSets[i].initialize(m_marchPipeline.getDescriptorSetLayout(), "Clouds.march");
        m_temporalSets[i].initialize(m_temporalPipeline.getDescriptorSetLayout(), "Clouds.temporal");
        m_applySets[i].initialize(m_applyPipeline.getDescriptorSetLayout(), "Clouds.apply");
    }

    m_noiseSampler = createSampler(vk::SamplerAddressMode::eRepeat, 1000.0f, "Clouds.noise");
    m_linearSampler = createSampler(vk::SamplerAddressMode::eClampToEdge, 0.0f, "Clouds.linear");
    createNoiseTexture(m_base, BASE_SIZE, true, "Clouds.base");
    createNoiseTexture(m_detail, DETAIL_SIZE, true, "Clouds.detail");
    createNoiseTexture(m_weather, WEATHER_SIZE, false, "Clouds.weather");
    createNoiseTexture(m_curl, CURL_SIZE, false, "Clouds.curl");
    generateNoise();
    createShadowMap();
    createSkyClouds();
    recreateImages(fullWidth, fullHeight);
}

void CloudPipeline::reloadShaders(vk::RenderPass sceneRenderPass)
{
    ComputePipelineLayout noise3DLayout;  buildNoiseLayout(noise3DLayout, true);
    ComputePipelineLayout noise2DLayout;  buildNoiseLayout(noise2DLayout, false);
    ComputePipelineLayout shadowLayout;   buildShadowLayout(shadowLayout);
    ComputePipelineLayout skyLayout;      buildSkyLayout(skyLayout);
    ComputePipelineLayout marchLayout;    buildMarchLayout(marchLayout);
    ComputePipelineLayout temporalLayout; buildTemporalLayout(temporalLayout);
    GraphicsPipelineLayout applyLayout;   buildApplyLayout(applyLayout);
    bool ok = m_noise3DPipeline.reloadShaders(noise3DLayout);
    ok = m_noise2DPipeline.reloadShaders(noise2DLayout) && ok;
    ok = m_shadowPipeline.reloadShaders(shadowLayout) && ok;
    ok = m_skyPipeline.reloadShaders(skyLayout) && ok;
    ok = m_marchPipeline.reloadShaders(marchLayout) && ok;
    ok = m_temporalPipeline.reloadShaders(temporalLayout) && ok;
    ok = m_applyPipeline.reloadShaders(sceneRenderPass, applyLayout) && ok;
    if (!ok)
        printf("CloudPipeline: shader reload failed, keeping previous pipeline(s)\n");
    generateNoise(); // a generator edit takes effect at once
}

oc::array<vk::DescriptorImageInfo, 4> CloudPipeline::noiseInfos() const
{
    return {
        sampledGeneral(m_noiseSampler, m_base.view),
        sampledGeneral(m_noiseSampler, m_detail.view),
        sampledGeneral(m_noiseSampler, m_weather.view),
        sampledGeneral(m_noiseSampler, m_curl.view),
    };
}

void CloudPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 viewIndex = RendererVKLayout::eyeToViewIndex(eye, m_viewCount);
    const uint32 prevFrame = (frameIdx + RendererVKLayout::NUM_FRAMES_IN_FLIGHT - 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    const uint32 cur = slot(frameIdx, eye);
    const uint32 prev = slot(prevFrame, eye);
    const uint32 gx = (m_width + 7) / 8;
    const uint32 gy = (m_height + 7) / 8;
    const CloudPC pc{ .viewIndex = viewIndex, .width = m_width, .height = m_height, .pad = 0 };
    auto uboInfo = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };

    // In: the sky map (GI compute writes), and this slot's images, last read by the previous use of this
    // slot's apply (fragment) and temporal (compute) - WAR before this frame's storage writes.
    vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier });

    { // -------- March --------
        const vk::DescriptorSet vkSet = m_marchSets[cur].getDescriptorSet();
        const oc::array<vk::DescriptorImageInfo, 4> noise = noiseInfos();
        oc::array<DescriptorSetUpdateInfo, 10> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledDepth(params.sceneDepthSampler, params.sceneDepthView) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(params.skyMapSampler, params.skyMapView) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_marchColor.view[cur]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_marchDepth.view[cur]) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[0] } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[1] } },
            DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[2] } },
            DescriptorSetUpdateInfo{ .binding = 8, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[3] } },
            DescriptorSetUpdateInfo{ .binding = 9, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_shadow.view) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_marchPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_marchPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_marchPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        cmd.pushConstants(m_marchPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(gx, gy, 1);
    }

    vk::MemoryBarrier2 marchToTemporal{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &marchToTemporal });

    { // -------- Temporal (read march[cur] + accum[prev], write accum[cur]) --------
        const vk::DescriptorSet vkSet = m_temporalSets[cur].getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 7> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_marchColor.view[cur]) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_marchDepth.view[cur]) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_accumColor.view[prev]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_accumDepth.view[prev]) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_accumColor.view[cur]) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_accumDepth.view[cur]) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_temporalPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_temporalPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_temporalPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        cmd.pushConstants(m_temporalPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(gx, gy, 1);
    }

    // accum[cur] -> the apply's fragment reads (and next frame's temporal, as history)
    vk::MemoryBarrier2 temporalToApply{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &temporalToApply });
}

void CloudPipeline::recordShadow(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, uint32 cascadeMask)
{
    if (cascadeMask == 0)
        return;
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    // WAR: the previous frames' consumers (lit / terrain / ocean fragments, fog + GI + clouds compute, the
    // particle vertex stage) sampled the map; the UBO copy is visible to compute too.
    vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader
            | vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eUniformRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier });

    const vk::DescriptorSet vkSet = m_shadowSets[frameIdx].getDescriptorSet();
    const oc::array<vk::DescriptorImageInfo, 4> noise = noiseInfos();
    oc::array<DescriptorSetUpdateInfo, 6> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_shadow.view) } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[0] } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[1] } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[2] } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[3] } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_shadowPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_shadowPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_shadowPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    const uint32 groups = (SHADOW_RESOLUTION + 7) / 8;
    for (uint32 c = 0; c < SHADOW_CASCADES; ++c)
    {
        if (!(cascadeMask & (1u << c)))
            continue;
        const ShadowPC pc{ .cascade = c, .resolution = SHADOW_RESOLUTION };
        cmd.pushConstants(m_shadowPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(groups, groups, 1);
    }

    vk::MemoryBarrier2 toConsumers{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toConsumers });
}

void CloudPipeline::recordSky(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, vk::ImageView skyMapView, vk::Sampler skyMapSampler)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    // In: last frame's sky-map bake read this image (WAR); the shadow map was just written; the UBO copy.
    vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eUniformRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier });

    const vk::DescriptorSet vkSet = m_skySets[frameIdx].getDescriptorSet();
    const oc::array<vk::DescriptorImageInfo, 4> noise = noiseInfos();
    oc::array<DescriptorSetUpdateInfo, 8> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { vk::DescriptorBufferInfo{ .buffer = ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) } } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_skyClouds.view) } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(skyMapSampler, skyMapView) } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_shadow.view) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[0] } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[1] } },
        DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[2] } },
        DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { noise[3] } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_skyPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_skyPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_skyPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.dispatch(GIProbePipeline::SKY_MAP_WIDTH / 8, GIProbePipeline::SKY_MAP_HEIGHT / 8, 1);

    // sky clouds -> the sky-map bake (compute, in the GI secondary)
    vk::MemoryBarrier2 toSkyMap{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &toSkyMap });
}

void CloudPipeline::recordApply(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const ApplyParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 viewIndex = RendererVKLayout::eyeToViewIndex(eye, m_viewCount);
    const uint32 cur = slot(frameIdx, eye);
    const vk::DescriptorSet vkSet = m_applySets[cur].getDescriptorSet();
    auto uboInfo = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };
    oc::array<DescriptorSetUpdateInfo, 4> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledDepth(params.sceneDepthSampler, params.sceneDepthView) } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_accumColor.view[cur]) } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_linearSampler, m_accumDepth.view[cur]) } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_applyPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_applyPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_applyPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.pushConstants(m_applyPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.draw(3, 1, 0, 0);
}
