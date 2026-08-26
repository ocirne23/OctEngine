export module RendererVK:ForceFieldPipeline;

import Core;
import Core.glm;
import Core.Frustum;

import :VK;
import :Allocator;
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
    ~ForceFieldPipeline(); // frees the shell-volume images (buffers/pipelines are RAII members)
    ForceFieldPipeline(const ForceFieldPipeline&) = delete;

    void initialize(vk::RenderPass sceneRenderPass, uint32 viewCount);
    void reloadShaders(vk::RenderPass sceneRenderPass);
    void setUseGrid(bool useGrid) { m_useGrid = useGrid; } // takes effect on the next reloadShaders
    bool getUseGrid() const { return m_useGrid; }

    // SHELL DRAW CULLING (upload-time, CPU): a drawable shell outside the view frustum, or whose
    // projected proxy radius is under minPixels, is compacted into the NON-drawn field partition
    // instead — its field, grid presence and readbacks are untouched, only the ray-march draw is
    // skipped. Frustum + camera come from the CENTER view (TAA jitter never bakes into it);
    // disabled in VR (one center frustum cannot serve both eyes).
    struct ShellCull
    {
        bool enabled = false;
        Frustum frustum;
        glm::vec3 cameraPos{ 0.0f };
        float pixelScale = 0.0f; // px per unit of (radius / distance): viewportH/2 / tan(fov/2)
        float minPixels = 0.0f;  // projected proxy radius below this skips the draw (0 = size cull off)
        bool bakeVolume = false; // a large emitter qualified this frame: dispatch the shell-volume
                                 // bake (the UBO's forceBake0/1 carry the fitted mapping)
    };

    // Compacts the ACTIVE emitter slots (building the big-emitter list against bigReachThreshold and
    // stamping each record's source slot for the readback; FORCE_FLAG_PASSIVE slots land in a tail
    // past the field count that only the force compute evaluates), uploads the query slots, and
    // patches the draw/dispatch counts. Call from present(), after the slot's fence wait.
    void upload(uint32 frameIdx, oc::span<const RendererVKLayout::ForceEmitterGpu> slots,
        oc::span<const RendererVKLayout::ForceQueryGpu> querySlots,
        oc::span<const glm::ivec4> bakeBricks, float bakeSampleY, float bigReachThreshold,
        const ShellCull& shellCull);

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
    // The baked pressure field of THIS frame slot: the data is ~2 frames old, so the brick list it
    // was evaluated for is returned WITH it (the per-slot copy stored at upload) — the pairing the
    // CPU-side sampler indexes by.
    RendererVKLayout::ForceBakeReadback getBakeReadback(uint32 frameIdx) const
    {
        return { m_bakeBrickLists[frameIdx], m_mappedBakeReadback[frameIdx] };
    }

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

    void buildShellBakeLayout(ComputePipelineLayout& layout); // storage IMAGES at 5/6, unlike the rest
    void createShellVolume(); // the two 3D field textures + sampler (one set: barrier-serialized)
    void destroyShellVolume();

    GraphicsPipeline m_pipeline;
    ComputePipeline m_gridPipeline;
    ComputePipeline m_emitterForcePipeline;
    ComputePipeline m_queryPipeline;
    ComputePipeline m_bakePipeline;
    ComputePipeline m_shellBakePipeline;
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
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeBrickBuffers;
    oc::array<oc::span<RendererVKLayout::ForceBakeBricksGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedBakeBricks;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeReadbackBuffers;
    oc::array<oc::span<glm::vec4>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedBakeReadback;
    oc::array<oc::vector<glm::ivec4>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeBrickLists; // per-slot pairing (see getBakeReadback)
    // The sampled shell tier's field volumes (RendererVKLayout::FORCE_SHELL_VOLUME_*): ONE set,
    // not per frame slot — the bake's acquire barrier (prev fragment reads -> compute writes)
    // serializes reuse on the queue. GENERAL layout for life.
    vk::Image m_shellVolumeImage[2]{};
    VmaAllocation m_shellVolumeMemory[2]{};
    vk::ImageView m_shellVolumeView[2]{};
    vk::Sampler m_shellVolumeSampler; // linear, clamp-to-border transparent black (outside = zero field)

    uint32 m_tableEntries = RendererVKLayout::INITIAL_FORCE_TABLE_ENTRIES;
    size_t m_gridDataSize = RendererVKLayout::INITIAL_FORCE_GRID_DATA_SIZE;

    static constexpr uint32 MAX_VIEWS = 2;
    static uint32 drawSlot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS> m_drawSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_gridSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitterForceSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_querySets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_shellBakeSets;
    uint32 m_viewCount = 1;

    // Offsets into the per-frame indirect buffer (uints): [0..3] draw, [4..6] grid insert groups
    // (x = emitter COUNT — the insert runs single-thread workgroups, see force_grid.cs.glsl),
    // [8..10] force groups, [12..14] query groups, [16..18] bake groups (x = brick count),
    // [20..22] shell-volume bake groups (x = 0 disables — the CB is cached, so the toggle rides here).
    static constexpr uint32 DRAW_CMD_OFFSET = 0;
    static constexpr uint32 GRID_DISPATCH_OFFSET = 4;
    static constexpr uint32 EMITTER_DISPATCH_OFFSET = 8;
    static constexpr uint32 QUERY_DISPATCH_OFFSET = 12;
    static constexpr uint32 BAKE_DISPATCH_OFFSET = 16;
    static constexpr uint32 SHELLBAKE_DISPATCH_OFFSET = 20;
    static constexpr uint32 INDIRECT_UINTS = 24;
};
