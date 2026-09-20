export module RendererVK:SceneColor;

import Core;
import :VK;
import :Allocator;

// Offscreen colour+depth target the lit scene renders into (instead of straight to the swapchain), so the
// TAA resolve has an isolated image to accumulate. The colour attachment uses the swapchain surface format
// (keeping the static-mesh / GI-debug pipelines render-pass-compatible with the swapchain pass) and ends the
// pass in SHADER_READ_ONLY for the TAA compute pass to sample. One instance per frame-in-flight.
//
// THE DEPTH IS THE SCENE DEPTH EVERY SCREEN-SPACE CONSUMER READS (there is no depth prepass): RTAO, the
// force march, decals, particles, fog apply, TAA, and - as "last frame's depth" out of the other slot -
// the forward pass's AO reprojection and the particle collision. It lives in exactly TWO layouts:
//   DEPTH_STENCIL_ATTACHMENT  while the depth-WRITING stages run (static meshes, GI probe debug),
//   DEPTH_STENCIL_READ_ONLY   the rest of the time: SCENE_DEPTH_SAMPLED_LAYOUT, the layout of every
//                             sampling descriptor AND of the read-only attachment of the later scene
//                             stages, which may therefore sample the depth they test against.
// No pass variant transitions the depth itself (initial == final == the reference layout, the first one
// clears from UNDEFINED); the Renderer emits the one ATTACHMENT -> READ_ONLY barrier per frame.
export constexpr vk::ImageLayout SCENE_DEPTH_SAMPLED_LAYOUT = vk::ImageLayout::eDepthStencilReadOnlyOptimal;

export class SceneColor final
{
public:
    SceneColor();
    ~SceneColor();
    SceneColor(const SceneColor&) = delete;

    // viewCount > 1 allocates the colour/depth as arraySize=viewCount with a single-layer framebuffer
    // per eye (getFramebuffer(eye)). The render pass stays non-multiview (viewMask 0) so the forward
    // pass's DGC execution set is allowed; the forward is rendered once per eye into its layer.
    bool initialize(vk::Format colorFormat, uint32 width, uint32 height, uint32 viewCount);
    void destroy();

    // The BASE pass: what the scene pipelines and the cached secondaries are built against. Never begun.
    vk::RenderPass  getRenderPass() const  { return m_renderPass; }
    vk::Framebuffer getFramebuffer() const { return m_framebuffers[0]; }
    vk::Framebuffer getFramebuffer(uint32 eye) const { return m_framebuffers[eye]; }
    // The scene renders as one render-pass instance PER STAGE: the GPU profiler brackets each stage
    // (timestamps are illegal inside a secondaries subpass), and the depth switches from written to
    // read-only + sampled between two of them. colour: `first` clears, `last` hands the colour to TAA
    // (SHADER_READ_ONLY); depth: `depthReadOnly` binds it as a read-only attachment (load, no store
    // needed but kept), otherwise it is written (the first instance clears it). All variants are
    // COMPATIBLE with the base pass (identical dependency arrays; only load/store ops and layouts
    // differ), so the secondaries, the pipelines and the framebuffers serve every one of them.
    // Inter-instance attachment hazards are explicit barriers in the primary (the deps must stay
    // identical for compatibility, so they cannot carry them).
    vk::RenderPass getStageRenderPass(bool first, bool last, bool depthReadOnly) const
    {
        return m_stagePasses[(first ? 1 : 0) | (last ? 2 : 0) | (depthReadOnly ? 4 : 0)];
    }
    vk::ImageView   getColorView() const   { return m_colorLayerViews[0]; } // 2D, layer 0 (sampling)
    vk::ImageView   getColorLayerView(uint32 layer) const { return m_colorLayerViews[layer]; }
    vk::Image       getColorImage() const  { return m_colorImage; }
    vk::ImageView   getDepthView() const   { return m_depthLayerViews[0]; }
    vk::ImageView   getDepthView(uint32 eye) const { return m_depthLayerViews[eye]; }
    vk::Image       getDepthImage() const  { return m_depthImage; }
    uint32          getViewCount() const   { return m_viewCount; }
    vk::Sampler     getSampler() const     { return m_sampler; } // linear, clamp
    vk::Sampler     getDepthSampler() const { return m_depthSampler; } // nearest, clamp (point sampling for reconstruction)
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
    oc::array<vk::RenderPass, 8> m_stagePasses{};   // see getStageRenderPass
    vk::Sampler m_sampler;
    vk::Sampler m_depthSampler;
};
