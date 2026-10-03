module RendererVK;

import :VK;
import :Device;
import :Allocator;
import :CommandBuffer;

constexpr vk::Format SHADOW_DEPTH_FORMAT = vk::Format::eD32Sfloat;

ShadowMap::ShadowMap() {}
ShadowMap::~ShadowMap()
{
    destroy();
}

void ShadowMap::destroy()
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_depthSampler) vkDevice.destroySampler(m_depthSampler);
    if (m_sampler)      vkDevice.destroySampler(m_sampler);
    if (m_framebuffer)  vkDevice.destroyFramebuffer(m_framebuffer);
    if (m_renderPass)   vkDevice.destroyRenderPass(m_renderPass);
    if (m_sampleView)   vkDevice.destroyImageView(m_sampleView);
    if (m_extraFramebuffer) vkDevice.destroyFramebuffer(m_extraFramebuffer);
    if (m_extraRenderPass)  vkDevice.destroyRenderPass(m_extraRenderPass);
    if (m_extraView)        vkDevice.destroyImageView(m_extraView);
    m_extraFramebuffer = nullptr; m_extraRenderPass = nullptr; m_extraView = nullptr;
    Globals::gpuAllocator.destroyImage(m_image, m_imageMemory);
    m_sampler = nullptr; m_depthSampler = nullptr; m_framebuffer = nullptr; m_renderPass = nullptr; m_sampleView = nullptr; m_image = nullptr; m_imageMemory = nullptr;
}

