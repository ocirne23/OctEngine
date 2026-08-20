export module UI:MemoryPanel;

import Core;
import Core.MemoryTracker;

// The Memory window: a squarified treemap of the MemoryTracker's attribution tree - one nested box
// per profile-scope path, box AREA proportional to the bytes attributed there, children nested
// inside their parent scope's box, colored by profile category. Click a box to zoom into it,
// breadcrumb to zoom back out; metric selects live bytes, cumulative (churn since startup), or
// CHURN RATE (allocator bandwidth: bytes allocated per second, EMA-smoothed per path - the
// tracker's cumulative counters are sampled every prepare and the delta over the sample gap is
// the instantaneous rate, so the treemap shows what code allocates every frame).
export class MemoryPanel
{
public:
    // prepare() = the treemap SNAPSHOT (walks the tracker's tree — atomics, reader-safe on any
    // thread), no ImGui: UI::prepare runs it on a job. render() draws it (prepares inline if
    // nothing ran ahead). The metric toggle it reads is what the header set last frame.
    void prepare();
    void render();

private:

    enum class EMetric : uint8
    {
        Live,       // live bytes at the path
        Cumulative, // total allocated since startup
        Churn,      // allocation bandwidth, bytes/sec (EMA-smoothed)
    };

    struct ViewNode
    {
        const MemScopeNode* src = nullptr;
        const char* name = nullptr;
        uint8 category = 0;
        int64 selfBytes = 0;      // selected metric, at exactly this path
        int64 inclusiveBytes = 0; // self + children
        int64 liveCount = 0;
        uint64 cumBytes = 0;
        uint64 cumCount = 0;
        float rateBytes = 0.0f;   // smoothed self churn, bytes/sec
        float rateAllocs = 0.0f;  // smoothed self churn, allocations/sec
        oc::vector<uint32> children; // indices into m_nodes, sorted desc by inclusiveBytes
    };

    // Per-path rate state, persistent across frames (m_nodes is rebuilt every frame). Keyed by the
    // tracker node pointer - MemScopeNodes are pool-allocated and never freed, so keys stay valid
    // and the map is bounded by MemoryTracker::MAX_NODES.
    struct RateState
    {
        uint64 lastBytes = 0;
        uint64 lastCount = 0;
        float bytesPerSec = 0.0f;
        float allocsPerSec = 0.0f;
        bool seeded = false; // first sample only records the baseline
    };

    uint32 buildSnapshot(const MemScopeNode* node);
    void drawHeader();
    void drawTreemap();
    void drawNode(uint32 nodeIdx, float x0, float y0, float x1, float y1, uint32 depth);

    oc::vector<ViewNode> m_nodes; // snapshot rebuilt every frame; index 0 = tree root
    bool m_prepared = false;       // prepare() ran for this UI frame (render clears it)
    const MemScopeNode* m_zoom = nullptr; // zoom target (null = root); MemScopeNodes are never freed
    EMetric m_metric = EMetric::Live;

    // churn-rate sampling (updated by prepare every frame the panel is open, whatever the metric,
    // so switching to Churn shows warm data)
    oc::unordered_map<const MemScopeNode*, RateState> m_rates;
    double m_lastRateSampleSec = -1.0;
    double m_rateDt = 0.0;    // last folded sample interval (kept while frozen for the per-frame readout)
    float m_rateAlpha = 0.0f; // EMA blend factor for this frame's instantaneous rates
    bool m_rateFold = false;  // fold this frame's deltas (false = reseed/freeze: tracking off or long gap)

    // per-frame draw state
    uint32 m_hoveredNode = UINT32_MAX;
    const MemScopeNode* m_clickedZoom = nullptr;
    bool m_canvasHovered = false;
};
