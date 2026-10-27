export module Threading:JobSystem;

import Core;
import :Types;
import :MPMCQueue;
import :FreeStack;
import :StealDeque;

export class JobMutex;

// A job runs on a fiber: a wait() parks it, and it may resume on another worker (/GT fiber-safe TLS).
struct Fiber
{
    enum class EState : uint32 { Idle, Running, Parked, Yielded, Finished }; // Yielded: see JobSystem::yieldToResumed

    void* handle = nullptr;      // Win32 fiber
    void* returnFiber = nullptr; // scheduler fiber of the worker currently running us
    Job* job = nullptr;          // the job this fiber was started for
    Job* currentJob = nullptr;   // the innermost job on it: `job`, or one nested by a pre-emption point
    EState state = EState::Idle;
    oc::atomic<uint32> switchDone = 0;  // parked fiber has fully switched out; resumers spin on this
    oc::atomic<uint32> debugRunning = 0; // debug-only: a fiber must never run on two workers

    // Open profile scopes travel with the fiber across a park (Profiler::suspendScopes / resumeScopes).
    // profileBase: the resuming thread's own open depth at fiber entry.
    ProfileScopeStack profileScopes;
    uint32 profileBase = 0;
};

struct alignas(64) WorkerContext
{
    StealDeque deque;
    Fiber* currentFiber = nullptr;  // non-null while this worker is inside a job fiber
    void* schedulerFiber = nullptr;
    uint32 index = 0;
    uint32 stealSeed = 0;
    uint32 idleStreak = 0;          // consecutive getWork misses; past a few, idle() sleeps anyway
    bool isWorker = false;          // false for the registered main thread (helps in wait, never parks)

    uint64 numExecuted = 0;
    uint64 numStolen = 0;
    uint64 numParked = 0;
    uint64 numResumed = 0;
    uint64 numSleeps = 0;
    uint64 numPreempted = 0; // higher-priority jobs run inline at a pre-emption point
    uint64 busyNs = 0; // summed measured job time (timed jobs only)
};

// Fiber-based work-stealing job scheduler. initialize() on the main thread (registers it as a helper context).
export class JobSystem final
{
public:

    // -- Lifetime --
    void initialize(const JobSystemDesc& desc = {});
    void shutdown(); // idempotent; joins workers, drops leftovers
    ~JobSystem() { shutdown(); }

    uint32 getNumWorkers() const { return m_numWorkers; }
    uint32 getNumContexts() const { return m_numContexts; } // sizes PerWorker arrays
    // 0 = main thread, 1..getNumWorkers() = workers. Never cache it across a wait().
    uint32 getWorkerIndex() const;
    JobSystemStats getStats() const;

    // -- Submitting --
    // The callable must fit Job::StorageSize. `profile` is required: execute() scopes the body with it.
    template<typename Func>
    void submit(Func&& func, JobProfile profile, EJobPriority priority = EJobPriority::Normal, JobCounter* counter = nullptr, uint8 flags = 0)
    {
        assert(profile.name && "every job must carry a JobProfile name (a string literal)");
        Job* job = allocatePooledJob();
        if (!job) // pool exhausted: run it here rather than lose it
        {
            m_numInlineFallbacks.fetch_add(1, oc::memory_order_relaxed);
            assert(false && "job pool exhausted - raise JobSystemDesc::jobPoolCapacity");
            ProfileScope scope(profile.name, profile.category);
            func();
            return;
        }
        setJobCallable(*job, oc::forward<Func>(func));
        job->priority = priority;
        job->effectivePriority = priority;
        job->flags |= flags;
        job->name = profile.name;
        job->profileCategory = profile.category;
        if (counter)
        {
            counter->add(1);
            counter->label = profile.name;
            job->signal = counter;
        }
        submitReady(job);
    }

    template<typename Func>
    void submitDelayed(double delaySec, Func&& func, JobProfile profile, EJobPriority priority = EJobPriority::Normal, JobCounter* counter = nullptr)
    {
        assert(profile.name && "every job must carry a JobProfile name (a string literal)");
        Job* job = allocatePooledJob();
        if (!job)
        {
            m_numInlineFallbacks.fetch_add(1, oc::memory_order_relaxed);
            assert(false && "job pool exhausted - raise JobSystemDesc::jobPoolCapacity");
            ProfileScope scope(profile.name, profile.category);
            func();
            return;
        }
        setJobCallable(*job, oc::forward<Func>(func));
        job->priority = priority;
        job->effectivePriority = priority;
        job->name = profile.name;
        job->profileCategory = profile.category;
        if (counter)
        {
            counter->add(1);
            counter->label = profile.name;
            job->signal = counter;
        }
        queueTimed(job, delaySec);
    }

