export module RendererVK:ShadowMap;

import Core;
import :VK;
import :Allocator;
import :Layout;

// Cascaded shadow map render target: a single D32 depth image with one array layer per cascade,
// a depth-only render pass, one framebuffer per cascade layer, and a comparison sampler for PCF
// lookups in the lighting shader. Designed so additional shadow-casting lights can allocate their
// own ShadowMap instances later.
export class ShadowMap final
{
public:
    ShadowMap();
    ~ShadowMap();
    ShadowMap(const ShadowMap&) = delete;

    // extraLayer: one more array layer AFTER the cascades (layer numCascades), outside the multiview pass, with its
    // own single-layer render pass + framebuffer - the sun's NEAR GRASS cascade (GrassPipeline). The cascades' pass
    // discards it (its layout transition covers the whole view), so it must be drawn after them, every frame it is read.
    bool initialize(const char* debugName, uint32 resolution = RendererVKLayout::SHADOW_MAP_RESOLUTION, uint32 numCascades = RendererVKLayout::NUM_SHADOW_CASCADES,
        bool extraLayer = false);

    vk::RenderPass getRenderPass() const { return m_renderPass; }
    vk::Framebuffer getFramebuffer() const { return m_framebuffer; } // single layered framebuffer (multiview)
    vk::RenderPass getExtraRenderPass() const { return m_extraRenderPass; }   // null without the extra layer
    vk::Framebuffer getExtraFramebuffer() const { return m_extraFramebuffer; }
    vk::ImageView getSampleView() const  { return m_sampleView; }
    vk::Sampler getSampler() const       { return m_sampler; }      // comparison sampler (hardware PCF)
    vk::Sampler getDepthSampler() const  { return m_depthSampler; } // non-comparison (raw depth for PCSS blocker search)
    vk::Image getImage() const           { return m_image; }
    uint32 getResolution() const         { return m_resolution; }
    uint32 getNumCascades() const        { return m_numCascades; }

private:

    void destroy();

    uint32 m_resolution = 0;
    uint32 m_numCascades = 0;
    vk::Image m_image;
    VmaAllocation m_imageMemory = nullptr;
    vk::ImageView m_sampleView; // e2DArray over all cascades (+ the extra layer); the render target and the sampled view
    vk::Framebuffer m_framebuffer;
    vk::RenderPass m_renderPass;
    vk::ImageView m_extraView;  // the extra layer alone (its render target)
    vk::RenderPass m_extraRenderPass;
    vk::Framebuffer m_extraFramebuffer;
    vk::Sampler m_sampler;      // comparison sampler for hardware PCF
    vk::Sampler m_depthSampler; // non-comparison sampler for raw-depth blocker search
};
