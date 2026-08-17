export module RendererVK:SceneColor;

import Core;
import :VK;
import :Allocator;

// Offscreen colour+depth target the lit scene renders into (instead of straight to the swapchain), so the
// TAA resolve has an isolated image to accumulate. The colour attachment uses the swapchain surface format
// (keeping the static-mesh / GI-debug pipelines render-pass-compatible with the swapchain pass) and ends the
// pass in SHADER_READ_ONLY for the TAA compute pass to sample. One instance per frame-in-flight (written then
// sampled within the same frame, like GBuffer / ShadowMap).
export class SceneColor final
{
public:
    SceneColor();
    ~SceneColor();
    SceneColor(const SceneColor&) = delete;

    // viewCount > 1 allocates the colour/depth as arraySize=viewCount with a single-layer framebuffer
    // per eye (getFramebuffer(eye)). The render pass stays non-multiview (viewMask 0) so the forward
    // pass's DGC execution set is allowed; the forward is rendered once per eye into its layer.
    // prepassDepthViews = the G-buffer's per-eye depth views: the REUSE pass variant binds them directly
    // as a read-only depth attachment ("Depth prepass reuse" — the forward early-Z tests the prepass
    // depth with no copy and writes none of its own).
    bool initialize(vk::Format colorFormat, uint32 width, uint32 height, uint32 viewCount, const oc::array<vk::ImageView, 2>& prepassDepthViews);
    void destroy();

    vk::RenderPass  getRenderPass() const  { return m_renderPass; }
    vk::Framebuffer getFramebuffer() const { return m_framebuffers[0]; }
    vk::Framebuffer getFramebuffer(uint32 eye) const { return m_framebuffers[eye]; }
    // Depth-prepass-reuse variant: same colour attachment, depth = the G-BUFFER depth bound READ-ONLY.
    vk::RenderPass  getReuseRenderPass() const { return m_reuseRenderPass; }
    vk::Framebuffer getReuseFramebuffer(uint32 eye) const { return m_reuseFramebuffers[eye]; }
    vk::ImageView   getColorView() const   { return m_colorLayerViews[0]; } // 2D, layer 0 (sampling)
    vk::ImageView   getColorLayerView(uint32 layer) const { return m_colorLayerViews[layer]; }
    vk::Image       getColorImage() const  { return m_colorImage; }
    uint32          getViewCount() const   { return m_viewCount; }
    vk::Sampler     getSampler() const     { return m_sampler; } // linear, clamp
    uint32 getWidth() const  { return m_width; }
    uint32 getHeight() const { return m_height; }

private:
    uint32 m_width = 0;
    uint32 m_height = 0;
    uint32 m_viewCount = 1;
    vk::Format m_colorFormat = vk::Format::eUndefined;

    vk::Image m_colorImage;
    VmaAllocation m_colorMemory = nullptr;
    oc::array<vk::ImageView, 2> m_colorLayerViews{}; // per-eye 2D colour views (layer i); [0] also used for sampling

    vk::Image m_depthImage;
    VmaAllocation m_depthMemory = nullptr;
    oc::array<vk::ImageView, 2> m_depthLayerViews{}; // per-eye 2D depth views (layer i)

    vk::RenderPass m_renderPass;
    oc::array<vk::Framebuffer, 2> m_framebuffers{}; // one single-layer framebuffer per eye
    vk::RenderPass m_reuseRenderPass;
    oc::array<vk::Framebuffer, 2> m_reuseFramebuffers{}; // per eye, depth = the G-buffer depth (read-only)
    vk::Sampler m_sampler;
};
