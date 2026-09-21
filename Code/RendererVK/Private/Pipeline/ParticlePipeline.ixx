export module RendererVK:ParticlePipeline;

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
import :GIProbePipeline;

// GPU-driven particle system. The particle pool, dead-index stack and two alive lists are persistent
// device-local state (single copy - frames in flight are serialized on the graphics queue, with a WAR
// barrier at the head of each frame's sim pass). Per frame, three compute passes run with a cached
// secondary command buffer and indirect args, so emitter/spawn-count changes never re-record:
//   1. begin : (indirect, CPU-sized) pool init/reset OR zero this frame's OUT alive count + size the
//              sim dispatch to survivors + spawns.
//   2. emit  : (indirect, CPU-sized) one thread per spawn, popping the dead stack, initializing from
//              the CPU-written emitter table and appending to the IN alive list.
//   3. sim   : (indirect, GPU-sized) integrate + optional screen-space depth collision, compacting
//              survivors into the OUT alive list - whose count IS the draw's instanceCount.
// The draw pass renders one quad per OUT entry inside the scene-color pass (after the opaque forward
// draw, before fog apply): billboards/velocity-stretch, flipbooks, per-particle GI+sun lighting, soft
// depth fade, premultiplied blend covering alpha through additive per emitter.
export class ParticlePipeline final
{
public:
    ParticlePipeline() = default;
    ~ParticlePipeline() = default;
    ParticlePipeline(const ParticlePipeline&) = delete;

    // CPU-written per-frame parameters (mapped UBO). Must match the ParticleParams block in
    // particle_begin/emit/sim.cs.glsl.
    struct FrameParams
    {
        float dt;
        uint32 spawnCount;
        uint32 parity;
        uint32 reset;
        uint32 frameIndex;
        uint32 collision;
        uint32 pad0, pad1;
    };
    static_assert(sizeof(FrameParams) == 32);

    void initialize(vk::RenderPass sceneRenderPass, uint32 maxTextures, uint32 numTextureDescriptors, uint32 viewCount);
    void reloadShaders(vk::RenderPass sceneRenderPass);

    // Copies this frame's emitter table + spawn map + params into the frame slot's mapped buffers
    // (call from present(), after the slot's fence wait). spawnRequests are (emitter slot, count)
    // pairs, expanded here into the per-spawn map; the total clamps to MAX_PARTICLE_SPAWNS_PER_FRAME.
    // reset runs the pool initialization this frame INSTEAD of the per-frame prepare; the caller
    // (Renderer) owns the pending-reset flag and must only clear it once the frame that carried
    // reset=true was actually SUBMITTED with the sim executing - a reset consumed by a frame that
    // never runs (acquire failure, empty scene) would leave the pool permanently uninitialized
    // (dead stack empty -> every emit drops its spawn).
    void update(uint32 frameIdx, oc::span<const RendererVKLayout::ParticleEmitterGpu> emitters,
        oc::span<const oc::pair<uint16, uint16>> spawnRequests, float dt, bool collision, bool reset);

    struct SimParams
    {
        Buffer& ubo;
        vk::ImageView prevDepthView;   // last frame's scene depth (collision)
        vk::Sampler   sceneDepthSampler;
        vk::ImageView rainOcclusionView;    // THIS frame's top-down rain occlusion depth (weather volume shelter)
        vk::Sampler   rainOcclusionSampler; // non-comparison, clamp-to-border white (outside = open sky)
        vk::ImageView oceanMapsView;        // FFT displacement maps (PARTICLE_FLAG_WATER_FLOOR: the live surface)
        vk::Sampler   oceanMapsSampler;
        vk::ImageView terrainView;          // terrain-data cascades (the shore weighting of that surface)
        vk::Sampler   terrainSampler;
    };
    // Records begin/emit/sim into a begun secondary command buffer (outside any render pass).
    void recordSim(CommandBuffer& commandBuffer, uint32 frameIdx, const SimParams& params);
    // Points the sim's terrain-data binding (UPDATE_AFTER_BIND) at the active ping-pong image; refreshed
    // per frame by the Renderer so a CPU re-bake swaps images without re-recording the cached CB.
    void updateTerrainDescriptor(uint32 frameIdx, vk::ImageView terrainView, vk::Sampler terrainSampler);

