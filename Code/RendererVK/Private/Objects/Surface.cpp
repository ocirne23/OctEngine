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

bool Surface::initialize(const Window& window)
{
    if (m_surface)
    {
        Globals::instance.getInstance().destroySurfaceKHR(m_surface);
    }
    VkSurfaceKHR vkSurface = nullptr;
    if (SDL_Vulkan_CreateSurface((SDL_Window*)window.getWindowHandle(), Globals::instance.getInstance(), nullptr, &vkSurface) != 1
        || vkSurface == nullptr)
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
    oc::vector<vk::QueueFamilyProperties> queueFamilyProperties = physicalDevice.getQueueFamilyProperties();
    if (physicalDevice.getSurfaceSupportKHR(Globals::device.getGraphicsQueueIndex(), m_surface).result != vk::Result::eSuccess)
    {
        assert(false && "Separate present queue not supported!");
        return false;
    }
    return true;
}
