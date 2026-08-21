module Core.Profiler;

import Core;
import Core.Allocator;
import Core.Windows;

const char* Profiler::internName(oc::string_view name)
{
    if (name.empty())
        return "";
    const std::lock_guard<std::mutex> lock(m_internMutex);
    const auto it = m_internedNames.find(name);
    if (it != m_internedNames.end())
        return it->data();
    oc::unique_ptr<char[]> copy = oc::make_unique<char[]>(name.size() + 1);
    oc::char_traits<char>::copy(copy.get(), name.data(), name.size());
    copy[name.size()] = '\0';
    const char* stable = copy.get();
    m_internedStorage.push_back(oc::move(copy));
    m_internedNames.insert(oc::string_view(stable, name.size()));
    return stable;
}

// The calling thread's track, set only by registerThread (registration is always explicit). Safe
// under /GT because every access re-reads it through the accessor in the same call; the one hazard
// (a ProfileScope spanning a fiber wait) is asserted in the scope destructor instead.
static thread_local ProfileTrack* t_profileTrack = nullptr;

Profiler::Profiler()
{
    initialize();
}

void Profiler::initialize()
{
    LARGE_INTEGER freq, qpc;
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = (uint64)freq.QuadPart;

    QueryPerformanceCounter(&qpc);
    m_qpcAnchor0 = (uint64)qpc.QuadPart;
    m_tickAnchor0 = tick();

    // Short busy-wait (~0.5ms) for a usable initial rdtsc<->QPC ratio; every endFrame afterwards
    // re-estimates it over the whole run, so it only ever gets more accurate.
    const uint64 waitQpcTicks = m_qpcFreq / 2000;
    do { QueryPerformanceCounter(&qpc); } while ((uint64)qpc.QuadPart - m_qpcAnchor0 < waitQpcTicks);
    const uint64 t = tick();

    m_ticksPerQpc = (double)(t - m_tickAnchor0) / (double)((uint64)qpc.QuadPart - m_qpcAnchor0);
    m_tickAnchor = t;
    m_qpcAnchor = (uint64)qpc.QuadPart;
    m_ticksPerMs = m_ticksPerQpc * (double)m_qpcFreq / 1000.0;
    m_msPerTick = 1.0 / m_ticksPerMs;

    // Everything from here (the profiler constructs FIRST after the allocator) until
    // endStaticInit() at the top of main() runs under a synthetic "Static init" scope: static
    // initializers' allocations (Assimp schema tables, engine globals, ...) attribute to their own
    // path instead of "<unscoped>", and the whole phase gets a timed record.
    ProfileTrack* mainTrack = registerThread("Main", SORT_KEY_MAIN);
    if (mainTrack != nullptr)
    {
        m_staticInitStart = tick();
        mainTrack->m_openNames[0] = "Static init";
        mainTrack->m_openCategories[0] = (uint8)EProfileCategory::Core;
        mainTrack->m_openStarts[0] = m_staticInitStart;
        mainTrack->m_openDepth = 1;
    }
}

void Profiler::suspendScopes(ProfileScopeStack& out, uint32 baseDepth)
{
    ProfileTrack* track = threadTrack();
    const uint64 now = tick();
    assert(track->m_openDepth <= ProfileTrack::MAX_OPEN_DEPTH && "scopes past MAX_OPEN_DEPTH cannot migrate with a parking fiber");
    const uint32 depth = oc::min(track->m_openDepth, ProfileTrack::MAX_OPEN_DEPTH);
    out.depth = 0;
    for (uint32 i = baseDepth; i < depth; ++i)
    {
        out.names[out.depth] = track->m_openNames[i];
        out.categories[out.depth] = track->m_openCategories[i];
        out.depth++;
        // Close this scope's current on-thread segment: the thread genuinely stops running it here.
        track->push(track->m_openStarts[i], now, track->m_openNames[i], (uint16)i, (EProfileCategory)track->m_openCategories[i]);
    }
    track->m_openDepth = baseDepth;
}

uint32 Profiler::resumeScopes(const ProfileScopeStack& saved)
{
    ProfileTrack* track = threadTrack();
    const uint64 now = tick();
    const uint32 base = track->m_openDepth;
    assert(base + saved.depth <= ProfileTrack::MAX_OPEN_DEPTH && "resumed fiber scopes overflow the open-scope stack");
    for (uint32 i = 0; i < saved.depth && base + i < ProfileTrack::MAX_OPEN_DEPTH; ++i)
    {
        track->m_openNames[base + i] = saved.names[i];
        track->m_openCategories[base + i] = saved.categories[i];
        track->m_openStarts[base + i] = now; // fresh segment
    }
    track->m_openDepth = base + saved.depth;
    return base;
}

