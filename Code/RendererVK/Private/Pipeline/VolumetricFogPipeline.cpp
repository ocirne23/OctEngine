module RendererVK;

import Core;
import File;
import :Device;
import :Allocator;
import :CommandBuffer;
import :RenderPass;

namespace
{
    // rgb = in-scatter, a = extinction (scatter grid) / transmittance (integrated grid).
    constexpr vk::Format FOG_FORMAT = vk::Format::eR16G16B16A16Sfloat;

    auto imgInfoGeneral(vk::ImageView view) { return vk::DescriptorImageInfo{ .imageView = view, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledGeneral(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eGeneral }; }
    auto sampledRO(vk::Sampler s, vk::ImageView v) { return vk::DescriptorImageInfo{ .sampler = s, .imageView = v, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal }; }
    auto bufInfo(const Buffer& b) { return vk::DescriptorBufferInfo{ .buffer = b.getBuffer(), .range = vk::WholeSize }; }
}

void VolumetricFogPipeline::buildScatterLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/vol_scatter.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    for (uint32 i = 1; i <= 3; ++i)
        b.push_back(vk::DescriptorSetLayoutBinding{ .binding = i, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 5, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 6, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 7, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 8, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 9, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 10, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.descriptorBindingFlags.resize(b.size());
    // Terrain height map: ping-pong image refreshed per frame without re-recording the cached fog CB.
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    // FFT ocean displacement maps (persistent image, rewritten in place by the sim each frame): the
    // scatter pass samples the live wave height around the waterline for the underwater fog boundary.
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 11, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    layout.descriptorBindingFlags.resize(b.size());
}

void VolumetricFogPipeline::buildIntegrateLayout(ComputePipelineLayout& layout)
{
    layout.computeShaderDebugFilePath = "Shaders/vol_integrate.cs.glsl";
    layout.computeShaderText = FileSystem::readFileStr(layout.computeShaderDebugFilePath);
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute });
}

void VolumetricFogPipeline::buildApplyLayout(GraphicsPipelineLayout& layout)
{
    layout.vertexShader.debugFilePath = "Shaders/composite.vs.glsl";
    layout.fragmentShader.debugFilePath = "Shaders/vol_apply.fs.glsl";
    layout.vertexShader.text = FileSystem::readFileStr(layout.vertexShader.debugFilePath);
    layout.fragmentShader.text = FileSystem::readFileStr(layout.fragmentShader.debugFilePath);
    layout.cullMode = vk::CullModeFlagBits::eNone;
    // out = inScatter + sceneColor * transmittance, over everything already drawn (meshes + sky).
    layout.blendEnable = true;
    layout.srcColorBlendFactor = vk::BlendFactor::eOne;
    layout.dstColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    layout.depthTestEnable = false;
    layout.depthWriteEnable = false;
    auto& b = layout.descriptorSetLayoutBindings;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 0, .descriptorType = vk::DescriptorType::eUniformBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 2, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    // 3 = terrain data cascades (far field ground march), 4 = GI probe SH volume (its ambient).
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 3, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorBindingFlags.resize(b.size());
    // Ping-pong image behind a cached CB, like the scatter set's copy.
    layout.descriptorBindingFlags.back() = vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    b.push_back(vk::DescriptorSetLayoutBinding{ .binding = 4, .descriptorType = vk::DescriptorType::eStorageBuffer, .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eFragment });
    layout.descriptorBindingFlags.resize(b.size());
    // Eye index (per-eye depth reconstruction + projection; 0 on desktop / left eye).
    layout.pushConstantRanges.push_back(vk::PushConstantRange{
        .stageFlags = vk::ShaderStageFlagBits::eFragment, .offset = 0, .size = sizeof(uint32) });
}

void VolumetricFogPipeline::createImageSet(ImageSet& set)
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e3D,
            .format = FOG_FORMAT,
            .extent = { RendererVKLayout::VOL_FROXEL_X, RendererVKLayout::VOL_FROXEL_Y, RendererVKLayout::VOL_FROXEL_Z },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, // TransferDst: cleared once at init
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        (void)Globals::gpuAllocator.createImage(info, set.image[i], set.memory[i], "VolumetricFog");

        vk::ImageViewCreateInfo viewInfo{
            .image = set.image[i],
            .viewType = vk::ImageViewType::e3D,
            .format = FOG_FORMAT,
            .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eColor, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
        };
        auto viewResult = vkDevice.createImageView(viewInfo);
        assert(viewResult.result == vk::Result::eSuccess);
        set.view[i] = viewResult.value;
    }
}

