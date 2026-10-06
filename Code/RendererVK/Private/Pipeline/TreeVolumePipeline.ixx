export module RendererVK:TreeVolumePipeline;

import Core;
import Core.glm;

import :VK;
import :Allocator;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :GraphicsPipeline;
import :DescriptorSet;
import :Layout;
import :Settings;
import :TreeRecordPool;

// FAR TREES as a marched volume (Docs/TreeRenderingPlan.md T2, prototype P1). Three stages:
//   bake  : the GPU tree sets' trees splatted into ONE camera-centred POLAR volume of extinction (1/m): angle x
//           log radius from "Far start" to "Far end" (cells grow linearly with the distance - tree_volume.inc.glsl),
//           z = height above the column's tree FLOOR. tree_volume_splat.cs twice (one workgroup per tree): its
//           TREE_FLOOR_PASS variant (each column's lowest tree base, R32UI 2D), then the splat (fixed-point atomics,
//           R32UI 3D); then tree_volume_resolve.cs (in place: the extinction's float bits) and, after the hand-over,
//           tree_volume_copy.cs (-> the R16F density, filterable). Re-baked only when the camera leaves the snap radius
//           around the bake centre, a set changes, or a volume setting changes.
//           The WORLD TREE RECORDS: within "Far record detail" each record expands to its exact trees (the splat's
//           TREE_SPLAT_RECORDS variants, in every pass); beyond, their mass per column (tree_volume_records.cs, a tent
//           over the crown's columns), then per column the terrain floor where no tree floor is and the slices from
//           the type's height profile (tree_volume_far.cs). A dynamic set (the world) is never splatted.
//   march : tree_volume_march.cs, full res, the view ray from the start distance to the scene surface.
//   temporal: cloud_temporal.cs's TREE_TEMPORAL instance ("Far temporal blend" > 0): last frame's result reprojected
//           at the trees' mean distance, rejected / neighbourhood-clamped as for the clouds, blended in. OFF COSTS
//           NOTHING: the plain march variant writes the result directly, the barriers are the march's own, and the
//           temporal images do not exist (created at the first frame with it on).
//   apply : a scene-forward stage (before the cloud / fog apply): colour + scene x transmittance.
// Desktop only (one view). Every image stays in GENERAL for life.
//
// The type / piece layouts below are MIRRORED in tree_volume_splat.cs.glsl - keep them in step.

// A tree TYPE's volume: its box (type space), its float mip chain in the data buffer (res^3 at `offset`, the
// smaller mips following), its mean albedo. res 0 = the type adds nothing. albedo.w 0 = a SOLID (a rock): the chain is
// occupancy (x "Far rock extinction", not / the scale), the colour the climate's bedrock (the resolve), not albedo.
export struct TreeVolumeTypeGpu
{
    glm::vec3 boxMin{ 0.0f };
    uint32 offset = 0;
    glm::vec3 boxMax{ 0.0f };
    uint32 res = 0;
    glm::vec4 albedo{ 0.0f };
};
static_assert(sizeof(TreeVolumeTypeGpu) == 48);

// A placed tree: its transform (uniform scale) and type.
export struct TreeVolumePieceGpu
{
    glm::vec4 posScale{ 0.0f };
    glm::vec4 quat{ 0.0f, 0.0f, 0.0f, 1.0f };
    uint32 type = 0;
    uint32 pad[3] = {};
};
static_assert(sizeof(TreeVolumePieceGpu) == 48);

export class TreeVolumePipeline final
{
public:
    TreeVolumePipeline() = default;
    ~TreeVolumePipeline();
    TreeVolumePipeline(const TreeVolumePipeline&) = delete;

