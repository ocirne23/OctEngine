module UI;

import Core;
import Core.Allocator;
import Core.imgui;
import Core.Windows;
import Core.MemoryTracker;
import Core.Time;
import RendererVK;
import :MemoryPanel;

namespace
{
    constexpr float kTitleHeight = 15.0f;  // label strip at the top of a box with visible children
    constexpr float kBoxPadding = 2.0f;
    constexpr float kMinBoxSize = 3.0f;    // below this a box draws as a filled sliver, no recursion
    constexpr uint32 kMaxDrawDepth = 12;
    constexpr double kRateHalfLifeSec = 0.5;   // churn-rate EMA half-life (per-frame deltas are bursty)
    constexpr double kRateMaxGapSec = 1.0;     // sample gap past this (panel closed) = reseed, no fold

    // Indexed by MemoryPanel::EVramGroup
    constexpr const char* kVramGroupNames[] = { "GPU memory", "Images", "Buffers", "Host-visible", "VMA block slack" };
    constexpr uint32 kVramGroupColors[] = { 0xFF403C3C, 0xFFC88246, 0xFF5AA050, 0xFF3C8CC8, 0xFF6E6E6E };
    constexpr uint64 kVramRootId = 14695981039346656037ull; // FNV-1a offset basis

    uint64 hashSegment(uint64 parentId, const char* segment, uint32 len)
    {
        uint64 hash = parentId ^ 0x2F; // separator byte, so "ab"+"c" and "a"+"bc" differ
        hash *= 1099511628211ull;
        for (uint32 i = 0; i < len; ++i)
        {
            hash ^= (uint8)segment[i];
            hash *= 1099511628211ull;
        }
        return hash != 0 ? hash : 1; // 0 = the root zoom sentinel
    }

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

    // Squarified treemap (Bruls et al.): greedily grow a row while it improves the worst aspect
    // ratio, lay each finished row along the current short side. areas are in px^2 and must sum to
    // at most rect area; outRects parallels areas (both `count` long, caller-owned).
    double rowWorstAspect(const double* areas, uint32 begin, uint32 end, double shortSide)
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

    void layoutRow(const double* areas, uint32 begin, uint32 end, FRect& remaining, FRect* outRects)
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