void Profiler::endStaticInit()
{
    ProfileTrack* track = threadTrack();
    if (track == nullptr || m_staticInitStart == 0)
        return;
    assert(track->m_openDepth == 1 && "endStaticInit: unbalanced scopes opened during static init");
    track->m_openDepth = 0;
    track->push(m_staticInitStart, tick(), "Static init", 0, EProfileCategory::Core);
    m_staticInitStart = 0;
}

void Profiler::endFrame()
{
    const uint64 t = tick();
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    const uint64 qpcElapsed = (uint64)qpc.QuadPart - m_qpcAnchor0;
    if (qpcElapsed > 0)
    {
        m_ticksPerQpc = (double)(t - m_tickAnchor0) / (double)qpcElapsed;
        m_ticksPerMs = m_ticksPerQpc * (double)m_qpcFreq / 1000.0;
        m_msPerTick = 1.0 / m_ticksPerMs;
    }
    m_tickAnchor = t;
    m_qpcAnchor = (uint64)qpc.QuadPart;

    // Paused: the clock keeps calibrating (cheap, keeps GPU alignment fresh for the resume) but
    // frame marks stop - the frame graph freezes on the recorded history.
    if (!g_profilerPaused.load(oc::memory_order_relaxed))
    {
        m_frameIsGap[m_frameCount % FRAME_HISTORY] = 0; // slot recycled from an old gap frame
        m_frameMarks[m_frameCount % FRAME_HISTORY] = t;
        m_frameCount++;
    }
}

void Profiler::setPaused(bool paused)
{
    const bool wasPaused = g_profilerPaused.exchange(paused, oc::memory_order_relaxed);
    if (wasPaused && !paused)
    {
        // Resume: a fresh boundary lets the next real frame measure cleanly. The pseudo-frame this
        // creates spans the whole paused stretch - flag it so the panel excludes it from the frame
        // graph's scale instead of compressing every real bar around it.
        m_frameIsGap[m_frameCount % FRAME_HISTORY] = 1;
        m_frameMarks[m_frameCount % FRAME_HISTORY] = tick();
        m_frameCount++;
    }
}

ProfileTrack* Profiler::threadTrack()
{
    return t_profileTrack;
}

ProfileTrack* Profiler::registerThread(const char* name, uint32 sortKey)
{
    if (ProfileTrack* track = t_profileTrack; track != nullptr)
    {
        track->setName(name);
        track->setSortKey(sortKey);
        return track;
    }
    ProfileTrack* track = registerTrack(name, (uint32)GetCurrentThreadId(), sortKey);
    t_profileTrack = track;
    return track;
}

ProfileTrack* Profiler::createNamedTrack(const char* name, uint32 sortKey)
{
    return registerTrack(name, 0, sortKey);
}

ProfileTrack* Profiler::registerTrack(const char* name, uint32 threadId, uint32 sortKey)
{
    // The track + its ring are PERMANENT profiler infrastructure, and on a freshly registering
    // thread they allocate before t_profileTrack exists - suppress the memory hooks so they don't
    // land misattributed in "<other threads>"/"<unscoped>".
    MemoryHookSuppress suppressTracking;

    while (m_registerLock.exchange(1, oc::memory_order_acquire) != 0)
        std::this_thread::yield();

    ProfileTrack* track = nullptr;
    const uint32 idx = m_numTracks.load(oc::memory_order_relaxed);
    if (idx < MAX_TRACKS)
    {
        m_tracks[idx] = oc::make_unique<ProfileTrack>();
        m_tracks[idx]->initialize(name, threadId, sortKey);
        track = m_tracks[idx].get();
        m_numTracks.store(idx + 1, oc::memory_order_release); // publish after construction
    }

    m_registerLock.store(0, oc::memory_order_release);
    return track;
}

// ---------------------------------------------------------------------------------------------------
// Text report (Profiler::buildReport)
// ---------------------------------------------------------------------------------------------------
namespace
{
    struct ReportNode
    {
        const char* name = nullptr;
        uint8 category = 0;
        uint64 incl = 0;       // ticks, clipped to the window
        uint64 childTicks = 0; // sum of direct children's incl (self = incl - childTicks)
        uint64 maxTicks = 0;   // longest single segment
        uint64 calls = 0;
        oc::vector<uint32> children;
    };