    void initialize(uint32 width, uint32 height, vk::RenderPass sceneRenderPass);
    void recreateImages(uint32 width, uint32 height); // the march outputs (render size)
    void reloadShaders(vk::RenderPass sceneRenderPass); // the caller has waited for the GPU
    void markDirty() { m_dirty = true; }               // a tree set changed: re-bake (after a running bake)
    // What a running bake reads is going away (a tree set, the record types, the pool): drop it, re-bake from scratch.
    // A bake already handing over (or copying) reads none of that any more: it finishes, the re-bake follows.
    void invalidate()
    {
        m_dirty = true;
        if (m_job.active && !handingOver())
        {
            m_job.active = false;
            m_jobDropped = true; // the next record() holds the next start back (its snapshots may still be read)
        }
    }
    // The frame UBO's u_treeHandover as of this frame's real time (built before record(), which alone changes the state):
    // xy = the new bake's centre, z = the cross-fade to it (0..1, the fraction of rays that pick it), w unused; all 0
    // without one.
    glm::vec4 handoverUbo() const;
    float handoverFade() const;
    // The frame the running bake started on (UINT64_MAX: none): the record pool keeps the chunks it saw alive.
    uint64 bakeHoldSince() const { return m_job.active ? m_job.startFrame : UINT64_MAX; }
    // Before recording (main thread): (re)creates the volume when its resolution changed, and the temporal images
    // when the temporal blend is first turned on - drains the GPU then.
    void prepare(const FarTreeParams& settings);

