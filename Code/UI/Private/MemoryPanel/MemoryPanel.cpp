module UI;

import Core;
import Core.Allocator;
import Core.imgui;
import Core.Windows;
import Core.MemoryTracker;
import Core.Time;
import :MemoryPanel;

namespace
{
    constexpr float kTitleHeight = 15.0f;  // label strip at the top of a box with visible children
    constexpr float kBoxPadding = 2.0f;
    constexpr float kMinBoxSize = 3.0f;    // below this a box draws as a filled sliver, no recursion
    constexpr uint32 kMaxDrawDepth = 12;
    constexpr double kRateHalfLifeSec = 0.5;   // churn-rate EMA half-life (per-frame deltas are bursty)
    constexpr double kRateMaxGapSec = 1.0;     // sample gap past this (panel closed) = reseed, no fold

    void formatBytes(char* buf, size_t bufSize, double bytes)
    {
        const double absBytes = bytes < 0.0 ? -bytes : bytes;
        if (absBytes >= 1024.0 * 1024.0 * 1024.0)
            sprintf_s(buf, bufSize, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else if (absBytes >= 1024.0 * 1024.0)
            sprintf_s(buf, bufSize, "%.2f MB", bytes / (1024.0 * 1024.0));
        else if (absBytes >= 1024.0)
            sprintf_s(buf, bufSize, "%.1f KB", bytes / 1024.0);
        else
            sprintf_s(buf, bufSize, "%.0f B", bytes);
    }

    uint32 scaleColor(uint32 abgr, float scale)
    {
        const uint32 r = oc::min(255u, (uint32)((abgr & 0xFF) * scale));
        const uint32 g = oc::min(255u, (uint32)(((abgr >> 8) & 0xFF) * scale));
        const uint32 b = oc::min(255u, (uint32)(((abgr >> 16) & 0xFF) * scale));
        return (abgr & 0xFF000000) | (b << 16) | (g << 8) | r;
    }

    uint32 boxTextColor(uint32 barColor)
    {
        const uint32 r = barColor & 0xFF, g = (barColor >> 8) & 0xFF, b = (barColor >> 16) & 0xFF;
        return (r * 3 + g * 4 + b * 2) > 1300 ? 0xE0101010 : 0xF0F0F0F0;
    }

    struct FRect { float x, y, w, h; };

    // Squarified treemap (Bruls et al.): greedily grow a row while it improves the worst aspect
    // ratio, lay each finished row along the current short side. areas are in px^2 and must sum to
    // at most rect area; outRects parallels areas.
    double rowWorstAspect(const oc::vector<double>& areas, uint32 begin, uint32 end, double shortSide)
    {
        double sum = 0.0, minArea = 1e300, maxArea = 0.0;
        for (uint32 i = begin; i < end; ++i)
        {
            sum += areas[i];
            minArea = oc::min(minArea, areas[i]);
            maxArea = oc::max(maxArea, areas[i]);
        }
        if (sum <= 0.0)
            return 1e300;
        const double s2 = sum * sum, w2 = shortSide * shortSide;
        return oc::max(w2 * maxArea / s2, s2 / (w2 * minArea));
    }

    void layoutRow(const oc::vector<double>& areas, uint32 begin, uint32 end, FRect& remaining, oc::vector<FRect>& outRects)
    {
        double rowArea = 0.0;
        for (uint32 i = begin; i < end; ++i)
            rowArea += areas[i];
        if (rowArea <= 0.0 || remaining.w <= 0.0f || remaining.h <= 0.0f)
        {
            for (uint32 i = begin; i < end; ++i)
                outRects[i] = FRect{ remaining.x, remaining.y, 0.0f, 0.0f };
            return;
        }
        if (remaining.w >= remaining.h) // row = vertical strip on the left
        {
            const float stripW = oc::min((float)(rowArea / remaining.h), remaining.w);
            float y = remaining.y;
            for (uint32 i = begin; i < end; ++i)
            {
                const float itemH = (float)(areas[i] / rowArea) * remaining.h;
                outRects[i] = FRect{ remaining.x, y, stripW, itemH };
                y += itemH;
            }
            remaining.x += stripW;
            remaining.w -= stripW;
        }
        else // row = horizontal strip on top
        {
            const float stripH = oc::min((float)(rowArea / remaining.w), remaining.h);
            float x = remaining.x;
            for (uint32 i = begin; i < end; ++i)
            {
                const float itemW = (float)(areas[i] / rowArea) * remaining.w;
                outRects[i] = FRect{ x, remaining.y, itemW, stripH };
                x += itemW;
            }
            remaining.y += stripH;
            remaining.h -= stripH;
        }
    }

    void squarify(const oc::vector<double>& areas, const FRect& rect, oc::vector<FRect>& outRects)
    {
        outRects.resize(areas.size());
        FRect remaining = rect;
        uint32 i = 0;
        while (i < (uint32)areas.size())
        {
            const double shortSide = oc::max(1.0, (double)oc::min(remaining.w, remaining.h));
            const uint32 rowStart = i;
            double best = rowWorstAspect(areas, rowStart, i + 1, shortSide);
            ++i;
            while (i < (uint32)areas.size())
            {
                const double with = rowWorstAspect(areas, rowStart, i + 1, shortSide);
                if (with > best)
                    break;
                best = with;
                ++i;
            }
            layoutRow(areas, rowStart, i, remaining, outRects);
        }
    }
}

void MemoryPanel::prepare()
{
    ProfileScope scope("Memory panel prepare", EProfileCategory::UI);
    m_prepared = true;
    m_nodes.clear();

    // Churn-rate sample window for this frame: the delta of each node's cumulative counters since
    // the previous prepare, folded into the EMA. A long gap (panel just opened) only reseeds the
    // baselines, else the whole gap's churn would read as one giant frame. Tracking disabled =
    // the counters stand still, so folding would decay every rate to zero - freeze instead.
    const double now = Globals::time.getElapsedSec();
    const double gap = m_lastRateSampleSec >= 0.0 ? now - m_lastRateSampleSec : -1.0;
    m_lastRateSampleSec = now;
    m_rateFold = gap > 0.0 && gap < kRateMaxGapSec && Globals::memoryTracker.isEnabled();
    if (m_rateFold)
    {
        m_rateDt = gap;
        m_rateAlpha = (float)(1.0 - std::pow(0.5, gap / kRateHalfLifeSec));
    }

    if (const MemScopeNode* root = Globals::memoryTracker.getRoot())
        buildSnapshot(root);
}

void MemoryPanel::render()
{
    if (!m_prepared)
        prepare(); // nothing ran ahead of us — inline
    m_prepared = false;
    const MemScopeNode* root = Globals::memoryTracker.getRoot();
    if (root == nullptr || m_nodes.empty())
    {
        ImGui::TextDisabled("MemoryTracker not initialized");
        return;
    }

    drawHeader();
    drawTreemap();

    if (m_clickedZoom != nullptr)
    {
        m_zoom = m_clickedZoom == root ? nullptr : m_clickedZoom;
        m_clickedZoom = nullptr;
    }
}

uint32 MemoryPanel::buildSnapshot(const MemScopeNode* node)
{
    const uint32 idx = (uint32)m_nodes.size();
    m_nodes.emplace_back();
    {
        ViewNode& view = m_nodes[idx];
        view.src = node;
        view.name = node->name;
        view.category = node->category;
        const int64 liveBytes = node->selfBytes.load(oc::memory_order_relaxed);
        view.cumBytes = node->totalAllocBytes.load(oc::memory_order_relaxed);
        view.cumCount = node->totalAllocCount.load(oc::memory_order_relaxed);
        view.liveCount = node->selfCount.load(oc::memory_order_relaxed);

        RateState& rate = m_rates[node];
        if (rate.seeded && m_rateFold)
        {
            const float instBytes = (float)((double)(view.cumBytes - rate.lastBytes) / m_rateDt);
            const float instAllocs = (float)((double)(view.cumCount - rate.lastCount) / m_rateDt);
            rate.bytesPerSec += m_rateAlpha * (instBytes - rate.bytesPerSec);
            rate.allocsPerSec += m_rateAlpha * (instAllocs - rate.allocsPerSec);
        }
        rate.lastBytes = view.cumBytes;
        rate.lastCount = view.cumCount;
        rate.seeded = true;
        view.rateBytes = rate.bytesPerSec;
        view.rateAllocs = rate.allocsPerSec;

        switch (m_metric)
        {
        case EMetric::Live:       view.selfBytes = oc::max<int64>(liveBytes, 0); break;
        case EMetric::Cumulative: view.selfBytes = (int64)view.cumBytes; break;
        case EMetric::Churn:      view.selfBytes = (int64)oc::max(view.rateBytes, 0.0f); break;
        }
    }
    int64 inclusive = m_nodes[idx].selfBytes;
    for (const MemScopeNode* child = node->firstChild.load(oc::memory_order_acquire); child != nullptr;
         child = child->nextSibling.load(oc::memory_order_relaxed))
    {
        const uint32 childIdx = buildSnapshot(child); // (m_nodes may reallocate - re-index below)
        m_nodes[idx].children.push_back(childIdx);
        inclusive += m_nodes[childIdx].inclusiveBytes;
    }
    ViewNode& view = m_nodes[idx];
    view.inclusiveBytes = inclusive;
    oc::sort(view.children.begin(), view.children.end(), [this](uint32 a, uint32 b)
        { return m_nodes[a].inclusiveBytes > m_nodes[b].inclusiveBytes; });
    return idx;
}

void MemoryPanel::drawHeader()
{
    MemoryTracker& tracker = Globals::memoryTracker;
    char bytesBuf[64], bytesBuf2[64];

    bool enabled = tracker.isEnabled();
    if (ImGui::Checkbox("Track", &enabled))
        tracker.setEnabled(enabled);
    ImGui::SameLine();
    int metric = (int)m_metric;
    bool metricChanged = ImGui::RadioButton("Live", &metric, (int)EMetric::Live);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Box sizes from LIVE bytes (currently allocated)");
    ImGui::SameLine();
    metricChanged |= ImGui::RadioButton("Cumulative", &metric, (int)EMetric::Cumulative);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Box sizes from CUMULATIVE allocated bytes (total churn since startup)");
    ImGui::SameLine();
    metricChanged |= ImGui::RadioButton("Churn/s", &metric, (int)EMetric::Churn);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Box sizes from ALLOCATOR BANDWIDTH: bytes allocated per second at each path,\nsmoothed - shows what code churns memory every frame");
    if (metricChanged)
    {
        m_metric = (EMetric)metric;
        m_zoom = nullptr; // metric switch: restart from the top
    }

    formatBytes(bytesBuf, sizeof(bytesBuf), (double)(Globals::allocator.getUsedSize() + getAlignedAllocatedSize()));
    formatBytes(bytesBuf2, sizeof(bytesBuf2), (double)tracker.getTrackedBytes());
    ImGui::SameLine();
    ImGui::Text("engine: %s  |  tracked: %s in %u allocs, %u paths", bytesBuf, bytesBuf2, tracker.getTrackedCount(), tracker.getNumNodes());

    // Whole process, for perspective: the gap vs "engine" is everything the hooks can't see
    // (driver + host-visible GPU memory, onnxruntime, raw-malloc C libs, images, stacks, heap
    // overhead - see getProcessMemoryUsage's commit/working-set distinction).
    SIZE_T processPrivate = 0, processWorkingSet = 0;
    if (getProcessMemoryUsage(processPrivate, processWorkingSet))
    {
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)processPrivate);
        formatBytes(bytesBuf2, sizeof(bytesBuf2), (double)processWorkingSet);
        ImGui::SameLine();
        ImGui::TextDisabled("|  process: %s commit, %s working set", bytesBuf, bytesBuf2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Whole-process memory (what Task Manager shows). The difference vs 'engine' is\nmemory the allocator hooks never see: GPU driver + host-visible allocations,\nonnxruntime/DirectML, raw-malloc C libraries, mapped images, stacks, heap overhead.");
    }
    if (tracker.getDroppedAllocs() > 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(%llu dropped)", (unsigned long long)tracker.getDroppedAllocs());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Allocations the tracker could not record (pointer map shard or node pool full) -\nthe view under-reports by whatever those hold");
    }

    // Total allocator bandwidth (root inclusive = every path) while the churn metric is up
    const char* suffix = m_metric == EMetric::Churn ? "/s" : "";
    if (m_metric == EMetric::Churn && !m_nodes.empty())
    {
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)m_nodes[0].inclusiveBytes);
        formatBytes(bytesBuf2, sizeof(bytesBuf2), (double)m_nodes[0].inclusiveBytes * m_rateDt);
        ImGui::Text("bandwidth: %s/s  (~%s per frame)", bytesBuf, bytesBuf2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bytes allocated per second across every tracked path (EMA-smoothed);\nthe per-frame figure is that rate over the last sample interval");
    }

    // Per-category totals (self metric summed over every path)
    oc::array<int64, (uint32)EProfileCategory::Count> categoryBytes = {};
    for (const ViewNode& view : m_nodes)
        categoryBytes[view.category] += view.selfBytes;
    bool first = true;
    for (uint32 category = 0; category < (uint32)EProfileCategory::Count; ++category)
    {
        if (categoryBytes[category] < 1024)
            continue;
        if (!first)
            ImGui::SameLine();
        first = false;
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)categoryBytes[category]);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(profileCategoryColor((EProfileCategory)category)),
            "%s %s%s", profileCategoryName((EProfileCategory)category), bytesBuf, suffix);
    }
    if (first)
        ImGui::TextDisabled("no attributed allocations yet");

    // Breadcrumb for the zoom path
    if (m_zoom != nullptr)
    {
        // walk up to build the path root-first
        const MemScopeNode* path[64];
        uint32 pathLen = 0;
        for (const MemScopeNode* walk = m_zoom; walk != nullptr && pathLen < 64; walk = walk->parent)
            path[pathLen++] = walk;
        for (uint32 i = pathLen; i-- > 0;)
        {
            if (ImGui::SmallButton(path[i]->name))
                m_clickedZoom = path[i];
            if (i != 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled(">");
                ImGui::SameLine();
            }
        }
    }
}

