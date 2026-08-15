export module UI:MemoryPanel;

import Core;
import Core.MemoryTracker;

// The Memory window: a squarified treemap of the MemoryTracker's attribution tree - one nested box
// per profile-scope path, box AREA proportional to the bytes attributed there, children nested
// inside their parent scope's box, colored by profile category. Click a box to zoom into it,
// breadcrumb to zoom back out; metric toggles between live bytes and cumulative (churn).
export class MemoryPanel
{
public:
    // prepare() = the treemap SNAPSHOT (walks the tracker's tree — atomics, reader-safe on any
    // thread), no ImGui: UI::prepare runs it on a job. render() draws it (prepares inline if
    // nothing ran ahead). The metric toggle it reads is what the header set last frame.
    void prepare();
    void render();

private:

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
        std::vector<uint32> children; // indices into m_nodes, sorted desc by inclusiveBytes
    };

    uint32 buildSnapshot(const MemScopeNode* node);
    void drawHeader();
    void drawTreemap();
    void drawNode(uint32 nodeIdx, float x0, float y0, float x1, float y1, uint32 depth);

    std::vector<ViewNode> m_nodes; // snapshot rebuilt every frame; index 0 = tree root
    bool m_prepared = false;       // prepare() ran for this UI frame (render clears it)
    const MemScopeNode* m_zoom = nullptr; // zoom target (null = root); MemScopeNodes are never freed
    bool m_showCumulative = false;

    // per-frame draw state
    uint32 m_hoveredNode = UINT32_MAX;
    const MemScopeNode* m_clickedZoom = nullptr;
    bool m_canvasHovered = false;
};