    // One GPU tree set's volume data (device addresses).
    struct Source
    {
        vk::DeviceAddress pieces = 0; // TreeVolumePieceGpu[numPieces]
        vk::DeviceAddress types = 0;  // TreeVolumeTypeGpu[]
        vk::DeviceAddress data = 0;   // float[]
        uint32 numPieces = 0;
    };
    // The WORLD TREE RECORDS (TreeRecordPool; W4): every record's tree. A chunk inside "Far record detail" of the bake
    // centre EXPANDS each record exactly as Procedural's world expansion (variant, scale, yaw, its bushes; the ground
    // from the terrain map) into the world set's volume types and splats it in detail (tree_volume_splat.cs, its
    // TREE_SPLAT_RECORDS variants); beyond it each record adds its exact mass to its columns (tree_volume_records.cs),
    // spread over the slices by its type's height profile (tree_volume_far.cs). numTypes 0 = none.
    struct RecordSource
    {
        vk::DeviceAddress records = 0;     // uint32 per record; each chunk's ground right before its records
        vk::DeviceAddress map = 0;         // TreeRecordMapGpu, mapSize^2 (the chunk map: a position's ground)
        uint32 mapSize = 0;
        vk::DeviceAddress chunks = 0;      // TreeRecordChunkGpu per chunk
        oc::span<const TreeRecordChunkGpu> chunksCpu; // the same table on the CPU: the bake's snapshot, its detail chunks
        oc::span<const TreeRecordMapGpu> mapCpu;      // the chunk map on the CPU: the bake's snapshot
        vk::DeviceAddress types = 0;       // TreeRecordTypeGpu per record type
        vk::DeviceAddress volumeTypes = 0; // the world set's TreeVolumeTypeGpu (the variants' grids)
        vk::DeviceAddress volumeData = 0;
        uint32 numChunks = 0;
        uint32 numTypes = 0;
        float chunkSize = 256.0f;
        uint32 worldSeed = 1;
    };
    // ROCKS (R5): the climate's bedrock colour per column comes from the terrain's ROCK splat materials - their diffuse
    // textures' smallest mip (the mean), weighted by the climate boxes as the terrain picks them (the resolve).
    static constexpr uint32 ROCK_TEXTURES = 8;
    struct RecordParams
    {
        Buffer& ubo;
        // The terrain's rock materials' diffuse views, in slot order (ROCK_TEXTURES of them: a fallback past the count,
        // u_terrainTexParams0.z, which the resolve reads from the UBO).
        oc::span<const vk::ImageView> rockTextures;
        vk::ImageView sceneDepthView; // SCENE_DEPTH_SAMPLED_LAYOUT, after the opaque stages
        vk::Sampler sceneDepthSampler;
        vk::ImageView terrainView;    // the baked terrain-data cascades (SHADER_READ_ONLY)
        vk::Sampler terrainSampler;
        vk::ImageView skyMapView;     // GI's sky bake (GENERAL; baked earlier this frame): the canopy's sky light
        vk::Sampler skyMapSampler;
        vk::ImageView cloudShadowView; // the cloud Beer shadow map (GENERAL): the sun on the canopy under clouds
        vk::Sampler cloudShadowSampler;
        glm::vec3 cameraPos{ 0.0f };
        float startDistance = 0.0f;   // the march's start (3D, m): "Far start" scaled with the camera's height
                                      // (Renderer::farTreesStart), the billboards' hand-over
        oc::span<const Source> sources;
        RecordSource records;
        const FarTreeParams& settings;
        uint32 frameNumber = 0;       // monotonic: the history is last frame's only when this follows the last march
    };
    // Straight into the primary, after the opaque stages: this frame's share of the bake when due (recordBake), then the
    // march (record, which reads the depth). Two calls so each gets a GPU scope of its own.
    void recordBake(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params);
    void record(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params);
    // The fullscreen apply (fog OFF); the caller is inside the scene-colour render pass with the viewport set.
    void recordApply(CommandBuffer& commandBuffer, uint32 frameIdx);
    // The result of a frame slot (GENERAL, render size): rgb = in-scatter, a = transmittance; and the
    // transmittance-weighted mean distance (R16F, m). With the fog ON the fog apply composites them inside the fog.
    vk::ImageView getOutView(uint32 frameIdx) const { return m_out[frameIdx].view; }
    vk::ImageView getOutDepthView(uint32 frameIdx) const { return m_outDepth[frameIdx].view; }
    vk::Sampler getSampler() const { return m_linearSampler; }

private:
    struct Image
    {
        vk::Image image;
        VmaAllocation memory{};
        vk::ImageView view;
    };
    void buildSplatLayout(ComputePipelineLayout& layout, uint32 floorPass, bool records);
    void buildResolveLayout(ComputePipelineLayout& layout);
    void buildFloorSmoothLayout(ComputePipelineLayout& layout);
    void buildClearLayout(ComputePipelineLayout& layout);
    void buildCopyLayout(ComputePipelineLayout& layout);
    void snapshotRecords(const RecordParams& params, const oc::function<void(Buffer&, const void*, size_t, const char*)>& upload);
    void setBakeBudgets();
    void buildRecordsLayout(ComputePipelineLayout& layout);
    void buildFarLayout(ComputePipelineLayout& layout);
    void buildMarchLayout(ComputePipelineLayout& layout, bool temporalOut, uint32 scale, uint32 skip, bool handover = false);
    // Builds (or rebuilds) a march variant and its TREE_HANDOVER twin with the same defines. False when either failed.
    bool buildMarchPair(ComputePipeline& march, ComputePipeline& handover, bool temporalOut, uint32 scale, uint32 skip, bool reload);
    bool ready(const FarTreeParams& s) const; // prepare() ran for these settings' resolutions
    bool handingOver() const { return m_job.active && (m_job.stage == EBakeStage::Handover || m_job.stage == EBakeStage::Copy); }
    void buildTemporalLayout(ComputePipelineLayout& layout, uint32 scale, bool checker);
    void buildUpsampleLayout(ComputePipelineLayout& layout);
    void buildApplyLayout(GraphicsPipelineLayout& layout);
    void buildFloorMaxLayout(ComputePipelineLayout& layout, bool dilate);
    vk::Extent3D floorMaxExtent() const; // the max-floor grid's size (from m_angularRes / m_radialRes)
    void createVolume(uint32 angularRes, uint32 radialRes, uint32 slices);
    void createTemporalImages(uint32 scale);
    void destroyTemporalImages();
    void destroyImage(Image& image);
    // THE BAKE, SPREAD OVER FRAMES ("Far bake frames"): startBake snapshots everything it reads, stepBake runs this
    // frame's share - the clears; the floor coverage, the floor and the splat (the record splat's workgroups spread
    // evenly); the smoothing; the records' mass; the far columns; the resolve; the max-floor grid; the hand-over; the
    // copy + the front / back swap. The march reads the FRONT floor, colour, max floor and the density - during the
    // hand-over the BACK ones and the accumulation too.
    void startBake(const RecordParams& params, uint32 frameNumber, glm::vec2 centre);
    // BAKE AHEAD ("Far bake ahead"): the camera's horizontal velocity (real time, smoothed) x the last bake's real
    // duration (start -> swap), capped at the ring's margin - where the camera will be when a bake started now swaps.
    void trackCamera(glm::vec2 camera);
    glm::vec2 bakeLead(const FarTreeParams& s) const;
    void stepBake(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params);