void MemoryPanel::drawTreemap()
{
    ProfileScope scope("Memory treemap", EProfileCategory::UI);
    // Find the zoomed node's snapshot index (fall back to root if it vanished from view).
    uint32 rootIdx = 0;
    if (m_zoom != nullptr)
        for (uint32 i = 0; i < (uint32)m_nodes.size(); ++i)
            if (m_nodes[i].src == m_zoom) { rootIdx = i; break; }

    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = oc::max(avail.x, 60.0f);
    const float height = oc::max(avail.y, 60.0f);
    ImGui::InvisibleButton("##treemap", ImVec2(width, height));
    m_canvasHovered = ImGui::IsItemHovered();
    m_hoveredNode = UINT32_MAX;

    ImGui::GetWindowDrawList()->AddRectFilled(canvasPos, ImVec2(canvasPos.x + width, canvasPos.y + height), 0xFF161618);
    if (m_nodes[rootIdx].inclusiveBytes > 0)
        drawNode(rootIdx, canvasPos.x, canvasPos.y, canvasPos.x + width, canvasPos.y + height, 0);
    else
        ImGui::GetWindowDrawList()->AddText(ImVec2(canvasPos.x + 8.0f, canvasPos.y + 8.0f), 0x60FFFFFF, "nothing tracked in this view yet");

    if (m_hoveredNode != UINT32_MAX)
    {
        const ViewNode& view = m_nodes[m_hoveredNode];
        char bytesBuf[64];
        ImGui::BeginTooltip();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(profileCategoryColor((EProfileCategory)view.category)), "%s", view.name);
        // full path
        char pathBuf[256] = {};
        const MemScopeNode* path[64];
        uint32 pathLen = 0;
        for (const MemScopeNode* walk = view.src; walk != nullptr && pathLen < 64; walk = walk->parent)
            path[pathLen++] = walk;
        size_t pathOffset = 0;
        for (uint32 i = pathLen; i-- > 0 && pathOffset + 4 < sizeof(pathBuf);)
            pathOffset += (size_t)sprintf_s(pathBuf + pathOffset, sizeof(pathBuf) - pathOffset, i == pathLen - 1 ? "%s" : " > %s", path[i]->name);
        ImGui::TextDisabled("%s", pathBuf);

        const char* metricLabel = m_metric == EMetric::Live ? "Live"
            : m_metric == EMetric::Cumulative ? "Cumulative" : "Churn";
        const char* suffix = m_metric == EMetric::Churn ? "/s" : "";
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.inclusiveBytes);
        ImGui::Text("%s: %s%s", metricLabel, bytesBuf, suffix);
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.selfBytes);
        if (m_metric == EMetric::Churn)
            ImGui::Text("Self: %s/s, %.0f allocs/s", bytesBuf, (double)view.rateAllocs);
        else
            ImGui::Text("Self: %s in %lld allocs", bytesBuf, (long long)view.liveCount);
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.cumBytes);
        ImGui::Text("Churn: %s in %llu allocs total", bytesBuf, (unsigned long long)view.cumCount);
        if (m_metric != EMetric::Churn)
        {
            formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.rateBytes);
            ImGui::Text("Rate: %s/s, %.0f allocs/s", bytesBuf, (double)view.rateAllocs);
        }
        ImGui::Text("Category: %s", profileCategoryName((EProfileCategory)view.category));
        if (!view.children.empty())
            ImGui::TextDisabled("click to zoom");
        ImGui::EndTooltip();
    }
    if (m_canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_zoom != nullptr)
        m_clickedZoom = m_zoom->parent != nullptr ? m_zoom->parent : m_zoom; // right-click = up one level
}

