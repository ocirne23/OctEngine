module RendererVK;

import :VK;
import :Device;
import :Allocator;
import :CommandBuffer;

namespace
{
    constexpr vk::Format GBUFFER_NORMAL_FORMAT = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format GBUFFER_DEPTH_FORMAT  = vk::Format::eD32Sfloat;

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
        if (!Globals::gpuAllocator.createImage(info, outImage, outMemory, name)) { assert(false && "gbuffer image"); return false; }
        return true;
    }

    bool createView(vk::Device vkDevice, vk::Image image, vk::Format format, vk::ImageAspectFlags aspect, uint32 baseLayer, vk::ImageView& outView)
    {
        vk::ImageViewCreateInfo info{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = { .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = baseLayer, .layerCount = 1 },
        };
        auto result = vkDevice.createImageView(info);
        if (result.result != vk::Result::eSuccess) { assert(false && "gbuffer view"); return false; }
        outView = result.value;
        return true;
    }
}

GBuffer::GBuffer() {}
GBuffer::~GBuffer() { destroy(); }

void GBuffer::destroy()
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_sampler)      vkDevice.destroySampler(m_sampler);
    for (vk::Framebuffer& fb : m_framebuffers) { if (fb) vkDevice.destroyFramebuffer(fb); fb = nullptr; }
    if (m_renderPass)   vkDevice.destroyRenderPass(m_renderPass);
    for (vk::ImageView& v : m_normalViews) { if (v) vkDevice.destroyImageView(v); v = nullptr; }
    for (vk::ImageView& v : m_depthViews)  { if (v) vkDevice.destroyImageView(v); v = nullptr; }
    Globals::gpuAllocator.destroyImage(m_normalImage, m_normalMemory);
    Globals::gpuAllocator.destroyImage(m_depthImage, m_depthMemory);
    m_sampler = nullptr; m_renderPass = nullptr;
    m_normalImage = nullptr; m_depthImage = nullptr;
    m_normalMemory = nullptr; m_depthMemory = nullptr;
}

bool GBuffer::initialize(uint32 width, uint32 height, uint32 viewCount)
{
    vk::Device vkDevice = Globals::device.getDevice();
    destroy();
    m_width = width;
    m_height = height;
    m_viewCount = viewCount;

    if (!createImage(vkDevice, width, height, GBUFFER_NORMAL_FORMAT,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, viewCount, m_normalImage, m_normalMemory, "GBuffer.normal")) return false;
    if (!createImage(vkDevice, width, height, GBUFFER_DEPTH_FORMAT,
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled, viewCount, m_depthImage, m_depthMemory, "GBuffer.depth")) return false;
    for (uint32 i = 0; i < viewCount; ++i)
    {
        if (!createView(vkDevice, m_normalImage, GBUFFER_NORMAL_FORMAT, vk::ImageAspectFlagBits::eColor, i, m_normalViews[i])) return false;
        if (!createView(vkDevice, m_depthImage, GBUFFER_DEPTH_FORMAT, vk::ImageAspectFlagBits::eDepth, i, m_depthViews[i])) return false;
    }

    // ---- Render pass: color (normal) + depth, both end SHADER_READ_ONLY for the AO compute pass ----
    oc::array<vk::AttachmentDescription2, 2> attachments{
        vk::AttachmentDescription2{ // 0: normal color
            .format = GBUFFER_NORMAL_FORMAT,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::AttachmentDescription2{ // 1: depth
            .format = GBUFFER_DEPTH_FORMAT,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eUndefined,
            .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
    };
    vk::AttachmentReference2 colorRef{ .attachment = 0, .layout = vk::ImageLayout::eColorAttachmentOptimal, .aspectMask = vk::ImageAspectFlagBits::eColor };
    vk::AttachmentReference2 depthRef{ .attachment = 1, .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal, .aspectMask = vk::ImageAspectFlagBits::eDepth };
    vk::SubpassDescription2 subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorRef,
        .pDepthStencilAttachment = &depthRef,
    };
    oc::array<vk::SubpassDependency2, 2> dependencies{
        vk::SubpassDependency2{
            .srcSubpass = vk::SubpassExternal,
            .dstSubpass = 0,
            .srcStageMask = vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eFragmentShader,
            .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
            .srcAccessMask = vk::AccessFlagBits::eShaderRead,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        },
        vk::SubpassDependency2{
            .srcSubpass = 0,
            .dstSubpass = vk::SubpassExternal,
            .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
            .dstStageMask = vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
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
    if (rpResult.result != vk::Result::eSuccess) { assert(false && "gbuffer renderpass"); return false; }
    m_renderPass = rpResult.value;

    for (uint32 i = 0; i < viewCount; ++i)
    {
        oc::array<vk::ImageView, 2> fbViews{ m_normalViews[i], m_depthViews[i] };
        vk::FramebufferCreateInfo fbInfo{
            .renderPass = m_renderPass,
            .attachmentCount = (uint32)fbViews.size(),
            .pAttachments = fbViews.data(),
            .width = width,
            .height = height,
            .layers = 1,
        };
        auto fbResult = vkDevice.createFramebuffer(fbInfo);
        if (fbResult.result != vk::Result::eSuccess) { assert(false && "gbuffer framebuffer"); return false; }
        m_framebuffers[i] = fbResult.value;
    }

    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = vk::False,
        .compareEnable = vk::False,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = vk::BorderColor::eFloatOpaqueWhite,
        .unnormalizedCoordinates = vk::False,
    };
    auto samplerResult = vkDevice.createSampler(samplerInfo);
    if (samplerResult.result != vk::Result::eSuccess) { assert(false && "gbuffer sampler"); return false; }
    m_sampler = samplerResult.value;

    // One-time layout init to SHADER_READ_ONLY so that a never-yet-rendered G-buffer (e.g. the other
    // frame's buffer sampled as "previous frame" on the very first frame) is in a legal sampling layout.
    // The render pass uses loadOp=Clear with initialLayout=Undefined, so it doesn't depend on this.
    {
        CommandBuffer init;
        init.initialize(vk::CommandBufferLevel::ePrimary);
        vk::CommandBuffer cmd = init.begin(true);
        oc::array<vk::ImageMemoryBarrier2, 2> bars{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = m_normalImage,
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, viewCount },
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = m_depthImage,
                .subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, 0, viewCount },
            },
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .imageMemoryBarrierCount = (uint32)bars.size(), .pImageMemoryBarriers = bars.data() });
        init.end();
        init.submitGraphics();
        (void)Globals::device.getGraphicsQueue().waitIdle();
    }

    return true;
}