    ComputePipeline m_floorCoverPipeline; // the splat shader's TREE_FLOOR_PASS 1 variant (the coverage per column)
    ComputePipeline m_floorPipeline;      // ... and 2 (the dominant tree's base)
    ComputePipeline m_splatPipeline;
    ComputePipeline m_resolvePipeline;
    ComputePipeline m_clearPipeline;       // tree_volume_clear.cs: the accumulation, a slice range per dispatch
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_clearSets;
    ComputePipeline m_copyPipeline;        // tree_volume_copy.cs: the new bake's extinction -> the density, after the hand-over
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_copySets;
    // The march's TREE_HANDOVER variants (the plain one and the temporal one, the same baked scale / skip as theirs): run
    // only while a hand-over lasts.
    ComputePipeline m_marchHandoverPipeline;
    ComputePipeline m_marchTemporalHandoverPipeline;
    ComputePipeline m_floorSmoothPipeline; // the floor's separable blur (floor -> floorCover -> floor)
    ComputePipeline m_recordsPipeline;     // tree_volume_records.cs: the records' mass per column
    // The splat's TREE_SPLAT_RECORDS variants, per pass: [0] the splat, [1] the floor coverage, [2] the floor.
    oc::array<ComputePipeline, 3> m_recordSplatPipelines;
    oc::array<DescriptorSet, 3 * RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_recordSplatSets;
    ComputePipeline m_farPipeline;         // tree_volume_far.cs: the columns' floor from the terrain + their slices
    // tree_volume_floor_max.cs: the floor -> the block max, then (TREE_FLOOR_MAX_DILATE) the dilated max-floor grid.
    ComputePipeline m_floorMaxPipeline;
    ComputePipeline m_floorDilatePipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * 2> m_floorMaxSets; // per slot: the two passes
    ComputePipeline m_marchPipeline;
    ComputePipeline m_marchTemporalPipeline; // TREE_TEMPORAL_OUT: the log2 distances for the temporal pass
    // What the baked variants were compiled with (record() dispatches by these, never by the live settings):
    uint32 m_plainBakedSkip = 1;          // m_marchPipeline's TREE_MARCH_SKIP (the default setting: 1 of 2)
    uint32 m_temporalBakedScale = 1;      // m_marchTemporalPipeline / m_temporalPipeline's scale ...
    bool m_temporalBakedChecker = false;  // ... and checkerboard
    ComputePipeline m_temporalPipeline;      // cloud_temporal.cs.glsl, TREE_TEMPORAL
    ComputePipeline m_upsamplePipeline;      // tree_volume_upsample.cs.glsl ("Far half res")
    GraphicsPipeline m_applyPipeline;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_floorCoverSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_floorSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_splatSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_resolveSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_recordsSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_farSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * 2> m_floorSmoothSets; // per slot: the two axes
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_marchSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_temporalSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_upsampleSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_applySets;

