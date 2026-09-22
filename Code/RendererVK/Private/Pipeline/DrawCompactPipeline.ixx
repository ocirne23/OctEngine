export module RendererVK:DrawCompactPipeline;

import Core;

import :VK;
import :Buffer;
import :ComputePipeline;

// Compacts a cull's per-mesh-slot draw sequences in place and writes one draw count per list
// (draw_compact.cs.glsl). One workgroup, no descriptor set: the buffers go in as device addresses.
export class DrawCompactPipeline final
{
public:
    static constexpr uint32 MAX_LISTS = 4;

    void initialize(uint32 numLists);
    void reloadShaders();
    // slotCount: [0] = the registered mesh count. lists: the cull's sequence buffers (compacted in place).
    // drawCounts: numLists uint32s, one per list. Record between the cull's dispatch and the draws' barrier;
    // it syncs with the cull itself.
    void record(vk::CommandBuffer cb, const Buffer& slotCount, oc::span<Buffer* const> lists, const Buffer& drawCounts);

private:
    void buildLayout(ComputePipelineLayout& layout);

    ComputePipeline m_pipeline;
    uint32 m_numLists = 1;
};
