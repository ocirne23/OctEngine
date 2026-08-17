export module RendererVK:GpuProfiler;

import Core;
import :VK;
import :Layout;

// GPU pass timings via vkCmdWriteTimestamp, pushed into the Profiler's "GPU" track so the UI panel
// shows them on the same timeline as the CPU threads. One query pool per frame-in-flight; scopes are
// recorded into the PRIMARY command buffer (re-recorded every frame) OUTSIDE render passes only
// (multiview render passes replicate timestamp writes per view). Results are read back in
// Renderer::beginFrame after the slot's fence wait (~NUM_FRAMES_IN_FLIGHT frames latent) and mapped
// onto the CPU tick timeline via VK_KHR_calibrated_timestamps (QPC domain) when available, else
// anchored to the CPU submit time.
export class GpuProfiler final
{
public:
    static constexpr uint32 MAX_SCOPES = 128; // per frame slot; 2 timestamps each

    void initialize(); // after Device; also requires Globals::profiler.initialize() to have run (main.cpp does it first)

    // After the slot's fence wait, before anything re-records into it: reads the slot's previous
    // results and pushes them to the profiler GPU track.
    void collect(uint32 frameIdx);

    // At primary command buffer begin (outside any render pass): resets the slot's pool + scope list.
    void beginRecord(vk::CommandBuffer cmd, uint32 frameIdx);
    void beginScope(vk::CommandBuffer cmd, const char* name); // name must be a string literal
    void endScope(vk::CommandBuffer cmd);

    void onSubmit(uint32 frameIdx); // right before queue submit: CPU-time anchor for the uncalibrated fallback

private:

    struct ScopeMeta
    {
        const char* name;
        uint16 depth;
    };
    struct FrameSlot
    {
        vk::QueryPool queryPool;
        oc::array<ScopeMeta, MAX_SCOPES> scopes;
        uint32 numScopes = 0;
        uint64 cpuSubmitTick = 0;
        bool pending = false;
    };

    oc::array<FrameSlot, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_slots;
    oc::array<uint32, MAX_SCOPES> m_openStack = {};
    uint32 m_openDepth = 0;
    uint32 m_recordSlot = 0;
    double m_timestampPeriodNs = 0.0;
    uint64 m_timestampMask = ~0ull;
    ProfileTrack* m_track = nullptr;
    bool m_supported = false;
    bool m_useCalibration = false;
};