void VolumetricFogPipeline::destroyImageSet(ImageSet& set)
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        if (set.view[i]) vkDevice.destroyImageView(set.view[i]);
        Globals::gpuAllocator.destroyImage(set.image[i], set.memory[i]);
        set.view[i] = nullptr; set.image[i] = nullptr; set.memory[i] = nullptr;
    }
}

VolumetricFogPipeline::~VolumetricFogPipeline()
{
    destroyImageSet(m_scatter);
    destroyImageSet(m_integrated);
    if (m_sampler)
        Globals::device.getDevice().destroySampler(m_sampler);
}

void VolumetricFogPipeline::initialize()
{
    ComputePipelineLayout scatterLayout;   buildScatterLayout(scatterLayout);     m_scatterPipeline.initialize(scatterLayout);
    ComputePipelineLayout integrateLayout; buildIntegrateLayout(integrateLayout); m_integratePipeline.initialize(integrateLayout);
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        m_scatterSets[i].initialize(m_scatterPipeline.getDescriptorSetLayout());
        m_integrateSets[i].initialize(m_integratePipeline.getDescriptorSetLayout());
    }

    createImageSet(m_scatter);
    createImageSet(m_integrated);

    // One-time UNDEFINED -> GENERAL (images stay in GENERAL forever) + clear: the scatter history must read
    // zeros on the first frame, and the integrated grid must hold "no fog" (transmittance 1) while disabled.
    CommandBuffer init;
    init.initialize(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer cmd = init.begin(true);
    const ImageSet* sets[] = { &m_scatter, &m_integrated };
    oc::vector<vk::ImageMemoryBarrier2> bars;
    for (const ImageSet* s : sets)
        for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
            bars.push_back(vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eClear,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = s->image[i],
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
            });
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
    const vk::ImageSubresourceRange fullRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    for (uint32 i = 0; i < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++i)
    {
        cmd.clearColorImage(m_scatter.image[i], vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } }, { fullRange });
        cmd.clearColorImage(m_integrated.image[i], vk::ImageLayout::eGeneral, vk::ClearColorValue{ std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } }, { fullRange });
    }
    vk::MemoryBarrier2 clearToRead{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &clearToRead });
    init.end();
    init.submitGraphics();
    (void)Globals::device.getGraphicsQueue().waitIdle();

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
    m_sampler = samplerResult.value;
}

void VolumetricFogPipeline::initializeApply(vk::RenderPass renderPass, uint32 viewCount)
{
    m_applyViewCount = viewCount;
    GraphicsPipelineLayout layout;
    buildApplyLayout(layout);
    m_applyPipeline.initialize(renderPass, layout);
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    for (uint32 e = 0; e < m_applyViewCount; ++e)
        m_applySets[applySlot(f, e)].initialize(m_applyPipeline.getDescriptorSetLayout());
}

void VolumetricFogPipeline::reloadShaders(vk::RenderPass renderPass)
{
    ComputePipelineLayout scatterLayout;   buildScatterLayout(scatterLayout);
    ComputePipelineLayout integrateLayout; buildIntegrateLayout(integrateLayout);
    GraphicsPipelineLayout applyLayout;    buildApplyLayout(applyLayout);
    bool ok = m_scatterPipeline.reloadShaders(scatterLayout);
    ok = m_integratePipeline.reloadShaders(integrateLayout) && ok;
    ok = m_applyPipeline.reloadShaders(renderPass, applyLayout) && ok;
    if (!ok)
        printf("VolumetricFogPipeline: shader reload failed, keeping previous pipeline(s)\n");
}

void VolumetricFogPipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    const uint32 prevIdx = (frameIdx + RendererVKLayout::NUM_FRAMES_IN_FLIGHT - 1) % RendererVKLayout::NUM_FRAMES_IN_FLIGHT;
    const uint32 gx = (RendererVKLayout::VOL_FROXEL_X + 7) / 8;
    const uint32 gy = (RendererVKLayout::VOL_FROXEL_Y + 7) / 8;

    // Make prior producers visible to this compute pass: the light grid / GI SH (compute storage writes)
    // and, in cascade mode, the sun shadow map depth writes. Also re-publish last frame's scatter image
    // (this frame's temporal history) for sampled reads.
    vk::MemoryBarrier2 inputBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &inputBarrier });

    auto uboInfo = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };

    { // -------- Pass 1: scatter (write scatter[cur], read scatter[prev] as history) --------
        DescriptorSet& set = m_scatterSets[frameIdx];
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 10> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.lightInfosBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.lightGridsBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.lightTableBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.shadowMapSampler, params.shadowMapView) } },
            DescriptorSetUpdateInfo{ .binding = 6, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.fogVolumesBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 7, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_sampler, m_scatter.view[prevIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 8, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_scatter.view[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 9, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.giGridDataBuffer) } },
            DescriptorSetUpdateInfo{ .binding = 11, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledRO(params.oceanMapsSampler, params.oceanMapsView) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_scatterPipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
        vk::WriteDescriptorSetAccelerationStructureKHR asInfo{ .accelerationStructureCount = 1, .pAccelerationStructures = &params.tlas };
        vk::WriteDescriptorSet asWrite{ .pNext = &asInfo, .dstSet = vkSet, .dstBinding = 4, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eAccelerationStructureKHR };
        Globals::device.getDevice().updateDescriptorSets(1, &asWrite, 0, nullptr);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_scatterPipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_scatterPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        cmd.dispatch(gx, gy, RendererVKLayout::VOL_FROXEL_Z);
    }

    vk::MemoryBarrier2 scatterToIntegrate{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &scatterToIntegrate });

    { // -------- Pass 2: integrate along Z (read scatter[cur], write integrated[cur]) --------
        DescriptorSet& set = m_integrateSets[frameIdx];
        vk::DescriptorSet vkSet = set.getDescriptorSet();
        oc::array<DescriptorSetUpdateInfo, 3> updates{
            DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
            DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_sampler, m_scatter.view[frameIdx]) } },
            DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageImage, .imageInfos = { imgInfoGeneral(m_integrated.view[frameIdx]) } },
        };
        commandBuffer.cmdUpdateDescriptorSets(m_integratePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, vkSet, updates);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_integratePipeline.getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_integratePipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
        cmd.dispatch(gx, gy, 1);
    }

    // integrated grid -> fog apply fragment reads in the scene-color pass
    vk::MemoryBarrier2 integrateToApply{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &integrateToApply });
}

void VolumetricFogPipeline::recordApply(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const ApplyParams& params)
{
    vk::CommandBuffer cmd = commandBuffer.getCommandBuffer();
    // eye (0/1) selects the per-eye apply descriptor set; viewIndex selects the UBO view used to reconstruct
    // depth (0 = centre/desktop, 1/2 = the eyes in VR). The froxel volume itself is the shared centre view.
    const uint32 viewIndex = RendererVKLayout::eyeToViewIndex(eye, m_applyViewCount);
    DescriptorSet& set = m_applySets[applySlot(frameIdx, eye)];
    vk::DescriptorSet vkSet = set.getDescriptorSet();
    auto uboInfo = vk::DescriptorBufferInfo{ .buffer = params.ubo.getBuffer(), .range = sizeof(RendererVKLayout::Ubo) };
    oc::array<DescriptorSetUpdateInfo, 4> updates{
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { uboInfo } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = {
            vk::DescriptorImageInfo{ .sampler = params.gbufferSampler, .imageView = params.gbufferDepthView, .imageLayout = params.gbufferDepthLayout } } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eCombinedImageSampler, .imageInfos = { sampledGeneral(m_sampler, m_integrated.view[frameIdx]) } },
        // Binding 3 (terrain) is UPDATE_AFTER_BIND, written per frame by updateTerrainDescriptor.
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(params.giGridDataBuffer) } },
    };
    commandBuffer.cmdUpdateDescriptorSets(m_applyPipeline.getPipelineLayout(), vk::PipelineBindPoint::eGraphics, vkSet, updates);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_applyPipeline.getPipeline());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_applyPipeline.getPipelineLayout(), 0, 1, &vkSet, 0, nullptr);
    cmd.pushConstants(m_applyPipeline.getPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32), &viewIndex);
    cmd.draw(3, 1, 0, 0);
}

void VolumetricFogPipeline::updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler)
{
    // Points the terrain height binding (10, UPDATE_AFTER_BIND) at the active ping-pong image; refreshed
    // every frame so a CPU re-bake swaps images without re-recording the cached fog CB.
    vk::DescriptorImageInfo imageInfo{ .sampler = terrainSampler, .imageView = terrainView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal };
    // Scatter set (binding 10), then every eye's apply set (binding 3).
    oc::array<vk::WriteDescriptorSet, 1 + MAX_VIEWS> writes{};
    uint32 numWrites = 0;
    writes[numWrites++] = vk::WriteDescriptorSet{ .dstSet = m_scatterSets[frameIdx].getDescriptorSet(), .dstBinding = 10, .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    for (uint32 eye = 0; eye < m_applyViewCount; ++eye)
        writes[numWrites++] = vk::WriteDescriptorSet{ .dstSet = m_applySets[applySlot(frameIdx, eye)].getDescriptorSet(), .dstBinding = 3, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &imageInfo };
    Globals::device.getDevice().updateDescriptorSets(numWrites, writes.data(), 0, nullptr);
}
