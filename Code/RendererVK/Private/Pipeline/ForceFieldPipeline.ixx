export module RendererVK:ForceFieldPipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :GraphicsPipeline;
import :ComputePipeline;
import :DescriptorSet;
import :Layout;

// Forcefield bubbles (Force library): the render pipeline (ray-marched shells) plus the compute
// side (emitter hash-grid build, per-emitter applied forces, gameplay point queries).
//
// Draw: one instanced indirect draw of unit cubes (36 verts, front faces culled, fixed-function
// depth off — the camera can sit inside a bubble) inside the scene-color pass after the debug
// overlays; each instance is oriented to its emitter's directional reach box and the fragment
// shader ray-marches the analytic team field (force_field.inc.glsl), depth-testing manually
// against the G-buffer depth. The live emitter slots are compacted into a mapped per-frame buffer
// each present and instanceCount rides a mapped indirect buffer, so emitter changes never
// re-record the cached command buffers.
//
// Compute (recordCompute, after the light grid, outside any render pass): fill-clear + rebuild the
// uniform 32 m emitter hash grid (big emitters ride the emitter header's global list instead), then
// one thread per emitter integrates the opposing field pressure (13-sample own-field-weighted
// integral) and one thread per registered query point evaluates the per-team fields — both write
// SLOT-indexed results straight into host-visible readback buffers the CPU reads ~2 frames later
// (ocean-readback contract: read the current slot between beginFrame's fence wait and present).
//
// The FORCE_GRID define (setUseGrid) switches every consumer between hash-grid gathering and a
// brute-force scan (A-B correctness toggle); flipping it reloads the pipelines.
export class ForceFieldPipeline final
{
public:
    ForceFieldPipeline() = default;
    ~ForceFieldPipeline() = default;
    ForceFieldPipeline(const ForceFieldPipeline&) = delete;

    void initialize(vk::RenderPass sceneRenderPass, uint32 viewCount);
    void reloadShaders(vk::RenderPass sceneRenderPass);
    void setUseGrid(bool useGrid) { m_useGrid = useGrid; } // takes effect on the next reloadShaders
    bool getUseGrid() const { return m_useGrid; }

    // Compacts the ACTIVE emitter slots (building the big-emitter list against bigReachThreshold and
    // stamping each record's source slot for the readback), uploads the query slots, and patches the
    // draw/dispatch counts. Call from present(), after the slot's fence wait.
    void upload(uint32 frameIdx, oc::span<const RendererVKLayout::ForceEmitterGpu> slots,
        oc::span<const RendererVKLayout::ForceQueryGpu> querySlots, float bigReachThreshold);

    // Records grid clear + insert + force/query dispatches + readback barriers (outside any render
    // pass; ubo is the frame's UBO). All dispatches ride mapped indirect buffers, so emitter/query
    // count changes never re-record.
    void recordCompute(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo);

    struct DrawParams
    {
        Buffer& ubo;
        vk::ImageView gbufferDepthView;
        // DEPTH_STENCIL_READ_ONLY while depth-prepass reuse binds this image as the scene pass depth.
        vk::ImageLayout gbufferDepthLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::Sampler gbufferSampler;
    };
    // Records the indirect instanced box draw; the caller has begun a command buffer inside the
    // scene-color render pass and set the viewport/scissor. eye selects the per-eye set/views.
    void recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params);

    // This frame slot's readbacks, slot-indexed (safe between beginFrame's fence wait and present;
    // contents are ~2 frames old). Forces: xyz = applied force, w = mean opposing pressure.
    oc::span<const glm::vec4> getForceReadback(uint32 frameIdx) const { return m_mappedForceReadback[frameIdx]; }
    oc::span<const RendererVKLayout::ForceQueryResult> getQueryReadback(uint32 frameIdx) const { return m_mappedQueryReadback[frameIdx]; }

    // Grid capacity contract (checkForceGridCapacity): last frame's demand counters, and growth.
    struct GridDemand { uint32 numCells; uint32 dataCounter; };
    GridDemand getGridDemand(uint32 frameIdx);
    uint32 getTableEntries() const { return m_tableEntries; }
    size_t getGridDataSize() const { return m_gridDataSize; }
    // Recreates the per-frame grid buffers at the new sizes (caller has drained the GPU and will
    // re-record; per-frame scratch rebuilt every frame, nothing to preserve).
    void growGridBuffers(size_t neededDataBytes, uint32 neededTableEntries);

private:
    void buildDrawLayout(GraphicsPipelineLayout& layout);
    void buildComputeLayout(ComputePipelineLayout& layout, const char* shaderPath);
    void createGridBuffers();

    GraphicsPipeline m_pipeline;
    ComputePipeline m_gridPipeline;
    ComputePipeline m_emitterForcePipeline;
    ComputePipeline m_queryPipeline;
    bool m_useGrid = true;

    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitterBuffers;
    oc::array<oc::span<RendererVKLayout::ForceEmittersGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedEmitters;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_queryBuffers;
    oc::array<oc::span<RendererVKLayout::ForceQueriesGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedQueries;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_indirectBuffers;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedIndirect; // DrawIndirectCommand + 2x DispatchIndirectCommand
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_gridTableBuffers; // DeviceLocal|HostVisible: header demand readback
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_gridDataBuffers;
    // GPU-written readbacks (HostVisible|HostCoherent storage, persistently mapped, zeroed at init).
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_forceReadbackBuffers;
    oc::array<oc::span<glm::vec4>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedForceReadback;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_queryReadbackBuffers;
    oc::array<oc::span<RendererVKLayout::ForceQueryResult>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedQueryReadback;

    uint32 m_tableEntries = RendererVKLayout::INITIAL_FORCE_TABLE_ENTRIES;
    size_t m_gridDataSize = RendererVKLayout::INITIAL_FORCE_GRID_DATA_SIZE;

    static constexpr uint32 MAX_VIEWS = 2;
    static uint32 drawSlot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS> m_drawSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_gridSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitterForceSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_querySets;
    uint32 m_viewCount = 1;

    // Offsets into the per-frame indirect buffer (uints): [0..3] draw, [4..6] grid insert groups
    // (x = emitter COUNT — the insert runs single-thread workgroups, see force_grid.cs.glsl),
    // [8..10] force groups, [12..14] query groups.
    static constexpr uint32 DRAW_CMD_OFFSET = 0;
    static constexpr uint32 GRID_DISPATCH_OFFSET = 4;
    static constexpr uint32 EMITTER_DISPATCH_OFFSET = 8;
    static constexpr uint32 QUERY_DISPATCH_OFFSET = 12;
    static constexpr uint32 INDIRECT_UINTS = 16;
};
