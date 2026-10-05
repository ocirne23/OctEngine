module RendererVK;

import :VK;
import :Device;
import :Surface;
import :CommandBuffer;
import :Streamline;

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
        Streamline::destroySwapchain(vkDevice, m_swapChain);
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

        Globals::device.setDebugName(m_syncObjects[i].presentComplete, oc::format("SwapChain.presentComplete[{}]", i).c_str());
        Globals::device.setDebugName(m_syncObjects[i].renderComplete, oc::format("SwapChain.renderComplete[{}]", i).c_str());
        Globals::device.setDebugName(m_syncObjects[i].inFlight, oc::format("SwapChain.inFlight[{}]", i).c_str());
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

    // The swapchain calls go through Streamline's proxies while it is loaded (manual hooking).
    if (Streamline::createSwapchain(vkDevice, createInfo, m_swapChain) != vk::Result::eSuccess)
    {
        assert(false && "Failed to create swap chain");
        return false;
    }
    Globals::device.setDebugName(m_swapChain, "SwapChain");

    if (Streamline::getSwapchainImages(vkDevice, m_swapChain, m_images) != vk::Result::eSuccess)
    {
        assert(false && "Failed to get swapchain images");
        return false;
    }
    for (uint32 i = 0; i < (uint32)m_images.size(); i++)
        Globals::device.setDebugName(m_images[i], oc::format("Swapchain[{}]", i).c_str());

    return true;
}

bool SwapChain::acquireNextImage()
{
    vk::Device vkDevice = Globals::device.getDevice();
    SyncObjects& syncObjects = m_syncObjects[m_currentFrame];

    vk::Result result = vkDevice.waitForFences(1, &syncObjects.inFlight, vk::True, UINT64_MAX);
    if (result != vk::Result::eSuccess)
        assert(false && "Failed to wait for fence");

    constexpr std::chrono::nanoseconds timeout = std::chrono::seconds(10);
    uint32 imageIndex = 0;
    const vk::Result acquireResult = Streamline::acquireNextImage(vkDevice, m_swapChain, timeout.count(), syncObjects.presentComplete, imageIndex);
    switch (acquireResult)
    {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
        // The image IS acquired and presentComplete WILL signal: render and present it (the present reports the
        // suboptimal swapchain and the caller rebuilds then). Dropping it leaked the image and left the semaphore
        // signaled for its next use.
        break;
    case vk::Result::eErrorOutOfDateKHR:
        return false; // nothing acquired: the fence stays SIGNALED (reset below only on success)
    default:
        assert(false && "Failed to acquire next image");
        return false;
    }
    // Reset only now that this frame WILL submit (and signal it): reset before a failed acquire, the fence stayed
    // unsignaled with nothing to signal it, and the next frame's wait (UINT64_MAX, above) hung for good whenever the
    // swapchain was not rebuilt in between (the renderer's resize hold).
    result = vkDevice.resetFences(1, &syncObjects.inFlight);
    if (result != vk::Result::eSuccess)
        assert(false && "Failed to reset fence");
    m_currentImageIdx = imageIndex;
    return true;
}

SwapChain::EFenceWait SwapChain::waitForFrame(uint32 frameIdx, uint64 timeoutNs)
{
    const vk::Result result = Globals::device.getDevice().waitForFences(
        1, &m_syncObjects[frameIdx].inFlight, vk::True, timeoutNs);
    if (result == vk::Result::eSuccess)
        return EFenceWait::Signaled;
    if (result == vk::Result::eTimeout)
        return EFenceWait::Timeout;
    assert(false && "Failed to wait for fence");
    return EFenceWait::Error;
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

    vk::Result result;
    {
        // Queue calls need external synchronization; staging overflow submits can come from workers.
        std::lock_guard<std::mutex> lock(Globals::device.getGraphicsQueueMutex());
        result = Streamline::queuePresent(Globals::device.getGraphicsQueue(), presentInfo);
    }
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