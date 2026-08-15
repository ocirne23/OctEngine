export module UI:ProfilerPanel;

import Core;

// The Profiler window: frame-time graph (click a bar to pause + inspect that frame), a per-thread +
// GPU flame timeline (wheel zoom, drag pan), and a sortable aggregate stats table. Purely a READER
// of Globals::profiler - pausing here freezes the panel's snapshot, recording continues.
export class ProfilerPanel
{
public:
    void render();

private:

    struct TrackView
    {
        uint32 trackIdx = 0;
        uint32 sortKey = 0;
        const char* name = nullptr; // points at the ProfileTrack's stable name storage
        uint32 maxDepth = 0;
        double busyMs = 0.0; // depth-0 time inside the window
        std::vector<ProfileRecord> records;
    };
    struct StatsRow
    {
        const char* name = nullptr;
        uint8 category = 0;
        uint32 calls = 0;
        double totalMs = 0.0;
        double selfMs = 0.0;
    };
    struct SmoothedRow
    {
        double totalMs = 0.0;
        double selfMs = 0.0;
        double calls = 0.0;
        uint64 lastFrame = 0;
    };

    void refresh();
    void selectFrame(uint64 frameIdx);
    void snapshotTracks();
    void drawToolbar();
    void drawFrameGraph();
    void drawTimeline();
    void drawStatsTable();

    // ---- auto pause ----
    bool m_autoPause = false;
    float m_autoPauseMs = 33.4f;
    uint64 m_autoPauseChecked = 0; // newest frame index already tested (reset on enable/resume so history/the pause gap can't trigger)

    // ---- uncapped fps: what the frame WOULD run at without the vsync/fence throttle = the longer
    // ---- of the CPU work ("main loop", which excludes the fence wait) and the GPU work ("GPU Frame")
    // ---- of the displayed frame. Live view shows a short EMA so the number is readable; paused = exact.
    double m_uncappedMainMs = 0.0;
    double m_uncappedGpuMs = 0.0;
    double m_uncappedEmaMs = 0.0;
    uint64 m_uncappedEmaFrame = 0;       // frame the EMA was last fed (feed once per displayed frame)

    // ---- displayed window ----
    bool m_paused = false;
    uint64 m_displayedFrame = 0;         // profiler frame index shown
    uint64 m_windowStart = 0;            // ticks; the FRAME window (stats aggregate exactly this)
    uint64 m_windowEnd = 0;
    uint64 m_snapshotStart = 0;          // ticks; frame window expanded by the zoom/pan view, what snapshotTracks copies
    uint64 m_snapshotEnd = 0;
    std::vector<TrackView> m_tracks;

    // ---- timeline view state (ms relative to m_windowStart) ----
    double m_viewMin = 0.0;
    double m_viewMax = 16.0;
    bool m_userView = false;             // user zoomed/panned; stop auto-fitting
    std::unordered_map<uint32, bool> m_collapsed; // per trackIdx
    std::unordered_map<uint32, uint32> m_trackMaxDepth; // per trackIdx, monotonic: lane count stays constant so tracks don't shift vertically frame to frame

    // ---- stats state ----
    int m_trackFilter = -1;              // index into m_tracks, -1 = all
    bool m_smooth = true;
    char m_nameFilter[96] = {};
    std::unordered_map<const char*, SmoothedRow> m_smoothed;

    // scratch (persistent to avoid per-frame allocation)
    std::vector<StatsRow> m_statsRows;
    std::vector<uint64> m_childSumScratch;
};
