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

private:

    vk::SurfaceKHR m_surface;
    vk::Instance m_instance;
};