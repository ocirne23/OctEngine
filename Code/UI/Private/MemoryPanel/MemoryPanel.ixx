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
//
// The VRAM metric swaps the source: every live GPU allocation (the renderer's VMA registry), its
// debug name split into a path ('/' or '\' in the name, else '.'), under a top level of images /
// buffers / host-visible memory / VMA block slack. Equal names merge into one box.
//
// The panel itself allocates NOTHING per frame once warm (it would otherwise show up in its own
// treemap): the snapshot is a flat vector of trivially-copyable nodes whose children sit in a
// CONTIGUOUS index range (no per-node vector), and the treemap layout uses two kept stacks.
struct FRect { float x, y, w, h; }; // module-internal (the layout helpers in the .cpp use it too)

export class MemoryPanel
{
public:
    // prepare() = the treemap SNAPSHOT (walks the tracker's tree - atomics, reader-safe on any
    // thread - or the GPU allocation registry under its lock), no ImGui: UI::prepare runs it on a
    // job. render() draws it (prepares inline if nothing ran ahead). The metric toggle it reads is
    // what the header set last frame.
    void prepare();
    void render();

private:

    enum class EMetric : uint8
    {
        Live,       // live bytes at the path
        Cumulative, // total allocated since startup
        Churn,      // allocation bandwidth, bytes/sec (EMA-smoothed)
        Vram,       // live GPU allocations by debug name
    };

    enum EVramGroup : uint8 // ViewNode::category under the VRAM metric
    {
        VramRoot,
        VramImages,
        VramBuffers,
        VramHost,
        VramSlack,
        VramGroupCount,
    };

    struct ViewNode
    {
        const MemScopeNode* src = nullptr; // null under the VRAM metric
        const char* name = nullptr;
        uint64 id = 0;            // zoom identity, stable across snapshots: src, or the VRAM path hash
        uint32 parent = UINT32_MAX;
        uint8 category = 0;       // EProfileCategory, or EVramGroup under the VRAM metric
        int64 selfBytes = 0;      // selected metric, at exactly this path
        int64 inclusiveBytes = 0; // self + children
        int64 liveCount = 0;
        uint64 cumBytes = 0;
        uint64 cumCount = 0;      // VRAM: allocations in the whole subtree
        float rateBytes = 0.0f;   // smoothed self churn, bytes/sec
        float rateAllocs = 0.0f;  // smoothed self churn, allocations/sec
        // The children are m_nodes[firstChild .. firstChild + numChildren): the block is reserved
        // BEFORE the recursion descends into them, so it stays contiguous, and is sorted desc by
        // inclusiveBytes in place (sortChildren re-points the grandchildren's parent indices).
        uint32 firstChild = 0;
        uint32 numChildren = 0;
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

    // One GPU allocation of the VRAM snapshot. Its name in m_vramNames is the path with a '\0'
    // after every segment, so a plain byte compare sorts siblings together and every segment is a
    // C string in place.
    struct VramEntry
    {
        uint32 nameOffset = 0;
        uint32 nameLen = 0; // without the final '\0'
        uint64 bytes = 0;
        uint8 group = 0;
    };

    void buildSnapshot(uint32 idx, const MemScopeNode* node); // fills m_nodes[idx] (already sized) + its subtree
    void buildVramSnapshot();
    void addVramEntry(uint8 group, const char* name, uint64 bytes); // name nullptr = the group itself
    void buildVramNode(uint32 idx, uint32 begin, uint32 end, uint32 prefixLen); // m_vramEntries[begin, end) share prefixLen name bytes
    void sortChildren(uint32 first, uint32 count);
    uint32 nodeColor(const ViewNode& view) const;
    void drawHeader();
    void drawBreadcrumb();
    void drawTreemap();
    void drawNode(uint32 nodeIdx, float x0, float y0, float x1, float y1, uint32 depth);

    oc::vector<ViewNode> m_nodes; // snapshot rebuilt every frame (trivial nodes: capacity kept); index 0 = tree root
    // drawNode's layout scratch, used as STACKS: a node appends its children's areas/rects, recurses
    // (the children append above), then pops back - no per-node vector, no per-frame allocation.
    oc::vector<double> m_areaStack;
    oc::vector<FRect> m_rectStack;
    bool m_prepared = false;       // prepare() ran for this UI frame (render clears it)
    uint64 m_zoomId = 0;           // zoom target's ViewNode::id (0 = root)
    uint32 m_zoomIdx = 0;          // its index in this frame's snapshot (root if it vanished)
    EMetric m_metric = EMetric::Live;
    EMetric m_snapshotMetric = EMetric::Live; // what m_nodes was built with (the header may switch m_metric mid-render)

    // churn-rate sampling (updated by prepare every frame the panel is open under a CPU metric,
    // so switching to Churn shows warm data)
    oc::unordered_map<const MemScopeNode*, RateState> m_rates;
    double m_lastRateSampleSec = -1.0;
    double m_rateDt = 0.0;    // last folded sample interval (kept while frozen for the per-frame readout)
    float m_rateAlpha = 0.0f; // EMA blend factor for this frame's instantaneous rates
    bool m_rateFold = false;  // fold this frame's deltas (false = reseed/freeze: tracking off or long gap)

    // VRAM snapshot (kept capacity; the names stay valid until the next prepare)
    oc::vector<VramEntry> m_vramEntries;
    oc::vector<char> m_vramNames;
    uint64 m_vramUsed = 0;
    uint64 m_vramReserved = 0;
    uint64 m_vramBudget = 0;
    uint64 m_vramDriverUsage = 0;

    // per-frame draw state
    uint32 m_hoveredNode = UINT32_MAX;
    uint32 m_clickedZoom = UINT32_MAX; // snapshot index to zoom to at the end of render
    bool m_canvasHovered = false;
};