    // An aggregated call tree: one node per (parent, name CONTENT) - the same literal from two TUs is
    // two pointers, so children are keyed by text, not pointer.
    struct ReportTree
    {
        oc::vector<ReportNode> nodes;                                  // [0] = root
        oc::vector<oc::unordered_map<oc::string_view, uint32>> lookup; // parallel to nodes

        ReportTree() { nodes.emplace_back(); lookup.emplace_back(); }

        uint32 child(uint32 parent, const char* name, uint8 category)
        {
            const oc::string_view key(name);
            auto& map = lookup[parent];
            if (const auto it = map.find(key); it != map.end())
                return it->second;
            const uint32 idx = (uint32)nodes.size();
            nodes.push_back(ReportNode{ .name = name, .category = category });
            lookup.emplace_back();
            lookup[parent][key] = idx; // after the emplace: lookup may have reallocated
            nodes[parent].children.push_back(idx);
            return idx;
        }

        // records sorted by (start, depth). Parent = the last record one level up that CONTAINS this
        // one; a record whose parent is missing (ring lapped, scope still open) hangs off the root.
        void accumulate(const oc::vector<ProfileRecord>& records, uint64 tMin, uint64 tMax)
        {
            constexpr uint32 MaxDepth = 64;
            int32 lastRec[MaxDepth];
            uint32 lastNode[MaxDepth];
            for (uint32 i = 0; i < MaxDepth; ++i) { lastRec[i] = -1; lastNode[i] = 0; }
            for (uint32 i = 0; i < (uint32)records.size(); ++i)
            {
                const ProfileRecord& record = records[i];
                const uint32 depth = oc::min((uint32)record.depth, MaxDepth - 1);
                uint32 parent = 0;
                if (depth > 0 && lastRec[depth - 1] >= 0)
                {
                    const ProfileRecord& parentRecord = records[lastRec[depth - 1]];
                    if (parentRecord.start <= record.start && parentRecord.end >= record.end)
                        parent = lastNode[depth - 1];
                }
                const uint32 idx = child(parent, record.name, record.category);
                const uint64 s = oc::max(record.start, tMin);
                const uint64 e = oc::min(record.end, tMax);
                const uint64 dur = e > s ? e - s : 0;
                ReportNode& node = nodes[idx];
                node.incl += dur;
                node.calls++;
                node.maxTicks = oc::max(node.maxTicks, dur);
                nodes[parent].childTicks += dur;
                lastRec[depth] = (int32)i;
                lastNode[depth] = idx;
            }
        }
    };

    struct FlatRow
    {
        const char* name = nullptr;
        uint8 category = 0;
        double inclMs = 0.0, selfMs = 0.0, maxMs = 0.0;
        uint64 calls = 0;
        uint32 tracks = 0; // distinct tracks that ran it (global table)
    };

    void appendLine(oc::string& out, const char* text) { out += text; out += '\n'; }

