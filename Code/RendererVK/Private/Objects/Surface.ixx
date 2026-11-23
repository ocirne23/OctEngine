export module RendererVK:Surface;

import Core.fwd;
import :VK;

export class Surface final
{
public:
    Surface();
    ~Surface();
    Surface(const Surface&) = delete;

    bool initialize(Window& window); // non-const: surface creation is marshaled onto the window thread

    bool deviceSupportsSurface() const;
    vk::SurfaceKHR getSurface() const { return m_surface; }
    Window* getWindow() const { return m_window; } // the swapchain's size where the surface reports none (Wayland)

private:

    Window* m_window = nullptr;
    vk::SurfaceKHR m_surface;
    vk::Instance m_instance;
};