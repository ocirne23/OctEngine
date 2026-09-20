module RendererVK;

import :VK;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    constexpr vk::Format SCENE_DEPTH_FORMAT = vk::Format::eD32Sfloat;

    bool createImage(vk::Device vkDevice, uint32 w, uint32 h, vk::Format format, vk::ImageUsageFlags usage,
        uint32 arrayLayers, vk::Image& outImage, VmaAllocation& outMemory, const char* name)
    {
        vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = { w, h, 1 },
            .mipLevels = 1,
            .arrayLayers = arrayLayers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        if (!Globals::gpuAllocator.createImage(info, outImage, outMemory, name)) { assert(false && "scenecolor image"); return false; }
        return true;
    }

    bool createView(vk::Device vkDevice, vk::Image image, vk::Format format, vk::ImageAspectFlags aspect,
        vk::ImageViewType viewType, uint32 baseLayer, uint32 layerCount, vk::ImageView& outView)
    {
        vk::ImageViewCreateInfo info{
            .image = image,
            .viewType = viewType,
            .format = format,
            .subresourceRange = { .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = baseLayer, .layerCount = layerCount },
        };
        auto result = vkDevice.createImageView(info);
        if (result.result != vk::Result::eSuccess) { assert(false && "scenecolor view"); return false; }
        outView = result.value;
        return true;
    }
}

SceneColor::SceneColor() {}
SceneColor::~SceneColor() { destroy(); }

void SceneColor::destroy()
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_sampler)      vkDevice.destroySampler(m_sampler);
    if (m_depthSampler) vkDevice.destroySampler(m_depthSampler);
    for (vk::Framebuffer& fb : m_framebuffers) { if (fb) vkDevice.destroyFramebuffer(fb); fb = nullptr; }
    if (m_renderPass)   vkDevice.destroyRenderPass(m_renderPass);
    for (vk::RenderPass& rp : m_stagePasses) { if (rp) vkDevice.destroyRenderPass(rp); rp = nullptr; }
    for (vk::ImageView& v : m_colorLayerViews) { if (v) vkDevice.destroyImageView(v); v = nullptr; }
    for (vk::ImageView& v : m_depthLayerViews) { if (v) vkDevice.destroyImageView(v); v = nullptr; }
    Globals::gpuAllocator.destroyImage(m_colorImage, m_colorMemory);
    Globals::gpuAllocator.destroyImage(m_depthImage, m_depthMemory);
    m_sampler = nullptr; m_depthSampler = nullptr; m_renderPass = nullptr;
    m_colorImage = nullptr; m_depthImage = nullptr;
    m_colorMemory = nullptr; m_depthMemory = nullptr;
}

