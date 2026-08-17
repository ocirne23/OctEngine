module RendererVK;

import :VK;
import :Device;
import :Surface;
import :CommandBuffer;

SwapChain::SwapChain() {}
SwapChain::~SwapChain()
{
    destroy();
}

void SwapChain::destroy()
{
    if (m_swapChain)
    {
        vk::Device vkDevice = Globals::device.getDevice();
        for (SyncObjects& syncObjects : m_syncObjects)
        {
            vkDevice.destroySemaphore(syncObjects.presentComplete);
            vkDevice.destroySemaphore(syncObjects.renderComplete);
            vkDevice.destroyFence(syncObjects.inFlight);
        }
        vkDevice.destroySwapchainKHR(m_swapChain);
        m_swapChain = VK_NULL_HANDLE;
        m_currentFrame = 0;
        m_currentImageIdx = 0;
    }
}

bool SwapChain::initialize(const Surface& surface, uint32 swapChainSize, bool vsync)
{
    vk::Device vkDevice = Globals::device.getDevice();
    if (m_swapChain)
    {
        destroy();
    }
    vk::PhysicalDevice vkPhysicalDevice = Globals::device.getPhysicalDevice();
    vk::SurfaceKHR vkSurface = surface.getSurface();

    auto surfaceCapResult = vkPhysicalDevice.getSurfaceCapabilitiesKHR(vkSurface);
    if (surfaceCapResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to get surface capabilities");
        return false;
    }
    vk::SurfaceCapabilitiesKHR capabilities = surfaceCapResult.value;
    auto presentModesResult = vkPhysicalDevice.getSurfacePresentModesKHR(vkSurface);
    if (presentModesResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to get surface present modes");
        return false;
    }
    oc::vector<vk::PresentModeKHR> presentModes = oc::fromStd(presentModesResult.value);
    auto surfaceFormatsResult = vkPhysicalDevice.getSurfaceFormatsKHR(vkSurface);
    if (surfaceFormatsResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to get surface formats");
        return false;
    }
    oc::vector<vk::SurfaceFormatKHR> surfaceFormats = oc::fromStd(surfaceFormatsResult.value);

    vk::SurfaceFormatKHR surfaceFormat = surfaceFormats[0];
    for (const auto& availableFormat : surfaceFormats)
    {
        if (availableFormat.format == vk::Format::eB8G8R8A8Unorm)// && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
        {
            surfaceFormat = availableFormat;
            break;
        }
    }
    vk::CompositeAlphaFlagBitsKHR compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;

    const uint32 imageCount = oc::max(capabilities.minImageCount, oc::min(swapChainSize, capabilities.maxImageCount));
    m_syncObjects.resize(imageCount);
    m_syncObjects.shrink_to_fit();

    for (uint32 i = 0; i < imageCount; i++)
    {
        auto createSemaphore1 = vkDevice.createSemaphore(vk::SemaphoreCreateInfo{});
        if (createSemaphore1.result != vk::Result::eSuccess)
        {
            assert(false && "Failed to create semaphore");
            return false;
        }
        m_syncObjects[i].presentComplete = createSemaphore1.value;

        auto createSemaphore2 = vkDevice.createSemaphore(vk::SemaphoreCreateInfo{});
        if (createSemaphore2.result != vk::Result::eSuccess)
        {
            assert(false && "Failed to create semaphore");
            return false;
        }
        m_syncObjects[i].renderComplete = createSemaphore2.value;

        auto inFlightCreateFenceResult = vkDevice.createFence(vk::FenceCreateInfo{ .flags = vk::FenceCreateFlagBits::eSignaled });
        if (inFlightCreateFenceResult.result != vk::Result::eSuccess)
        {
            assert(false && "Failed to create fence");
            return false;
        }
        m_syncObjects[i].inFlight = inFlightCreateFenceResult.value;
    }
    
    vk::SwapchainCreateInfoKHR createInfo{
        .surface = vkSurface,
        .minImageCount = imageCount,
        .imageFormat = surfaceFormat.format,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageExtent = capabilities.currentExtent,
        .imageArrayLayers = 1,// capabilities.maxImageArrayLayers,
        // eTransferSrc lets the VR path blit the composited frame into the eye swapchains.
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = compositeAlpha,
        .presentMode = vsync ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate,
        .clipped = vk::True,
    };

    m_layout.numImages = imageCount;
    m_layout.surfaceFormat = surfaceFormat;
    m_layout.extent = capabilities.currentExtent;
    m_layout.presentMode = createInfo.presentMode;

    auto createSwapChainResult = vkDevice.createSwapchainKHR(createInfo);
    if (createSwapChainResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to create swap chain");
        return false;
    }
    m_swapChain = createSwapChainResult.value;

    auto imagesResult = vkDevice.getSwapchainImagesKHR(m_swapChain);
    if (imagesResult.result != vk::Result::eSuccess)
    {
        assert(false && "Failed to get swapchain images");
        return false;
    }
    m_images = oc::fromStd(imagesResult.value);

    return true;
}

