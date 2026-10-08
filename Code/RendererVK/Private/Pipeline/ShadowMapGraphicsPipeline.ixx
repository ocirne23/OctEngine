export module RendererVK:ShadowMapGraphicsPipeline;

import Core;

import :VK;
import :Buffer;
import :Layout;
import :GraphicsPipeline;
import :IndirectCommandsLayout;
import :DescriptorSet;
import :Sampler;
import :ShadowMap;

class CommandBuffer;

// Depth-only pipeline that renders the shadow caster list into one cascade of the sun ShadowMap.
// Mirrors StaticMeshGraphicsPipeline: it consumes the cull's IndirectDrawSequence buffer through
// vkCmdExecuteGeneratedCommandsEXT so the draw path matches the main pass. All cascades render in one
// multiview render pass (gl_ViewIndex picks the cascade matrix); because multiview disallows an Indirect
// Execution Set, DGC here uses no execution set and binds the single depth-only pipeline directly.
export class ShadowMapGraphicsPipeline final
{
public:
    ShadowMapGraphicsPipeline();
    ~ShadowMapGraphicsPipeline();
    ShadowMapGraphicsPipeline(const ShadowMapGraphicsPipeline&) = delete;

    struct RecordParams
    {
        DescriptorSet& descriptorSet;
        Buffer& ubo;                   // 0 - main UBO (holds cascadeViewProj[]; cascade chosen by gl_ViewIndex)
        // The main cull's buffers, as the shadow cull wrote them this frame (it runs before the main cull).
        Buffer& meshInstanceBuffer;    // 1 - OutShadowMeshInstance per instance (carries the alpha-mask tex idx)
        Buffer& vertexBuffer;
        Buffer& indexBuffer;
        Buffer& instanceIdxBuffer;     // vertex binding 2 - the instance indices per draw
        Buffer& indirectCommandBuffer; // the IndirectDrawSequence list (the opaque one)
        Buffer& drawCountBuffer;       // uint32 compacted sequence count (DGC sequenceCountAddress), GPU-written by the cull
        Buffer& preprocessBuffer;      // the DGC scratch: the static mesh pass's opaque one, borrowed (sized for both)
    };

    void initialize(ShadowMap& shadowMap, uint32 maxUniqueMeshes, uint32 maxTextures);
    void reloadShaders(uint32 maxTextures);
    void record(CommandBuffer& commandBuffer, RecordParams& params);
    // Rewrites one slot of the texture array (binding 7) with a streamed texture's current view.
    void updateTextureDescriptor(vk::DescriptorSet descriptorSet, uint32 slotIdx, vk::ImageView view);
    // Asks the DGC preprocess requirement again for a grown unique-mesh capacity.
    void resizeMeshCapacity(uint32 maxUniqueMeshes);
    // What the scratch it borrows must hold (StaticMeshGraphicsPipeline::reserveOpaquePreprocess). Grow-only.
    vk::DeviceSize getPreprocessRequirement() const { return m_preprocessSize; }

    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_graphicsPipeline.getDescriptorSetLayout(); }

private:

    void buildPipelineLayout(GraphicsPipelineLayout& layout, uint32 maxTextures);
    void buildIndirectState(uint32 maxUniqueMeshes);
    void queryPreprocessSize(uint32 maxUniqueMeshes);

    GraphicsPipeline m_graphicsPipeline;
    IndirectCommandsLayout m_indirectCommandsLayout;
    Sampler m_sampler; // for the diffuse texture array used by the alpha-mask discard
    vk::RenderPass m_renderPass;

    vk::DeviceSize m_preprocessSize = 0;
    uint32 m_maxUniqueMeshes = 0; // what the requirement was last asked for (a shader reload asks again)
};
