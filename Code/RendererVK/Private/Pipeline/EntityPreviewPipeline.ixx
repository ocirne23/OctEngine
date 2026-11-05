export module RendererVK:EntityPreviewPipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :DescriptorSet;
import :EntityPreviewTypes;
import :GraphicsPipeline;
import :Layout;
import :Sampler;

// The Entity Editor's preview: its own small colour + depth target, drawn with plain drawIndexed calls straight from the mesh mega-buffers, re-recorded in the
// primary each frame the panel is visible. It does not touch the instance stream, the cull, the frame UBO or any lighting state of the scene, so what is
// previewed is never in the live world. Created on the first submit; a session that never opens the panel pays nothing.
//
// ONE target for both frame slots: every use is on the graphics queue, so the render pass's external dependency orders it against the sampled read of the
// frame before. A fixed size, so its ImTextureID never changes and a resize never stalls: the panel draws into a sub-rect and shows it through its UVs.
export class EntityPreviewPipeline final
{
public:
    static constexpr uint32 TARGET_SIZE = 2048;

    EntityPreviewPipeline() = default;
    ~EntityPreviewPipeline();
    EntityPreviewPipeline(const EntityPreviewPipeline&) = delete;

    bool isInitialized() const { return m_renderPass != vk::RenderPass(nullptr); }
    void initialize(uint32 maxTextures, uint32 numTextureDescriptors);
    void reloadShaders(uint32 maxTextures);

    vk::ImageView getView() const { return m_colorView; }
    vk::Sampler getSampler() const { return m_colorSampler.getSampler(); }

    // The caller is outside any render pass, this slot's fence has been waited.
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const EntityPreviewFrame& frame, oc::span<const RendererVKLayout::MeshInfo> meshInfos,
        Buffer& vertexBuffer, Buffer& indexBuffer, Buffer& materialBuffer);

    // Bindless texture array maintenance (same contract as the other texture-array pipelines).
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);
    void resizeTextureDescriptors(uint32 numTextureDescriptors);
    void invalidateTextures(uint32 frameIdx) { m_writtenTextures[frameIdx] = UINT32_MAX; } // rewrites the whole array at the next record

private:
    void buildMeshLayout(GraphicsPipelineLayout& layout, uint32 maxTextures);
    void buildGridLayout(GraphicsPipelineLayout& layout);
    void createRenderPass();
    void createTargets();

    vk::RenderPass m_renderPass;
    vk::Framebuffer m_framebuffer;
    vk::Image m_colorImage;
    VmaAllocation m_colorMemory = nullptr;
    vk::ImageView m_colorView;
    vk::Image m_depthImage;
    VmaAllocation m_depthMemory = nullptr;
    vk::ImageView m_depthView;
    Sampler m_colorSampler;
    Sampler m_textureSampler;

    GraphicsPipeline m_meshPipeline; // variant 1 = wireframe
    GraphicsPipeline m_gridPipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets;
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_writtenTextures{ UINT32_MAX, UINT32_MAX }; // how many array slots this slot's set holds
};