bool SwapChain::acquireNextImage()
{
    vk::Device vkDevice = Globals::device.getDevice();
    SyncObjects& syncObjects = m_syncObjects[m_currentFrame];

    vk::Result result = vkDevice.waitForFences(1, &syncObjects.inFlight, vk::True, UINT64_MAX);
    if (result != vk::Result::eSuccess)
        assert(false && "Failed to wait for fence");
    result = vkDevice.resetFences(1, &syncObjects.inFlight);
    if (result != vk::Result::eSuccess)
        assert(false && "Failed to reset fence");

    constexpr std::chrono::nanoseconds timeout = std::chrono::seconds(10);
    auto imageResult = vkDevice.acquireNextImageKHR(m_swapChain, timeout.count(), syncObjects.presentComplete);
    switch (imageResult.result)
    {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
        return false;
    case vk::Result::eErrorOutOfDateKHR:
        return false;
    default:
        assert(false && "Failed to acquire next image");
        return false;
    }
    m_currentImageIdx = imageResult.value;
    return true;
}

bool SwapChain::waitForFrame(uint32 frameIdx)
{
    vk::Device vkDevice = Globals::device.getDevice();
    SyncObjects& syncObjects = m_syncObjects[frameIdx];

    vk::Result result = vkDevice.waitForFences(1, &syncObjects.inFlight, vk::True, UINT64_MAX);
    if (result != vk::Result::eSuccess)
    {
        assert(false && "Failed to wait for fence");
        return false;
    }
    return true;
}

void SwapChain::submitCommandBuffer(CommandBuffer& commandBuffer)
{
    SyncObjects& syncObjects = m_syncObjects[m_currentFrame];
    vk::PipelineStageFlags2 waitStage = { vk::PipelineStageFlagBits2::eColorAttachmentOutput };
    commandBuffer.setWaitStage(waitStage);
    commandBuffer.addWaitSemaphore(syncObjects.presentComplete, vk::PipelineStageFlagBits::eColorAttachmentOutput);
    commandBuffer.addSignalSemaphore(m_syncObjects[m_currentImageIdx].renderComplete);
    commandBuffer.submitGraphics(syncObjects.inFlight);
}

bool SwapChain::present()
{
    vk::PipelineStageFlags2 waitStage = { vk::PipelineStageFlagBits2::eColorAttachmentOutput };
    vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &m_syncObjects[m_currentImageIdx].renderComplete,
        .swapchainCount = 1,
        .pSwapchains = &m_swapChain,
        .pImageIndices = &m_currentImageIdx,
    };

    vk::Result result = Globals::device.getGraphicsQueue().presentKHR(presentInfo);
    switch (result)
    {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
        return false;
    case vk::Result::eErrorOutOfDateKHR:
        return false;
    default:
        assert(false && "Failed to present image");
        return false;
    }
    m_currentFrame = (m_currentFrame + 1) % m_layout.numImages;
    return true;
}