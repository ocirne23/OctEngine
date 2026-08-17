export module RendererVK:DecalPipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :GraphicsPipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;

// Projected box decals, drawn inside the scene-color render pass immediately after the opaque forward
// draw (before particles/fog, so those layer on top). Decals are submitted per frame like lights
// (Renderer::addDecal, lock-free atomic cursor into a mapped per-frame buffer) and rendered with ONE
// instanced indirect draw of unit cubes (36 verts, front faces culled, no depth test — works with the
// camera inside the volume): the fragment shader reconstructs the surface from the G-buffer depth,
// projects it into decal space and blends premultiplied over the lit scene. instanceCount rides in a
// mapped indirect buffer, so the cached secondary command buffer never re-records for count changes.
export class DecalPipeline final
{
public:
    DecalPipeline() = default;
    ~DecalPipeline() = default;
    DecalPipeline(const DecalPipeline&) = delete;

    void initialize(vk::RenderPass sceneRenderPass, uint32 maxTextures, uint32 numTextureDescriptors, uint32 viewCount);
    void reloadShaders(vk::RenderPass sceneRenderPass);

    // The current frame slot's mapped decal array; Renderer::addDecal writes claimed slots directly.
    oc::span<RendererVKLayout::DecalInfo> getMapped(uint32 frameIdx) { return m_mappedDecals[frameIdx]; }
    // Publishes this frame's decal count (call from present(), after the slot's fence wait).
    void upload(uint32 frameIdx, uint32 decalCount);

    struct DrawParams
    {
        Buffer& ubo;
        Buffer& giGridDataBuffer;
        vk::ImageView gbufferDepthView;
        // DEPTH_STENCIL_READ_ONLY while depth-prepass reuse binds this image as the scene pass depth.
        vk::ImageLayout gbufferDepthLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::ImageView gbufferNormalView;
        vk::Sampler   gbufferSampler;
    };
    // Records the indirect instanced box draw; the caller has begun a command buffer inside the
    // scene-color render pass and set the viewport/scissor. eye selects the per-eye set/views.
    void recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params);

    // Bindless texture array maintenance (same contract as the other texture-array pipelines).
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);
    void resizeTextureDescriptors(uint32 numTextureDescriptors);

private:
    void buildLayout(GraphicsPipelineLayout& layout, uint32 maxTextures);

    GraphicsPipeline m_pipeline;
    Sampler m_textureSampler;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_decalBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_indirectBuffers;
    oc::array<oc::span<RendererVKLayout::DecalInfo>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedDecals;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedIndirect; // vk::DrawIndirectCommand as 4 uints
    static constexpr uint32 MAX_VIEWS = 2;
    static uint32 drawSlot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS> m_sets;
    uint32 m_viewCount = 1;
};
