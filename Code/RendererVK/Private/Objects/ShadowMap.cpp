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
    Globals::gpuAllocator.destroyImage(m_image, m_imageMemory);
    m_sampler = nullptr; m_depthSampler = nullptr; m_framebuffer = nullptr; m_renderPass = nullptr; m_sampleView = nullptr; m_image = nullptr; m_imageMemory = nullptr;
}

bool ShadowMap::initialize(uint32 resolution, uint32 numCascades)
{
    vk::Device vkDevice = Globals::device.getDevice();
    m_resolution = resolution;
    m_numCascades = numCascades;

    // ---- Depth array image -------------------------------------------------
    vk::ImageCreateInfo imageInfo{
        .imageType = vk::ImageType::e2D,
        .format = SHADOW_DEPTH_FORMAT,
        .extent = { resolution, resolution, 1 },
        .mipLevels = 1,
        .arrayLayers = numCascades,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    if (!Globals::gpuAllocator.createImage(imageInfo, m_image, m_imageMemory, "ShadowMap")) { assert(false && "shadow image"); return false; }

    // ---- Views -------------------------------------------------------------
    vk::ImageViewCreateInfo sampleViewInfo{
        .image = m_image,
        .viewType = vk::ImageViewType::e2DArray,
        .format = SHADOW_DEPTH_FORMAT,
        .subresourceRange = { .aspectMask = vk::ImageAspectFlagBits::eDepth, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = numCascades },
    };
    auto sampleViewResult = vkDevice.createImageView(sampleViewInfo);
    if (sampleViewResult.result != vk::Result::eSuccess) { assert(false && "shadow sample view"); return false; }
    m_sampleView = sampleViewResult.value;

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
    initCmd.initialize(vk::CommandBufferLevel::ePrimary);
    vk::CommandBuffer cmd = initCmd.begin(true);
    cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &initBarrier });
    initCmd.end();
    initCmd.submitGraphics();
    (void)Globals::device.getGraphicsQueue().waitIdle();

    return true;
}
