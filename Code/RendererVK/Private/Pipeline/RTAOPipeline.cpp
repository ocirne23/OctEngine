module RendererVK;

import Core;
import Core.Tweaks;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import :TextureManager;
import :Texture;

namespace
{
    // rgb = bent normal (world space, the average unoccluded direction), a = scalar AO. The forward pass
    // evaluates the GI probe SH along the bent normal for directional indirect occlusion.
    constexpr vk::Format AO_FORMAT = vk::Format::eR16G16B16A16Sfloat;

    struct RtaoPC
    {
        uint32 numRays;
        float  radius;
        float  power;
        float  intensity;
        uint32 aoWidth;
        uint32 aoHeight;
        uint32 viewIndex;
        float  fadeStart;
        float  maxDistance;
        float  normalBias;
        float  distanceBias;
    };
    struct TemporalPC
    {
        uint32 aoWidth;
        uint32 aoHeight;
        float  maxHistory;
        uint32 viewIndex;
    };
    struct SpatialPC
    {
        uint32 aoWidth;
        uint32 aoHeight;
        int32  radius;
        uint32 viewIndex;
    };

    auto imgInfoGeneral(vk::ImageView view) { return vk::DescriptorImageInfo{ .imageView = view, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledGeneral(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledRO(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal }; }
}

void RTAOPipeline::buildTraceLayout(ComputePipelineLayout& layout, uint32 maxTextures)
{
    layout.computeShaderDebugFilePath = "Shaders/rtao.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    // Alpha-masked rays are a compile-time variant: the opaque path drops the geometry fetch + alpha test
    // entirely. Toggling the tweak rebuilds this pipeline (see RTAOParams::registerTweaks onReloadShaders).
    if (m_pParams && m_pParams->alphaTest)
        layout.defines.push_back({ "RTAO_ALPHA_TEST", "1" });
    auto storageBinding = [](uint32 binding) {
        return vk::DescriptorSetLayoutBinding{ .binding = binding, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute };
    };
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    // Geometry + materials for the alpha-masked candidate test (binding 5..9), then the variable-count
    // texture array last (binding 10 = highest, required for eVariableDescriptorCount).
    b.push_back(storageBinding(5)); // vertices
    b.push_back(storageBinding(6)); // indices
    b.push_back(storageBinding(7)); // meshInfos
    b.push_back(storageBinding(8)); // instances
    b.push_back(storageBinding(9)); // materials
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 10, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = maxTextures, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.descriptorBindingFlags.resize(b.size());
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eVariableDescriptorCount | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(RtaoPC) });
}

void RTAOPipeline::updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view)
{
    // Streamed texture slot rewrite in this frame's per-eye trace sets. Binding 10 only feeds the
    // alpha-masked shader variant, but stale views must never dangle once the old image is destroyed.
    vk::DescriptorImageInfo imageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = view, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    for (uint32 eye = 0; eye < m_viewCount; ++eye)
    {
        vk::WriteDescriptorSet write{ .dstSet = m_traceSets[slot(frameIdx, eye)].getDescriptorSet(), .dstBinding = 10, .dstArrayElement = slotIdx, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
        Globals::device.getDevice().updateDescriptorSets(1, &write, 0, nullptr);
    }
}

void RTAOPipeline::buildTemporalLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/ao_temporal.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 i = 1; i <= 4; ++i)
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = i, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 5, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(TemporalPC) });
}

void RTAOPipeline::buildSpatialLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/ao_spatial.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 i = 1; i <= 3; ++i)
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = i, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.pushConstantRanges.push_back(vk::PushConstantRange{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = sizeof(SpatialPC) });
}

RTAOPipeline::~RTAOPipeline()
{
    destroyImageSet(m_raw);
    destroyImageSet(m_accum);
    destroyImageSet(m_final);
    if (m_aoSampler)
        Globals::device.getDevice().destroySampler(m_aoSampler);
}

void RTAOPipeline::destroyImageSet(ImageSet& set)
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

void RTAOPipeline::createImageSet(ImageSet& set)
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
    {
        const uint32 i = slot(f, e);
        vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = AO_FORMAT,
            .extent = { m_width, m_height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        (void)Globals::gpuAllocator.createImage(info, set.image[i], set.memory[i], "Rtao");

        vk::ImageViewCreateInfo viewInfo{
            .image = set.image[i],
            .viewType = vk::ImageViewType::e2D,
            .format = AO_FORMAT,
            .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
        };
        auto viewResult = vkDevice.createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        set.view[i] = viewResult.value;
    }
}