bool ShadowMap::initialize(const char* debugName, uint32 resolution, uint32 numCascades, bool extraLayer)
{
    vk::Device vkDevice = Globals::device.getDevice();
    m_resolution = resolution;
    m_numCascades = numCascades;
    const uint32 numLayers = numCascades + (extraLayer ? 1u : 0u);

    // ---- Depth array image -------------------------------------------------
    vk::ImageCreateInfo imageInfo{
        .imageType = vk::ImageType::e2D,
        .format = SHADOW_DEPTH_FORMAT,
        .extent = { resolution, resolution, 1 },
        .mipLevels = 1,
        .arrayLayers = numLayers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    if (!Globals::gpuAllocator.createImage(imageInfo, m_image, m_imageMemory, debugName)) { assert(false && "shadow image"); return false; }

    // ---- Views -------------------------------------------------------------
    vk::ImageViewCreateInfo sampleViewInfo{
        .image = m_image,
        .viewType = vk::ImageViewType::e2DArray,
        .format = SHADOW_DEPTH_FORMAT,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eDepth, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = numLayers },
    };
    auto sampleViewResult = vkDevice.createImageView(sampleViewInfo);
    if (sampleViewResult.result != vk::Result::eSuccess) { assert(false && "shadow sample view"); return false; }
    m_sampleView = sampleViewResult.value;
    Globals::device.setDebugName(m_sampleView, debugName);

    // ---- Depth-only multiview render pass (one view per cascade) ----------
    vk::AttachmentDescription2 depthAttachment{
        .format = SHADOW_DEPTH_FORMAT,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    vk::AttachmentReference2 depthRef{ .attachment = 0, .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal };
    const uint32 viewMask = (numCascades >= 32) ? 0xFFFFFFFFu : ((1u << numCascades) - 1u);
    vk::SubpassDescription2 subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .viewMask = viewMask, // multiview: broadcast the subpass to one layer per cascade
        .colorAttachmentCount = 0,
        .pDepthStencilAttachment = &depthRef,
    };
    oc::array<vk::SubpassDependency2, 2> dependencies{
        // Wait for prior shader reads of this layer to finish before we overwrite the depth.
        // Compute included: GI probe trace and volumetric fog sample the cascades from compute.
        vk::SubpassDependency2{
            .srcSubpass = vk::SubpassExternal,
            .dstSubpass = 0,
            .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            .dstStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
            .srcAccessMask = vk::AccessFlagBits::eShaderRead,
            .dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        },
        // Make the written depth (and the endRenderPass layout transition) visible to the lighting
        // fragment shader and the compute consumers (GI probe trace, fog scatter).
        vk::SubpassDependency2{
            .srcSubpass = 0,
            .dstSubpass = vk::SubpassExternal,
            .srcStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
            .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        },
    };
    vk::RenderPassCreateInfo2 renderPassInfo{
        .attachmentCount = 1,
        .pAttachments = &depthAttachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = (uint32)dependencies.size(),
        .pDependencies = dependencies.data(),
    };
    auto rpResult = vkDevice.createRenderPass2(renderPassInfo);
    if (rpResult.result != vk::Result::eSuccess) { assert(false && "shadow renderpass"); return false; }
    m_renderPass = rpResult.value;
    Globals::device.setDebugName(m_renderPass, debugName);

    // ---- Single layered framebuffer (multiview targets the array view) ----
    // With multiview the framebuffer has layers = 1; the array view supplies one layer per view.
    vk::FramebufferCreateInfo fbInfo{
        .renderPass = m_renderPass,
        .attachmentCount = 1,
        .pAttachments = &m_sampleView,
        .width = resolution,
        .height = resolution,
        .layers = 1,
    };
    auto fbResult = vkDevice.createFramebuffer(fbInfo);
    if (fbResult.result != vk::Result::eSuccess) { assert(false && "shadow framebuffer"); return false; }
    m_framebuffer = fbResult.value;
    Globals::device.setDebugName(m_framebuffer, debugName);

    // ---- The extra layer: its own view, single-view render pass and framebuffer ----
    // Drawn AFTER the cascades' pass (whose UNDEFINED -> read-only transition covers this layer too): its source
    // dependency waits for that pass's depth writes and transition as well as for last frame's shader reads.
    if (extraLayer)
    {
        vk::ImageViewCreateInfo extraViewInfo = sampleViewInfo;
        extraViewInfo.viewType = vk::ImageViewType::e2D;
        extraViewInfo.subresourceRange.baseArrayLayer = numCascades;
        extraViewInfo.subresourceRange.layerCount = 1;
        auto extraViewResult = vkDevice.createImageView(extraViewInfo);
        if (extraViewResult.result != vk::Result::eSuccess) { assert(false && "shadow extra view"); return false; }
        m_extraView = extraViewResult.value;
        Globals::device.setDebugName(m_extraView, debugName);

        vk::SubpassDescription2 extraSubpass{
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 0,
            .pDepthStencilAttachment = &depthRef,
        };
        oc::array<vk::SubpassDependency2, 2> extraDependencies = dependencies;
        extraDependencies[0].srcStageMask |= vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
        extraDependencies[0].srcAccessMask |= vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        vk::RenderPassCreateInfo2 extraPassInfo{
            .attachmentCount = 1,
            .pAttachments = &depthAttachment,
            .subpassCount = 1,
            .pSubpasses = &extraSubpass,
            .dependencyCount = (uint32)extraDependencies.size(),
            .pDependencies = extraDependencies.data(),
        };
        auto extraPassResult = vkDevice.createRenderPass2(extraPassInfo);
        if (extraPassResult.result != vk::Result::eSuccess) { assert(false && "shadow extra renderpass"); return false; }
        m_extraRenderPass = extraPassResult.value;
        Globals::device.setDebugName(m_extraRenderPass, debugName);

        vk::FramebufferCreateInfo extraFbInfo{
            .renderPass = m_extraRenderPass,
            .attachmentCount = 1,
            .pAttachments = &m_extraView,
            .width = resolution,
            .height = resolution,
            .layers = 1,
        };
        auto extraFbResult = vkDevice.createFramebuffer(extraFbInfo);
        if (extraFbResult.result != vk::Result::eSuccess) { assert(false && "shadow extra framebuffer"); return false; }
        m_extraFramebuffer = extraFbResult.value;
        Globals::device.setDebugName(m_extraFramebuffer, debugName);
    }

    // ---- Comparison sampler (hardware PCF) --------------------------------
    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToBorder,
        .addressModeV = vk::SamplerAddressMode::eClampToBorder,
        .addressModeW = vk::SamplerAddressMode::eClampToBorder,
        .anisotropyEnable = vk::False,
        .compareEnable = vk::True,
        .compareOp = vk::CompareOp::eLessOrEqual,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = vk::BorderColor::eFloatOpaqueWhite, // outside the map => fully lit
        .unnormalizedCoordinates = vk::False,
    };
    auto samplerResult = vkDevice.createSampler(samplerInfo);
    if (samplerResult.result != vk::Result::eSuccess) { assert(false && "shadow sampler"); return false; }
    m_sampler = samplerResult.value;
    Globals::device.setDebugName(m_sampler, oc::format("{}.compare", debugName).c_str());

    // ---- Non-comparison sampler (raw depth reads for the PCSS blocker search) ----------
    vk::SamplerCreateInfo depthSamplerInfo{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToBorder,
        .addressModeV = vk::SamplerAddressMode::eClampToBorder,
        .addressModeW = vk::SamplerAddressMode::eClampToBorder,
        .anisotropyEnable = vk::False,
        .compareEnable = vk::False,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = vk::BorderColor::eFloatOpaqueWhite, // outside the map => far depth (no blocker)
        .unnormalizedCoordinates = vk::False,
    };
    auto depthSamplerResult = vkDevice.createSampler(depthSamplerInfo);
    if (depthSamplerResult.result != vk::Result::eSuccess) { assert(false && "shadow depth sampler"); return false; }
    m_depthSampler = depthSamplerResult.value;
    Globals::device.setDebugName(m_depthSampler, oc::format("{}.depth", debugName).c_str());

    // One-time UNDEFINED -> SHADER_READ_ONLY so the image is in its sampled layout even if the cascade
    // render pass never runs (RT sun shadows skip it); the render pass starts from UNDEFINED anyway.
    vk::ImageMemoryBarrier2 initBarrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = m_image,
        .subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, 0, numCascades },
    };
    CommandBuffer initCmd;
    initCmd.initialize(vk::CommandBufferLevel::ePrimary, "ShadowMap.init");
    vk::CommandBuffer cmd = initCmd.begin(true);
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &initBarrier });
    initCmd.end();
    initCmd.submitGraphics();
    (void)Globals::device.graphicsQueueWaitIdle();

    return true;
}
