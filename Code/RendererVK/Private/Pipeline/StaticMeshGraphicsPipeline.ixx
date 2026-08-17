export module RendererVK:StaticMeshGraphicsPipeline;

import Core;

import :VK;
import :Buffer;
import :Layout;
import :GraphicsPipeline;
import :IndirectExecutionSet;
import :IndirectCommandsLayout;
import :DescriptorSet;
import :Texture;
import :Sampler;

class CommandBuffer;
class ObjectContainer;

export class StaticMeshGraphicsPipeline final
{
public:
    StaticMeshGraphicsPipeline();
    ~StaticMeshGraphicsPipeline();
    StaticMeshGraphicsPipeline(const StaticMeshGraphicsPipeline&) = delete;

    struct RecordParams
    {
        DescriptorSet& descriptorSet;
        Buffer& ubo;
        Buffer& vertexBuffer;
        Buffer& indexBuffer;
        Buffer& materialInfoBuffer;
        Buffer& instanceIdxBuffer;
        Buffer& meshInstanceBuffer;
        Buffer& indirectCommandBuffer;            // opaque draw sequences
        Buffer& transparentIndirectCommandBuffer; // transparent draw sequences
        Buffer& meshCountBuffer;                  // uint32 live mesh count (DGC sequenceCountAddress), CPU-written per frame

        Buffer& lightInfosBuffer;
		Buffer& lightGridsBuffer;
		Buffer& lightTableBuffer;

		Buffer& giGridDataBuffer;    // GI probe clipmap SH volume (read in the fragment shader)

		Buffer& meshInfoBuffer;        // shadow-ray alpha test (firstIndex/vertexOffset per mesh)
		Buffer& rtMeshInstancesBuffer; // the unculled instance list the TLAS records index into

		vk::ImageView shadowMapView;       // sun cascaded shadow map (e2DArray depth)
		vk::Sampler shadowMapSampler;      // comparison sampler for PCF
		vk::Sampler shadowMapDepthSampler; // non-comparison sampler for the PCSS blocker search

		vk::ImageView gbufferDepthView;    // full-res depth (AO bilateral upsample edge weights)
		vk::Sampler gbufferSampler;

		vk::ImageView oceanMapsView;       // FFT ocean displacement/gradient array (VS displacement + FS shading)
		vk::Sampler oceanMapsSampler;

		uint32 viewIndex = 0; // selects u_views[viewIndex] via push constant (0 = centre/desktop, 1/2 = eyes)
    };

    void initialize(vk::RenderPass renderPass, uint32 maxUniqueMeshes, uint32 maxTextures, bool stereo = false);
    void reloadShaders(vk::RenderPass renderPass, uint32 maxTextures);
    void resizeMeshCapacity(uint32 maxUniqueMeshes);
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& params, bool updateDescriptors = true);
    void updateAODescriptor(vk::DescriptorSet descriptorSet, vk::ImageView aoView, vk::Sampler aoSampler);
    void updateTlasDescriptor(vk::DescriptorSet descriptorSet, vk::AccelerationStructureKHR tlas);
    // Rewrites one slot of the texture array (binding 20) with a streamed texture's current view.
    void updateTextureDescriptor(vk::DescriptorSet descriptorSet, uint32 slotIdx, vk::ImageView view);
    // Points the terrain-data cascade binding (19) at the active ping-pong image (refreshed per frame).
    void updateTerrainHeightDescriptor(vk::DescriptorSet descriptorSet, vk::ImageView terrainView, vk::Sampler terrainSampler);
    void update(uint32 frameIdx, oc::vector<ObjectContainer*>& objectContainers);
    // Ocean variant: light refraction/reflection ray hits with the grid lights (OCEAN_HIT_LIGHTS define).
    // Takes effect on the next reloadShaders (the Renderer reloads when the tweak flips).
    void setOceanHitLights(bool enabled) { m_oceanHitLights = enabled; }
    // Global wireframe ("Renderer/Wireframe" tweak): scene variants rasterize as lines.
    // Takes effect on the next reloadShaders (the Renderer reloads when the tweak flips).
    void setWireframe(bool enabled) { m_wireframe = enabled; }
    // Depth-prepass reuse: the scene pass binds the G-buffer prepass depth READ-ONLY, so no scene variant
    // may write depth (rebuild via reloadShaders after flipping, like wireframe). Default matches the
    // Renderer's m_depthPrepassReuse default.
    void setDepthReadOnly(bool readOnly) { m_depthReadOnly = readOnly; }
    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_graphicsPipeline.getDescriptorSetLayout(); }
    const IndirectExecutionSet& getIndirectExecutionSet() const { return m_indirectExecutionSet; }
    const IndirectCommandsLayout& getIndirectCommandsLayout() const { return m_indirectCommandsLayout; }

private:

    void buildPipelineLayout(GraphicsPipelineLayout& layout, uint32 maxTextures);

    GraphicsPipeline m_graphicsPipeline;
    IndirectExecutionSet m_indirectExecutionSet;
    IndirectCommandsLayout m_indirectCommandsLayout;
    Sampler m_sampler;
    vk::RenderPass m_renderPass;
    bool m_stereo = false;
    bool m_oceanHitLights = false; // OCEAN_HIT_LIGHTS define on the ocean fragment variant
    bool m_wireframe = false;      // global wireframe: scene variants get vk::PolygonMode::eLine
    bool m_depthReadOnly = true;   // depth-prepass reuse: all scene variants depthWrite off (read-only depth attachment)

    vk::DeviceSize m_preprocessSize = 0;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_preprocessBuffers;            // opaque pass
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_transparentPreprocessBuffers; // transparent pass

    void createPreprocessBuffers(uint32 maxUniqueMeshes);
    void recordExecuteGeneratedCommands(vk::CommandBuffer vkCommandBuffer, Buffer& indirectCommandBuffer, Buffer& preprocessBuffer, Buffer& meshCountBuffer);
};