    struct DrawParams
    {
        Buffer& ubo;
        Buffer& giGridDataBuffer;
        GiVolumeDescriptors giVolume; // empty = the volume is off (the shader reads the probe buffer)
        vk::ImageView sceneDepthView; // this frame's opaque depth (soft particles)
        // SCENE_DEPTH_SAMPLED_LAYOUT: the scene depth is this stage's read-only attachment AND this sampled image.
        vk::ImageLayout sceneDepthLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::Sampler   sceneDepthSampler;
        vk::ImageView terrainView;      // terrain-data cascades (PARTICLE_FLAG_GROUND_FADE)
        vk::Sampler   terrainSampler;
        Buffer* lightInfosBuffer = nullptr; // the scene's lights + light grid (LIT particles), this frame slot's
        Buffer* lightGridsBuffer = nullptr;
        Buffer* lightTableBuffer = nullptr;
    };
    // Records the indirect billboard draw; the caller has begun a command buffer inside the
    // scene-color render pass and set the viewport/scissor. eye selects the per-eye set/view.
    void recordDraw(CommandBuffer& commandBuffer, uint32 frameIdx, uint32 eye, const DrawParams& params);

    // Bindless texture array maintenance (same contract as the other texture-array pipelines).
    void updateTextureDescriptor(uint32 frameIdx, uint32 slotIdx, vk::ImageView view);
    void resizeTextureDescriptors(uint32 numTextureDescriptors);

    // Snapshot of the GPU counters, copied into a per-frame host buffer at the end of each sim pass
    // (~NUM_FRAMES_IN_FLIGHT frames stale; safe to read between beginFrame's fence wait and present).
    struct DebugCounters
    {
        uint32 simGroups;   // sim dispatch group count the begin pass computed
        uint32 alive[2];    // per-parity alive counts (draw instanceCounts)
        int32  deadCount;   // free pool entries
        uint32 gpuSpawns;   // GPU spawn requests the begin pass latched that frame (clamped)
        uint32 dropSpawns;  // spawns the emit passes dropped because the pool was empty
        uint32 dropBroken;  // non-finite particles the sim retired (see the NaN guard in particle_sim)
    };
    DebugCounters getDebugCounters(uint32 frameIdx) const
    {
        const oc::span<const uint32> counters = m_mappedReadback[frameIdx];
        // [13] is c_gpuSpawnCount, which the begin pass ZEROED for the next frame's producers before this
        // readback was taken - always 0 here. The latched count is [14] (c_gpuSpawnConsume).
        return DebugCounters{ counters[0], { counters[4 + 1], counters[8 + 1] }, (int32)counters[12],
            counters[14], counters[15], counters[20] };
    }

    // The GPU SPAWN PATH's producer-side buffers (particle_spawn.inc.glsl): a compute pass that runs
    // before the particle sim binds both as storage buffers and appends spawn requests. Single copy,
    // like the pool; the request count lives in the counters block.
    Buffer& getCountersBuffer() { return m_countersBuffer; }
    Buffer& getSpawnRequestBuffer() { return m_spawnRequestBuffer; }

private:
    void buildBeginLayout(ComputePipelineLayout& layout);
    void buildEmitLayout(ComputePipelineLayout& layout, bool gpuSpawn);
    void buildSimLayout(ComputePipelineLayout& layout);
    void buildDrawLayout(GraphicsPipelineLayout& layout, uint32 maxTextures);

    ComputePipeline m_beginPipeline;
    ComputePipeline m_emitPipeline;    // the CPU spawn map
    ComputePipeline m_emitGpuPipeline; // the GPU request buffer (PARTICLE_GPU_SPAWN variant)
    ComputePipeline m_simPipeline;
    GraphicsPipeline m_drawPipeline;

    // Persistent GPU state (single copy, see header comment).
    Buffer m_poolBuffer;
    oc::array<Buffer, 2> m_aliveBuffers; // ping-pong by frame parity
    Buffer m_deadListBuffer;
    Buffer m_countersBuffer; // sim dispatch args + per-parity draw args + dead-stack top + GPU spawn counter/dispatch
    Buffer m_spawnRequestBuffer; // MAX_PARTICLE_GPU_SPAWNS requests, appended by producer passes

    // Per-frame-in-flight CPU-written inputs.
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitterBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_spawnBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_paramsBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_beginDispatchBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitDispatchBuffers;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_readbackBuffers; // counters snapshot (stats/debug)
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedReadback;
    oc::array<oc::span<RendererVKLayout::ParticleEmitterGpu>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedEmitters;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedSpawns;
    oc::array<oc::span<FrameParams>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedParams;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedBeginDispatch;
    oc::array<oc::span<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedEmitDispatch;

    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_beginSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_emitGpuSets;
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_simSets;
    static constexpr uint32 MAX_VIEWS = 2;
    static uint32 drawSlot(uint32 frameIdx, uint32 eye) { return frameIdx * MAX_VIEWS + eye; }
    oc::array<DescriptorSet, RendererVKLayout::NUM_FRAMES_IN_FLIGHT * MAX_VIEWS> m_drawSets;

    Sampler m_textureSampler;
    uint32 m_viewCount = 1;
    uint32 m_frameCounter = 0; // RNG stream for the emit pass
};
