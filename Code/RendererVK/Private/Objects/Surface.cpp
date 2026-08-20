module RendererVK;

import Core;
import Core.SDL;
import Core.Window;
import :VK;
import :Instance;
import :Device;

Surface::Surface() {}
Surface::~Surface()
{
    if (m_surface)
    {
        Globals::instance.getInstance().destroySurfaceKHR(m_surface);
    }
}

bool Surface::initialize(Window& window)
{
    if (m_surface)
    {
        Globals::instance.getInstance().destroySurfaceKHR(m_surface);
    }
    // Window-affine SDL call: runs ON the window thread (init-time blocking marshal - see Core.Window).
    VkSurfaceKHR vkSurface = nullptr;
    bool created = false;
    window.runOnWindowThread([&]
    {
        created = SDL_Vulkan_CreateSurface((SDL_Window*)window.getWindowHandle(),
            Globals::instance.getInstance(), nullptr, &vkSurface) == 1;
    }, /*wait*/ true);
    if (!created || vkSurface == nullptr)
    {
        printf("Failed to create Vulkan surface: %s\n", SDL_GetError());
        assert(false);
        return false;
    }
    m_surface = vkSurface;
    return true;
}

bool Surface::deviceSupportsSurface() const
{
    vk::PhysicalDevice physicalDevice = Globals::device.getPhysicalDevice();
    oc::vector<vk::QueueFamilyProperties> queueFamilyProperties = oc::fromStd(physicalDevice.getQueueFamilyProperties());
    if (physicalDevice.getSurfaceSupportKHR(Globals::device.getGraphicsQueueIndex(), m_surface).result != vk::Result::eSuccess)
    {
        assert(false && "Separate present queue not supported!");
        return false;
    }
    return true;
}
