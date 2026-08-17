export module RendererVK:GBuffer;

import Core;
import :VK;
import :Allocator;
import :Layout;

// Thin G-buffer used to drive screen-space ray-traced AO (and, later, other screen-space effects).
// One color attachment storing the world-space shading normal (RGBA16F; .a unused for now) plus a
// D32 depth attachment. Both attachments end the prepass in SHADER_READ_ONLY so the RTAO compute
// pass can sample them. Sized to the swapchain extent; one instance per frame-in-flight (written and
// then sampled within the same frame, like ShadowMap).
export class GBuffer final
{
public:
    GBuffer();
    ~GBuffer();
    GBuffer(const GBuffer&) = delete;

    // viewCount > 1 (VR) allocates the normal/depth as arraySize=viewCount with a single-layer framebuffer
    // and per-layer 2D sampling view per eye; the prepass is rendered once per eye into its layer.
    bool initialize(uint32 width, uint32 height, uint32 viewCount = 1);
    void destroy();

    vk::RenderPass getRenderPass() const  { return m_renderPass; }
    vk::Framebuffer getFramebuffer() const { return m_framebuffers[0]; }
    vk::Framebuffer getFramebuffer(uint32 eye) const { return m_framebuffers[eye]; }
    vk::ImageView getNormalView() const   { return m_normalViews[0]; }
    vk::ImageView getNormalView(uint32 eye) const { return m_normalViews[eye]; }
    vk::ImageView getDepthView() const    { return m_depthViews[0]; }
    vk::ImageView getDepthView(uint32 eye) const { return m_depthViews[eye]; }
    vk::Image getDepthImage() const       { return m_depthImage; } // bound read-only as the scene pass depth (depth-prepass reuse)
    vk::Sampler getSampler() const        { return m_sampler; } // nearest, clamp (point sampling for reconstruction)
    uint32 getViewCount() const { return m_viewCount; }
    uint32 getWidth() const  { return m_width; }
    uint32 getHeight() const { return m_height; }

private:
    uint32 m_width = 0;
    uint32 m_height = 0;
    uint32 m_viewCount = 1;

    vk::Image m_normalImage;
    VmaAllocation m_normalMemory = nullptr;
    oc::array<vk::ImageView, 2> m_normalViews{}; // per-eye 2D views (layer i)

    vk::Image m_depthImage;
    VmaAllocation m_depthMemory = nullptr;
    oc::array<vk::ImageView, 2> m_depthViews{}; // per-eye 2D views (layer i)

    vk::RenderPass m_renderPass;
    oc::array<vk::Framebuffer, 2> m_framebuffers{}; // one single-layer framebuffer per eye
    vk::Sampler m_sampler;
};