    void squarify(const double* areas, uint32 count, const FRect& rect, FRect* outRects)
    {
        FRect remaining = rect;
        uint32 i = 0;
        while (i < count)
        {
            const double shortSide = oc::max(1.0, (double)oc::min(remaining.w, remaining.h));
            const uint32 rowStart = i;
            double best = rowWorstAspect(areas, rowStart, i + 1, shortSide);
            ++i;
            while (i < count)
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
    m_snapshotMetric = m_metric;
    if (m_metric == EMetric::Vram)
    {
        buildVramSnapshot();
        return;
    }

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
    {
        m_nodes.resize(1); // trivial nodes: clear + resize keeps the capacity
        buildSnapshot(0, root);
    }
}

void MemoryPanel::render()
{
    if (!m_prepared)
        prepare(); // nothing ran ahead of us - inline
    m_prepared = false;

    m_zoomIdx = 0;
    if (m_zoomId != 0)
        for (uint32 i = 1; i < (uint32)m_nodes.size(); ++i)
            if (m_nodes[i].id == m_zoomId) { m_zoomIdx = i; break; }

    drawHeader();
    if (m_nodes.empty())
    {
        ImGui::TextDisabled("MemoryTracker not initialized");
        return;
    }
    drawBreadcrumb();
    drawTreemap();

    if (m_clickedZoom != UINT32_MAX)
    {
        m_zoomId = m_clickedZoom == 0 ? 0 : m_nodes[m_clickedZoom].id;
        m_clickedZoom = UINT32_MAX;
    }
}

void MemoryPanel::buildSnapshot(uint32 idx, const MemScopeNode* node)
{
    {
        ViewNode& view = m_nodes[idx];
        view = ViewNode();
        view.src = node;
        view.id = (uint64)(uintptr_t)node;
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
    // Reserve the children's CONTIGUOUS block first, then descend: everything the subtrees append
    // lands above it, so the block never fragments. (m_nodes may reallocate - index, never hold.)
    uint32 numChildren = 0;
    for (const MemScopeNode* child = node->firstChild.load(oc::memory_order_acquire); child != nullptr;
         child = child->nextSibling.load(oc::memory_order_relaxed))
        ++numChildren;
    const uint32 first = (uint32)m_nodes.size();
    m_nodes.resize(first + numChildren);
    m_nodes[idx].firstChild = first;
    m_nodes[idx].numChildren = numChildren;
    int64 inclusive = m_nodes[idx].selfBytes;
    uint32 k = 0;
    for (const MemScopeNode* child = node->firstChild.load(oc::memory_order_acquire); child != nullptr;
         child = child->nextSibling.load(oc::memory_order_relaxed), ++k)
    {
        buildSnapshot(first + k, child);
        m_nodes[first + k].parent = idx;
        inclusive += m_nodes[first + k].inclusiveBytes;
    }
    m_nodes[idx].inclusiveBytes = inclusive;
    sortChildren(first, numChildren);
}

void MemoryPanel::sortChildren(uint32 first, uint32 count)
{
    oc::sort(m_nodes.begin() + first, m_nodes.begin() + first + count,
        [](const ViewNode& a, const ViewNode& b) { return a.inclusiveBytes > b.inclusiveBytes; });
    // The sort moved the children: re-point their own children's parent index at the new slots.
    for (uint32 c = first; c < first + count; ++c)
        for (uint32 g = 0; g < m_nodes[c].numChildren; ++g)
            m_nodes[m_nodes[c].firstChild + g].parent = c;
}

void MemoryPanel::addVramEntry(uint8 group, const char* name, uint64 bytes)
{
    const uint32 offset = (uint32)m_vramNames.size();
    const char* label = kVramGroupNames[group];
    m_vramNames.insert(m_vramNames.end(), label, label + strlen(label));
    if (name != nullptr)
    {
        // A path splits on its slashes; a plain name like "GI.volumeSky" on its dots.
        const bool isPath = strpbrk(name, "/\\") != nullptr;
        bool inSegment = false;
        for (const char* c = name; *c != '\0'; ++c)
        {
            if (*c == '/' || *c == '\\' || (!isPath && *c == '.'))
            {
                inSegment = false;
                continue;
            }
            if (!inSegment)
                m_vramNames.push_back('\0');
            inSegment = true;
            m_vramNames.push_back(*c);
        }
    }
    const uint32 nameLen = (uint32)m_vramNames.size() - offset;
    m_vramNames.push_back('\0');
    m_vramEntries.push_back(VramEntry{ offset, nameLen, bytes, group });
}

void MemoryPanel::buildVramSnapshot()
{
    m_vramEntries.clear();
    m_vramNames.clear();
    Globals::rendererVK.forEachGpuAllocation(+[](void* ctx, const char* name, uint64 bytes, bool image, bool deviceLocal)
        {
            const uint8 group = !deviceLocal ? VramHost : image ? VramImages : VramBuffers;
            static_cast<MemoryPanel*>(ctx)->addVramEntry(group, name != nullptr && name[0] != '\0' ? name : "<unnamed>", bytes);
        }, this);

    const auto usage = Globals::rendererVK.getGpuMemoryUsage();
    m_vramUsed = usage.usedBytes;
    m_vramReserved = usage.reservedBytes;
    m_vramBudget = usage.budgetBytes;
    m_vramDriverUsage = usage.deviceLocalUsageBytes;
    if (usage.reservedBytes > usage.usedBytes)
        addVramEntry(VramSlack, nullptr, usage.reservedBytes - usage.usedBytes);

    const char* names = m_vramNames.data();
    oc::sort(m_vramEntries.begin(), m_vramEntries.end(), [names](const VramEntry& a, const VramEntry& b)
        {
            const int order = memcmp(names + a.nameOffset, names + b.nameOffset, oc::min(a.nameLen, b.nameLen));
            return order != 0 ? order < 0 : a.nameLen < b.nameLen;
        });

    m_nodes.resize(1);
    ViewNode& root = m_nodes[0];
    root = ViewNode();
    root.name = kVramGroupNames[VramRoot];
    root.id = kVramRootId;
    root.category = VramRoot;
    buildVramNode(0, 0, (uint32)m_vramEntries.size(), 0);
}

void MemoryPanel::buildVramNode(uint32 idx, uint32 begin, uint32 end, uint32 prefixLen)
{
    const char* names = m_vramNames.data();
    // Entries whose whole name IS the prefix are this node's own bytes; they sort first.
    int64 selfBytes = 0, selfCount = 0;
    uint32 i = begin;
    for (; i < end && m_vramEntries[i].nameLen == prefixLen; ++i)
    {
        selfBytes += (int64)m_vramEntries[i].bytes;
        ++selfCount;
    }

    // The rest groups into runs of equal next segments, one child each.
    const uint32 segStart = prefixLen == 0 ? 0 : prefixLen + 1; // past the '\0' separator
    auto runEnd = [&](uint32 j, uint32 segLen)
    {
        const char* segment = names + m_vramEntries[j].nameOffset + segStart;
        uint32 k = j + 1;
        while (k < end)
        {
            const VramEntry& entry = m_vramEntries[k];
            if (entry.nameLen < segStart + segLen || names[entry.nameOffset + segStart + segLen] != '\0'
                || memcmp(names + entry.nameOffset + segStart, segment, segLen) != 0)
                break;
            ++k;
        }
        return k;
    };
    auto segmentLen = [&](uint32 j) { return (uint32)strlen(names + m_vramEntries[j].nameOffset + segStart); };

    uint32 numChildren = 0;
    for (uint32 j = i; j < end; j = runEnd(j, segmentLen(j)))
        ++numChildren;

    // Reserve the children's contiguous block before descending (see buildSnapshot)
    const uint32 first = (uint32)m_nodes.size();
    m_nodes.resize(first + numChildren);
    int64 inclusive = selfBytes;
    uint64 inclusiveCount = (uint64)selfCount;
    uint32 child = first;
    for (uint32 j = i; j < end; ++child)
    {
        const uint32 segLen = segmentLen(j);
        const uint32 k = runEnd(j, segLen);
        {
            ViewNode& view = m_nodes[child];
            view = ViewNode();
            view.name = names + m_vramEntries[j].nameOffset + segStart;
            view.id = hashSegment(m_nodes[idx].id, view.name, segLen);
            view.parent = idx;
            view.category = m_vramEntries[j].group;
        }
        buildVramNode(child, j, k, segStart + segLen);
        inclusive += m_nodes[child].inclusiveBytes;
        inclusiveCount += m_nodes[child].cumCount;
        j = k;
    }

    ViewNode& view = m_nodes[idx];
    view.selfBytes = selfBytes;
    view.liveCount = selfCount;
    view.inclusiveBytes = inclusive;
    view.cumBytes = (uint64)inclusive;
    view.cumCount = inclusiveCount;
    view.firstChild = first;
    view.numChildren = numChildren;
    sortChildren(first, numChildren);
}

uint32 MemoryPanel::nodeColor(const ViewNode& view) const
{
    if (m_snapshotMetric == EMetric::Vram)
        return kVramGroupColors[view.category < VramGroupCount ? view.category : VramRoot];
    return profileCategoryColor((EProfileCategory)view.category);
}

void MemoryPanel::drawHeader()
{
    MemoryTracker& tracker = Globals::memoryTracker;
    char bytesBuf[64], bytesBuf2[64];

    if (m_metric != EMetric::Vram)
    {
        bool enabled = tracker.isEnabled();
        if (ImGui::Checkbox("Track", &enabled))
            tracker.setEnabled(enabled);
        ImGui::SameLine();
    }
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
    ImGui::SameLine();
    metricChanged |= ImGui::RadioButton("VRAM", &metric, (int)EMetric::Vram);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Box sizes from live GPU allocations (VMA), grouped by resource debug name:\na name with '/' splits into folders, else on '.'. Equal names merge into one box.");
    if (metricChanged)
    {
        m_metric = (EMetric)metric;
        m_zoomId = 0; // metric switch: restart from the top
    }

    if (m_snapshotMetric == EMetric::Vram)
    {
        const uint64 numAllocs = m_nodes.empty() ? 0 : m_nodes[0].cumCount;
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)m_vramUsed);
        formatBytes(bytesBuf2, sizeof(bytesBuf2), (double)m_vramReserved);
        ImGui::SameLine();
        ImGui::Text("used: %s in %llu allocs  |  VMA blocks: %s", bytesBuf, (unsigned long long)numAllocs, bytesBuf2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("used = the live allocations, every heap (host-visible included)\nVMA blocks = the memory VMA holds; the difference is the 'VMA block slack' box");
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)m_vramDriverUsage);
        formatBytes(bytesBuf2, sizeof(bytesBuf2), (double)m_vramBudget);
        ImGui::SameLine();
        ImGui::TextDisabled("|  device-local: %s of %s budget", bytesBuf, bytesBuf2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What the driver reports this process uses in device-local heaps (VK_EXT_memory_budget),\nagainst the budget it grants. Past the VMA blocks it holds swapchain images and driver-internal memory.");

        // Per-group totals: the root's children are the groups
        bool first = true;
        if (!m_nodes.empty())
            for (uint32 i = 0; i < m_nodes[0].numChildren; ++i)
            {
                const ViewNode& group = m_nodes[m_nodes[0].firstChild + i];
                if (!first)
                    ImGui::SameLine();
                first = false;
                formatBytes(bytesBuf, sizeof(bytesBuf), (double)group.inclusiveBytes);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(nodeColor(group)), "%s %s", group.name, bytesBuf);
            }
        if (first)
            ImGui::TextDisabled("no GPU allocations");
        return;
    }