    // R32UI 3D, TWO SLICES per texel (m_accumDepth deep): the splat's 16-bit fixed-point sums; after the resolve, each
    // slice's extinction as a half float.
    Image m_accum;
    vk::ImageView m_accumFloatView; // ... read as RG16F (MUTABLE_FORMAT): the new bake's density during the hand-over
    uint32 m_accumDepth = 0;        // (m_slices + 1) / 2
    Image m_density; // R16F 3D: extinction (1/m), sampled
    // FRONT (m_front: the march's) and BACK (the running bake's) copies, swapped at the bake's last step:
    oc::array<Image, 2> m_colour; // RGBA8 2D: the albedo per column; a = 1 - its ROCK fraction (the march's rock lighting)
    oc::array<Image, 2> m_floor;  // R32UI 2D: the dominant tree's base per column (tree_volume.inc.glsl's encoding; 0 = none)
    // RG32F 2D, one texel per FLOOR_MAX_BLOCK^2 columns (the march's skip): x = the highest floor within one block each
    // way, y = AHEAD - over this row and every row farther out, TV_FLOOR_AHEAD_SECTORS blocks each way.
    oc::array<Image, 2> m_floorMax;
    Image m_floorBlock; // R32F: the undilated block max (the bake's last step only)
    uint32 m_front = 0;
    Image m_floorCover; // R32UI 2D: the largest tree coverage per column (the floor's first pass)
    Image m_farAmount;  // R32UI 2D: the records' mass per column (fixed point; TV_AMOUNT_SCALE)
    Image m_farType;    // R32UI 2D: the record type whose profile a column takes (the lowest: a tree before a rock)
    // R32UI 2D: the ROCKS' share of a column, in the accumulation's units summed over the slices (the detail splat's
    // solid voxels, the far records' rock mass / the slice height). The resolve: rock fraction = this / the column's
    // sum -> the colour's alpha (1 - fraction) and its rgb toward the climate's bedrock.
    Image m_rockSum;
    oc::array<Image, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_out;      // RGBA16F, render size: in-scatter + T
    oc::array<Image, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_outDepth; // R16F: the weighted mean distance (m)
    // The TEMPORAL images (only while the temporal path is on - FarTreeParams::temporalPath; RGBA16F at the march size,
    // the render size / m_temporalScale): this frame's march and its log2 distances (cloud_temporal's format), per
    // slot the history distances, and at half res per slot the history colour (at full res: the slot's m_out).
    Image m_raw;
    Image m_rawDepth;
    oc::array<Image, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_histDepth;
    oc::array<Image, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_histColour;
    bool m_hasTemporalImages = false;
    uint32 m_temporalScale = 1; // 1, or 2 at "Far half res"
    bool m_temporalLastFrame = false; // last frame ran the temporal pass (its reads need this frame's WAR barrier)
    vk::Sampler m_linearSampler; // u repeats (the volume's angle)
    vk::Sampler m_mipSampler;    // every mip (the rock textures' smallest: their mean colour)
    vk::Sampler m_screenSampler; // clamp: the temporal pass's screen images
    uint32 m_lastMarchFrame = UINT32_MAX; // frameNumber of the last march (the history's validity)
    uint32 m_lastPlainMarchFrame = UINT32_MAX; // ... of the last plain PIXEL-SKIP march (else UINT32_MAX)
    int m_lastSkipMode = 0;                    // its mode (a change restarts the latest images)
    // The plain pixel skip's latest march per pixel (render size; only while the plain path skips pixels): colour
    // RGBA16F + distance R16F, cleared to "no trees, distance 0" at every restart.
    Image m_latest;
    Image m_latestDepth;
    bool m_hasLatest = false;
    void createLatestImages();
    void destroyLatestImages();
    uint32 m_width = 0;
    uint32 m_height = 0;
    uint32 m_angularRes = 0;
    uint32 m_radialRes = 0;
    uint32 m_slices = 0;

