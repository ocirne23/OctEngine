export module Threading:JobSystemState;

import Core;

import :JobTypes;
import :Job;
import :MPMCQueue;
import :FreeStack;
import :StealDeque;

export class JobSystem; // declared before their friend lines
export class JobGraph;
export class JobMutex;
export class JobEvent;
export class ThreadLocalScope;
export template<typename T> class PerWorker;

// A job runs on a fiber: a wait() parks it, and it may resume on another worker (/GT fiber-safe TLS).
struct Fiber
{
    enum class EState : uint32 { Idle, Running, Parked, Yielded, Finished }; // Yielded: see JobSystemState::yieldToResumed

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
    uint64 busyNs = 0;       // summed measured job time
};

// Everything of the JobSystem but its public API (JobSystem.ixx): the state and the private helpers.
export class JobSystemState
{
protected:

    friend class JobGraph;
    friend class JobMutex;
    friend class JobEvent;
    friend class ThreadLocalScope;
    template<typename T> friend class PerWorker;

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

    void shutdown(); // idempotent; joins workers, drops leftovers
    JobSystemStats collectStats() const;

    // 0 = main thread, 1..numWorkers = workers. Never cache it across a wait() (PerWorker::local).
    uint32 getWorkerIndex() const;

    // ---- Submit plumbing (JobGraph too) ----

    // A pooled job carrying `func`, filled in; nullptr = the pool is empty and `func` already ran here (out of phase, never lost).
    template<typename Func>
    Job* buildJob(Func&& func, JobProfile profile, EJobPriority priority, uint8 flags, JobCounter* counter);
    Job* allocatePooledJob();
    void releasePooledJob(Job* job);
    void submitReady(Job* job); // job->pending must already be zero
    void submitReadyBatch(oc::span<Job* const> jobs);
    void pushReadyJob(Job* job);
    void queueTimed(Job* job, double delaySec);
    void queuePostUpdate(Job* job, EPostUpdateBatch batch);
    void dropJob(Job* job); // shutdown path

    template<typename Func>
    void parallelForImpl(uint32 begin, uint32 end, uint32 grainSize, JobCost* cost, JobProfile profile, Func&& func, EJobPriority priority);

    // ---- Workers and fibers ----

    static void __stdcall fiberEntry(void* param);
    void workerMain(uint32 contextIndex);
    void timerMain();
    void fiberMain(Fiber& fiber);
    void runFiber(WorkerContext& ctx, Fiber* fiber);
    void execute(Job& job);
    void yieldToResumed(WorkerContext& ctx);

    // ---- Finding work ----

    Job* getWork(WorkerContext& ctx);
    Job* getHelpWork(WorkerContext& ctx); // never a Low job
    Job* trySteal(WorkerContext& ctx);
    bool tryRunOneJob();                  // a registered non-worker thread (main) pumps one ready job (the JobMutex slow path)
    bool hasHigherPriorityReady(EJobPriority mine) const;
    bool runHigherPriorityJobs(EJobPriority mine);

    // ---- Sleep and wake ----

    void idle(WorkerContext& ctx, bool forceSleep);
    bool anyWorkForWorker() const;
    void wakeMany(uint32 count);
    void wakeOne() { wakeMany(1); }
    void wakeHelper(); // the external helper's eventcount

    // ---- Waits ----

    void waitCounter(JobCounter& counter);
    void signalCounter(JobCounter& counter); // decrements, resuming waiters at zero; pair every signal with a preceding add()
    void fiberWait(JobCounter& counter, WorkerContext& ctx);
    void helpWait(JobCounter& counter, WorkerContext& ctx);
    void lockJobMutexSlow(JobMutex& mutex);
    void unlockJobMutexSlow(JobMutex& mutex);
    static void debugThreadLocalPin(int delta); // ThreadLocalScope's per-thread pin count; a no-op in non-debug

    // ---- Pools and contexts ----

    oc::unique_ptr<Job[]> m_jobPool;
    oc::unique_ptr<Fiber[]> m_fibers;
    oc::unique_ptr<WorkerContext[]> m_contexts; // [0] = main thread, [1..numWorkers] = workers, last = the external helper
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

    // ---- Queues ----

    MPMCQueue<Fiber*> m_resumeQueue;
    MPMCQueue<Fiber*> m_yieldQueue; // fibers that stepped aside at a pre-emption point
    MPMCQueue<Job*> m_readyQueues[NumJobPriorities];
    PostUpdateBatch m_postUpdate[uint32(EPostUpdateBatch::Count)];

    // ---- Sleep and wake ----

    alignas(64) oc::atomic<uint32> m_wakeEpoch = 0;
    oc::atomic<uint32> m_numSleepers = 0;
    oc::atomic<uint32> m_helperEpoch = 0; // external helper (window thread) eventcount
    oc::atomic<uint32> m_helperSleeping = 0;

    // ---- Delayed jobs ----

    std::thread m_timerThread;
    std::mutex m_timedMutex;
    std::condition_variable m_timedCv;
    oc::priority_queue<TimedJob, oc::vector<TimedJob>, TimedJobLater> m_timedJobs;

    // ---- The frame's physics step ----

    bool m_framePhysicsStep = false;
    bool m_prevFramePhysicsStep = false;
};

// ---- Inline bodies ----

template<typename Func>
Job* JobSystemState::buildJob(Func&& func, JobProfile profile, EJobPriority priority, uint8 flags, JobCounter* counter)
{
    assert(profile.name && "every job must carry a JobProfile name (a string literal)");
    Job* job = allocatePooledJob();
    if (!job)
    {
        m_numInlineFallbacks.fetch_add(1, oc::memory_order_relaxed);
        assert(false && "job pool exhausted - raise JobSystemDesc::jobPoolCapacity");
        ProfileScope scope(profile.name, profile.category);
        func();
        return nullptr;
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
        counter->m_label = profile.name;
        job->signal = counter;
    }
    return job;
}

template<typename Func>
void JobSystemState::parallelForImpl(uint32 begin, uint32 end, uint32 grainSize, JobCost* cost, JobProfile profile, Func&& func, EJobPriority priority)
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
    const uint32 chunks = (count + grainSize - 1) / grainSize;
    const uint32 numHelpers = oc::min(m_numWorkers, chunks - 1);
    JobCounter counter;
    {
        ProfileScope scope(profile.name, profile.category);
        for (uint32 i = 0; i < numHelpers; ++i)
            if (Job* job = buildJob(runner, profile, priority, 0, &counter))
                submitReady(job);
        runner();
    }
    waitCounter(counter);
}
