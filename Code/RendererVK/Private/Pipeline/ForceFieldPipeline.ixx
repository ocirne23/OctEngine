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
import :GridClaim;
import :Layout;

// Forcefield bubbles (Force library): the render pipeline (ray-marched shells) plus the compute
// side (per-emitter applied forces, gameplay point queries) and the CPU-built emitter hash grid.
//
// Draw: one instanced indirect draw of unit cubes (36 verts, front faces culled, fixed-function
// depth off - the camera can sit inside a bubble) inside the scene-color pass after the debug
// overlays; each instance is oriented to its emitter's directional reach box and the fragment
// shader ray-marches the analytic team field (force_field.inc.glsl), depth-testing manually
// against the G-buffer depth. The live emitter slots are compacted into a mapped per-frame buffer
// each present and instanceCount rides a mapped indirect buffer, so emitter changes never
// re-record the cached command buffers.
//
// The emitter hash grid (uniform 16 m cells) is BUILT ON THE CPU, off the main thread: the
// renderer's grid job runs upload (the compaction) then buildGrid - lock-free cell claims over the
// compacted field emitters on a parallelFor (GridClaim / GridTouches, shared with the light grid),
// a counting sort into fixed-capacity cell records - and uploadGrid writes the cell data + the
// hash table straight into the (host-visible) buffers every force shader reads. No insert shader.
// Growth is synchronous and exact (GridDemand from buildGrid, growGridBuffers on the main thread
// after the join when it does not fit, then uploadGrid).
//
// Compute (recordCompute, after the light grid, outside any render pass): one thread per emitter
// integrates the opposing field pressure (13-sample own-field-weighted integral) and one thread per
// registered query point evaluates the per-team fields - both write SLOT-indexed results straight
// into host-visible readback buffers the CPU reads ~2 frames later (ocean-readback contract: read
// the current slot between beginFrame's fence wait and present).
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
    // DEBUG density view (FORCE_DENSITY_VIEW define in force_shell.fs) - rebuild-class like useGrid,
    // so the release shader carries none of the march: caller is GPU-idle and follows with reloadShaders.
    void setDensityView(bool enabled) { m_densityView = enabled; }
    bool getDensityView() const { return m_densityView; }
    // LIVE team count (the NUM_FORCE_TEAMS shader define): remakes the team-sized resources (shell
    // volume: ONE RGBA16F texture at <= 4 teams instead of two; bake readback stride) - caller
    // guarantees GPU idle and follows with reloadShaders (the useGrid toggle pattern).
    void setNumTeams(uint32 numTeams);
    uint32 getNumTeams() const { return m_numTeams; }
    // HALF-RES union march toggle (rebuild-class, like useGrid): ON = the march runs in its own
    // half-res pass and the scene stage upsamples; OFF = the march draws directly into scene
    // color at full res (no march framebuffer exists at all). Caller is GPU-idle and follows
    // with reloadShaders + resizeIntervalTarget (the targets change size/existence).
    void setUnionHalfRes(bool halfRes) { m_unionHalfRes = halfRes; }
    bool getUnionHalfRes() const { return m_unionHalfRes; }
    // March-phase jitter (FORCE_UNION_JITTER define in force_union.fs) - rebuild-class, no
    // resource changes: caller is GPU-idle and follows with reloadShaders.
    void setUnionJitter(bool jitter) { m_unionJitter = jitter; }
    bool getUnionJitter() const { return m_unionJitter; }

    // SHELL DRAW CULLING (upload-time, CPU): a drawable shell outside the view frustum, or whose
    // projected proxy radius is under minPixels, is compacted into the NON-drawn field partition
    // instead - its field, grid presence and readbacks are untouched, only the ray-march draw is
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
        // The UNION MARCH (one analytic march per pixel): the analytic-tier drawables rasterize
        // only their ray intervals; the fullscreen union pass marches each covered pixel once.
        // Off (VR, "Union march" tweak, density debug view) = the analytic tier draws per-proxy.
        bool unionPass = false;
        // The tier partition threshold, in VISIBLE bubble radius (forceEmitterVisibleRadius) -
        // NOT authored reach. FLT_MAX = all analytic.
        float sampledRadius = 3.4e38f;
        bool logTierDebug = false; // "Force/Debug/Log tier classification": drawables' radii/tiers, 1/s
    };

    // Compacts the ACTIVE emitter slots (one classification sweep into the partition
    // [sampled-tier drawable | analytic drawable | non-drawn field | PASSIVE tail], stamping each
    // record's source slot for the readback), uploads the query slots, and patches the
    // draw/dispatch counts. Call from present(), after the slot's fence wait.
    void upload(uint32 frameIdx, oc::span<const RendererVKLayout::ForceEmitterGpu> slots,
        oc::span<const RendererVKLayout::ForceQueryGpu> querySlots,
        oc::span<const glm::ivec4> bakeChunks, float bakeSampleY, const ShellCull& shellCull);

    // Records grid clear + insert + force/query dispatches + readback barriers (outside any render
    // pass; ubo is the frame's UBO). All dispatches ride mapped indirect buffers, so emitter/query
    // count changes never re-record.
    void recordCompute(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo);

    struct DrawParams
    {
        Buffer& ubo;
        vk::ImageView gbufferDepthView;
        // SCENE_DEPTH_SAMPLED_LAYOUT: the scene depth is this stage's read-only attachment AND this sampled image.
        vk::ImageLayout gbufferDepthLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::Sampler gbufferSampler;
    };
    // Which half of the shell rendering to record - the desktop primary gives each its own
    // scene-stage secondary so the GPU profiler splits "Force shells" / "Force union march";
    // VR records Both into the one per-eye pass.
    enum class EDrawPart { Proxies, UnionMarch, Both };

    // Records the indirect instanced box draw (sampled-tier proxies - or every drawable when the
    // union pass is off) and/or the union-march fullscreen draw (vertexCount 0 when inactive); the
    // caller has begun a command buffer inside the scene-color render pass and set the
    // viewport/scissor. eye selects the per-eye set/views.
    void recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params,
        EDrawPart part = EDrawPart::Both);

    // The union march's INTERVAL pass: its own tiny render pass (RG16F, cleared to fp16-max,
    // MIN-blended (tEntry, -tExit) per analytic proxy) right before the scene stages; the target ends
    // SHADER_READ_ONLY for the union FS. Cheap: rasterization only, no marching. Instance count rides
    // the indirect buffer (0 when the union pass is off: a clear + no draws). A render pass can only
    // begin in a PRIMARY, so beginIntervalPass opens it there (SECONDARY_COMMAND_BUFFERS contents; the
    // caller ends it) and recordIntervalDraw records the draw into a CACHED render-pass-continue
    // secondary inheriting getIntervalRenderPass / getIntervalFramebuffer.
    void beginIntervalPass(vk::CommandBuffer primary);
    void recordIntervalDraw(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo,
        const vk::Viewport& viewport, const vk::Rect2D& scissor);
    vk::RenderPass getIntervalRenderPass() const { return m_intervalRenderPass; }
    vk::Framebuffer getIntervalFramebuffer() const { return m_intervalFramebuffer; }
    // The union MARCH at HALF RESOLUTION: its own render pass (RGBA16F premultiplied, cleared to
    // 0, ends SHADER_READ_ONLY) right after the interval pass, split the same primary/secondary way.
    // Each covered pixel marches once at half res; the "Force union blend" scene stage (recordDraw
    // UnionMarch) then upsamples depth-aware into scene color. The viewport/scissor are the HALF
    // ones (the caller halves the full-res viewport - same 0.5 factor the march FS's uv applies).
    // gbufferDepth = THIS frame's scene depth in SCENE_DEPTH_SAMPLED_LAYOUT (the march runs after the opaque scene stages).
    // Half-res mode only (no march target otherwise).
    void beginUnionMarchPass(vk::CommandBuffer primary);
    void recordUnionMarchDraw(CommandBuffer& commandBuffer, uint32 frameIdx, Buffer& ubo,
        const vk::Viewport& viewport, const vk::Rect2D& scissor,
        vk::ImageView gbufferDepthView, vk::Sampler gbufferSampler);
    vk::RenderPass getMarchRenderPass() const { return m_marchRenderPass; }
    vk::Framebuffer getMarchFramebuffer() const { return m_marchFramebuffer; }
    // (Re)creates the interval + march targets at HALF the given (swapchain) extent - call at
    // init + on resize.
    void resizeIntervalTarget(uint32 width, uint32 height);

    // This frame slot's readbacks, slot-indexed (safe between beginFrame's fence wait and present;
    // contents are ~2 frames old). Forces: xyz = applied force, w = mean opposing pressure.
    oc::span<const glm::vec4> getForceReadback(uint32 frameIdx) const { return m_mappedForceReadback[frameIdx]; }
    oc::span<const RendererVKLayout::ForceQueryResult> getQueryReadback(uint32 frameIdx) const { return m_mappedQueryReadback[frameIdx]; }
    // The baked pressure field of THIS frame slot: the data is ~2 frames old, so the chunk list it
    // was evaluated for is returned WITH it (the per-slot copy stored at upload) - the pairing the
    // CPU-side sampler indexes by.
    RendererVKLayout::ForceBakeReadback getBakeReadback(uint32 frameIdx) const
    {
        return { m_bakeChunkLists[frameIdx], m_mappedBakeReadback[frameIdx] };
    }

    // The CPU grid build (a job, after upload's compaction): the frame's exact demand, failed
    // claims included. gridFits -> uploadGrid at once; else the main thread drains the GPU,
    // growGridBuffers (per-frame scratch, nothing to preserve; the caller re-records) and uploadGrid.
    struct GridDemand { uint32 numCells = 0; size_t dataBytes = 0; };
    GridDemand buildGrid();
    bool gridFits(const GridDemand& demand) const;
    void growGridBuffers(const GridDemand& demand);
    void uploadGrid(uint32 frameIdx);