    // Decrements a counter, resuming waiters at zero. Pair every signal with a preceding add().
    void signal(JobCounter& counter) { signalCounter(counter); }

    // -- Post-update jobs --
    // Kicked before present, joined in the next frame. The job overlaps present() and what follows it.
    // Frame: joined first thing in the next frame. Sim: joined before the entity-change drains.
    enum class EPostUpdateBatch : uint8 { Frame, Sim, Count };
    template<typename Func>
    void submitPostUpdate(Func&& func, JobProfile profile, EJobPriority priority = EJobPriority::Normal, uint8 flags = 0,
        EPostUpdateBatch batch = EPostUpdateBatch::Frame)
    {
        submitPostUpdateImpl(oc::forward<Func>(func), profile, priority, flags, batch);
    }
    // Main loop only.
    void kickPostUpdateJobs();
    void joinPostUpdateJobs(EPostUpdateBatch batch = EPostUpdateBatch::Frame);

    // -- parallelFor --
    // Chunk k is [begin + k*grain, begin + (k+1)*grain) clipped to end: index per-chunk staging by
    // (chunkBegin - begin) / grainSize, numChunks() slots. func must be safe to run concurrently with itself.
    static constexpr uint32 numChunks(uint32 count, uint32 grainSize)
    {
        grainSize = grainSize > 1 ? grainSize : 1;
        return (count + grainSize - 1) / grainSize;
    }
    template<typename Func>
    void parallelFor(uint32 begin, uint32 end, uint32 grainSize, JobProfile profile, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        parallelForImpl(begin, end, grainSize, nullptr, profile, oc::forward<Func>(func), priority);
    }
    // Auto-grain from the measured ns/item (JobSystemDesc::parallelForTargetChunkNs).
    template<typename Func>
    void parallelFor(uint32 begin, uint32 end, JobCost& cost, JobProfile profile, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        const uint64 grain = oc::max<uint64>(1, m_targetChunkNs / cost.nsPerItem());
        parallelForImpl(begin, end, uint32(oc::min<uint64>(grain, UINT32_MAX)), &cost, profile, oc::forward<Func>(func), priority);
    }

    // -- Waiting --
    // A job fiber parks; the main thread runs jobs meanwhile; any other thread blocks.
    void wait(JobCounter& counter);
    // Registered non-worker threads (main) can pump one ready job manually.
    bool tryRunOneJob();
    // PRE-EMPTION POINT (cooperative): runs every queued job of a strictly higher priority inline, then
    // returns. No ThreadLocalScope alive, no JobMutex held that the nested job could take.
    bool preemptionPoint();

    // -- The external helper (the window thread, Core.Window) --
    // Claims the reserved external helper context. Once, on that thread.
    void registerExternalHelper();
    // High-priority ring only: the window thread must never pick up a long job.
    bool tryRunOneHighJob();
    // Parks until a High job is submitted or wakeExternalHelper() fires. wakeNow(user) is the caller's own
    // wake condition, re-checked after the sleep is announced (else a lost wake); nullptr = none.
    void externalHelperWait(bool (*wakeNow)(const void*), const void* user);
    void wakeExternalHelper();

    // -- The frame's physics step --
    // Published by main before any kick. Work that can wait a frame asks deferFromPhysicsFrame() (false
    // below the step rate, where every frame steps).
    void setFrameHasPhysicsStep(bool step) { m_prevFramePhysicsStep = m_framePhysicsStep; m_framePhysicsStep = step; }
    bool frameHasPhysicsStep() const { return m_framePhysicsStep; }
    bool prevFrameHadPhysicsStep() const { return m_prevFramePhysicsStep; }
    bool deferFromPhysicsFrame() const { return m_framePhysicsStep && !m_prevFramePhysicsStep; }

    // -- Debug --
    // ThreadLocalScope's per-thread pin count. A no-op in non-debug.
    static void debugThreadLocalPin(int delta);

private:

    friend class JobGraph;
    friend class JobMutex;

    struct TimedJob
    {
        Clock::time_point due;
        Job* job;
    };
    struct TimedJobLater
    {
        bool operator()(const TimedJob& a, const TimedJob& b) const { return a.due > b.due; }
    };
    struct PostUpdateBatch
    {
        MPMCQueue<Job*> queue;
        JobCounter counter;
    };

    // -- Submit plumbing (JobGraph too) --
    Job* allocatePooledJob();
    void releasePooledJob(Job* job);
    void submitReady(Job* job);                        // job->pending must already be zero
    void submitReadyBatch(oc::span<Job* const> jobs);
    void pushReadyJob(Job* job);
    void queueTimed(Job* job, double delaySec);
    void dropJob(Job* job); // shutdown path

    template<typename Func>
    void submitPostUpdateImpl(Func&& func, JobProfile profile, EJobPriority priority, uint8 flags, EPostUpdateBatch batch)
    {
        assert(profile.name && "every job must carry a JobProfile name (a string literal)");
        Job* job = allocatePooledJob();
        if (!job) // pool exhausted: run it here rather than lose it
        {
            m_numInlineFallbacks.fetch_add(1, oc::memory_order_relaxed);
            assert(false && "job pool exhausted - raise JobSystemDesc::jobPoolCapacity");
            ProfileScope scope(profile.name, profile.category);
            func();
            return;
        }
        setJobCallable(*job, oc::forward<Func>(func));
        job->priority = priority;
        job->effectivePriority = priority;
        job->flags |= flags;
        job->name = profile.name;
        job->profileCategory = profile.category;
        if (!m_postUpdate[uint32(batch)].queue.push(job))
        {
            // full: run it out of phase rather than lose it
            m_numInlineFallbacks.fetch_add(1, oc::memory_order_relaxed);
            assert(false && "post-update queue full - raise JobSystemDesc::postUpdateQueueCapacity");
            execute(*job);
        }
    }

    template<typename Func>
    void parallelForImpl(uint32 begin, uint32 end, uint32 grainSize, JobCost* cost, JobProfile profile, Func&& func, EJobPriority priority)
    {
        assert(profile.name && "every parallelFor must carry a JobProfile name (a string literal)");
        if (end <= begin)
            return;
        const uint32 count = end - begin;
        grainSize = oc::max(grainSize, 1u);
        if (count <= grainSize || m_numWorkers == 0)
        {
            ProfileScope scope(profile.name, profile.category);
            const Clock::time_point start = cost ? Clock::now() : Clock::time_point{};
            func(begin, end);
            if (cost)
                cost->addSample(uint64(std::chrono::nanoseconds(Clock::now() - start).count()), count);
            return;
        }
        struct Shared
        {
            oc::atomic<uint64> next;
            uint64 end;
            uint32 grain;
            JobCost* cost;
            std::remove_reference_t<Func>* func;
            EJobPriority priority;
        };
        Shared shared{ begin, end, grainSize, cost, &func, priority };
        auto runner = [&shared, this]()
        {
            const Clock::time_point start = shared.cost ? Clock::now() : Clock::time_point{};
            uint32 numItems = 0;
            uint64 preemptedNs = 0;
            for (;;)
            {
                const uint64 chunkBegin = shared.next.fetch_add(shared.grain, oc::memory_order_relaxed);
                if (chunkBegin >= shared.end)
                    break;
                const uint32 chunkEnd = uint32(oc::min(chunkBegin + shared.grain, shared.end));
                (*shared.func)(uint32(chunkBegin), chunkEnd);
                numItems += chunkEnd - uint32(chunkBegin);
                // Pre-emption point between chunks; its time is left out of the JobCost sample.
                if (shared.priority != EJobPriority::High && hasHigherPriorityReady(shared.priority))
                {
                    const Clock::time_point preemptStart = shared.cost ? Clock::now() : Clock::time_point{};
                    if (runHigherPriorityJobs(shared.priority) && shared.cost)
                        preemptedNs += uint64(std::chrono::nanoseconds(Clock::now() - preemptStart).count());
                }
            }
            if (shared.cost && numItems)
                shared.cost->addSample(uint64(std::chrono::nanoseconds(Clock::now() - start).count()) - preemptedNs, numItems);
        };
        const uint32 chunks = numChunks(count, grainSize);
        const uint32 numHelpers = oc::min(m_numWorkers, chunks - 1);
        JobCounter counter;
        {
            ProfileScope scope(profile.name, profile.category);
            for (uint32 i = 0; i < numHelpers; ++i)
                submit(runner, profile, priority, &counter);
            runner();
        }
        wait(counter);
    }

