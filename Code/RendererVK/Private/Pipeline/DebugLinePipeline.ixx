export module RendererVK:DebugLinePipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :GraphicsPipeline;
import :DescriptorSet;
import :Layout;

// World-space debug line overlay (physics collider wireframes, etc.), drawn into the main color pass
// after the forward pass (depth-tested against the scene, no depth writes). Lines are accumulated on
// the CPU each frame (Renderer::addDebugLine), copied into a mapped per-frame vertex buffer in
// present(), and drawn with one indirect call — the count rides in a mapped indirect buffer, so the
// cached secondary command buffer never re-records for line-count changes. GPU buffers are allocated
// lazily on first use (a debug feature shouldn't cost VRAM while off).
export class DebugLinePipeline final
{
public:
    struct LineVertex
    {
        glm::vec3 pos;
        uint32 color; // packed RGBA8, R in the low byte (GLSL unpackUnorm4x8)
    };

    void initialize(vk::RenderPass renderPass);
    void reloadShaders(vk::RenderPass renderPass);

    // Copies this frame's vertices into the frame slot's mapped buffers (call after the slot's fence
    // wait, i.e. from present()). Returns true when the lazy GPU buffers were just created so the
    // caller re-records its command buffers.
    bool upload(uint32 frameIdx, oc::span<const LineVertex> verts);

    bool hasBuffers() const { return m_buffersReady; }
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo);

private:
    void buildLayout(GraphicsPipelineLayout& layout);

    GraphicsPipeline m_pipeline;
    vk::RenderPass m_renderPass;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_vertexBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_indirectBuffers;
    oc::array<oc::span<LineVertex>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedVerts;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedIndirect; // vk::DrawIndirectCommand as 4 uints
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets;
    bool m_buffersReady = false;
    bool m_overflowWarned = false;
};