    // The baked state (the FRONT: what the march reads): re-bake when any of it changes.
    bool m_dirty = true;
    bool m_baked = false;
    glm::vec2 m_centre{ 0.0f };
    FarTreeParams m_bakedSettings;

    // The running bake (the BACK).
    // Resolve: in place, slice ranges (a column pass first). FloorMax: the max-floor grid, then the hand-over (an earlier
    // bake on screen) or straight to the copy. Handover: the march's TREE_HANDOVER variant cross-fades over "Far swap
    // time". Copy: the new extinction -> the density, slice ranges; then the swap.
    enum class EBakeStage : uint8 { Clear, FloorCover, Floor, Smooth, Splat, RecordMass, FarColumns, Resolve, FloorMax, Handover, Copy };
    struct BakeJob
    {
        bool active = false;
        EBakeStage stage = EBakeStage::Clear;
        // THE STAGES SPREAD OVER FRAMES ("Far bake frames"): each takes a share of them, so no stage runs whole in one
        // frame unless it is small (the smoothing, the max-floor grid) - or a copy without a cross-fade (crossFade).
        uint32 progress = 0;      // the units done in this stage (slices / pieces + records / chunks / rows)
        uint32 perFrame = 1;      // the splat passes' units (the static sets' pieces, then the detail records) per frame
        uint32 clearPerFrame = 1; // the accumulation's slices per frame
        uint32 massPerFrame = 1;  // the records' mass: chunks per frame
        uint32 rowsPerFrame = 8;  // the far columns: radial rows per frame (a multiple of the 8-row group)
        uint32 resolvePerFrame = 1; // the resolve's slices per frame
        uint32 copyPerFrame = 1;    // the copy's slices per frame
        // The new bake CROSS-FADES in (an earlier bake on screen, of the same volume geometry: the march reads both with
        // the shown bake's ring and height). Else the copy runs whole in one frame and the swap follows at once.
        bool crossFade = false;
        uint32 handoverStart = 0;   // the frame the hand-over began (the march's variant starts the frame after)
        double handoverStartSec = 0.0; // ... and its real time (the UBO's fade counts from it: "Far swap time")
        uint32 staticPieces = 0;  // the static sets' pieces (each splat pass walks them before the records)
        uint32 startFrame = 0;
        double startSec = 0.0;    // real time (the next bake's lead: this one's duration)
        glm::vec2 centre{ 0.0f };
        FarTreeParams settings;
        oc::vector<Source> sources; // the static sets
        RecordSource records;       // with the snapshot buffers' addresses
        uint32 detailRecords = 0;
        uint32 numDetailChunks = 0;
    };
    BakeJob m_job;
    // A new bake starts from this frame on: NUM_FRAMES_IN_FLIGHT after the last swap OR DROP (its snapshot buffers are
    // rewritten - a dropped bake's dispatches may still be in flight - and after a swap the old front, the new back,
    // may still be read).
    uint32 m_bakeStartFrame = 0;
    bool m_jobDropped = false;
    double m_bakeDurationSec = 0.0;     // the last bake's start -> swap, real time (0: none yet - no lead)
    glm::vec2 m_cameraVelocity{ 0.0f }; // horizontal, m/s, smoothed
    glm::vec2 m_lastCamera{ 0.0f };
    double m_lastCameraSec = -1.0;
    // The bake's SNAPSHOTS (host-visible, written at its start): the record table, the chunk map, and the detail chunks -
    // the records within "Far record detail" of the bake centre, one workgroup per RECORD (a workgroup per chunk ran its
    // ~3000 plants in sequence - 31 ms per bake), each (coord.x, coord.y, its first record's pool word, its first
    // workgroup).
    Buffer m_bakeTable;
    Buffer m_bakeMap;
    Buffer m_bakeDetail;
    oc::vector<glm::uvec4> m_detailScratch;
};
