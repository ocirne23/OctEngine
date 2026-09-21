module RendererVK;

import Core;
import :VrEyeTargets;
import :Device;
import :Allocator;

void VrEyeTargets::initialize(vk::DescriptorSetLayout compositeLayout)
{
    for (DescriptorSet& set : m_compositeSet)
        set.initialize(compositeLayout);
}

void VrEyeTargets::create(vk::RenderPass swapchainRenderPass, vk::Extent2D extent, vk::Format colorFormat)
{
    destroy();
    vk::Device vkDevice = Globals::device.getDevice();

    // Shared depth (the swapchain render pass declares a depth attachment; the composite doesn't use it).
    vk::ImageCreateInfo depthInfo{
        .imageType = vk::ImageType::e2D, .format = vk::Format::eD32Sfloat, .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment, .sharingMode = vk::SharingMode::eExclusive, .initialLayout = vk::ImageLayout::eUndefined };
    Globals::gpuAllocator.createImage(depthInfo, m_depthImage, m_depthMem, "VR.eyeDepth");
    vk::ImageViewCreateInfo depthViewInfo{ .image = m_depthImage, .viewType = vk::ImageViewType::e2D, .format = vk::Format::eD32Sfloat,
        .subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1 } };
    m_depthView = vkDevice.createImageView(depthViewInfo).value;

    for (uint32 i = 0; i < 2; ++i)
    {
        vk::ImageCreateInfo colorInfo{
            .imageType = vk::ImageType::e2D, .format = colorFormat, .extent = { extent.width, extent.height, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
            .sharingMode = vk::SharingMode::eExclusive, .initialLayout = vk::ImageLayout::eUndefined };
        Globals::gpuAllocator.createImage(colorInfo, m_colorImage[i], m_colorMem[i], "VR.eyeColor");
        vk::ImageViewCreateInfo colorViewInfo{ .image = m_colorImage[i], .viewType = vk::ImageViewType::e2D, .format = colorFormat,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 } };
        m_colorView[i] = vkDevice.createImageView(colorViewInfo).value;
        oc::array<vk::ImageView, 2> atts{ m_colorView[i], m_depthView };
        vk::FramebufferCreateInfo fbInfo{ .renderPass = swapchainRenderPass, .attachmentCount = (uint32)atts.size(), .pAttachments = atts.data(),
            .width = extent.width, .height = extent.height, .layers = 1 };
        m_framebuffer[i] = vkDevice.createFramebuffer(fbInfo).value;
    }
}

void VrEyeTargets::destroy()
{
    vk::Device vkDevice = Globals::device.getDevice();
    for (uint32 i = 0; i < 2; ++i)
    {
        if (m_framebuffer[i]) { vkDevice.destroyFramebuffer(m_framebuffer[i]); m_framebuffer[i] = nullptr; }
        if (m_colorView[i]) { vkDevice.destroyImageView(m_colorView[i]); m_colorView[i] = nullptr; }
        if (m_colorImage[i]) { Globals::gpuAllocator.destroyImage(m_colorImage[i], m_colorMem[i]); m_colorImage[i] = nullptr; m_colorMem[i] = nullptr; }
    }
    if (m_depthView) { vkDevice.destroyImageView(m_depthView); m_depthView = nullptr; }
    if (m_depthImage) { Globals::gpuAllocator.destroyImage(m_depthImage, m_depthMem); m_depthImage = nullptr; m_depthMem = nullptr; }
}
