export module RendererVK:SnowCapPipeline;

import Core;
import Core.glm;
import :VK;
import :Buffer;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;
import :PushBlock;

// THE SNOW CAPS (Snow.md "The caps"): the snow's VOLUME on the object tops - a heightfield over the snow site map's texels
// (0.25 m), drawn in the opaque pass before the opaque set (the tops under a cap then fail early depth). Its compute half
// and its buffers; the draw is StaticMeshGraphicsPipeline's (it needs the lit core's set).
// THE PREPARE (snow_cap_prepare.cs.glsl, inline in the primary right after the site trace, every frame): one workgroup per
// 8 m TILE of the site window (SNOW_CAP_TILES^2 slots) within "Cap range" + a tile. It writes one WORD per site texel (the
// cap's height over the top + its quad bits; RendererVKLayout::SNOW_CAP_*) - every cap vertex reads only these and the
// tops, so the vertices that share a texel always agree (no cracks inside a tile, between tiles or LODs) - and appends
// each visible tile with a cap to its LOD's draw list (16 B records, read by gl_InstanceIndex by device address), and its
// LOD to the LOD TABLE (one uint per tile slot, reset each frame: the VS stitches a border vertex to a coarser neighbour).
// THE DRAW: one indexed indirect draw per LOD (SNOW_CAP_LODS): (32 >> lod)^2 quads of 4 vertices of their own (a dropped
// quad collapses to a point), a static index buffer, no vertex buffer (snow_cap.vs.glsl).
// The words: one buffer (4 MB), written after every earlier frame's reads (the barrier at the start of the prepare).
export class SnowCapPipeline final
{
public:
    SnowCapPipeline() = default;
    SnowCapPipeline(const SnowCapPipeline&) = delete;

    void initialize();
    void reloadShaders();

    struct RecordParams
    {
        vk::Buffer ubo;               // that slot's frame UBO
        vk::ImageView terrainView;    // the terrain-data cascades (the climate, the water level)
        vk::Sampler terrainSampler;
        vk::ImageView siteTopView;    // the snow site map (texelFetch: the sampler's filter is unused)
        vk::ImageView sitePassView;
        vk::ImageView siteClassView;  // R8UI storage, GENERAL
        vk::Sampler siteSampler;
        vk::ImageView deformCoarseView; // the deformation's coarse map (its R16G16 view): the tracks on the caps
        glm::ivec2 siteOrigin{ 0 };   // the window the site map HOLDS (its last recorded trace)
    };
    // Into the primary (desktop), after the site trace, on a live frame: the draws reset, the prepare, the barrier to the draw.
    void record(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params);
    // A frame without the caps: no draws.
    void recordClear(vk::CommandBuffer cmd, uint32 frameIdx);

    uint64 wordsAddress() const { return m_words.getDeviceAddress(); }
    uint64 drawsAddress(uint32 frameIdx) const { return m_draws[frameIdx].getDeviceAddress(); }
    uint64 lodsAddress(uint32 frameIdx) const { return m_lods[frameIdx].getDeviceAddress(); } // the LOD table (by tile slot)

    struct Draw
    {
        Buffer* commands = nullptr; // SNOW_CAP_LODS x VkDrawIndexedIndirectCommand
        Buffer* indices = nullptr;  // uint16: quad q = vertices 4q .. 4q + 3
    };
    Draw getDraw(uint32 frameIdx) { return Draw{ &m_commands[frameIdx], &m_indices }; }

private:
    void buildLayout(ComputePipelineLayout& layout);
    void resetCommands(vk::CommandBuffer cmd, uint32 frameIdx);

    struct PreparePush
    {
        PushGroup g;
        PushValue<glm::ivec2> origin = g("origin"); // the site window held (lattice min corner)
    };
    PushBlock m_block;
    PreparePush m_push{ { m_block } };
    ComputePipeline m_pipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets;
    Buffer m_words;
    bool m_wordsCleared = false; // the first record zeroes the words
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_draws;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_lods; // per tile slot: its drawn LOD, 0xFFFFFFFF = none (the VS's stitch)
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_commands;
    Buffer m_indices;
};