private:
    void buildDrawLayout(GraphicsPipelineLayout& layout);
    void buildComputeLayout(ComputePipelineLayout& layout, const char* shaderPath);
    void createGridBuffers();

    void buildShellBakeLayout(ComputePipelineLayout& layout); // storage IMAGES at 5/6, unlike the rest
    // The shell-volume field textures + sampler (one set: barrier-serialized). TEAM-SIZED: ONE
    // RGBA16F volume at <= 4 teams, two at 5-8 (the second view slot stays null and binding 6
    // falls back to view A - never statically used by those shaders).
    void createShellVolume();
    void destroyShellVolume();
    void createBakeReadbackBuffers(); // team-sized stride (see Layout's bake comment)
    void recordUnionDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 viewIndex, const DrawParams& params);
    uint32 bakeVec4PerSample() const { return (m_numTeams + 3u) / 4u; }
    void buildIntervalLayout(GraphicsPipelineLayout& layout); // shell VS + interval FS, MIN blend
    void buildUnionLayout(GraphicsPipelineLayout& layout);    // fullscreen VS + union-march FS (half-res pass)
    void buildUpsampleLayout(GraphicsPipelineLayout& layout); // fullscreen VS + depth-aware upsample FS (scene color)
    void createIntervalRenderPass(); // format-fixed, made once at initialize
    void createMarchRenderPass();    // the half-res march target's pass, same lifetime
    void destroyIntervalTarget();

    // The grid build's per-emitter phase (any worker of the job's parallelFor) and its scatter.
    using Touch = GridTouches::Touch;
    struct EmitterWalk { glm::ivec3 cellMin, cellMax; bool valid; };
    EmitterWalk walkOf(uint32 emitterIdx) const;
    // Claims + touches the walk's cells into out[]; false = a claim met the cell capacity (the
    // caller discards the block).
    bool touchCells(const EmitterWalk& walk, uint32 emitterIdx, Touch* out);
    void createClaimTables();

    GraphicsPipeline m_pipeline;
    ComputePipeline m_emitterForcePipeline;
    ComputePipeline m_queryPipeline;
    ComputePipeline m_bakePipeline;
    ComputePipeline m_shellBakePipeline;
    GraphicsPipeline m_intervalPipeline;
    GraphicsPipeline m_unionPipeline;    // half-res: m_marchRenderPass; full-res: the scene pass
    GraphicsPipeline m_upsamplePipeline; // the scene-color depth-aware blend (half-res mode only)
    bool m_useGrid = true;
    bool m_densityView = false; // FORCE_DENSITY_VIEW define on the shell fragment (setDensityView)
    bool m_unionHalfRes = true;
    bool m_unionJitter = true;
    uint32 m_numTeams = RendererVKLayout::MAX_FORCE_TEAMS;

    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitterBuffers;
    oc::array<oc::span<RendererVKLayout::ForceEmittersGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedEmitters;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_queryBuffers;
    oc::array<oc::span<RendererVKLayout::ForceQueriesGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedQueries;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_indirectBuffers;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedIndirect; // DrawIndirectCommand + 2x DispatchIndirectCommand
    // CPU-written (DeviceLocal|HostVisible, persistently mapped): the hash table + the cell records.
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_gridTableBuffers;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedGridTable;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_gridDataBuffers;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedGridData;
    // The grid build state: the compacted field emitters (a CPU copy - the mapped emitter buffer
    // is write-combined), the claims (capacity = tableEntries / 4), the touches, and the assembled
    // cell records the upload copies (one per claim slot, at slot * FORCE_CELL_UINTS).
    oc::vector<RendererVKLayout::ForceEmitterGpu> m_compactEmitters;
    uint32 m_compactFieldCount = 0;
    GridClaim m_gridClaim;
    GridTouches m_gridTouches;
    oc::vector<Touch> m_extraTouches;
    oc::vector<uint32> m_cellData;
    GridDemand m_gridDemand;
    // GPU-written readbacks (HostVisible|HostCoherent storage, persistently mapped, zeroed at init).
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_forceReadbackBuffers;
    oc::array<oc::span<glm::vec4>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedForceReadback;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_queryReadbackBuffers;
    oc::array<oc::span<RendererVKLayout::ForceQueryResult>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedQueryReadback;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeChunkBuffers;
    oc::array<oc::span<RendererVKLayout::ForceBakeChunksGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedBakeChunks;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeReadbackBuffers;
    oc::array<oc::span<glm::vec4>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedBakeReadback;
    oc::array<oc::vector<glm::ivec4>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeChunkLists; // per-slot pairing (see getBakeReadback)
    // The sampled shell tier's field volumes (RendererVKLayout::FORCE_SHELL_VOLUME_*): ONE set,
    // not per frame slot - the bake's acquire barrier (prev fragment reads -> compute writes)
    // serializes reuse on the queue. GENERAL layout for life.
    vk::Image m_shellVolumeImage[2]{};
    VmaAllocation m_shellVolumeMemory[2]{};
    vk::ImageView m_shellVolumeView[2]{};
    vk::Sampler m_shellVolumeSampler; // linear, clamp-to-border transparent black (outside = zero field)
    // The union march's per-pixel interval target (RG16F, swapchain extent; single image - written
    // and read within the frame, re-cleared every frame by its render pass).
    vk::Image m_intervalImage;
    VmaAllocation m_intervalMemory = nullptr;
    vk::ImageView m_intervalView;
    vk::RenderPass m_intervalRenderPass;
    vk::Framebuffer m_intervalFramebuffer;
    vk::Sampler m_intervalSampler; // nearest (the union FS texelFetches its own pixel; upsample too)
    uint32 m_intervalWidth = 0, m_intervalHeight = 0; // HALF the swapchain extent (march resolution)
    // The half-res union march target (RGBA16F premultiplied; same extent as the interval target).
    vk::Image m_marchImage;
    VmaAllocation m_marchMemory = nullptr;
    vk::ImageView m_marchView;
    vk::RenderPass m_marchRenderPass;
    vk::Framebuffer m_marchFramebuffer;

    oc::array<oc::vector<uint32>, 4> m_uploadBuckets; // upload scratch: slot indices per partition
    uint32 m_tableEntries = RendererVKLayout::INITIAL_FORCE_TABLE_ENTRIES;
    size_t m_gridDataSize = RendererVKLayout::INITIAL_FORCE_GRID_DATA_SIZE;

    static constexpr uint32 MAX_VIEWS = 2;
    static uint32 drawSlot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS> m_drawSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitterForceSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_querySets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_bakeSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_shellBakeSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_intervalSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_unionSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_upsampleSets;
    uint32 m_viewCount = 1;

    // Offsets into the per-frame indirect buffer (uints): [0..3] draw (sampled-tier proxies - or
    // ALL drawables with the union pass off), [4..6] unused (the old grid insert dispatch - the
    // grid is CPU-built now), [8..10] force groups,
    // [12..14] query groups, [16..18] bake groups (x = chunk count), [20..22] shell-volume bake
    // groups (x = 0 disables - the CB is cached, so the toggle rides here), [24..27] the interval
    // pass draw (analytic drawables, firstInstance = the partition split), [28..31] the union
    // fullscreen draw (vertexCount 3 or 0).
    static constexpr uint32 DRAW_CMD_OFFSET = 0;
    static constexpr uint32 GRID_DISPATCH_OFFSET = 4;
    static constexpr uint32 EMITTER_DISPATCH_OFFSET = 8;
    static constexpr uint32 QUERY_DISPATCH_OFFSET = 12;
    static constexpr uint32 BAKE_DISPATCH_OFFSET = 16;
    static constexpr uint32 SHELLBAKE_DISPATCH_OFFSET = 20;
    static constexpr uint32 INTERVAL_DRAW_OFFSET = 24;
    static constexpr uint32 UNION_DRAW_OFFSET = 28;
    static constexpr uint32 INDIRECT_UINTS = 32;
};
