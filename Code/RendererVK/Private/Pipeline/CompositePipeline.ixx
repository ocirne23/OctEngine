export module RendererVK:CompositePipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :GraphicsPipeline;
import :DescriptorSet;
import :RenderPass;

// Fullscreen composite: samples the TAA-resolved scene colour and writes it into the swapchain (drawn
// before ImGui in the swapchain render pass). This is the HDR -> display mapping: exposure and the
// selected tonemap operator run here (composite.fs.glsl), everything upstream is linear radiance.
// No vertex buffers; a single combined-image-sampler binding plus a small push-constant block.
export class CompositePipeline final
{
public:
    void initialize(const RenderPass& renderPass);
    void reloadShaders(const RenderPass& renderPass);

    struct RecordParams
    {
        DescriptorSet& descriptorSet;
        vk::ImageView resolvedView; // scene colour to tonemap (TAA-resolved, or a SceneColor eye layer in VR)
        vk::ImageLayout resolvedLayout = vk::ImageLayout::eGeneral; // GENERAL for TAA, SHADER_READ_ONLY for SceneColor
        vk::Sampler   sampler;
        vk::Buffer    exposureBuffer; // eye-adaptation { avgLum, exposure } (storage)
        // Bloom (BloomPipeline): level 0 + its linear sampler (always bound) and the uv transform full-frame uv ->
        // level 0's viewport region. The exposure, the tonemapper and the bloom mix ride the frame UBO (u_post).
        vk::ImageView bloomView;
        vk::Sampler   bloomSampler;
        glm::vec4 bloomUv = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        // The motion blur GATHER runs here (MotionBlurPipeline): its velocity + neighbour max (+ the sampler),
        // the scene depth (SCENE_DEPTH_SAMPLED_LAYOUT), the frame UBO. Always bound; u_post_mbSamples 0 = off.
        vk::ImageView mbVelocityView;
        vk::ImageView mbNeighborMaxView;
        vk::ImageView mbDepthView;
        vk::Sampler   mbSampler;
        vk::Buffer ubo;
    };
    void record(CommandBuffer& commandBuffer, const RecordParams& params);

    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_graphicsPipeline.getDescriptorSetLayout(); }

private:
    void buildPipelineLayout(GraphicsPipelineLayout& layout);

    // Push constants; must match the PostPC block in composite.fs.glsl.
    struct CompositePC
    {
        glm::vec4 bloomUv;
    };
    static_assert(sizeof(CompositePC) == 16);

    GraphicsPipeline m_graphicsPipeline;
};
