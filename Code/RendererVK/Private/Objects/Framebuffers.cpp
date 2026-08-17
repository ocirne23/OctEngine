module RendererVK;

import :VK;
import :Device;
import :Allocator;
import :RenderPass;
import :SwapChain;

Framebuffers::Framebuffers() {}
Framebuffers::~Framebuffers()
{
    vk::Device vkDevice = Globals::device.getDevice();

    if (m_depthImageView)
        vkDevice.destroyImageView(m_depthImageView);
    Globals::gpuAllocator.destroyImage(m_depthImage, m_depthImageMemory);
    for (vk::ImageView imageView : m_imageViews)
        vkDevice.destroyImageView(imageView);
    for (vk::Framebuffer framebuffer : m_framebuffers)
        vkDevice.destroyFramebuffer(framebuffer);
}

bool Framebuffers::initialize(const RenderPass& renderPass, const SwapChain& swapChain)
{
    vk::Device vkDevice = Globals::device.getDevice();

    if (!m_framebuffers.empty())
    {
        if (m_depthImageView)
            vkDevice.destroyImageView(m_depthImageView);
        Globals::gpuAllocator.destroyImage(m_depthImage, m_depthImageMemory);
        m_depthImage = nullptr; m_depthImageMemory = nullptr;
        for (vk::ImageView imageView : m_imageViews)
            vkDevice.destroyImageView(imageView);
        m_imageViews.clear();
        for (vk::Framebuffer framebuffer : m_framebuffers)
            vkDevice.destroyFramebuffer(framebuffer);
        m_framebuffers.clear();
    }

    auto swapchainImagesResult = vkDevice.getSwapchainImagesKHR(swapChain.getSwapChain());
    if (swapchainImagesResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to get swapchain images");
        return false;
    }
    oc::vector<vk::Image> images = swapchainImagesResult.value;
    m_imageViews.reserve(images.size());
    for (const vk::Image& image : images)
    {
        vk::ImageViewCreateInfo viewInfo =
        {
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = swapChain.getLayout().surfaceFormat.format,
            .components = {},
            .subresourceRange =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        auto createViewResult = vkDevice.createImageView(viewInfo);
        if (createViewResult.result != vk::Result::eSuccess)
        {
            assert(false && "Could not create an image view\n");
            return false;
        }
        m_imageViews.push_back(createViewResult.value);
    }
    const vk::Extent2D size = swapChain.getLayout().extent;

    vk::ImageCreateInfo depthImageInfo =
    {
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eD32Sfloat,
        .extent = { size.width, size.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    if (!Globals::gpuAllocator.createImage(depthImageInfo, m_depthImage, m_depthImageMemory, "Framebuffers.depth"))
    {
        assert(false && "Could not create a depth image\n");
        return false;
    }
    vk::ImageViewCreateInfo depthImageViewInfo{
        .image = m_depthImage,
        .viewType = vk::ImageViewType::e2D,
        .format = depthImageInfo.format,
        .components = {},
        .subresourceRange =
        {
            .aspectMask = vk::ImageAspectFlagBits::eDepth,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    auto createDepthImageViewResult = vkDevice.createImageView(depthImageViewInfo);
    if (createDepthImageViewResult.result != vk::Result::eSuccess)
    {
        assert(false && "Could not create a depth image view\n");
        return false;
    }
    m_depthImageView = createDepthImageViewResult.value;
    oc::array<vk::ImageView, 2> attachments;
    attachments[1] = createDepthImageViewResult.value;
    m_framebuffers.reserve(m_imageViews.size());
    for (size_t i = 0; i < m_imageViews.size(); i++)
    {
        attachments[0] = m_imageViews[i];
        vk::FramebufferCreateInfo framebufferInfo =
        {
            .renderPass = renderPass.getRenderPass(),
            .attachmentCount = static_cast<uint32>(attachments.size()),
            .pAttachments = attachments.data(),
            .width = size.width,
            .height = size.height,
            .layers = 1
        };
        auto createFramebufferResult = vkDevice.createFramebuffer(framebufferInfo);
        if (createFramebufferResult.result != vk::Result::eSuccess)
        {
            assert(false && "Could not create a framebuffer\n");
            return false;
        }
        m_framebuffers.push_back(createFramebufferResult.value);
    }

    return true;
}