    template<typename... Args>
    void appendf(oc::string& out, const char* fmt, Args... args)
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), fmt, args...);
        out += buf;
    }

    void printTree(oc::string& out, const ReportTree& tree, uint32 nodeIdx, uint32 level, double msPerTick, double invFrames,
        const ProfileReportOptions& options)
    {
        const ReportNode& node = tree.nodes[nodeIdx];
        oc::vector<uint32> order(node.children.begin(), node.children.end());
        oc::sort(order.begin(), order.end(), [&](uint32 a, uint32 b) { return tree.nodes[a].incl > tree.nodes[b].incl; });
        uint32 printed = 0;
        uint32 folded = 0;
        uint64 foldedTicks = 0;
        for (const uint32 childIdx : order)
        {
            const ReportNode& child = tree.nodes[childIdx];
            const double inclMs = (double)child.incl * msPerTick * invFrames;
            if (printed >= options.maxChildren || inclMs < options.minMsPerFrame)
            {
                folded++;
                foldedTicks += child.incl;
                continue;
            }
            const uint64 selfTicks = child.incl > child.childTicks ? child.incl - child.childTicks : 0;
            appendf(out, "%9.3f %9.3f %9.2f %9.3f  %*s%s\n", inclMs, (double)selfTicks * msPerTick * invFrames,
                (double)child.calls * invFrames, (double)child.maxTicks * msPerTick, (int)(level * 2), "", child.name);
            printed++;
            if (level + 1 < options.maxDepth)
                printTree(out, tree, childIdx, level + 1, msPerTick, invFrames, options);
        }
        if (folded > 0)
            appendf(out, "%9.3f %9s %9s %9s  %*s(+%u more)\n", (double)foldedTicks * msPerTick * invFrames, "", "", "", (int)(level * 2), "", folded);
    }

    void collectFlat(const ReportTree& tree, oc::unordered_map<oc::string_view, uint32>& index, oc::vector<FlatRow>& rows, double msPerTick, double invFrames,
        oc::unordered_set<oc::string_view>* seenThisTrack)
    {
        for (uint32 i = 1; i < (uint32)tree.nodes.size(); ++i)
        {
            const ReportNode& node = tree.nodes[i];
            const oc::string_view key(node.name);
            uint32 rowIdx;
            if (const auto it = index.find(key); it != index.end())
                rowIdx = it->second;
            else
            {
                rowIdx = (uint32)rows.size();
                rows.push_back(FlatRow{ .name = node.name, .category = node.category });
                index[key] = rowIdx;
            }
            FlatRow& row = rows[rowIdx];
            const uint64 selfTicks = node.incl > node.childTicks ? node.incl - node.childTicks : 0;
            row.inclMs += (double)node.incl * msPerTick * invFrames;
            row.selfMs += (double)selfTicks * msPerTick * invFrames;
            row.maxMs = oc::max(row.maxMs, (double)node.maxTicks * msPerTick);
            row.calls += node.calls;
            if (seenThisTrack != nullptr && seenThisTrack->insert(key).second)
                row.tracks++;
        }
    }

    void printFlat(oc::string& out, oc::vector<FlatRow>& rows, uint32 maxRows, double minMs, double invFrames, bool showTracks)
    {
        oc::sort(rows.begin(), rows.end(), [](const FlatRow& a, const FlatRow& b) { return a.selfMs > b.selfMs; });
        if (showTracks)
            appendLine(out, "     self      incl  calls/fr    max ms  thr  category    name");
        else
            appendLine(out, "     self      incl  calls/fr    max ms  category    name");
        uint32 printed = 0;
        uint32 folded = 0;
        double foldedSelf = 0.0;
        for (const FlatRow& row : rows)
        {
            if (printed >= maxRows || row.selfMs < minMs)
            {
                folded++;
                foldedSelf += row.selfMs;
                continue;
            }
            if (showTracks)
                appendf(out, "%9.3f %9.3f %9.2f %9.3f  %3u  %-10s  %s\n", row.selfMs, row.inclMs, (double)row.calls * invFrames, row.maxMs,
                    row.tracks, profileCategoryName((EProfileCategory)row.category), row.name);
            else
                appendf(out, "%9.3f %9.3f %9.2f %9.3f  %-10s  %s\n", row.selfMs, row.inclMs, (double)row.calls * invFrames, row.maxMs,
                    profileCategoryName((EProfileCategory)row.category), row.name);
            printed++;
        }
        if (folded > 0)
            appendf(out, "%9.3f %9s %9s %9s  (+%u more)\n", foldedSelf, "", "", "", folded);
    }
}

