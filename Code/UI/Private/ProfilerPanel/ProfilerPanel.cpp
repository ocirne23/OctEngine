module UI;

import Core;
import Core.imgui;
import Threading;
import :ProfilerPanel;

namespace
{
    constexpr float kFrameGraphHeight = 56.0f;
    constexpr float kTrackHeaderHeight = 20.0f;
    constexpr float kRowHeight = 16.0f;
    constexpr float kTrackPadding = 5.0f;
    constexpr double kMinViewSpanMs = 0.0002; // 200ns
    constexpr uint32 kMaxStatDepth = 64;

    constexpr uint32 kColHeaderBg = 0xFF262629;
    constexpr uint32 kColHeaderText = 0xFFE0E0E0;
    constexpr uint32 kColLaneBg = 0x14FFFFFF;
    constexpr uint32 kColGridLine = 0x28FFFFFF;
    constexpr uint32 kColGridText = 0x50FFFFFF;
    constexpr uint32 kColFrameLine = 0x60FFD080;
    constexpr uint32 kColBarOutline = 0x40000000;

    char toLowerAscii(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

    int strCompare(const char* a, const char* b)
    {
        while (*a != 0 && *a == *b) { ++a; ++b; }
        return (int)(uint8)*a - (int)(uint8)*b;
    }

    void formatTime(char* buf, size_t bufSize, double ms)
    {
        const double absMs = ms < 0.0 ? -ms : ms; // unit from the magnitude: negative offsets (ruler left of the frame start) must not fall through to ns
        if (absMs >= 1.0)
            sprintf_s(buf, bufSize, "%.2f ms", ms);
        else if (absMs >= 0.001)
            sprintf_s(buf, bufSize, "%.1f us", ms * 1000.0);
        else
            sprintf_s(buf, bufSize, "%.0f ns", ms * 1e6);
    }

    // Dark text on bright bars, light text on dark bars.
    uint32 barTextColor(uint32 barColor)
    {
        const uint32 r = barColor & 0xFF, g = (barColor >> 8) & 0xFF, b = (barColor >> 16) & 0xFF;
        return (r * 3 + g * 4 + b * 2) > 1300 ? 0xE0101010 : 0xF0F0F0F0;
    }

    double niceGridStep(double spanMs)
    {
        double step = pow(10.0, floor(log10(spanMs / 5.0)));
        if (spanMs / step > 25.0)      step *= 5.0;
        else if (spanMs / step > 10.0) step *= 2.0;
        return step;
    }
}

void ProfilerPanel::prepare()
{
    ProfileScope scope("Profiler panel prepare", EProfileCategory::UI);
    Profiler& profiler = Globals::profiler;
    m_prepared = true;
    if (profiler.getFrameCount() < 4)
        return;

    // Auto pause: stop on the first NEW frame exceeding the threshold. Only frames <= frameCount-3
    // are tested - pausing freezes the rings, so pausing sooner would drop the spike frame's own
    // GPU records (they land when its frame slot's fence is next waited). The rings comfortably
    // retain those 2 extra frames, so nothing of the spike is lost. (selectFrame only ever SETS
    // paused — an atomic exchange — so this is safe off the main thread; resume stays in render.)
    if (m_autoPause && !m_paused)
    {
        const uint64 frameCount = profiler.getFrameCount();
        const uint64 newest = frameCount - 3;
        if (m_autoPauseChecked < newest)
        {
            const uint64 first = std::max<uint64>(m_autoPauseChecked + 1, newest > 64 ? newest - 64 : 1);
            for (uint64 f = first; f <= newest; ++f)
            {
                if (profiler.isFrameGap(f))
                    continue; // pause gap, not a real frame
                const double frameMs = (double)(profiler.getFrameMark(f) - profiler.getFrameMark(f - 1)) * profiler.getMsPerTick();
                if (frameMs > (double)m_autoPauseMs)
                {
                    selectFrame(f); // pauses + displays the offending frame
                    break;
                }
            }
            m_autoPauseChecked = newest;
        }
    }

    refresh(); // every UI frame, paused too: zoom/pan may need a wider snapshot of the (frozen) rings
    if (m_statsVisible)
        aggregateStats();
}

void ProfilerPanel::render()
{
    Profiler& profiler = Globals::profiler;
    if (!m_prepared)
        prepare(); // nothing ran ahead of us (first frame after opening, or no UI::prepare) — inline
    m_prepared = false;
    if (profiler.getFrameCount() < 4)
    {
        ImGui::TextDisabled("Waiting for frames...");
        return;
    }

    // Space toggles pause while the panel has focus.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Space, false))
    {
        m_paused = !m_paused;
        Globals::profiler.setPaused(m_paused); // freezes ring writes + frame marks with it
        if (!m_paused)
            m_autoPauseChecked = Globals::profiler.getFrameCount(); // skip the pause-gap frame
    }

    drawToolbar();
    drawFrameGraph();

    if (ImGui::BeginTabBar("##profilerTabs"))
    {
        if (ImGui::BeginTabItem("Timeline"))
        {
            drawTimeline();
            ImGui::EndTabItem();
        }
        const bool statsOpen = ImGui::BeginTabItem("Stats");
        if (statsOpen)
        {
            if (!m_statsVisible)
                aggregateStats(); // tab just opened: prepare skipped it — catch up inline this once
            drawStatsTable();
            ImGui::EndTabItem();
        }
        m_statsVisible = statsOpen;
        ImGui::EndTabBar();
    }
}

void ProfilerPanel::refresh()
{
    Profiler& profiler = Globals::profiler;
    if (!m_paused)
    {
        // Show a frame the GPU has fully caught up on: timestamps come back when their frame slot's
        // fence is next waited (~NUM_FRAMES_IN_FLIGHT frames), so 3 frames back is always complete.
        // Never a pause-gap pseudo-frame - its window spans the paused stretch and would compress
        // the timeline for the one frame it passes through the live slot.
        uint64 frame = profiler.getFrameCount() - 3;
        while (frame > 1 && profiler.isFrameGap(frame))
            --frame;
        m_displayedFrame = frame;
    }
    m_windowStart = profiler.getFrameMark(m_displayedFrame - 1);
    m_windowEnd = profiler.getFrameMark(m_displayedFrame);

    // Snapshot the VISIBLE range, not just the frame window: a zoomed-out/panned view shows
    // neighboring frames' records too (snapshotting only the frame made them flicker in and out
    // as the live window slid forward each frame).
    double loMs = 0.0;
    double hiMs = (double)(m_windowEnd - m_windowStart) * profiler.getMsPerTick();
    if (m_userView)
    {
        loMs = std::min(loMs, m_viewMin);
        hiMs = std::max(hiMs, m_viewMax);
    }
    const double ticksPerMs = profiler.getTicksPerMs();
    const int64 lo = (int64)m_windowStart + (int64)(loMs * ticksPerMs);
    m_snapshotStart = (uint64)std::max<int64>(lo, 0);
    m_snapshotEnd = m_windowStart + (uint64)(std::max(hiMs, 0.0) * ticksPerMs);
    snapshotTracks();
}

void ProfilerPanel::selectFrame(uint64 frameIdx)
{
    Profiler& profiler = Globals::profiler;
    const uint64 frameCount = profiler.getFrameCount();
    if (frameIdx < 1 || frameIdx >= frameCount || frameCount - frameIdx >= Profiler::FRAME_HISTORY - 1)
        return;
    m_paused = true;
    profiler.setPaused(true);
    m_displayedFrame = frameIdx;
    m_userView = false; // refresh() next render re-windows + re-snapshots around the selection
}

void ProfilerPanel::snapshotTracks()
{
    ProfileScope scope("Profiler tracks snapshot", EProfileCategory::UI);
    Profiler& profiler = Globals::profiler;
    const uint32 numTracks = profiler.getNumTracks();
    // One slot per track, filled IN PARALLEL: the copy out of each ring plus its sort is the bulk of
    // the panel's cost, and the tracks are independent (each ring is single-writer; a reader on any
    // thread is what snapshotTrack is for). Last frame's record buffers are handed back to their
    // slots first so the vectors keep their capacity — no per-frame reallocation of tens of
    // thousands of records.
    m_trackScratch.resize(numTracks);
    for (TrackView& shown : m_tracks)
        if (shown.trackIdx < numTracks)
        {
            m_trackScratch[shown.trackIdx].records = std::move(shown.records);
            m_trackScratch[shown.trackIdx].records.clear();
        }
    m_tracks.clear();
    const double msPerTick = profiler.getMsPerTick();
    Globals::jobSystem.parallelFor(0u, numTracks, 1u, [&](uint32 begin, uint32 end)
    {
    // Per chunk (~one track each): the fan-out lands on whichever workers pick it up, so each
    // piece carries its own marker on that worker's track.
    ProfileScope chunkScope("Profiler track snapshot", EProfileCategory::UI);
    for (uint32 i = begin; i < end; ++i)
    {
        TrackView& view = m_trackScratch[i];
        view.trackIdx = i;
        view.name = nullptr;
        view.maxDepth = 0;
        view.busyMs = 0.0;
        profiler.snapshotTrack(i, m_snapshotStart, m_snapshotEnd, view.records);
        const ProfileTrack& track = profiler.getTrack(i);
        // Keep tracks that have EVER recorded even when this window is empty - an idle worker shows
        // a stable empty lane instead of flickering in and out of the list frame to frame. Only
        // tracks that never recorded anything (registered-but-unprofiled service threads) stay hidden.
        if (view.records.empty() && track.getCursor() == 0)
            continue; // name stays null = hidden
        view.name = track.getName();
        view.sortKey = track.getSortKey();

        // Chronological order (children after their parent at equal start) - the stats pass and the
        // hover hit test rely on it.
        std::sort(view.records.begin(), view.records.end(), [](const ProfileRecord& a, const ProfileRecord& b)
            { return a.start != b.start ? a.start < b.start : a.depth < b.depth; });

        for (const ProfileRecord& record : view.records)
        {
            view.maxDepth = std::max(view.maxDepth, (uint32)record.depth);
            if (record.depth == 0)
            {
                const uint64 clampedStart = std::max(record.start, m_windowStart);
                const uint64 clampedEnd = std::min(record.end, m_windowEnd);
                if (clampedEnd > clampedStart)
                    view.busyMs += (double)(clampedEnd - clampedStart) * msPerTick;
            }
        }
    }
    });
    // Serial merge: the visible list + the per-track lane-count memory (a map — not for workers).
    // The record buffers MOVE into m_tracks and come back at the top of the next snapshot.
    for (uint32 i = 0; i < numTracks; ++i)
    {
        TrackView& view = m_trackScratch[i];
        if (view.name == nullptr)
            continue;
        // Lane count from the deepest nesting EVER seen on this track, not just this window: the
        // vertical offsets between tracks stay constant instead of shifting when one frame nests
        // deeper than its neighbors.
        uint32& cachedMaxDepth = m_trackMaxDepth[view.trackIdx];
        cachedMaxDepth = std::max(cachedMaxDepth, view.maxDepth);
        view.maxDepth = cachedMaxDepth;
        m_tracks.push_back(std::move(view));
    }
    std::sort(m_tracks.begin(), m_tracks.end(), [](const TrackView& a, const TrackView& b)
        { return a.sortKey != b.sortKey ? a.sortKey < b.sortKey : a.trackIdx < b.trackIdx; });

    // Uncapped fps inputs: the displayed frame's "main loop" (Main track, depth 0 — the fence wait
    // sits OUTSIDE it, so this is pure CPU work) and "GPU Frame" (GPU track). A scope's record can
    // straddle the frame marks (the GPU frame lands a slot late), so per track take the matching
    // depth-0 record with the largest overlap with the frame window and use its FULL duration.
    const auto scopeMs = [&](std::string_view scopeName) -> double
    {
        uint64 bestOverlap = 0;
        double bestMs = 0.0;
        for (const TrackView& track : m_tracks)
            for (const ProfileRecord& record : track.records)
            {
                if (record.depth != 0 || !record.name || scopeName != record.name)
                    continue;
                const uint64 lo = std::max(record.start, m_windowStart);
                const uint64 hi = std::min(record.end, m_windowEnd);
                const uint64 overlap = hi > lo ? hi - lo : 0;
                if (overlap > bestOverlap)
                {
                    bestOverlap = overlap;
                    bestMs = (double)(record.end - record.start) * msPerTick;
                }
            }
        return bestMs;
    };
    m_uncappedMainMs = scopeMs("main loop");
    m_uncappedGpuMs = scopeMs("GPU Frame");
    const double uncappedMs = std::max(m_uncappedMainMs, m_uncappedGpuMs);
    if (m_uncappedEmaFrame != m_displayedFrame) // feed once per displayed frame, not per UI redraw
    {
        m_uncappedEmaFrame = m_displayedFrame;
        m_uncappedEmaMs = m_uncappedEmaMs > 0.0 ? m_uncappedEmaMs * 0.9 + uncappedMs * 0.1 : uncappedMs;
    }
}

void ProfilerPanel::drawToolbar()
{
    Profiler& profiler = Globals::profiler;

    if (m_paused)
    {
        if (ImGui::Button("Resume"))
        {
            m_paused = false;
            profiler.setPaused(false);
            m_autoPauseChecked = profiler.getFrameCount(); // skip the pause-gap frame (one huge bar) - it must not re-trigger auto pause
        }
    }
    else
    {
        if (ImGui::Button("Pause "))
        {
            m_paused = true;
            Globals::profiler.setPaused(true); // freezes ring writes + frame marks: the whole frame history stays inspectable
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Auto pause >", &m_autoPause) && m_autoPause)
        m_autoPauseChecked = profiler.getFrameCount(); // only frames completed from now on can trigger, not history
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64.0f);
    ImGui::DragFloat("##autoPauseMs", &m_autoPauseMs, 0.5f, 1.0f, 1000.0f, "%.1f ms");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pause recording when a frame exceeds this time.\nTriggers 2 frames late so the spike frame's GPU data is complete (still fully retained).");

    const double frameMs = (double)(m_windowEnd - m_windowStart) * profiler.getMsPerTick();
    ImGui::SameLine();
    ImGui::Text("Frame %llu  |  %.2f ms (%.0f fps)", (unsigned long long)m_displayedFrame, frameMs, frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
    // Uncapped: exact for the displayed frame when paused, a short EMA live (per-frame values flicker).
    const double uncappedMs = m_paused ? std::max(m_uncappedMainMs, m_uncappedGpuMs) : m_uncappedEmaMs;
    ImGui::SameLine();
    ImGui::TextColored(m_uncappedMainMs >= m_uncappedGpuMs ? ImVec4(0.75f, 0.85f, 1.0f, 1.0f) : ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
        "|  Uncapped %.0f fps (%.2f ms, %s-bound)", uncappedMs > 0.0 ? 1000.0 / uncappedMs : 0.0, uncappedMs,
        m_uncappedMainMs >= m_uncappedGpuMs ? "CPU" : "GPU");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("What this frame would run at without the vsync / frame-slot throttle:\n"
            "max(main loop %.2f ms, GPU Frame %.2f ms). The main loop excludes the fence wait.",
            m_uncappedMainMs, m_uncappedGpuMs);
    if (m_paused)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "PAUSED");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click a frame bar to inspect it.\nTimeline: mouse wheel = zoom, drag = pan, double-click = fit, Space = pause.");
}

void ProfilerPanel::drawFrameGraph()
{
    ProfileScope scope("Profiler frame graph", EProfileCategory::UI);
    Profiler& profiler = Globals::profiler;
    const double msPerTick = profiler.getMsPerTick();
    const uint64 frameCount = profiler.getFrameCount();
    const uint64 latest = frameCount - 2; // newest frame with both boundary marks

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 60.0f);
    ImGui::InvisibleButton("##frameGraph", ImVec2(width, kFrameGraphHeight));
    const bool hovered = ImGui::IsItemHovered();

    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + width, canvasPos.y + kFrameGraphHeight), 0xFF1A1A1C);

    const float barWidth = 3.0f;
    const uint32 numBars = (uint32)(width / barWidth);
    const uint64 historyLimit = Profiler::FRAME_HISTORY - 2;
    const uint64 oldest = latest > std::min<uint64>(numBars, historyLimit) ? latest - std::min<uint64>(numBars, historyLimit) : 1;

    // Scale to the worst REAL frame in view (min 20ms so a smooth 60fps doesn't fill the graph).
    // Pause-gap pseudo-frames span the whole paused stretch and would compress everything else.
    double maxMs = 20.0;
    for (uint64 f = oldest; f <= latest; ++f)
        if (!profiler.isFrameGap(f))
            maxMs = std::max(maxMs, (double)(profiler.getFrameMark(f) - profiler.getFrameMark(f - 1)) * msPerTick);

    // 60fps reference line
    const float refY = canvasPos.y + kFrameGraphHeight * (1.0f - (float)(16.667 / maxMs));
    drawList->AddLine(ImVec2(canvasPos.x, refY), ImVec2(canvasPos.x + width, refY), 0x3050FF50);

    const ImVec2 mousePos = ImGui::GetMousePos();
    uint64 hoveredFrame = 0;
    for (uint64 f = oldest; f <= latest; ++f)
    {
        const float x1 = canvasPos.x + width - (float)(latest - f) * barWidth;
        const float x0 = x1 - barWidth + 1.0f;
        if (profiler.isFrameGap(f))
        {
            // pause gap: a dim full-height sliver marking the discontinuity, not a data bar
            drawList->AddRectFilled(ImVec2(x0, canvasPos.y), ImVec2(x1, canvasPos.y + kFrameGraphHeight), 0xFF3A3A3E);
            continue;
        }
        const double ms = (double)(profiler.getFrameMark(f) - profiler.getFrameMark(f - 1)) * msPerTick;
        const float h = kFrameGraphHeight * (float)std::min(ms / maxMs, 1.0);
        uint32 color = 0xFF50C878;                    // green
        if (ms > 33.4)      color = 0xFF5060E8;       // red
        else if (ms > 16.9) color = 0xFF50C8E8;       // yellow
        drawList->AddRectFilled(ImVec2(x0, canvasPos.y + kFrameGraphHeight - h), ImVec2(x1, canvasPos.y + kFrameGraphHeight), color);
        if (f == m_displayedFrame)
            drawList->AddRect(ImVec2(x0 - 1.0f, canvasPos.y), ImVec2(x1 + 1.0f, canvasPos.y + kFrameGraphHeight), 0xFFFFFFFF);
        if (hovered && mousePos.x >= x0 - 1.0f && mousePos.x < x1)
            hoveredFrame = f;
    }

    if (hoveredFrame != 0)
    {
        const double ms = (double)(profiler.getFrameMark(hoveredFrame) - profiler.getFrameMark(hoveredFrame - 1)) * msPerTick;
        ImGui::SetTooltip("Frame %llu: %.2f ms", (unsigned long long)hoveredFrame, ms);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            selectFrame(hoveredFrame);
    }
}

