export module RendererVK:BeamCardPipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :GraphicsPipeline;
import :DescriptorSet;
import :Layout;

// BEAM CARDS: a beam's glow in the air (Renderer::addBeamCard) drawn as ONE camera-facing quad around the beam, in the
// scene-colour pass right after the fog apply, added to the scene (blend ONE, ONE). Its fragment shader runs the fog
// apply's column glow (beam_glow.inc.glsl) for that one beam, so a card looks like a column - but a column costs every
// pixel of the screen and a card only the pixels it covers (Shaders/Sky/beam_card.*.glsl). No depth test: the shader
// cuts the glow at the scene depth itself, as the columns do.
// The cards are copied into a mapped per-slot buffer in present() and drawn with ONE indirect call (the vertex count
// rides in a mapped indirect buffer), so the cached secondary never re-records for a count change. One descriptor set
// per slot and eye (the depth and the clouds are per eye).
export class BeamCardPipeline final
{
public:
    static constexpr uint32 MAX_CARDS = 256;

    void initialize(vk::RenderPass renderPass, uint32 viewCount);
    void reloadShaders(vk::RenderPass renderPass);

    // The frame's cards into this slot's mapped buffer (call after the slot's fence wait, from present()). Beams with no
    // length or range are skipped.
    void upload(uint32 frameIdx, oc::span<const RendererVKLayout::LightInfo> beams);
    uint32 count(uint32 frameIdx) const { return m_counts[frameIdx]; }

    struct RecordParams
    {
        Buffer& ubo;
        vk::ImageView sceneDepthView;
        vk::ImageLayout sceneDepthLayout;
        vk::Sampler sceneDepthSampler;
        vk::ImageView cloudColorView;
        vk::ImageView cloudDepthView;
        vk::Sampler cloudSampler;
    };
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const RecordParams& params);

private:
    // One card: the shader's 4 vec4 (beam_card.vs.glsl).
    struct Card
    {
        glm::vec4 footRadius; // xyz = the beam's start, w = the core radius
        glm::vec4 axisLength; // xyz = the unit axis, w = the length
        glm::vec4 colorRange; // rgb = the colour x intensity, w = the halo's range
        glm::vec4 fade;       // x = the tip fade (m)
    };
    void buildLayout(GraphicsPipelineLayout& layout);
    uint32 slot(uint32 frameIdx, uint32 eye) const { return frameIdx * m_viewCount + eye; }

    GraphicsPipeline m_pipeline;
    uint32 m_viewCount = 1;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_cardBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_indirectBuffers;
    oc::array<oc::span<Card>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedCards;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedIndirect; // vk::DrawIndirectCommand as 4 uints
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_counts{};
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * 2> m_sets; // [slot][eye]
};