void RTAOPipeline::recreateImages(uint32 fullWidth, uint32 fullHeight)
{
    destroyImageSet(m_raw);
    destroyImageSet(m_accum);
    destroyImageSet(m_final);
    m_width = (fullWidth + 1) / 2;
    m_height = (fullHeight + 1) / 2;
    createImageSet(m_raw);
    createImageSet(m_accum);
    createImageSet(m_final);

    // One-time UNDEFINED -> GENERAL for every AO image, so the history accum buffer can be sampled on the very first frame
    ImageSet* sets[] = { &m_raw, &m_accum, &m_final };
    oc::vector<vk::ImageMemoryBarrier2> bars;
    for (ImageSet* s : sets)
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
                .image = s->image[i],
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
            });
            s->initialized[i] = true;
        }
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer cmd = init.begin(true);
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
    init.end();
    init.submitGraphics();
    (void)Globals::device.getGraphicsQueue().waitIdle();
}

void RTAOPipeline::initialize(const RTAOParams* pParams, uint32 fullWidth, uint32 fullHeight, uint32 maxTextures, uint32 numTextureDescriptors, uint32 viewCount)
{
    m_pParams = pParams;
    m_viewCount = viewCount;
    m_maxTextures = maxTextures;
    m_textureSampler.initialize();
    ComputePipelineLayout traceLayout;    buildTraceLayout(traceLayout, maxTextures); m_tracePipeline.initialize(traceLayout);
    ComputePipelineLayout temporalLayout; buildTemporalLayout(temporalLayout); m_temporalPipeline.initialize(temporalLayout);
    ComputePipelineLayout spatialLayout;  buildSpatialLayout(spatialLayout);   m_spatialPipeline.initialize(spatialLayout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
    {
        const uint32 i = slot(f, e);
        m_traceSets[i].initialize(m_tracePipeline.getDescriptorSetLayout(), numTextureDescriptors);
        m_temporalSets[i].initialize(m_temporalPipeline.getDescriptorSetLayout());
        m_spatialSets[i].initialize(m_spatialPipeline.getDescriptorSetLayout());
    }

    recreateImages(fullWidth, fullHeight);

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
        .borderColor = vk::BorderColor::eFloatOpaqueWhite,
        .unnormalizedCoordinates = vk::False,
    };
    auto samplerResult = Globals::device.getDevice().createSampler(samplerInfo);
    assert(samplerResult.result == vk::Result::eSuccess);
    m_aoSampler = samplerResult.value;
}

void RTAOPipeline::resizeTextureDescriptors(uint32 numTextureDescriptors)
{
    // Variable-count texture binding: only the trace sets are re-allocated with the grown live count; the
    // layout/pipeline declare the fixed device-limit cap and stay untouched (mirrors the GI probe path).
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_viewCount; ++e)
        m_traceSets[slot(f, e)].initialize(m_tracePipeline.getDescriptorSetLayout(), numTextureDescriptors);
}

void RTAOPipeline::reloadShaders()
{
    ComputePipelineLayout traceLayout;    buildTraceLayout(traceLayout, m_maxTextures);
    ComputePipelineLayout temporalLayout; buildTemporalLayout(temporalLayout);
    ComputePipelineLayout spatialLayout;  buildSpatialLayout(spatialLayout);
    bool ok = m_tracePipeline.reloadShaders(traceLayout);
    ok = m_temporalPipeline.reloadShaders(temporalLayout) && ok;
    ok = m_spatialPipeline.reloadShaders(spatialLayout) && ok;
    if (!ok)
        printf("RTAOPipeline: shader reload failed, keeping previous pipeline(s)\n");
}

void RTAOPipeline::transitionToGeneral(vk::CommandBuffer cmd, ImageSet& set, uint32 slotIdx, bool forWrite)
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

void RTAOPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    // eye (0/1) selects the per-eye history images; viewIndex selects the UBO matrices (0 = centre/desktop,
    // 1/2 = the eyes in VR). They differ in VR because u_views[0] is the centre view, not an eye.
    const uint32 viewIndex = RendererVKLayout::eyeToViewIndex(eye, m_viewCount);
    const uint32 prevFrame = (frameIdx + RendererVKLayout::NUM_FRAMES_IN_FLIGHT - 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    const uint32 cur = slot(frameIdx, eye);   // this eye's images this frame
    const uint32 prevIdx = slot(prevFrame, eye); // same eye's images last frame (history)
    vk::PipelineLayout traceLayout = m_tracePipeline.getPipelineLayout();
    vk::PipelineLayout temporalLayout = m_temporalPipeline.getPipelineLayout();
    vk::PipelineLayout spatialLayout = m_spatialPipeline.getPipelineLayout();
    const uint32 gx = (m_width + 7) / 8;
    const uint32 gy = (m_height + 7) / 8;

    auto storeWriteToSampledRead = [&]() {
        vk::MemoryBarrier2 bar{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
    };

    auto uboInfo = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };

    { // -------- Pass 1: trace raw AO --------
        vk::MemoryBarrier2 asVis{
            .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &asVis });
        transitionToGeneral(cmd, m_raw, cur, true);

        DescriptorSet& set = m_traceSets[cur];
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        auto bufInfo = [](Buffer& buf) { return vk::DescriptorBufferInfo{ .buffer = buf.getBuffer(), .range = buf.getSize() }; };
        oc::vector<DescriptorSetUpdateInfo> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.gbufferNormalView) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.gbufferDepthView) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_raw.view[cur]) } },
        };
        // Geometry + texture array (bindings 5..10) only feed the alpha-masked variant; skip them otherwise
        // (the opaque shader declares neither, and the bindings are PartiallyBound).
        if (m_pParams->alphaTest)
        {
            updates.push_back(DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.vertexBuffer) } });
            updates.push_back(DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.indexBuffer) } });
            updates.push_back(DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.meshInfos) } });
            updates.push_back(DescriptorSetUpdateInfo{ .binding = 8, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.meshInstances) } });
            updates.push_back(DescriptorSetUpdateInfo{ .binding = 9, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.materialInfos) } });
            DescriptorSetUpdateInfo texUpdate{ .binding = 10, .type = vk::DescriptorType::eCombinedImageSampler };
            for (uint16 texIdx = 0; texIdx < (uint16)Globals::textureManager.getNumTextures(); ++texIdx)
                texUpdate.imageInfos.push_back(vk::DescriptorImageInfo{ .sampler = m_textureSampler.getSampler(), .imageView = Globals::textureManager.getViewForDescriptor(texIdx), .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal });
            if (!texUpdate.imageInfos.empty())
                updates.push_back(oc::move(texUpdate));
        }
        commandBuffer.cmdUpdateDescriptorSets(traceLayout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        vk::WriteDescriptorSetAccelerationStructureKHR asInfo{ .accelerationStructureCount = 1, .pAccelerationStructures = &params.tlas };
        vk::WriteDescriptorSet asWrite{ .pNext = &asInfo, .dstSet = vkSet, .dstBinding = 3, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR };
        Globals::device.getDevice().updateDescriptorSets(1, &asWrite, 0, nullptr);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_tracePipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, traceLayout, 0, 1, &vkSet, 0, nullptr);
        RtaoPC pc{ .numRays = uint32(oc::max(m_pParams->rays, 1)), .radius = m_pParams->radius, .power = m_pParams->power,
            .intensity = m_pParams->intensity, .aoWidth = m_width, .aoHeight = m_height, .viewIndex = viewIndex,
            .fadeStart = m_pParams->fadeStart, .maxDistance = m_pParams->maxDistance,
            .normalBias = m_pParams->normalBias, .distanceBias = m_pParams->distanceBias };
        cmd.pushConstants(traceLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(gx, gy, 1);
    }
    storeWriteToSampledRead();

    { // -------- Pass 2: temporal accumulate (read raw + history accum[prev], write accum[cur]) --------
        transitionToGeneral(cmd, m_accum, cur, true);

        DescriptorSet& set = m_temporalSets[cur];
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 6> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.gbufferDepthView) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.prevGbufferDepthView) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_aoSampler, m_raw.view[cur]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_aoSampler, m_accum.view[prevIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_accum.view[cur]) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(temporalLayout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_temporalPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, temporalLayout, 0, 1, &vkSet, 0, nullptr);
        TemporalPC pc{ .aoWidth = m_width, .aoHeight = m_height, .maxHistory = m_pParams->maxHistory, .viewIndex = viewIndex };
        cmd.pushConstants(temporalLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(gx, gy, 1);
    }
    storeWriteToSampledRead();

    if (m_pParams->blurRadius > 0)
    { // -------- Pass 3: spatial bilateral blur (read accum[cur], write final[cur]) --------
        transitionToGeneral(cmd, m_final, cur, true);

        DescriptorSet& set = m_spatialSets[cur];
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 5> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.gbufferDepthView) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.gbufferSampler, params.gbufferNormalView) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_aoSampler, m_accum.view[cur]) } },
            DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_final.view[cur]) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(spatialLayout, vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_spatialPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, spatialLayout, 0, 1, &vkSet, 0, nullptr);
        SpatialPC pc{ .aoWidth = m_width, .aoHeight = m_height, .radius = m_pParams->blurRadius, .viewIndex = viewIndex };
        cmd.pushConstants(spatialLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
        cmd.dispatch(gx, gy, 1);

        storeWriteToSampledRead();
    }
}