void ProfilerPanel::drawTimeline()
{
    ProfileScope scope("Profiler timeline", EProfileCategory::UI);
    Profiler& profiler = Globals::profiler;
    const double msPerTick = profiler.getMsPerTick();
    const double windowMs = (double)(m_windowEnd - m_windowStart) * msPerTick;
    if (windowMs <= 0.0)
        return;
    if (!m_userView)
    {
        m_viewMin = 0.0;
        m_viewMax = windowMs;
    }

    ImGui::BeginChild("##timeline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = std::max(avail.x, 60.0f);

    float totalHeight = 14.0f; // time ruler strip
    for (const TrackView& view : m_tracks)
        totalHeight += kTrackHeaderHeight + kTrackPadding + (m_collapsed[view.trackIdx] ? 0.0f : (float)(view.maxDepth + 1) * kRowHeight);
    const float contentHeight = std::max(totalHeight, avail.y);

    ImGui::InvisibleButton("##timelineCanvas", ImVec2(width, contentHeight));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    double pxPerMs = width / (m_viewMax - m_viewMin);

    // ---- interaction: wheel zoom about the cursor, drag pan, double-click fit ----
    if (hovered && io.MouseWheel != 0.0f)
    {
        if (io.KeyShift)
        {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * kRowHeight * 3.0f);
        }
        else
        {
            const double mouseMs = m_viewMin + (double)(io.MousePos.x - canvasPos.x) / pxPerMs;
            const double factor = pow(1.25, (double)-io.MouseWheel);
            m_viewMin = mouseMs - (mouseMs - m_viewMin) * factor;
            m_viewMax = mouseMs + (m_viewMax - mouseMs) * factor;
            m_userView = true;
        }
    }
    if (active && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
    {
        m_viewMin -= (double)io.MouseDelta.x / pxPerMs;
        m_viewMax -= (double)io.MouseDelta.x / pxPerMs;
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        m_userView = true;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        m_userView = false;
    // clamp: maxSpan equals the full [minView, maxView] range, so the viewMin bounds can never
    // invert (std::clamp with hi < lo is UB and made wide zoom-outs snap/oscillate)
    {
        const double maxView = windowMs * 30.0; // +-30 frames of context around the displayed one
        const double span = std::clamp(m_viewMax - m_viewMin, kMinViewSpanMs, maxView * 2.0);
        m_viewMin = std::clamp(m_viewMin, -maxView, maxView - span);
        m_viewMax = m_viewMin + span;
        pxPerMs = width / span;
    }

    const auto xOfMs = [&](double ms) { return canvasPos.x + (float)((ms - m_viewMin) * pxPerMs); };
    const auto xOfTick = [&](uint64 t) { return xOfMs((double)((int64)(t - m_windowStart)) * msPerTick); };

    // ---- time grid + ruler ----
    const double gridStep = niceGridStep(m_viewMax - m_viewMin);
    const double gridStart = floor(m_viewMin / gridStep) * gridStep;
    char textBuf[128];
    for (double g = gridStart; g < m_viewMax; g += gridStep)
    {
        const float x = xOfMs(g);
        if (x < canvasPos.x - 1.0f)
            continue;
        drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + contentHeight), kColGridLine);
        formatTime(textBuf, sizeof(textBuf), g);
        drawList->AddText(ImVec2(x + 3.0f, canvasPos.y), kColGridText, textBuf);
    }
    // frame boundary lines (the displayed frame's own boundaries + neighbors when panned out)
    const uint64 frameCount = profiler.getFrameCount();
    for (uint64 f = m_displayedFrame > 64 ? m_displayedFrame - 64 : 1; f <= m_displayedFrame + 64 && f < frameCount; ++f)
    {
        if (frameCount - f >= Profiler::FRAME_HISTORY)
            continue;
        const float x = xOfTick(profiler.getFrameMark(f));
        if (x < canvasPos.x || x > canvasPos.x + width)
            continue;
        drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + contentHeight), kColFrameLine);
    }

    // ---- tracks ----
    const ProfileRecord* hoveredRecord = nullptr;
    const char* hoveredTrackName = nullptr;
    const ImVec2 mousePos = ImGui::GetMousePos();
    float y = canvasPos.y + 14.0f;
    for (const TrackView& view : m_tracks)
    {
        bool& collapsed = m_collapsed[view.trackIdx];

        // header
        drawList->AddRectFilled(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + width, y + kTrackHeaderHeight - 1.0f), kColHeaderBg);
        formatTime(textBuf, sizeof(textBuf), view.busyMs);
        char headerBuf[160];
        sprintf_s(headerBuf, sizeof(headerBuf), "%s %s  -  %s busy, %u scopes", collapsed ? ">" : "v", view.name, textBuf, (uint32)view.records.size());
        drawList->AddText(ImVec2(canvasPos.x + 6.0f, y + 2.0f), kColHeaderText, headerBuf);
        const bool headerHovered = hovered && mousePos.y >= y && mousePos.y < y + kTrackHeaderHeight;
        if (headerHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9.0f)
        {
            collapsed = !collapsed;
            if (collapsed)
                m_trackMaxDepth[view.trackIdx] = 0; // folding resets the sticky lane count: expanding re-learns from what's actually displayed
        }
        y += kTrackHeaderHeight;

        if (collapsed)
        {
            y += kTrackPadding;
            continue;
        }

        const float laneTop = y;
        const float laneBottom = y + (float)(view.maxDepth + 1) * kRowHeight;
        // subtle alternating depth lanes
        for (uint32 d = 0; d <= view.maxDepth; d += 2)
            drawList->AddRectFilled(ImVec2(canvasPos.x, laneTop + (float)d * kRowHeight), ImVec2(canvasPos.x + width, laneTop + (float)(d + 1) * kRowHeight), kColLaneBg);

        for (const ProfileRecord& record : view.records)
        {
            float x0 = xOfTick(record.start);
            float x1 = xOfTick(record.end);
            if (x1 < canvasPos.x || x0 > canvasPos.x + width)
                continue;
            x0 = std::max(x0, canvasPos.x - 2.0f);
            x1 = std::min(x1, canvasPos.x + width + 2.0f);
            const float barWidth = std::max(x1 - x0, 0.75f);
            const float barY = laneTop + (float)record.depth * kRowHeight;

            const uint32 color = profileCategoryColor((EProfileCategory)record.category);
            drawList->AddRectFilled(ImVec2(x0, barY), ImVec2(x0 + barWidth, barY + kRowHeight - 1.0f), color, 2.0f);
            if (barWidth > 5.0f)
                drawList->AddRect(ImVec2(x0, barY), ImVec2(x0 + barWidth, barY + kRowHeight - 1.0f), kColBarOutline, 2.0f);
            if (barWidth > 28.0f)
            {
                formatTime(textBuf, sizeof(textBuf), (double)(record.end - record.start) * msPerTick);
                char labelBuf[160];
                sprintf_s(labelBuf, sizeof(labelBuf), "%s  %s", record.name, textBuf);
                drawList->PushClipRect(ImVec2(x0 + 1.0f, barY), ImVec2(x0 + barWidth - 1.0f, barY + kRowHeight), true);
                drawList->AddText(ImVec2(x0 + 4.0f, barY + 1.0f), barTextColor(color), labelBuf);
                drawList->PopClipRect();
            }
            if (hovered && mousePos.x >= x0 && mousePos.x < x0 + barWidth && mousePos.y >= barY && mousePos.y < barY + kRowHeight)
            {
                hoveredRecord = &record;
                hoveredTrackName = view.name;
            }
        }
        y = laneBottom + kTrackPadding;
    }

    if (hoveredRecord != nullptr)
    {
        ImGui::BeginTooltip();
        const uint32 color = profileCategoryColor((EProfileCategory)hoveredRecord->category);
        const ImVec4 colorVec = ImGui::ColorConvertU32ToFloat4(color);
        ImGui::TextColored(colorVec, "%s", hoveredRecord->name);
        formatTime(textBuf, sizeof(textBuf), (double)(hoveredRecord->end - hoveredRecord->start) * msPerTick);
        ImGui::Text("Duration: %s", textBuf);
        formatTime(textBuf, sizeof(textBuf), (double)((int64)(hoveredRecord->start - m_windowStart)) * msPerTick);
        ImGui::Text("Start: %s into frame", textBuf);
        ImGui::Text("Category: %s", profileCategoryName((EProfileCategory)hoveredRecord->category));
        ImGui::Text("Track: %s  (depth %u)", hoveredTrackName, (uint32)hoveredRecord->depth);
        ImGui::EndTooltip();
    }

    ImGui::EndChild();
}

