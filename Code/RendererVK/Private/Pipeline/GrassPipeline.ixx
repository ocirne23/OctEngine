export module RendererVK:GrassPipeline;

import Core;
import Core.glm;
import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :GraphicsPipeline;
import :DescriptorSet;
import :Sampler;
import :Layout;

// PROCEDURAL GRASS, its compute half and its buffers (the blades draw in StaticMeshGraphicsPipeline: they need the
// lit core's descriptor set). The model is in grass.inc.glsl: the blades are ranked per base cell of a world grid, and
// drawn in RING CELLS around the camera (RendererVKLayout::GRASS_RING_*), each the first K blades of its LOD's mesh.
//
// Per frame slot: the FRAME TABLE (host-visible: the ring levels + the terrain chunks under them, written in present),
// and the cull's outputs - the cell records (instance-rate vertex attributes) and two lists of one indexed draw per
// CELL SLOT (the main view's; the NEAR GRASS CASCADE's casters - in view or not, around its box): every slot is
// written each frame, an empty draw where the cell is culled, so the lists are in the slots' order - near to far.
// The cull (grass_cull.cs.glsl) is a cached secondary: one thread per slot.
// THE BLADE MESH (the index buffer of vertex ids): one section per LOD, sized for the ring cell that draws the most
// blades at that LOD (setMesh: the renderer's ring layout) - a cell never draws past its ring's worst case.
// The near CASTERS draw here too (recordNearShadow): the blade VS's GRASS_SHADOW + GRASS_NEAR_SHADOW variant, depth
// only, into the near grass cascade's own map (it binds the UBO, the vertex mega-buffer and the noise only - its own set). The
// blades never cast into the scene cascades.
export class GrassPipeline final
{
public:
    GrassPipeline() = default;
    ~GrassPipeline() = default;
    GrassPipeline(const GrassPipeline&) = delete;

    using LodBlades = oc::array<uint32, RendererVKLayout::GRASS_LODS>; // blades in each LOD's section of the mesh
    void initialize(uint32 bladesPerPatch, const LodBlades& lodBlades);
    // The near grass cascade's caster pipeline: against its ShadowMap's pass.
    void initializeNearShadow(vk::RenderPass nearShadowRenderPass);
    void reloadShaders();
    // The blades per base cell and the mesh sections. Rebuilds the index buffer when a section changes; true then (the
    // caller has idled the GPU and re-records: the draw binds the buffer).
    bool setMesh(uint32 bladesPerPatch, const LodBlades& lodBlades);
    uint32 getBladesPerPatch() const { return m_bladesPerPatch; }
    uint32 lodBlades(uint32 lod) const { return m_lodBlades[lod]; }
    uint32 lodFirstIndex(uint32 lod) const { return m_lodFirstIndex[lod]; }

    struct RecordParams
    {
        Buffer* ubo = nullptr;     // that frame slot's main UBO
        vk::ImageView terrainView; // the baked terrain-data cascades (climate, water level)
        vk::Sampler terrainSampler;
        Buffer* vertexBuffer = nullptr; // the vertex mega-buffer (the terrain chunks' vertices)
        Buffer* clutterFrame = nullptr; // that frame slot's ClutterFrameGpu (the forest floor map: "Canopy thinning")
        vk::ImageView noiseView;        // the terrain noise texture (TerrainResources): the splat's crag wander, the grass value noise
        vk::ImageView snowSiteTopView;  // the snow site map (SnowSitePipeline): the splat's snow shelter
        vk::ImageView snowSitePassView;
        vk::Sampler snowSiteSampler;    // the site's own (linear, repeat: the toroidal wrap)
    };
    // The cull: the draw counts (every slot), dispatch, and the barrier to the draw (indirect + vertex attribute reads).
    void record(CommandBuffer& commandBuffer, uint32 frameIdx, const RecordParams& params);
    // A frame without the cull: no draws (the recorded draws read the counts).
    void recordClear(vk::CommandBuffer primary, uint32 frameIdx);
    // The near grass cascade's casters into its extra-layer pass (inside its secondary; the viewport is set).
    // noiseView: the terrain noise texture (the grass value noise).
    void recordNearShadow(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo, Buffer& vertexBuffer, vk::ImageView noiseView);
    // Points the terrain-data binding (UPDATE_AFTER_BIND) at the active ping-pong image.
    void updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler);

    // This slot's ground table (present, after the slot's fence). The clutter cull reads it too (frameBuffer).
    RendererVKLayout::GrassFrameGpu& frame(uint32 frameIdx) { return *m_mappedFrames[frameIdx]; }
    Buffer& frameBuffer(uint32 frameIdx) { return m_frames[frameIdx]; }
    void flushFrame(uint32 frameIdx);

    struct Draw
    {
        Buffer* patches = nullptr;  // GrassPatchGpu per cell slot (instance-rate vertex binding 0)
        Buffer* commands = nullptr; // VkDrawIndexedIndirectCommand per cell slot
        Buffer* count = nullptr;    // the draw counts (the slots, or 0 without the cull): the main one is the uint at 0
        Buffer* indices = nullptr;  // the blade mesh: vertex ids, a section per LOD
        uint32 maxDraws = 0;        // the cell slots
        uint32 simpleFirst = 0;     // the slots from here draw with the simple fragment shader (the caller's: the far rings)
    };
    Draw getDraw(uint32 frameIdx);

private:
    void buildLayout(ComputePipelineLayout& layout);
    void buildNearShadowLayout(GraphicsPipelineLayout& layout);
    void buildIndices();

    ComputePipeline m_pipeline;
    GraphicsPipeline m_nearShadowPipeline;
    vk::RenderPass m_nearShadowRenderPass;
    bool m_nearShadowBuilt = false;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_nearShadowSets;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_nearShadowCommands;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_sets;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_frames;
    oc::array<RendererVKLayout::GrassFrameGpu*, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedFrames{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_patches;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_commands;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_counts;
    Buffer m_indices;
    Sampler m_noiseSampler; // repeat: the noise texture tiles
    uint32 m_bladesPerPatch = 0;
    LodBlades m_lodBlades{};
    LodBlades m_lodFirstIndex{};
};
