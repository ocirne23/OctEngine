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

// FAR TREES as a marched volume (Docs/TreeRenderingPlan.md T2, prototype P1). Three stages:
//   bake  : the GPU tree sets' trees splatted into ONE camera-centred POLAR volume of extinction (1/m): angle x
//           log radius from "Far start" to "Far end" (cells grow linearly with the distance - tree_volume.inc.glsl),
//           z = height above the column's tree FLOOR. tree_volume_splat.cs twice (one workgroup per tree): its
//           TREE_FLOOR_PASS variant (each column's lowest tree base, R32UI 2D), then the splat (fixed-point atomics,
//           R32UI 3D); then tree_volume_resolve.cs (-> R16F, filterable). Re-baked only when the camera leaves the snap radius
//           around the bake centre, a set changes, or a volume setting changes.
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
// smaller mips following), its mean albedo. res 0 = the type adds nothing.
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
    void markDirty() { m_dirty = true; }               // a tree set changed: re-bake
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
    struct RecordParams
    {
        Buffer& ubo;
        vk::ImageView sceneDepthView; // SCENE_DEPTH_SAMPLED_LAYOUT, after the opaque stages
        vk::Sampler sceneDepthSampler;
        vk::ImageView terrainView;    // the baked terrain-data cascades (SHADER_READ_ONLY)
        vk::Sampler terrainSampler;
        glm::vec3 cameraPos{ 0.0f };
        float startDistance = 0.0f;   // the march's start (3D, m): "Far start" scaled with the camera's height
                                      // (Renderer::farTreesStart), the billboards' hand-over
        oc::span<const Source> sources;
        const FarTreeParams& settings;
        uint32 frameNumber = 0;       // monotonic: the history is last frame's only when this follows the last march
    };
    // Straight into the primary: the bake when due, then the march. After the opaque stages (reads the depth).
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
    void buildSplatLayout(ComputePipelineLayout& layout, uint32 floorPass);
    void buildResolveLayout(ComputePipelineLayout& layout);
    void buildMarchLayout(ComputePipelineLayout& layout, bool temporalOut, uint32 scale, uint32 skip);
    void buildTemporalLayout(ComputePipelineLayout& layout, uint32 scale, bool checker);
    void buildUpsampleLayout(ComputePipelineLayout& layout);
    void buildApplyLayout(GraphicsPipelineLayout& layout);
    void createVolume(uint32 angularRes, uint32 radialRes, uint32 slices);
    void createTemporalImages(uint32 scale);
    void destroyTemporalImages();
    void destroyImage(Image& image);
    void bake(vk::CommandBuffer cmd, uint32 frameIdx, const RecordParams& params);

    ComputePipeline m_floorCoverPipeline; // the splat shader's TREE_FLOOR_PASS 1 variant (the coverage per column)
    ComputePipeline m_floorPipeline;      // ... and 2 (the dominant tree's base)
    ComputePipeline m_splatPipeline;
    ComputePipeline m_resolvePipeline;
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
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_marchSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_temporalSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_upsampleSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_applySets;

    Image m_accum;   // R32UI 3D: the splat's fixed-point sums
    Image m_density; // R16F 3D: extinction (1/m), sampled
    Image m_colour;  // RGBA8 2D: the leaf albedo per column
    Image m_floor;   // R32UI 2D: the dominant tree's base per column (tree_volume.inc.glsl's encoding; 0 = none)
    Image m_floorCover; // R32UI 2D: the largest tree coverage per column (the floor's first pass)
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

    // The baked state: re-bake when any of it changes.
    bool m_dirty = true;
    bool m_baked = false;
    glm::vec2 m_centre{ 0.0f };
    FarTreeParams m_bakedSettings;
};