// The Stats tab's data pass (prepare side, no ImGui): per-name totals over the displayed window,
// live smoothing, and the name filter. The options it reads (track/name filter, Smooth) are what
// the table drew last frame.
void ProfilerPanel::aggregateStats()
{
    ProfileScope scope("Profiler stats aggregate", EProfileCategory::UI);
    Profiler& profiler = Globals::profiler;
    const double msPerTick = profiler.getMsPerTick();

    // ---- aggregate the displayed window ----
    m_statsRows.clear();
    std::unordered_map<const char*, uint32> rowByName;
    for (int t = 0; t < (int)m_tracks.size(); ++t)
    {
        if (m_trackFilter >= 0 && m_trackFilter != t)
            continue;
        const std::vector<ProfileRecord>& records = m_tracks[t].records; // sorted by (start, depth)
        const uint32 numRecords = (uint32)records.size();
        m_childSumScratch.assign(numRecords, 0);
        int32 lastAtDepth[kMaxStatDepth];
        for (uint32 i = 0; i < kMaxStatDepth; ++i)
            lastAtDepth[i] = -1;

        for (uint32 i = 0; i < numRecords; ++i)
        {
            const ProfileRecord& record = records[i];
            const uint32 depth = std::min((uint32)record.depth, kMaxStatDepth - 1);
            if (depth > 0 && lastAtDepth[depth - 1] >= 0)
            {
                const ProfileRecord& parent = records[lastAtDepth[depth - 1]];
                if (parent.start <= record.start && parent.end >= record.end)
                    m_childSumScratch[lastAtDepth[depth - 1]] += record.end - record.start;
            }
            lastAtDepth[depth] = (int32)i;
        }
        for (uint32 i = 0; i < numRecords; ++i)
        {
            const ProfileRecord& record = records[i];
            if (record.end < m_windowStart || record.start > m_windowEnd)
                continue; // the snapshot covers the zoomed VIEW; stats aggregate exactly the frame window
            uint32 rowIdx;
            auto it = rowByName.find(record.name);
            if (it != rowByName.end())
                rowIdx = it->second;
            else
            {
                // same text from a different TU is a different literal pointer: merge by content
                rowIdx = UINT32_MAX;
                for (uint32 r = 0; r < (uint32)m_statsRows.size(); ++r)
                    if (strCompare(m_statsRows[r].name, record.name) == 0) { rowIdx = r; break; }
                if (rowIdx == UINT32_MAX)
                {
                    rowIdx = (uint32)m_statsRows.size();
                    m_statsRows.push_back(StatsRow{ .name = record.name, .category = record.category });
                }
                rowByName[record.name] = rowIdx;
            }
            StatsRow& row = m_statsRows[rowIdx];
            const uint64 duration = record.end - record.start;
            const uint64 childSum = std::min(m_childSumScratch[i], duration);
            row.calls++;
            row.totalMs += (double)duration * msPerTick;
            row.selfMs += (double)(duration - childSum) * msPerTick;
        }
    }

    // ---- smoothing (live only) ----
    if (!m_paused && m_smooth)
    {
        const uint64 frame = m_displayedFrame;
        for (StatsRow& row : m_statsRows)
        {
            SmoothedRow& smoothed = m_smoothed[row.name];
            // reset the average after not being seen for a while (scope disappeared / renamed)
            const double alpha = (frame - smoothed.lastFrame > 30) ? 1.0 : 0.1;
            smoothed.totalMs += (row.totalMs - smoothed.totalMs) * alpha;
            smoothed.selfMs += (row.selfMs - smoothed.selfMs) * alpha;
            smoothed.calls += ((double)row.calls - smoothed.calls) * alpha;
            smoothed.lastFrame = frame;
            row.totalMs = smoothed.totalMs;
            row.selfMs = smoothed.selfMs;
            row.calls = (uint32)(smoothed.calls + 0.5);
        }
    }

    // ---- name filter ----
    if (m_nameFilter[0] != 0)
    {
        std::erase_if(m_statsRows, [this](const StatsRow& row)
            {
                // case-insensitive substring
                const char* haystack = row.name;
                for (; *haystack != 0; ++haystack)
                {
                    const char* a = haystack;
                    const char* b = m_nameFilter;
                    while (*a != 0 && *b != 0 && toLowerAscii(*a) == toLowerAscii(*b)) { ++a; ++b; }
                    if (*b == 0)
                        return false;
                }
                return true;
            });
    }

}

