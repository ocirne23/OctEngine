export module Threading:JobSystem;

import Core;

import :JobTypes;
import :JobSystemState; // the state and the private helpers

// THE JOB SYSTEM'S API: a fiber-based work-stealing job scheduler. initialize() on the main thread (registers it as a helper context). The value types are
// in JobTypes.ixx, the state and the private helpers in JobSystemState.ixx; the multi-line bodies are below the class.
export class JobSystem final : public JobSystemState
{
public:

    void initialize(const JobSystemDesc& desc = {});
    ~JobSystem() { shutdown(); } // joins workers, drops leftovers

    // ---- Submitting (the callable must fit Job::StorageSize; `profile` is required: execute() scopes the body with it) ----

    template<typename Func>
    void submit(Func&& func, JobProfile profile, EJobPriority priority = EJobPriority::Normal, JobCounter* counter = nullptr, uint8 flags = 0);

    template<typename Func>
    void submitDelayed(double delaySec, Func&& func, JobProfile profile, EJobPriority priority = EJobPriority::Normal, JobCounter* counter = nullptr);

    // kicked before present, joined in the next frame: the job overlaps present() and what follows it
    using EPostUpdateBatch = ::EPostUpdateBatch;
    template<typename Func>
    void submitPostUpdate(Func&& func, JobProfile profile, EJobPriority priority = EJobPriority::Normal, uint8 flags = 0, EPostUpdateBatch batch = EPostUpdateBatch::Frame);
    static constexpr uint32 numChunks(uint32 count, uint32 grainSize);

    template<typename Func>
    void parallelFor(uint32 begin, uint32 end, uint32 grainSize, JobProfile profile, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        parallelForImpl(begin, end, grainSize, nullptr, profile, oc::forward<Func>(func), priority);
    }
    // Auto-grain from the measured ns/item (JobSystemDesc::parallelForTargetChunkNs).
    template<typename Func>
    void parallelFor(uint32 begin, uint32 end, JobCost& cost, JobProfile profile, Func&& func, EJobPriority priority = EJobPriority::Normal);

    // ---- Waiting ----

    void wait(JobCounter& counter) { waitCounter(counter); } // A job fiber parks; the main thread runs jobs meanwhile; any other thread blocks.
    // runs every queued job of a strictly higher priority inline, then returns. No ThreadLocalScope alive, no JobMutex held that the nested job could take.
    bool preemptionPoint();

    // ---- Update ----
    void kickPostUpdateJobs(); // Main loop only.
    void joinPostUpdateJobs(EPostUpdateBatch batch = EPostUpdateBatch::Frame);

    void registerExternalHelper(); // claims the reserved external helper context; once, on that thread
    bool tryRunOneHighJob();       // the High ring only: the window thread must never pick up a long job
    // Parks until a High job is submitted or wakeExternalHelper() fires. wakeNow(user) is the caller's own wake condition, re-checked after the sleep is
    // announced (else a lost wake); nullptr = none.
    void externalHelperWait(bool (*wakeNow)(const void*), const void* user);
    void wakeExternalHelper() { wakeHelper(); }

    // ---- The frame's physics step (published by main before any kick; work that can wait a frame asks deferFromPhysicsFrame()) ----

    void setFrameHasPhysicsStep(bool step) { m_prevFramePhysicsStep = m_framePhysicsStep; m_framePhysicsStep = step; }
    bool frameHasPhysicsStep() const { return m_framePhysicsStep; }
    bool prevFrameHadPhysicsStep() const { return m_prevFramePhysicsStep; }
    bool deferFromPhysicsFrame() const { return m_framePhysicsStep && !m_prevFramePhysicsStep; } // false below the step rate, where every frame steps

    // ---- Stats ----

    uint32 getNumWorkers() const { return m_numWorkers; }
    uint32 getNumContexts() const { return m_numContexts; } // sizes PerWorker arrays
    JobSystemStats getStats() const { return collectStats(); }
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_JOB_SYSTEM)
    JobSystem jobSystem;
}

// ---- Inline bodies ----

template<typename Func>
void JobSystem::submit(Func&& func, JobProfile profile, EJobPriority priority, JobCounter* counter, uint8 flags)
{
    if (Job* job = buildJob(oc::forward<Func>(func), profile, priority, flags, counter))
        submitReady(job);
}

template<typename Func>
void JobSystem::submitDelayed(double delaySec, Func&& func, JobProfile profile, EJobPriority priority, JobCounter* counter)
{
    if (Job* job = buildJob(oc::forward<Func>(func), profile, priority, 0, counter))
        queueTimed(job, delaySec);
}

template<typename Func>
void JobSystem::submitPostUpdate(Func&& func, JobProfile profile, EJobPriority priority, uint8 flags, EPostUpdateBatch batch)
{
    if (Job* job = buildJob(oc::forward<Func>(func), profile, priority, flags, nullptr))
        queuePostUpdate(job, batch);
}

constexpr uint32 JobSystem::numChunks(uint32 count, uint32 grainSize)
{
    grainSize = grainSize > 1 ? grainSize : 1;
    return (count + grainSize - 1) / grainSize;
}

template<typename Func>
void JobSystem::parallelFor(uint32 begin, uint32 end, JobCost& cost, JobProfile profile, Func&& func, EJobPriority priority)
{
    const uint64 grain = oc::max<uint64>(1, m_targetChunkNs / cost.nsPerItem());
    parallelForImpl(begin, end, uint32(oc::min<uint64>(grain, UINT32_MAX)), &cost, profile, oc::forward<Func>(func), priority);
}
