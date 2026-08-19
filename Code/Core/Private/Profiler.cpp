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
