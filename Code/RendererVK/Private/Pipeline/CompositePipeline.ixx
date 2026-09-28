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
        float exposureEV = 0.0f;    // exposure in stops; the shader gets exp2(exposureEV)
        int32 tonemapper = 0;       // 0 = off (raw clip), 1 = Reinhard, 2 = ACES, 3 = AgX
        int32 autoExposure = 0;     // 1 = multiply by the eye-adaptation exposure
        // Bloom (BloomPipeline): level 0 + its linear sampler (always bound), the mix (0 = off) and the
        // uv transform full-frame uv -> level 0's viewport region.
        vk::ImageView bloomView;
        vk::Sampler   bloomSampler;
        float bloomIntensity = 0.0f;
        float bloomNormalize = 1.0f; // 1 / the sum of the level weights (BloomPipeline::getNormalize)
        // true (a threshold is on): scene + blur x intensity - the blur holds only the light above the threshold.
        // false: the energy-conserving scene x (1 - intensity) + blur x intensity.
        bool bloomAdditive = false;
        glm::vec4 bloomUv = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        // The motion blur GATHER runs here (MotionBlurPipeline): its velocity + neighbour max (+ the sampler),
        // the scene depth (SCENE_DEPTH_SAMPLED_LAYOUT), the frame UBO. Always bound; mbSamples 0 = off.
        vk::ImageView mbVelocityView;
        vk::ImageView mbNeighborMaxView;
        vk::ImageView mbDepthView;
        vk::Sampler   mbSampler;
        uint32 mbSamples = 0;
        vk::Buffer ubo;
    };
    void record(CommandBuffer& commandBuffer, const RecordParams& params);

    vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_graphicsPipeline.getDescriptorSetLayout(); }

private:
    void buildPipelineLayout(GraphicsPipelineLayout& layout);

    // Push constants; must match the PostPC block in composite.fs.glsl.
    struct CompositePC
    {
        float exposure;   // linear scale, exp2 of the EV tweak
        int32 tonemapper;
        int32 autoExposure;
        float bloomKeep;  // 1 - bloom intensity
        glm::vec4 bloomUv;
        float bloomScale; // bloom intensity / level count
        uint32 mbSamples; // motion blur gather samples (0 = off)
    };
    static_assert(sizeof(CompositePC) == 40);

    GraphicsPipeline m_graphicsPipeline;
};
