export module RendererVK:SwapChain;

import Core;
import :VK;
import :CommandBuffer;

class Surface;

export class SwapChain final
{
public:

    struct Layout
    {
        uint32 numImages;
        vk::SurfaceFormatKHR surfaceFormat;
        vk::Extent2D extent;
        vk::PresentModeKHR presentMode;
    };

    struct SyncObjects
    {
        vk::Semaphore presentComplete;
        vk::Semaphore renderComplete;
        vk::Fence inFlight;
        bool isInFlight = false;
    };

    SwapChain();
    ~SwapChain();
    SwapChain(const SwapChain&) = delete;

    bool initialize(const Surface& surface, uint32 swapChainSize, bool vsync);
    void destroy();

    vk::SwapchainKHR getSwapChain() const { return m_swapChain; }
    const Layout& getLayout() const { return m_layout; }
    uint32 getCurrentFrameIndex() const { return m_currentFrame; }
    uint32 getPrevFrameIdx() const { return (m_currentFrame == 0) ? (uint32)m_syncObjects.size() - 1 : m_currentFrame - 1; }
    uint32 getCurrentImageIdx() const { return m_currentImageIdx; }
    vk::Image getCurrentImage() const { return m_images[m_currentImageIdx]; }

    bool acquireNextImage();
    bool present();

    bool waitForFrame(uint32 frameIdx);
    void submitCommandBuffer(CommandBuffer& commandBuffer);

private:

    vk::SwapchainKHR m_swapChain;
    Layout m_layout;
    oc::vector<vk::Image> m_images; // owned by the swapchain; cached for the VR mirror blit

    uint32 m_currentFrame = 0;
    uint32 m_currentImageIdx = (~0u);
    oc::vector<SyncObjects> m_syncObjects;
};