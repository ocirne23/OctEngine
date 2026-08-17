module;

#include <intrin.h> // __rdtsc for the inline scope hot path
#include <cstdint>

export module Core.Profiler;

// Core.OcSTL, not `import Core;`: Core.ixx re-exports this module, so the interface must not depend
// back on Core. OcSTL sits below both -- it imports nothing but std header units.
import Core.OcSTL;

// Category a scope belongs to (which library / kind of code it measures); drives the timeline
// colors and the stats-table grouping. Wait is for parked/blocked time, GPU for renderer GPU passes.
export enum class EProfileCategory : uint8_t
{
    App, Core, Entity, Script, Animation, Physics, Audio, Particle, Force, Spatial,
    Threading, Procedural, Network, File, Renderer, GPU, UI, Input, Game, Wait, Other,
    Count
};

export constexpr const char* profileCategoryName(EProfileCategory category)
{
    constexpr const char* names[(uint32_t)EProfileCategory::Count] = {
        "App", "Core", "Entity", "Script", "Animation", "Physics", "Audio", "Particle", "Force", "Spatial",
        "Threading", "Procedural", "Network", "File", "Renderer", "GPU", "UI", "Input", "Game", "Wait", "Other",
    };
    return names[(uint32_t)category];
}

// Packed ABGR (what ImGui's IM_COL32 produces: a<<24|b<<16|g<<8|r), usable directly as an ImU32.
export constexpr uint32_t profileCategoryColor(EProfileCategory category)
{
    constexpr uint32_t colors[(uint32_t)EProfileCategory::Count] = {
        0xFFB0B0B0, // App        - light gray
        0xFF505050, // Core       - dark gray
        0xFF3C96E8, // Entity     - orange
        0xFFE86BA4, // Script     - purple
        0xFFB46BE8, // Animation  - pink
        0xFF5B5BE8, // Physics    - red
        0xFFB4C83C, // Audio      - teal
        0xFF3CD4E8, // Particle   - yellow
        0xFFE8B446, // Force      - cyan
        0xFF64C864, // Spatial    - green
        0xFF2878C8, // Threading  - dark orange
        0xFF46B4A0, // Procedural - olive
        0xFFE8785A, // Network    - blue
        0xFF5080A0, // File       - brown
        0xFFDCA050, // Renderer   - light blue
        0xFF78DC46, // GPU        - vivid green
        0xFFC850C8, // UI         - magenta
        0xFF50DC8C, // Input      - light green
        0xFF40A0F0, // Game       - amber
        0xFF706860, // Wait       - dark gray
        0xFF909090, // Other      - gray
    };
    return colors[(uint32_t)category];
}

// One completed scope. Written ONCE at scope exit (so pushes are naturally ordered by END time,
// which the snapshot's early-out relies on). name must be a string LITERAL - stored by pointer.
export struct ProfileRecord
{
    uint64_t start = 0; // profiler ticks (Profiler::tick domain)
    uint64_t end = 0;
    const char* name = nullptr;
    uint16_t depth = 0;
    uint8_t category = 0; // EProfileCategory
    uint8_t _pad0 = 0;
    uint32_t _pad1 = 0;
};
static_assert(sizeof(ProfileRecord) == 32);

// Pause = freeze the DATA, not the bookkeeping: while set, ring pushes and frame marks drop, so
// everything already recorded (the full frame history) stays inspectable instead of being
// overwritten. Scope open/close and the open-name stacks keep running - fiber migration and the
// Memory tracker's attribution stay live. Toggled by the Profiler panel via Profiler::setPaused.
export inline oc::atomic<bool> g_profilerPaused = false;

// A single-writer/many-reader ring of ProfileRecords: one per registered thread, plus named tracks
// (the renderer's GPU track). The owner writes records + a monotonic release cursor; readers
// (Profiler::snapshotTrack, main thread) copy behind the cursor and detect being lapped. NEVER
// written by two threads at once - a thread track is written only by its thread, a named track only
// by whoever created it (the GPU track: the main thread, at collect time).
export class ProfileTrack final
{
public:
    static constexpr uint32_t CAPACITY = 1 << 15; // records; 1 MiB per track, ring overwrite when full

    void initialize(const char* name, uint32_t threadId, uint32_t sortKey)
    {
        m_records = oc::make_unique<ProfileRecord[]>(CAPACITY);
        setName(name);
        m_threadId = threadId;
        m_sortKey = sortKey;
    }