bool SceneColor::initialize(vk::Format colorFormat, uint32 width, uint32 height, uint32 viewCount)
{
    vk::Device vkDevice = Globals::device.getDevice();
    destroy();
    m_width = width;
    m_height = height;
    m_viewCount = viewCount;
    m_colorFormat = colorFormat;

    // The colour/depth images hold one layer per eye; the forward pass renders into each layer in a
    // separate (non-multiview) pass. TRANSFER_SRC lets the VR path copy each layer into its eye swapchain.
    if (!createImage(vkDevice, width, height, colorFormat,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
        viewCount, m_colorImage, m_colorMemory, "SceneColor.color")) return false;
    // THE scene depth: written by the first scene stages, sampled by everything after them and by the
    // next frame (TRANSFER_DST = the one-time clear below).
    if (!createImage(vkDevice, width, height, SCENE_DEPTH_FORMAT,
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, viewCount, m_depthImage, m_depthMemory, "SceneColor.depth")) return false;

    for (uint32 i = 0; i < viewCount; ++i)
    {
        if (!createView(vkDevice, m_colorImage, colorFormat, vk::ImageAspectFlagBits::eColor, vk::ImageViewType::e2D, i, 1, m_colorLayerViews[i])) return false;
        if (!createView(vkDevice, m_depthImage, SCENE_DEPTH_FORMAT, vk::ImageAspectFlagBits::eDepth, vk::ImageViewType::e2D, i, 1, m_depthLayerViews[i])) return false;
    }

    // ---- BASE render pass (never begun - see getStageRenderPass): colour (ends SHADER_READ_ONLY for
    // the TAA compute pass) + the written depth ----
    oc::array<vk::AttachmentDescription2, 2> attachments{
        vk::AttachmentDescription2{ // 0: scene colour
            .format = colorFormat,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::AttachmentDescription2{ // 1: depth
            .format = SCENE_DEPTH_FORMAT,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        },
    };
    vk::AttachmentReference2 colorRef{ .attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal, .aspectMask = vk::ImageAspectFlagBits::eColor };
    vk::AttachmentReference2 depthRef{ .attachment = 1, .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal, .aspectMask = vk::ImageAspectFlagBits::eDepth };
    vk::SubpassDescription2 subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        // Non-multiview: each eye is a separate single-layer pass so the forward pass's DGC execution
        // set is allowed (an execution set requires viewMask == 0).
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef,
        .pDepthStencilAttachment = &depthRef,
    };
    // The external->0 dependencies below MUST match the swapchain pass's (RenderPass.cpp): the static-mesh
    // and GI-debug pipelines are created against that pass but execute here, and render-pass compatibility
    // (as enforced by validation) compares them. The 0->EXTERNAL dependency is NOT part of render-pass
    // compatibility (only attachment references/formats are), so it's safe to declare independently here -
    // it's what actually resolves the colour attachment's finalLayout->SHADER_READ_ONLY transition for TAA's
    // compute read (an explicit barrier in Renderer::recordCommandBuffers alone was not picked up by
    // synchronization validation for this cross-render-pass-boundary transition).
    const vk::PipelineStageFlags depthStages = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    oc::array<vk::SubpassDependency2, 3> dependencies{
        vk::SubpassDependency2{
            .srcSubpass = vk::SubpassExternal,
            .dstSubpass = 0,
            .srcStageMask = depthStages,
            .dstStageMask = depthStages,
            .srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        },
        vk::SubpassDependency2{
            .srcSubpass = vk::SubpassExternal,
            .dstSubpass = 0,
            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
            .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlags(),
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        },
        vk::SubpassDependency2{ // colour finalLayout transition -> TAA/eye-adapt compute sampled read
            .srcSubpass = 0,
            .dstSubpass = vk::SubpassExternal,
            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
            .dstStageMask = vk::PipelineStageFlagBits::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        },
    };
    vk::RenderPassCreateInfo2 rpInfo{
        .attachmentCount = (uint32)attachments.size(),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = (uint32)dependencies.size(),
        .pDependencies = dependencies.data(),
    };
    auto rpResult = vkDevice.createRenderPass2(rpInfo);
    if (rpResult.result != vk::Result::eSuccess) { assert(false && "scenecolor renderpass"); return false; }
    m_renderPass = rpResult.value;

    for (uint32 i = 0; i < viewCount; ++i)
    {
        oc::array<vk::ImageView, 2> fbViews{ m_colorLayerViews[i], m_depthLayerViews[i] };
        vk::FramebufferCreateInfo fbInfo{
            .renderPass = m_renderPass,
            .attachmentCount = (uint32)fbViews.size(),
            .pAttachments = fbViews.data(),
            .width = width,
            .height = height,
            .layers = 1,
        };
        auto fbResult = vkDevice.createFramebuffer(fbInfo);
        if (fbResult.result != vk::Result::eSuccess) { assert(false && "scenecolor framebuffer"); return false; }
        m_framebuffers[i] = fbResult.value;
    }

    // ---- STAGE variants (see getStageRenderPass): colour first/last x depth written/read-only.
    // Compatibility with the base pass (and the swapchain pass the pipelines are built against)
    // allows differences ONLY in load/store ops and layouts - the dependency array is carried
    // VERBATIM in every variant (it is part of render-pass compatibility), which is also why it
    // cannot express the inter-instance attachment hazards or the depth's ATTACHMENT -> READ_ONLY
    // switch: the Renderer emits explicit barriers between the instances instead. A read-only depth
    // attachment in DEPTH_STENCIL_READ_ONLY may be sampled by the pass that tests against it (a
    // writable one would be a feedback loop) - every pipeline of those stages has depthWrite off.
    {
        const auto makePass = [&](const oc::array<vk::AttachmentDescription2, 2>& atts,
            const vk::SubpassDescription2& sp, vk::RenderPass& out)
        {
            const vk::RenderPassCreateInfo2 info{
                .attachmentCount = (uint32)atts.size(),
                .pAttachments = atts.data(),
                .subpassCount = 1,
                .pSubpasses = &sp,
                .dependencyCount = (uint32)dependencies.size(),
                .pDependencies = dependencies.data(),
            };
            auto result = vkDevice.createRenderPass2(info);
            if (result.result != vk::Result::eSuccess) { assert(false && "scenecolor split renderpass"); return false; }
            out = result.value;
            return true;
        };
        const auto colorDesc = [&](vk::AttachmentLoadOp load, vk::ImageLayout initial, vk::ImageLayout final)
        {
            return vk::AttachmentDescription2{
                .format = colorFormat,
                .samples = vk::SampleCountFlagBits::e1,
                .loadOp = load,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .initialLayout = initial,
                .finalLayout = final,
            };
        };
        const auto depthDesc = [&](vk::AttachmentLoadOp load, vk::AttachmentStoreOp store,
            vk::ImageLayout initial, vk::ImageLayout final)
        {
            return vk::AttachmentDescription2{
                .format = SCENE_DEPTH_FORMAT,
                .samples = vk::SampleCountFlagBits::e1,
                .loadOp = load,
                .storeOp = store,
                .initialLayout = initial,
                .finalLayout = final,
            };
        };
        constexpr vk::ImageLayout colorAtt = vk::ImageLayout::eColorAttachmentOptimal;
        constexpr vk::ImageLayout depthAtt = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        constexpr vk::ImageLayout depthRo = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
        constexpr vk::ImageLayout colorRead = vk::ImageLayout::eShaderReadOnlyOptimal; // -> TAA
        const vk::AttachmentReference2 readOnlyDepthRef{ .attachment = 1, .layout = depthRo, .aspectMask = vk::ImageAspectFlagBits::eDepth };
        const vk::SubpassDescription2 readOnlySubpass{
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorRef,
            .pDepthStencilAttachment = &readOnlyDepthRef,
        };
        for (uint32 i = 0; i < (uint32)m_stagePasses.size(); ++i)
        {
            const bool first = (i & 1) != 0, last = (i & 2) != 0, readOnly = (i & 4) != 0;
            const vk::AttachmentDescription2 color = colorDesc(first ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
                first ? vk::ImageLayout::eUndefined : colorAtt, last ? colorRead : colorAtt);
            // The depth is always STORED: it outlives the pass (TAA, and next frame's readers).
            const vk::AttachmentDescription2 depth = readOnly
                ? depthDesc(vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, depthRo, depthRo)
                : first ? depthDesc(vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, vk::ImageLayout::eUndefined, depthAtt)
                        : depthDesc(vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, depthAtt, depthAtt);
            if (!makePass({ color, depth }, readOnly ? readOnlySubpass : subpass, m_stagePasses[i])) return false;
        }
    }

    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = vk::False,
        .compareEnable = vk::False,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = vk::BorderColor::eFloatOpaqueBlack,
        .unnormalizedCoordinates = vk::False,
    };
    auto samplerResult = vkDevice.createSampler(samplerInfo);
    if (samplerResult.result != vk::Result::eSuccess) { assert(false && "scenecolor sampler"); return false; }
    m_sampler = samplerResult.value;

    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    auto depthSamplerResult = vkDevice.createSampler(samplerInfo);
    if (depthSamplerResult.result != vk::Result::eSuccess) { assert(false && "scenecolor depth sampler"); return false; }
    m_depthSampler = depthSamplerResult.value;

    // One-time layout init to SHADER_READ_ONLY so a never-yet-rendered target (e.g. the other frame's image
    // sampled before it has been drawn) is in a legal sampling layout. The render pass uses loadOp=Clear with
    // initialLayout=Undefined, so it does not depend on this. The depth is cleared to the far plane
    // (reversed-Z 0 = "no geometry" to every reader) and parked in its sampled layout the same way.
    {
        CommandBuffer init;
        init.initialize(vk::CommandBufferLevel::ePrimary);
        vk::CommandBuffer cmd = init.begin(true);
        const vk::ImageSubresourceRange depthRange{ vk::ImageAspectFlagBits::eDepth, 0, 1, 0, viewCount };
        vk::ImageMemoryBarrier2 depthToClear{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = m_depthImage,
            .subresourceRange = depthRange,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &depthToClear });
        const vk::ClearDepthStencilValue farPlane{ 0.0f, 0 };
        cmd.clearDepthStencilImage(m_depthImage, vk::ImageLayout::eTransferDstOptimal, &farPlane, 1, &depthRange);
        vk::ImageMemoryBarrier2 depthToSampled{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = SCENE_DEPTH_SAMPLED_LAYOUT,
            .image = m_depthImage,
            .subresourceRange = depthRange,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &depthToSampled });
        vk::ImageMemoryBarrier2 bar{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = m_colorImage,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, viewCount },
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar });
        init.end();
        init.submitGraphics();
        (void)Globals::device.graphicsQueueWaitIdle();
    }

    return true;
}