    formatBytes(bytesBuf, sizeof(bytesBuf), (double)Globals::allocator.getUsedSize());
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
    const char* suffix = m_snapshotMetric == EMetric::Churn ? "/s" : "";
    if (m_snapshotMetric == EMetric::Churn && !m_nodes.empty())
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
}

void MemoryPanel::drawBreadcrumb()
{
    if (m_zoomIdx == 0)
        return;
    // walk up to build the path root-first
    uint32 path[64];
    uint32 pathLen = 0;
    for (uint32 walk = m_zoomIdx; walk != UINT32_MAX && pathLen < 64; walk = m_nodes[walk].parent)
        path[pathLen++] = walk;
    for (uint32 i = pathLen; i-- > 0;)
    {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton(m_nodes[path[i]].name))
            m_clickedZoom = path[i];
        ImGui::PopID();
        if (i != 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }
    }
}

void MemoryPanel::drawTreemap()
{
    ProfileScope scope("Memory treemap", EProfileCategory::UI);
    const uint32 rootIdx = m_zoomIdx;

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
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(nodeColor(view)), "%s", view.name);
        // full path
        char pathBuf[256] = {};
        uint32 path[64];
        uint32 pathLen = 0;
        for (uint32 walk = m_hoveredNode; walk != UINT32_MAX && pathLen < 64; walk = m_nodes[walk].parent)
            path[pathLen++] = walk;
        size_t pathOffset = 0;
        for (uint32 i = pathLen; i-- > 0;)
        {
            const int written = _snprintf_s(pathBuf + pathOffset, sizeof(pathBuf) - pathOffset, _TRUNCATE,
                i == pathLen - 1 ? "%s" : " > %s", m_nodes[path[i]].name);
            if (written < 0)
                break; // truncated: the buffer is full
            pathOffset += (size_t)written;
        }
        ImGui::TextDisabled("%s", pathBuf);

        if (m_snapshotMetric == EMetric::Vram)
        {
            formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.inclusiveBytes);
            ImGui::Text("Size: %s in %llu allocs", bytesBuf, (unsigned long long)view.cumCount);
            if (view.numChildren != 0 && view.liveCount != 0)
            {
                formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.selfBytes);
                ImGui::Text("Self: %s in %lld allocs", bytesBuf, (long long)view.liveCount);
            }
            ImGui::Text("Group: %s", kVramGroupNames[view.category < VramGroupCount ? view.category : VramRoot]);
        }
        else
        {
            const char* metricLabel = m_snapshotMetric == EMetric::Live ? "Live"
                : m_snapshotMetric == EMetric::Cumulative ? "Cumulative" : "Churn";
            const char* suffix = m_snapshotMetric == EMetric::Churn ? "/s" : "";
            formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.inclusiveBytes);
            ImGui::Text("%s: %s%s", metricLabel, bytesBuf, suffix);
            formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.selfBytes);
            if (m_snapshotMetric == EMetric::Churn)
                ImGui::Text("Self: %s/s, %.0f allocs/s", bytesBuf, (double)view.rateAllocs);
            else
                ImGui::Text("Self: %s in %lld allocs", bytesBuf, (long long)view.liveCount);
            formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.cumBytes);
            ImGui::Text("Churn: %s in %llu allocs total", bytesBuf, (unsigned long long)view.cumCount);
            if (m_snapshotMetric != EMetric::Churn)
            {
                formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.rateBytes);
                ImGui::Text("Rate: %s/s, %.0f allocs/s", bytesBuf, (double)view.rateAllocs);
            }
            ImGui::Text("Category: %s", profileCategoryName((EProfileCategory)view.category));
        }
        if (view.numChildren != 0)
            ImGui::TextDisabled("click to zoom");
        ImGui::EndTooltip();
    }
    if (m_canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && m_zoomIdx != 0)
        m_clickedZoom = m_nodes[m_zoomIdx].parent; // right-click = up one level
}

