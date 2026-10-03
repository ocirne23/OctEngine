export module RendererVK:GrassPipeline;

import Core;
import Core.glm;
import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// PROCEDURAL GRASS, its compute half and its buffers (the blades draw in StaticMeshGraphicsPipeline: they need the
// lit core's descriptor set). The model is in grass.inc.glsl: a grid of patches around the camera, each drawn as the
// first K blades of ONE ranked blade mesh.
//
// Per frame slot: the GROUND TABLE (host-visible: the patch grid + the terrain chunks under it, written in present by
// writeFrame), and the cull's outputs - the patch records (instance-rate vertex attributes), one indexed draw command
// per patch, and their count. The cull (grass_cull.cs.glsl) is a cached secondary: its dispatch covers every patch
// slot, and threads past the grid return.
export class GrassPipeline final
{
public:
    GrassPipeline() = default;
    ~GrassPipeline() = default;
    GrassPipeline(const GrassPipeline&) = delete;

    void initialize(uint32 bladesPerPatch);
    void reloadShaders();
    // Rebuilds the blade index buffer (the caller idles the GPU and re-records: the draw binds it).
    void setBladesPerPatch(uint32 bladesPerPatch);
    uint32 getBladesPerPatch() const { return m_bladesPerPatch; }

    struct RecordParams
    {
        Buffer* ubo = nullptr;     // that frame slot's main UBO
        vk::ImageView terrainView; // the baked terrain-data cascades (climate, water level)
        vk::Sampler terrainSampler;
        Buffer* vertexBuffer = nullptr; // the vertex mega-buffer (the terrain chunks' vertices)
    };
    // The cull: count reset, dispatch, and the barrier to the draw (indirect + vertex attribute + index reads).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);
    // A frame without the cull: no draws (the recorded draw reads the count).
    void recordClear(vk::CommandBuffer primary, uint32 frameIdx);
    // Points the terrain-data binding (UPDATE_AFTER_BIND) at the active ping-pong image.
    void updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler);

    // This slot's ground table (present, after the slot's fence).
    RendererVKLayout::GrassFrameGpu& frame(uint32 frameIdx) { return *m_mappedFrames[frameIdx]; }
    void flushFrame(uint32 frameIdx);

    struct Draw
    {
        Buffer* patches = nullptr;  // GrassPatchGpu per draw (instance-rate vertex binding 0)
        Buffer* commands = nullptr; // VkDrawIndexedIndirectCommand per draw
        Buffer* count = nullptr;    // the draw count (uint at 0)
        Buffer* indices = nullptr;  // the blade mesh: vertex ids, the LODs one after another
        uint32 maxDraws = 0;
    };
    Draw getDraw(uint32 frameIdx);

private:
    void buildLayout(ComputePipelineLayout& layout);
    void buildIndices();

    ComputePipeline m_pipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_frames;
    oc::array<RendererVKLayout::GrassFrameGpu*, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedFrames{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_patches;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_commands;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_counts;
    Buffer m_indices;
    uint32 m_bladesPerPatch = 0;
};