void ProfilerPanel::drawStatsTable()
{
    ProfileScope scope("Profiler stats table", EProfileCategory::UI);
    Profiler& profiler = Globals::profiler;
    const double msPerTick = profiler.getMsPerTick();
    const double windowMs = (double)(m_windowEnd - m_windowStart) * msPerTick;

    // ---- options row ----
    ImGui::SetNextItemWidth(150.0f);
    const char* trackPreview = m_trackFilter < 0 || m_trackFilter >= (int)m_tracks.size() ? "All tracks" : m_tracks[m_trackFilter].name;
    if (ImGui::BeginCombo("##trackFilter", trackPreview))
    {
        if (ImGui::Selectable("All tracks", m_trackFilter < 0))
            m_trackFilter = -1;
        for (int i = 0; i < (int)m_tracks.size(); ++i)
            if (ImGui::Selectable(m_tracks[i].name, m_trackFilter == i))
                m_trackFilter = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##nameFilter", "filter", m_nameFilter, sizeof(m_nameFilter));
    ImGui::SameLine();
    ImGui::Checkbox("Smooth", &m_smooth);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Exponential moving average over frames while live (paused shows exact values)");

    // ---- table ----
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
        | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##profilerStats", 7, tableFlags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 1.0f);
    ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.0f);
    ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.0f);
    ImGui::TableSetupColumn("Frame %", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 0.9f);
    ImGui::TableHeadersRow();

    if (const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs != nullptr && sortSpecs->SpecsCount > 0)
    {
        const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
        const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(m_statsRows.begin(), m_statsRows.end(), [&](const StatsRow& a, const StatsRow& b)
            {
                int compare = 0;
                switch (spec.ColumnIndex)
                {
                case 0: compare = strCompare(a.name, b.name); break;
                case 1: compare = strCompare(profileCategoryName((EProfileCategory)a.category), profileCategoryName((EProfileCategory)b.category)); break;
                case 2: compare = a.calls < b.calls ? -1 : (a.calls > b.calls ? 1 : 0); break;
                case 5: { const double avgA = a.calls > 0 ? a.totalMs / a.calls : 0.0, avgB = b.calls > 0 ? b.totalMs / b.calls : 0.0;
                          compare = avgA < avgB ? -1 : (avgA > avgB ? 1 : 0); break; }
                case 4: compare = a.selfMs < b.selfMs ? -1 : (a.selfMs > b.selfMs ? 1 : 0); break;
                default: compare = a.totalMs < b.totalMs ? -1 : (a.totalMs > b.totalMs ? 1 : 0); break; // Total + Frame %
                }
                return ascending ? compare < 0 : compare > 0;
            });
    }

    char textBuf[64];
    for (const StatsRow& row : m_statsRows)
    {
        ImGui::TableNextRow();
        const uint32 color = profileCategoryColor((EProfileCategory)row.category);

        ImGui::TableSetColumnIndex(0);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(ImVec2(cursor.x, cursor.y + 3.0f), ImVec2(cursor.x + 8.0f, cursor.y + 11.0f), color, 2.0f);
        ImGui::Dummy(ImVec2(11.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(row.name);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%s", profileCategoryName((EProfileCategory)row.category));

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", row.calls);

        ImGui::TableSetColumnIndex(3);
        formatTime(textBuf, sizeof(textBuf), row.totalMs);
        ImGui::TextUnformatted(textBuf);

        ImGui::TableSetColumnIndex(4);
        formatTime(textBuf, sizeof(textBuf), row.selfMs);
        ImGui::TextUnformatted(textBuf);

        ImGui::TableSetColumnIndex(5);
        formatTime(textBuf, sizeof(textBuf), row.calls > 0 ? row.totalMs / row.calls : 0.0);
        ImGui::TextUnformatted(textBuf);

        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%.1f%%", windowMs > 0.0 ? row.totalMs / windowMs * 100.0 : 0.0);
    }
    ImGui::EndTable();
}