    void push(uint64_t start, uint64_t end, const char* name, uint16_t depth, EProfileCategory category)
    {
        if (g_profilerPaused.load(oc::memory_order_relaxed)) [[unlikely]]
            return; // paused: the recorded data is frozen for inspection
        const uint64_t idx = m_cursor.load(oc::memory_order_relaxed);
        ProfileRecord& record = m_records[idx & (CAPACITY - 1)];
        record.start = start;
        record.end = end;
        record.name = name;
        record.depth = depth;
        record.category = (uint8_t)category;
        m_cursor.store(idx + 1, oc::memory_order_release); // publish: readers acquire the cursor
    }

    void setName(const char* name)
    {
        uint32_t i = 0;
        for (; name[i] != 0 && i < sizeof(m_name) - 1; ++i)
            m_name[i] = name[i];
        m_name[i] = 0;
    }
    void setSortKey(uint32_t sortKey) { m_sortKey = sortKey; }

    const char* getName() const { return m_name; }
    uint32_t getThreadId() const { return m_threadId; }
    uint32_t getSortKey() const { return m_sortKey; }
    uint64_t getCursor() const { return m_cursor.load(oc::memory_order_acquire); }
    const ProfileRecord& getRecord(uint64_t absIdx) const { return m_records[absIdx & (CAPACITY - 1)]; }

    uint32_t m_openDepth = 0; // owner-thread scope nesting depth (touched only by ProfileScope on the owner)

    // The currently-OPEN scope stack (names + categories + segment start ticks), maintained by
    // ProfileScope alongside m_openDepth. Owner-thread only - the Memory library's allocation hook
    // reads it on the allocating thread to attribute the allocation to the active marker path, and
    // the JobSystem migrates it with a parking fiber (Profiler::suspendScopes/resumeScopes).
    // m_openStarts is the start of the CURRENT on-thread segment: a resume rewrites it, so a scope
    // that survived a park closes only its last segment (earlier segments were pushed at suspend).
    // Entries past MAX_OPEN_DEPTH still time correctly but are not attributable and cannot migrate.
    static constexpr uint32_t MAX_OPEN_DEPTH = 32;
    const char* m_openNames[MAX_OPEN_DEPTH] = {};
    uint8_t m_openCategories[MAX_OPEN_DEPTH] = {};
    uint64_t m_openStarts[MAX_OPEN_DEPTH] = {};

private:

    oc::unique_ptr<ProfileRecord[]> m_records;
    oc::atomic<uint64_t> m_cursor = 0;
    char m_name[48] = {};
    uint32_t m_threadId = 0;
    uint32_t m_sortKey = 2; // display order: Profiler::SORT_KEY_* (main, GPU, workers by index, lazy threads last)
};

// The open-scope stack a parked fiber carries between threads (the JobSystem stores one per Fiber
// and calls Profiler::suspendScopes/resumeScopes around every park/resume). Plain POD - segment
// start ticks are NOT saved: suspend closes each open scope's current segment as a record, resume
// starts fresh segments.
export struct ProfileScopeStack
{
    const char* names[ProfileTrack::MAX_OPEN_DEPTH] = {};
    uint8_t categories[ProfileTrack::MAX_OPEN_DEPTH] = {};
    uint32_t depth = 0;
};

// Global CPU (+GPU track) profiler. Scope markers record into per-thread lock-free rings; the clock
// is __rdtsc (invariant TSC), calibrated against QPC every endFrame so GPU timestamps (which
// calibrate against QPC) and millisecond conversions stay accurate. Recording is ALWAYS on (a
// compile-time #ifdef kill switch is the plan, not a runtime toggle); the UI panel does its own
// pause/snapshot on top. All read APIs (snapshot, frame marks, conversions) are main-thread.
export class Profiler final
{
public:
    static constexpr uint32_t MAX_TRACKS = 64;
    static constexpr uint32_t FRAME_HISTORY = 512; // frame boundary ring for the UI frame graph

    // Constructed at STATIC INIT (init_seg below, right after the allocator): registers the
    // constructing (main) thread as "Main" and calibrates the clock, so ProfileScopes and
    // memory-tracked allocations inside other globals' static initializers already work.
    Profiler();

    void endFrame();   // once per frame from the main loop: frame mark + clock re-anchor