void MemoryPanel::drawNode(uint32 nodeIdx, float x0, float y0, float x1, float y1, uint32 depth)
{
    const ViewNode& view = m_nodes[nodeIdx];
    const float w = x1 - x0, h = y1 - y0;
    if (w < 0.5f || h < 0.5f)
        return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const uint32 baseColor = profileCategoryColor((EProfileCategory)view.category);
    const uint32 fillColor = scaleColor(baseColor, 1.0f - 0.07f * (float)oc::min(depth, 6u));

    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fillColor);
    drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), 0x66000000);

    // hover + click (deepest hovered box wins: children draw after and overwrite)
    const ImVec2 mousePos = ImGui::GetMousePos();
    const bool hovered = m_canvasHovered && mousePos.x >= x0 && mousePos.x < x1 && mousePos.y >= y0 && mousePos.y < y1;
    if (hovered)
    {
        m_hoveredNode = nodeIdx;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view.children.empty())
            m_clickedZoom = view.src;
    }

    const bool drawTitle = h >= kTitleHeight * 2.0f && w >= 40.0f && !view.children.empty();
    if (drawTitle || (view.children.empty() && w >= 32.0f && h >= 12.0f))
    {
        char bytesBuf[64], labelBuf[160];
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.inclusiveBytes);
        sprintf_s(labelBuf, sizeof(labelBuf), "%s  %s%s", view.name, bytesBuf,
            m_metric == EMetric::Churn ? "/s" : "");
        drawList->PushClipRect(ImVec2(x0 + 2.0f, y0), ImVec2(x1 - 2.0f, y0 + kTitleHeight), true);
        drawList->AddText(ImVec2(x0 + 4.0f, y0 + 1.0f), boxTextColor(fillColor), labelBuf);
        drawList->PopClipRect();
    }

    if (view.children.empty() || depth >= kMaxDrawDepth || w < kMinBoxSize * 4.0f || h < kMinBoxSize * 4.0f)
        return;

    // Children squarified into the content area under the title strip. No explicit "(self)" box:
    // children are sized by the parent's per-byte scale (areaScale divides by INCLUSIVE bytes), so
    // whatever parent background stays uncovered IS the node's own allocations - hovering it hits
    // the parent, whose tooltip shows the exact Self value.
    const float cx0 = x0 + kBoxPadding, cy0 = y0 + (drawTitle ? kTitleHeight : kBoxPadding);
    const float cx1 = x1 - kBoxPadding, cy1 = y1 - kBoxPadding;
    if (cx1 - cx0 < kMinBoxSize || cy1 - cy0 < kMinBoxSize || view.inclusiveBytes <= 0)
        return;

    const double areaScale = (double)(cx1 - cx0) * (double)(cy1 - cy0) / (double)view.inclusiveBytes;
    oc::vector<double> areas(view.children.size());
    for (size_t i = 0; i < view.children.size(); ++i)
        areas[i] = (double)oc::max<int64>(m_nodes[view.children[i]].inclusiveBytes, 0) * areaScale;
    oc::vector<FRect> rects;
    squarify(areas, FRect{ cx0, cy0, cx1 - cx0, cy1 - cy0 }, rects);

    for (size_t i = 0; i < view.children.size(); ++i)
    {
        const FRect& rect = rects[i];
        if (rect.w < 0.5f || rect.h < 0.5f)
            continue;
        drawNode(view.children[i], rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, depth + 1);
    }
}