oc::string Profiler::buildReport(const ProfileReportOptions& options) const
{
    oc::string out;
    if (m_frameCount < 3)
    {
        appendLine(out, "profiler report: fewer than 3 frames recorded");
        return out;
    }

    // ---- frame window: the last N completed frames, never across a pause gap or past the mark ring ----
    const uint64 lastFrame = m_frameCount - 1;
    uint64 firstFrame = lastFrame + 1 > (uint64)oc::max(options.frames, 1u) ? lastFrame + 1 - oc::max(options.frames, 1u) : 1;
    firstFrame = oc::max<uint64>(firstFrame, m_frameCount > FRAME_HISTORY - 1 ? m_frameCount - (FRAME_HISTORY - 1) : 1);
    for (uint64 f = lastFrame; f >= firstFrame; --f)
        if (isFrameGap(f)) { firstFrame = f + 1; break; }
    if (firstFrame > lastFrame)
    {
        appendLine(out, "profiler report: no complete frames since the last pause");
        return out;
    }
    const uint64 numFrames = lastFrame - firstFrame + 1;
    const double invFrames = 1.0 / (double)numFrames;
    const uint64 tMin = getFrameMark(firstFrame - 1);
    const uint64 tMax = getFrameMark(lastFrame);
    const double msPerTick = m_msPerTick;
    const double windowMs = (double)(tMax - tMin) * msPerTick;

    oc::vector<double> frameMs;
    frameMs.reserve(numFrames);
    for (uint64 f = firstFrame; f <= lastFrame; ++f)
        frameMs.push_back((double)(getFrameMark(f) - getFrameMark(f - 1)) * msPerTick);
    oc::sort(frameMs.begin(), frameMs.end());
    const auto percentile = [&](double p) { return frameMs[oc::min((size_t)(p * (double)(frameMs.size() - 1) + 0.5), frameMs.size() - 1)]; };

    appendLine(out, "OctEngine profiler report");
    appendLine(out, "columns: ms/frame = total time in the window / frames; self = minus direct children; calls/fr = segments per frame (a fiber-parked scope counts once per resume)");
    appendf(out, "frames %llu..%llu (%llu frames), window %.1f ms\n", (unsigned long long)firstFrame, (unsigned long long)lastFrame, (unsigned long long)numFrames, windowMs);
    appendf(out, "frame ms: avg %.2f  min %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f   (%.1f fps avg)\n",
        windowMs * invFrames, frameMs.front(), percentile(0.5), percentile(0.95), percentile(0.99), frameMs.back(), 1000.0 * numFrames / windowMs);

    // ---- snapshot + sort every track ----
    struct TrackData
    {
        uint32 idx = 0;
        const char* name = nullptr;
        uint32 sortKey = 0;
        bool worker = false;
        bool gpu = false;
        double busyMs = 0.0;   // depth-0 scopes, Wait category excluded
        double waitMs = 0.0;   // depth-0 Wait scopes (fence/vsync, frame limit, joins)
        double coverage = 1.0; // fraction of the window the ring still held (1 = complete)
        oc::vector<ProfileRecord> records;
    };
    const uint32 numTracks = getNumTracks();
    oc::vector<TrackData> tracks;
    tracks.reserve(numTracks);
    for (uint32 i = 0; i < numTracks; ++i)
    {
        const ProfileTrack& track = getTrack(i);
        if (track.getCursor() == 0)
            continue;
        TrackData data;
        data.idx = i;
        data.name = track.getName();
        data.sortKey = track.getSortKey();
        data.worker = data.sortKey >= SORT_KEY_WORKER && data.sortKey < SORT_KEY_BACKGROUND;
        data.gpu = oc::string_view(data.name) == "GPU";
        snapshotTrack(i, tMin, tMax, data.records);
        oc::sort(data.records.begin(), data.records.end(), [](const ProfileRecord& a, const ProfileRecord& b)
            { return a.start != b.start ? a.start < b.start : a.depth < b.depth; });
        for (const ProfileRecord& record : data.records)
            if (record.depth == 0)
            {
                const uint64 s = oc::max(record.start, tMin), e = oc::min(record.end, tMax);
                if (e > s)
                    ((EProfileCategory)record.category == EProfileCategory::Wait ? data.waitMs : data.busyMs) += (double)(e - s) * msPerTick;
            }
        const uint64 cursor = track.getCursor();
        if (cursor >= ProfileTrack::CAPACITY)
        {
            // the writer may be overwriting this slot right now - a diagnostic read, not data
            const uint64 oldestEnd = track.getRecord(cursor - ProfileTrack::CAPACITY + 1).end;
            if (oldestEnd > tMin && tMax > tMin)
                data.coverage = oldestEnd >= tMax ? 0.0 : (double)(tMax - oldestEnd) / (double)(tMax - tMin);
        }
        tracks.push_back(oc::move(data));
    }
    oc::sort(tracks.begin(), tracks.end(), [](const TrackData& a, const TrackData& b) { return a.sortKey != b.sortKey ? a.sortKey < b.sortKey : a.idx < b.idx; });

    // ---- thread busy % (depth-0 scopes clipped to the window) ----
    appendLine(out, "");
    appendLine(out, "thread busy (% of window, depth-0 scopes; wait = Wait-category top-level scopes: fence/vsync, frame limit, joins):");
    uint32 numWorkers = 0;
    double workerBusy = 0.0;
    for (const TrackData& track : tracks)
    {
        if (track.worker) { numWorkers++; workerBusy += track.busyMs; }
        appendf(out, "  %-16s busy %5.1f%% (%.2f ms/frame)  wait %5.1f%% (%.2f ms/frame)  %u records%s\n", track.name,
            track.busyMs / windowMs * 100.0, track.busyMs * invFrames, track.waitMs / windowMs * 100.0, track.waitMs * invFrames,
            (uint32)track.records.size(), track.coverage < 0.999 ? oc::format(", RING LAPPED: covers {:.0f}% of the window", track.coverage * 100.0).c_str() : "");
    }
    if (numWorkers > 0)
        appendf(out, "  workers average %.1f%% over %u tracks (%.2f worker-ms/frame)\n", workerBusy / (windowMs * numWorkers) * 100.0, numWorkers, workerBusy * invFrames);

    // ---- trees: one per track, workers merged unless asked otherwise ----
    struct TreeEntry { oc::string title; ReportTree tree; bool gpu = false; };
    oc::vector<TreeEntry> trees;
    trees.reserve(tracks.size() + 1); // never reallocates below, so the merged-workers pointer stays valid
    ReportTree* mergedWorkers = nullptr;
    for (TrackData& track : tracks)
    {
        if (track.worker && !options.perWorkerTrees)
        {
            if (mergedWorkers == nullptr)
            {
                trees.push_back(TreeEntry{ .title = oc::format("Workers (merged, {} tracks)", numWorkers) });
                mergedWorkers = &trees.back().tree;
            }
            mergedWorkers->accumulate(track.records, tMin, tMax);
            continue;
        }
        trees.push_back(TreeEntry{ .title = oc::string(track.name), .gpu = track.gpu });
        trees.back().tree.accumulate(track.records, tMin, tMax);
    }

    // ---- global top self time across CPU tracks ----
    {
        oc::vector<FlatRow> rows;
        oc::unordered_map<oc::string_view, uint32> index;
        for (TreeEntry& entry : trees)
        {
            if (entry.gpu)
                continue;
            oc::unordered_set<oc::string_view> seen;
            collectFlat(entry.tree, index, rows, msPerTick, invFrames, &seen);
        }
        appendLine(out, "");
        appendLine(out, "== top self time, all CPU tracks (ms/frame summed across threads; thr = tracks/trees it ran on) ==");
        printFlat(out, rows, options.topRows, options.minMsPerFrame, invFrames, true);
    }

    // ---- per track: tree + flat ----
    for (TreeEntry& entry : trees)
    {
        appendLine(out, "");
        appendf(out, "== %s ==\n", entry.title.c_str());
        appendLine(out, " ms/frame      self  calls/fr    max ms  tree");
        printTree(out, entry.tree, 0, 0, msPerTick, invFrames, options);
        oc::vector<FlatRow> rows;
        oc::unordered_map<oc::string_view, uint32> index;
        collectFlat(entry.tree, index, rows, msPerTick, invFrames, nullptr);
        appendf(out, "-- %s: flat by self --\n", entry.title.c_str());
        printFlat(out, rows, options.flatRows, options.minMsPerFrame, invFrames, false);
    }
    return out;
}