    // -- Workers and fibers --
    static void __stdcall fiberEntry(void* param);
    void workerMain(uint32 contextIndex);
    void timerMain();
    void fiberMain(Fiber& fiber);
    void runFiber(WorkerContext& ctx, Fiber* fiber);
    void execute(Job& job);
    void yieldToResumed(WorkerContext& ctx);

    // -- Finding work --
    Job* getWork(WorkerContext& ctx);
    Job* getHelpWork(WorkerContext& ctx); // never a Low job
    Job* trySteal(WorkerContext& ctx);
    bool hasHigherPriorityReady(EJobPriority mine) const;
    bool runHigherPriorityJobs(EJobPriority mine);

    // -- Sleep and wake --
    void idle(WorkerContext& ctx, bool forceSleep);
    bool anyWorkForWorker() const;
    void wakeMany(uint32 count);
    void wakeOne() { wakeMany(1); }

    // -- Waits --
    void signalCounter(JobCounter& counter);
    void fiberWait(JobCounter& counter, WorkerContext& ctx);
    void helpWait(JobCounter& counter, WorkerContext& ctx);
    void lockJobMutexSlow(JobMutex& mutex);
    void unlockJobMutexSlow(JobMutex& mutex);

    // -- Pools and contexts --
    oc::unique_ptr<Job[]> m_jobPool;
    oc::unique_ptr<Fiber[]> m_fibers;
    oc::unique_ptr<WorkerContext[]> m_contexts; // [0] = main thread, [1..numWorkers] = workers
    oc::vector<std::thread> m_threads;
    TaggedIndexStack m_freeJobs;
    TaggedIndexStack m_freeFibers;
    uint32 m_numWorkers = 0;
    uint32 m_numContexts = 0;
    uint32 m_numFibers = 0;
    uint32 m_jobPoolCapacity = 0;
    uint32 m_targetChunkNs = 25000;
    oc::atomic<bool> m_running = false;
    oc::atomic<uint64> m_numInlineFallbacks = 0;
    oc::atomic<int32> m_pooledInFlight = 0;

    // -- Queues --
    MPMCQueue<Fiber*> m_resumeQueue;
    MPMCQueue<Fiber*> m_yieldQueue; // fibers that stepped aside at a pre-emption point
    MPMCQueue<Job*> m_readyQueues[NumJobPriorities];
    PostUpdateBatch m_postUpdate[uint32(EPostUpdateBatch::Count)];

    // -- Sleep and wake --
    alignas(64) oc::atomic<uint32> m_wakeEpoch = 0;
    oc::atomic<uint32> m_numSleepers = 0;
    oc::atomic<uint32> m_helperEpoch = 0;    // external helper (window thread) eventcount
    oc::atomic<uint32> m_helperSleeping = 0;

    // -- Delayed jobs --
    std::thread m_timerThread;
    std::mutex m_timedMutex;
    std::condition_variable m_timedCv;
    oc::priority_queue<TimedJob, oc::vector<TimedJob>, TimedJobLater> m_timedJobs;

    // -- The frame's physics step --
    bool m_framePhysicsStep = false;
    bool m_prevFramePhysicsStep = false;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_JOB_SYSTEM)
    JobSystem jobSystem;
}

// Debug pin on the current thread's TLS (a thread_local, a PerWorker::local() slot, getWorkerIndex()):
// while alive, a fiber park or an inline job on this thread asserts. Nests; no-op in non-debug.
export class ThreadLocalScope final
{
public:
    ThreadLocalScope() { JobSystem::debugThreadLocalPin(+1); }
    ~ThreadLocalScope() { JobSystem::debugThreadLocalPin(-1); }
    ThreadLocalScope(const ThreadLocalScope&) = delete;
    ThreadLocalScope& operator=(const ThreadLocalScope&) = delete;
};