    // See g_profilerPaused. Resuming lays down a fresh frame boundary, so the paused gap shows as
    // a single (saturated) bar in the frame graph instead of polluting the next real frame.
    void setPaused(bool paused);
    bool isPaused() const { return g_profilerPaused.load(oc::memory_order_relaxed); }

    // Closes the synthetic "Static init" scope the constructor opened on the Main track - call
    // FIRST THING in main(). Between the two, every static initializer's allocation attributes to
    // the "Static init" path (instead of "<unscoped>") and the phase gets one timed record.
    void endStaticInit();

    // ---- Fiber scope migration (called by the JobSystem around every fiber park/resume) ----
    // A ProfileScope's state lives on the THREAD's track, but a job fiber can park mid-scope and
    // resume on another thread. suspendScopes (on the parking thread, before the switch) closes
    // every open scope ABOVE baseDepth as a partial record - the thread genuinely stops running
    // that code - saves them into out, and drops the track back to baseDepth so whatever the
    // worker runs next attributes cleanly. resumeScopes (on the resuming thread) replays them on
    // top of that thread's current stack with fresh segment starts, returning the base depth to
    // hand back to the next suspend. Scope timing sums the on-thread segments; parked time is not
    // counted. Memory attribution follows the fiber automatically.
    void suspendScopes(ProfileScopeStack& out, uint32_t baseDepth);
    uint32_t resumeScopes(const ProfileScopeStack& saved);

    static uint64_t tick() { return __rdtsc(); }

    // The calling thread's track, or nullptr when the thread never registered. Registration is
    // ALWAYS explicit - there is deliberately no lazy first-scope fallback, so a scope on an
    // unregistered thread is a debug assert instead of a mystery track with a raced name.
    ProfileTrack* threadTrack();

    // Registers the calling thread with an explicit name + display order - call at THREAD STARTUP
    // on every thread that will profile (main does it in initialize(), JobSystem workers in
    // workerMain; a new engine thread that wants markers must do the same). Registering here (not
    // on first scope) means no name race with a SetThreadDescription from the spawning thread, no
    // one-time registration cost polluting the first profiled scope, and a deterministic track
    // order in the UI. Renames in place if the thread already registered. Returns nullptr past
    // MAX_TRACKS; the pointer is stable for the process lifetime.
    ProfileTrack* registerThread(const char* name, uint32_t sortKey = SORT_KEY_WORKER);

    static constexpr uint32_t SORT_KEY_MAIN = 0;
    static constexpr uint32_t SORT_KEY_NAMED = 1;        // GPU track
    static constexpr uint32_t SORT_KEY_WORKER = 2;       // + worker index
    static constexpr uint32_t SORT_KEY_BACKGROUND = 100; // + offset: service threads (streamers, timer, loaders)

    // A track not bound to a thread (the renderer's GPU timeline). Caller owns pushing records into
    // it from ONE thread, with end-ordered pushes (sort before pushing a batch).
    ProfileTrack* createNamedTrack(const char* name, uint32_t sortKey = 1);

    // ---- Clock conversions (valid after initialize) ----
    double getMsPerTick() const { return m_msPerTick; }
    double getTicksPerMs() const { return m_ticksPerMs; }
    uint64_t ticksFromQpc(uint64_t qpc) const // maps a QueryPerformanceCounter value into tick() domain
    {
        return m_tickAnchor + (int64_t)((double)((int64_t)qpc - (int64_t)m_qpcAnchor) * m_ticksPerQpc);
    }

    // ---- Frame history (main thread) ----
    uint64_t getFrameCount() const { return m_frameCount; } // completed endFrame calls
    uint64_t getFrameMark(uint64_t frameIdx) const { return m_frameMarks[frameIdx % FRAME_HISTORY]; } // tick at END of frame frameIdx
    // True for the pseudo-frame spanning a pause (laid down by setPaused(false)): its duration is
    // the paused wall time, not a real frame - the panel excludes it from scaling/selection.
    bool isFrameGap(uint64_t frameIdx) const { return m_frameIsGap[frameIdx % FRAME_HISTORY] != 0; }

    // ---- Track reading (main thread) ----
    uint32_t getNumTracks() const { return m_numTracks.load(oc::memory_order_acquire); }
    const ProfileTrack& getTrack(uint32_t idx) const { return *m_tracks[idx]; }

    // Copies every record overlapping [tMin, tMax] into out (unspecified order: newest-first).
    // Returns false (out cleared) if the writer lapped the ring mid-copy - retry next frame.
    bool snapshotTrack(uint32_t trackIdx, uint64_t tMin, uint64_t tMax, oc::vector<ProfileRecord>& out) const;

private:

    void initialize();
    ProfileTrack* registerTrack(const char* name, uint32_t threadId, uint32_t sortKey);

    oc::array<oc::unique_ptr<ProfileTrack>, MAX_TRACKS> m_tracks;
    oc::atomic<uint32_t> m_numTracks = 0;
    oc::atomic<uint32_t> m_registerLock = 0; // registration-only spinlock

    oc::array<uint64_t, FRAME_HISTORY> m_frameMarks = {};
    oc::array<uint8_t, FRAME_HISTORY> m_frameIsGap = {}; // parallel to m_frameMarks, see isFrameGap
    uint64_t m_frameCount = 0;
    uint64_t m_staticInitStart = 0; // "Static init" scope open since the constructor; 0 once closed

    // rdtsc <-> QPC calibration: ratio estimated over the whole run (converges), anchor refreshed
    // every endFrame so QPC->tick conversions never extrapolate far.
    double m_msPerTick = 0.0;
    double m_ticksPerMs = 0.0;
    double m_ticksPerQpc = 0.0;
    uint64_t m_qpcFreq = 0;
    uint64_t m_tickAnchor = 0;
    uint64_t m_qpcAnchor = 0;
    uint64_t m_tickAnchor0 = 0;
    uint64_t m_qpcAnchor0 = 0;
};

// Static-init order (lexicographic CRT section sort): allocator (.CRT$XCA) -> profiler (XCA2) ->
// memory tracker (XCA3) -> everything else. The constructor registers "Main" + calibrates, so a
// ProfileScope in any later global's initializer (Tweak registration statics etc.) is valid.
OC_INIT_SEG(OC_SEG_CORE_PROFILER)
export namespace Globals
{
    Profiler profiler;
}

// RAII scope marker - the thing user code sprinkles into functions:
//     ProfileScope scope("SkinPalettes", EProfileCategory::Animation);
// ~15ns hot path: two rdtsc, one thread-local read, one 32-byte ring store. name MUST be a string
// literal (stored by pointer). The thread must have called Profiler::registerThread at startup
// (debug-asserted). MAY span a JobSystem wait(): the JobSystem migrates the open-scope stack with
// the parking fiber (suspendScopes/resumeScopes), so the scope deliberately caches NO track
// pointer - construction and destruction each address the CURRENT thread's track, and the
// destructor closes whatever segment started at the last resume (parked time is recorded as its
// own per-segment records, not double-counted here).
export class ProfileScope final
{
public:
    ProfileScope(const char* name, EProfileCategory category)
    {
        ProfileTrack* track = Globals::profiler.threadTrack(); // never null: every thread registers at startup
        assert(track != nullptr && "ProfileScope on an unregistered thread - call Globals::profiler.registerThread() at thread startup");
        m_name = name;
        m_category = category;
        m_start = Profiler::tick();
        const uint32_t depth = track->m_openDepth++;
        if (depth < ProfileTrack::MAX_OPEN_DEPTH)
        {
            track->m_openNames[depth] = name;
            track->m_openCategories[depth] = (uint8_t)category;
            track->m_openStarts[depth] = m_start;
        }
    }
    // Ends the scope early (for spans that can't be a plain block because declarations in the
    // middle are needed afterwards). The destructor is then a no-op.
    void stop()
    {
        if (m_name == nullptr)
            return;
        const uint64_t end = Profiler::tick();
        ProfileTrack* track = Globals::profiler.threadTrack(); // re-fetched: the fiber may have migrated threads since construction
        const uint32_t depth = --track->m_openDepth;
        assert(depth >= ProfileTrack::MAX_OPEN_DEPTH || track->m_openNames[depth] == m_name && "unbalanced ProfileScope nesting");
        // The track's start, not m_start: a resume rewrote it, so this record covers only the
        // segment since the last resume. Beyond MAX_OPEN_DEPTH there is no slot - fall back to the
        // construction tick (such scopes cannot migrate; see suspendScopes' assert).
        const uint64_t start = depth < ProfileTrack::MAX_OPEN_DEPTH ? track->m_openStarts[depth] : m_start;
        track->push(start, end, m_name, (uint16_t)depth, m_category);
        m_name = nullptr;
    }
    ~ProfileScope() { stop(); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:

    const char* m_name;
    uint64_t m_start;
    EProfileCategory m_category;
};