bool Profiler::snapshotTrack(uint32 trackIdx, uint64 tMin, uint64 tMax, oc::vector<ProfileRecord>& out) const
{
    out.clear();
    const ProfileTrack& track = *m_tracks[trackIdx];
    const uint64 cursor = track.getCursor();
    const uint64 lo = cursor > ProfileTrack::CAPACITY ? cursor - ProfileTrack::CAPACITY : 0;

    // Pushes happen at scope END, so records are ordered by end time: scan backwards from the
    // newest and stop at the first record that ended before the window.
    uint64 i = cursor;
    while (i > lo)
    {
        --i;
        ProfileRecord record = track.getRecord(i);
        if (record.end < tMin)
            break;
        if (record.start <= tMax)
        {
            record._pad1 = (uint32)(cursor - i); // age of the source slot, for the lap salvage below
            out.push_back(record);
        }
    }

    // Lap salvage: slots older than the writer's CURRENT tail may have been overwritten while we
    // copied - drop just those (ages grow along out, so they are a suffix) and keep the rest. The
    // old all-or-nothing discard made busy tracks flicker out of wide zoomed-out views entirely,
    // every time the writer advanced past the scan's oldest index.
    const uint64 written = track.getCursor() - cursor;
    if (written >= ProfileTrack::CAPACITY)
    {
        out.clear(); // writer lapped the whole ring mid-copy - nothing trustworthy
        return false;
    }
    const uint32 maxValidAge = (uint32)(ProfileTrack::CAPACITY - written);
    while (!out.empty() && out.back()._pad1 > maxValidAge)
        out.pop_back();
    return true;
}