void MemoryPanel::drawNode(uint32 nodeIdx, float x0, float y0, float x1, float y1, uint32 depth)
{
    const ViewNode& view = m_nodes[nodeIdx];
    const float w = x1 - x0, h = y1 - y0;
    if (w < 0.5f || h < 0.5f)
        return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const uint32 baseColor = nodeColor(view);
    const uint32 fillColor = scaleColor(baseColor, 1.0f - 0.07f * (float)oc::min(depth, 6u));

    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fillColor);
    drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), 0x66000000);

    // hover + click (deepest hovered box wins: children draw after and overwrite)
    const ImVec2 mousePos = ImGui::GetMousePos();
    const bool hovered = m_canvasHovered && mousePos.x >= x0 && mousePos.x < x1 && mousePos.y >= y0 && mousePos.y < y1;
    if (hovered)
    {
        m_hoveredNode = nodeIdx;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && view.numChildren != 0)
            m_clickedZoom = nodeIdx;
    }

    const bool drawTitle = h >= kTitleHeight * 2.0f && w >= 40.0f && view.numChildren != 0;
    if (drawTitle || (view.numChildren == 0 && w >= 32.0f && h >= 12.0f))
    {
        char bytesBuf[64], labelBuf[160];
        formatBytes(bytesBuf, sizeof(bytesBuf), (double)view.inclusiveBytes);
        _snprintf_s(labelBuf, sizeof(labelBuf), _TRUNCATE, "%s  %s%s", view.name, bytesBuf,
            m_snapshotMetric == EMetric::Churn ? "/s" : "");
        drawList->PushClipRect(ImVec2(x0 + 2.0f, y0), ImVec2(x1 - 2.0f, y0 + kTitleHeight), true);
        drawList->AddText(ImVec2(x0 + 4.0f, y0 + 1.0f), boxTextColor(fillColor), labelBuf);
        drawList->PopClipRect();
    }

    if (view.numChildren == 0 || depth >= kMaxDrawDepth || w < kMinBoxSize * 4.0f || h < kMinBoxSize * 4.0f)
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
    // Layout scratch as a stack: this node's range is [base, base + n); the recursion below appends
    // above it and pops back to our top, so the range stays ours. Index, never hold a reference -
    // a child's push may reallocate.
    const uint32 n = view.numChildren;
    const size_t base = m_areaStack.size();
    m_areaStack.resize(base + n);
    m_rectStack.resize(base + n);
    for (uint32 i = 0; i < n; ++i)
        m_areaStack[base + i] = (double)oc::max<int64>(m_nodes[view.firstChild + i].inclusiveBytes, 0) * areaScale;
    squarify(m_areaStack.data() + base, n, FRect{ cx0, cy0, cx1 - cx0, cy1 - cy0 }, m_rectStack.data() + base);

    for (uint32 i = 0; i < n; ++i)
    {
        const FRect rect = m_rectStack[base + i]; // a copy: the recursion may grow the stack
        if (rect.w < 0.5f || rect.h < 0.5f)
            continue;
        drawNode(view.firstChild + i, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, depth + 1);
    }
    m_areaStack.resize(base);
    m_rectStack.resize(base);
}
