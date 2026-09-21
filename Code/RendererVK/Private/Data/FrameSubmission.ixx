export module RendererVK:FrameSubmission;

import Core;
import :Buffer;
import :Layout;
import :LightGridComputePipeline;

// THE per-frame submission surface for what the entity pass ADDS rather than draws: lights, fog
// volumes and decals. Each is a LOCK-FREE bump claim from any job - a claim past the fixed maximum is
// simply dropped, and present() clamps the flush count back to it, so an over-full frame loses the
// tail instead of corrupting anything. (The instance push has its own, monotonic-cursor rules; see
// InstanceStream.)
//
// It also owns the light grid's per-slot GPU scratch, because the grid's capacity and the light
// buffer's are one decision: the table entries are 4x the pipeline's grid capacity - the CPU claim
// table IS the GPU table - so growLightGrid resizes both together.
//
// The decal payload itself lives in DecalPipeline's mapped array; only the claim is here.
export class FrameSubmission final
{
public:
    struct FrameSlot
    {
        Buffer lightInfos;
        Buffer fogVolumes;
        Buffer lightGrids; // GPU scratch, rewritten every frame
        Buffer lightTable; // device-local + host-visible (ReBAR): the CPU reads its 3-uint header back
        oc::span<RendererVKLayout::LightInfo> mappedLightInfos;
        oc::span<RendererVKLayout::FogVolumes> mappedFogVolumes; // single element: count header + array
    };

    // onGpuIdle runs before the light grid buffers are re-created; onInvalidate re-records.
    void initialize(oc::function<void()> onGpuIdle, oc::function<void()> onInvalidate);

    FrameSlot& slot(uint32 frameIdx) { return m_slots[frameIdx]; }
    const FrameSlot& slot(uint32 frameIdx) const { return m_slots[frameIdx]; }

    // ---- The add path (jobs, lock-free). UINT32_MAX = dropped, this frame is full. ----
    uint32 addLight(uint32 frameIdx, const RendererVKLayout::LightInfo& light);
    uint32 addFogVolume(uint32 frameIdx, const RendererVKLayout::FogVolumeInfo& fogVolume);
    uint32 claimDecal();

    // ---- Per-frame bookkeeping ----
    void beginFrame() { m_lightCount = 0; m_fogVolumeCount = 0; m_decalCount = 0; }
    void settleCounts(); // clamps the lock-free claims back to their maxima (idempotent)
    // Publishes the fog count header and flushes both mapped buffers for this frame's GPU reads.
    void flushFrame(uint32 frameIdx);

    uint32 getLightCount() const { return m_lightCount; }
    uint32 getFogVolumeCount() const { return m_fogVolumeCount; }
    uint32 getDecalCount() const { return m_decalCount; }
    // The lights the grid build reads. Mapped (write-combined) memory: only READ on the overflow
    // re-walk, which is a non-goal path.
    oc::span<const RendererVKLayout::LightInfo> getLights(uint32 frameIdx) const;

    uint32 getLightTableEntries() const { return m_lightTableEntries; }
    size_t getLightGridBufferSize() const { return m_lightGridBufferSize; }
    // True when this frame's exact demand does not fit; growLightGrid then fits it with headroom.
    bool lightGridNeedsGrow(const LightGridComputePipeline::Demand& demand, const LightGridComputePipeline& pipeline) const;
    // Synchronous, from present(): the CPU build knows the exact demand before anything is uploaded,
    // so growth fits it and no frame ever drops a light. The table stays under a quarter full (hash
    // collision quality), the grid data gets 1.5x.
    void growLightGrid(const LightGridComputePipeline::Demand& demand, LightGridComputePipeline& pipeline);

private:
    void createLightGridBuffers();

    oc::array<FrameSlot, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_slots;
    oc::function<void()> m_onGpuIdle;
    oc::function<void()> m_onInvalidate;

    uint32 m_lightCount = 0;
    uint32 m_fogVolumeCount = 0;
    uint32 m_decalCount = 0;

    size_t m_lightGridBufferSize = RendererVKLayout::INITIAL_LIGHT_GRID_BUFFER_SIZE;
    uint32 m_lightTableEntries = RendererVKLayout::INITIAL_LIGHT_TABLE_NUM_ENTRIES;
};
