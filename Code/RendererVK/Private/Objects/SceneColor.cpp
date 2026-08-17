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
    for (vk::Framebuffer& fb : m_framebuffers) { if (fb) vkDevice.destroyFramebuffer(fb); fb = nullptr; }
    for (vk::Framebuffer& fb : m_reuseFramebuffers) { if (fb) vkDevice.destroyFramebuffer(fb); fb = nullptr; }
    if (m_renderPass)   vkDevice.destroyRenderPass(m_renderPass);
    if (m_reuseRenderPass) vkDevice.destroyRenderPass(m_reuseRenderPass);
    for (vk::ImageView& v : m_colorLayerViews) { if (v) vkDevice.destroyImageView(v); v = nullptr; }
    for (vk::ImageView& v : m_depthLayerViews) { if (v) vkDevice.destroyImageView(v); v = nullptr; }
    Globals::gpuAllocator.destroyImage(m_colorImage, m_colorMemory);
    Globals::gpuAllocator.destroyImage(m_depthImage, m_depthMemory);
    m_sampler = nullptr; m_renderPass = nullptr; m_reuseRenderPass = nullptr;
    m_colorImage = nullptr; m_depthImage = nullptr;
    m_colorMemory = nullptr; m_depthMemory = nullptr;
}

bool SceneColor::initialize(vk::Format colorFormat, uint32 width, uint32 height, uint32 viewCount, const oc::array<vk::ImageView, 2>& prepassDepthViews)
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
    // Own depth: used only when "Depth prepass reuse" is OFF (the reuse pass binds the G-buffer depth).
    if (!createImage(vkDevice, width, height, SCENE_DEPTH_FORMAT,
        vk::ImageUsageFlagBits::eDepthStencilAttachment, viewCount, m_depthImage, m_depthMemory, "SceneColor.depth")) return false;

    for (uint32 i = 0; i < viewCount; ++i)
    {
        if (!createView(vkDevice, m_colorImage, colorFormat, vk::ImageAspectFlagBits::eColor, vk::ImageViewType::e2D, i, 1, m_colorLayerViews[i])) return false;
        if (!createView(vkDevice, m_depthImage, SCENE_DEPTH_FORMAT, vk::ImageAspectFlagBits::eDepth, vk::ImageViewType::e2D, i, 1, m_depthLayerViews[i])) return false;
    }

    // ---- Render pass: colour (ends SHADER_READ_ONLY for the TAA compute pass) + transient depth ----
    oc::array<vk::AttachmentDescription2, 2> attachments{
        vk::AttachmentDescription2{ // 0: scene colour
            .format = colorFormat,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::AttachmentDescription2{ // 1: depth (own transient depth; the tweak-OFF path rebuilds it from scratch)
            .format = SCENE_DEPTH_FORMAT,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eDontCare,
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

    // ---- Depth-prepass-reuse variant of the pass ("Depth prepass reuse" tweak): same colour attachment,
    // but the DEPTH attachment is the G-BUFFER's depth, bound READ-ONLY with no copy. The forward pass
    // tests eGreaterOrEqual directly against the prepass depth (bit-identical rasterization) and writes
    // none of its own — every scene pipeline flips depthWrite off in this mode, which is also what makes
    // the forward pass's own u_gbufferDepth sampling legal (read-only depth + sampled is allowed where a
    // writable attachment would be a feedback loop). loadOp LOAD + storeOp STORE + SHADER_READ_ONLY on
    // both ends hand the untouched prepass depth straight back to TAA/fog after the pass; the render pass
    // itself performs both layout transitions.
    {
        // Render-pass COMPATIBILITY (validation-enforced for pipelines created against the main pass and
        // for the cached secondaries' inheritance) allows the two passes to differ ONLY in attachment
        // load/store ops and layouts — the dependency array must be IDENTICAL, so the reuse pass carries
        // the main pass's dependencies verbatim and the depth's SHADER_READ_ONLY <-> DEPTH_READ_ONLY
        // layout round-trip is done with explicit barriers around the pass instead
        // (Renderer::recordReuseDepthBarrier). initial == final == the reference layout: this pass
        // performs no depth transitions itself.
        oc::array<vk::AttachmentDescription2, 2> reuseAttachments{
            attachments[0], // colour: identical to the main pass
            vk::AttachmentDescription2{
                .format = SCENE_DEPTH_FORMAT, // == the G-buffer depth format
                .samples = vk::SampleCountFlagBits::e1,
                .loadOp = vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .initialLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                .finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
            },
        };
        vk::AttachmentReference2 reuseDepthRef{ .attachment = 1, .layout = vk::ImageLayout::eDepthStencilReadOnlyOptimal, .aspectMask = vk::ImageAspectFlagBits::eDepth };
        vk::SubpassDescription2 reuseSubpass{
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorRef,
            .pDepthStencilAttachment = &reuseDepthRef,
        };
        vk::RenderPassCreateInfo2 reuseRpInfo{
            .attachmentCount = (uint32)reuseAttachments.size(),
            .pAttachments = reuseAttachments.data(),
            .subpassCount = 1,
            .pSubpasses = &reuseSubpass,
            .dependencyCount = (uint32)dependencies.size(),
            .pDependencies = dependencies.data(),
        };
        auto reuseRpResult = vkDevice.createRenderPass2(reuseRpInfo);
        if (reuseRpResult.result != vk::Result::eSuccess) { assert(false && "scenecolor reuse renderpass"); return false; }
        m_reuseRenderPass = reuseRpResult.value;

        for (uint32 i = 0; i < viewCount; ++i)
        {
            oc::array<vk::ImageView, 2> reuseFbViews{ m_colorLayerViews[i], prepassDepthViews[i] };
            vk::FramebufferCreateInfo reuseFbInfo{
                .renderPass = m_reuseRenderPass,
                .attachmentCount = (uint32)reuseFbViews.size(),
                .pAttachments = reuseFbViews.data(),
                .width = width,
                .height = height,
                .layers = 1,
            };
            auto reuseFbResult = vkDevice.createFramebuffer(reuseFbInfo);
            if (reuseFbResult.result != vk::Result::eSuccess) { assert(false && "scenecolor reuse framebuffer"); return false; }
            m_reuseFramebuffers[i] = reuseFbResult.value;
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

    // One-time layout init to SHADER_READ_ONLY so a never-yet-rendered target (e.g. the other frame's image
    // sampled before it has been drawn) is in a legal sampling layout. The render pass uses loadOp=Clear with
    // initialLayout=Undefined, so it does not depend on this.
    {
        CommandBuffer init;
        init.initialize(vk::CommandBufferLevel::ePrimary);
        vk::CommandBuffer cmd = init.begin(true);
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
        (void)Globals::device.getGraphicsQueue().waitIdle();
    }

    return true;
}
