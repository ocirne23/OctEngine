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

// The editor previews (the Entity Editor's, the Animation Editor's): a small colour + depth target PER PANEL, drawn with plain drawIndexed calls straight from
// the mesh mega-buffers, re-recorded in the primary each frame its panel is visible. It does not touch the instance stream, the cull, the frame UBO or any
// lighting state of the scene, so what is previewed is never in the live world. The pipelines are created on the first submit, a target on its panel's first
// submit: a session that never opens a panel pays nothing for it.
//
// ONE target per panel for both frame slots: every use is on the graphics queue, so the render pass's external dependency orders it against the sampled read
// of the frame before. A fixed size, so its ImTextureID never changes and a resize never stalls: the panel draws into a sub-rect and shows it through its UVs.
// Each target has its own descriptor sets and frame buffer (camera + skinning palette), so two targets record in one command buffer with no shared write.
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

    bool hasTarget(uint32 target) const { return m_targets[target].framebuffer != vk::Framebuffer(nullptr); }
    void createTarget(uint32 target);
    vk::ImageView getView(uint32 target) const { return m_targets[target].colorView; }
    vk::Sampler getSampler() const { return m_colorSampler.getSampler(); }

    // The caller is outside any render pass, this slot's fence has been waited.
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const EntityPreviewFrame& frame, oc::span<const RendererVKLayout::MeshInfo> meshInfos,
        Buffer& vertexBuffer, Buffer& indexBuffer, Buffer& materialBuffer, Buffer& skinningBuffer);

    // Bindless texture array maintenance (same contract as the other texture-array pipelines), for every target.
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);
    void resizeTextureDescriptors(uint32 numTextureDescriptors);
    void invalidateTextures(uint32 frameIdx); // rewrites the whole array at the next record

private:
    struct Target
    {
        vk::Framebuffer framebuffer;
        vk::Image colorImage;
        VmaAllocation colorMemory = nullptr;
        vk::ImageView colorView;
        vk::Image depthImage;
        VmaAllocation depthMemory = nullptr;
        vk::ImageView depthView;
        oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> sets;
        oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> writtenTextures{ UINT32_MAX, UINT32_MAX }; // how many array slots this slot's set holds
        // Per slot: vec4[0] = the camera, then the palette (4 vec4 per bone). Host-visible, grown on demand.
        oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> frameData;
        oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> frameDataVec4s{ 0, 0 };
    };

    void buildMeshLayout(GraphicsPipelineLayout& layout, uint32 maxTextures);
    void buildGridLayout(GraphicsPipelineLayout& layout);
    void createRenderPass();
    void destroyTarget(Target& target);
    uint32 writeFrameData(Target& target, uint32 frameIdx, const EntityPreviewFrame& frame); // returns the vec4 count written

    vk::RenderPass m_renderPass;
    Sampler m_colorSampler;
    Sampler m_textureSampler;
    uint32 m_numTextureDescriptors = 0;

    GraphicsPipeline m_meshPipeline; // variant 1 = wireframe
    GraphicsPipeline m_gridPipeline;
    oc::array<Target, PREVIEW_TARGET_COUNT> m_targets;
};
