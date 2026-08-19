export module Threading:JobSystem;

import Core;
import :Types;
import :MPMCQueue;
import :FreeStack;
import :StealDeque;

export class JobMutex;

// A job executes on a fiber, so it can wait() mid-execution without blocking its worker thread:
// the fiber parks on the counter's wait list, the worker switches back to its scheduler loop and
// runs other jobs, and when the counter hits zero the fiber is pushed to the resume queue and
// continues on whichever worker picks it up (possibly a different thread - hence /GT fiber-safe
// TLS in the root CMakeLists; job code must not cache thread_local addresses across a wait).
struct Fiber
{
    enum class EState : uint32 { Idle, Running, Parked, Finished };

    void* handle = nullptr;      // Win32 fiber
    void* returnFiber = nullptr; // scheduler fiber of the worker currently running us
    Job* job = nullptr;
    EState state = EState::Idle;
    oc::atomic<uint32> switchDone = 0;  // parked fiber has fully switched out; resumers spin on this
    oc::atomic<uint32> debugRunning = 0; // debug-only tripwire: a fiber must never run on two workers

    // Open profile scopes travel WITH the fiber across a park (saved on the parking thread before
    // the switch-out, replayed by runFiber on whichever thread resumes it - see
    // Profiler::suspendScopes/resumeScopes). profileBase is the resuming thread's own open depth
    // at fiber entry: a later suspend strips only the scopes above it, leaving e.g. the main
    // helper's outer scopes intact. Publication rides the existing switchDone protocol.
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
    bool isWorker = false;          // false for the registered main thread (helps in wait, never parks)
    uint64 numExecuted = 0;
    uint64 numStolen = 0;
    uint64 numParked = 0;
    uint64 numResumed = 0;
    uint64 numSleeps = 0;
    uint64 busyNs = 0; // summed measured job time (timed jobs only)
};

// Fiber-based work-stealing job scheduler. All memory is allocated in initialize(); the
// steady-state hot paths are lock-free (Chase-Lev deque per worker, Vyukov MPMC rings for the
// priority queues / fiber freelist / resume queue / job pool) and workers sleep on an eventcount
// (oc::atomic wait -> WaitOnAddress) when idle. Priorities order the shared queues; a worker's
// own continuations always run LIFO from its local deque first, which is what makes fork/join
// fast. Delayed jobs sit in a min-heap serviced by a dedicated timer thread. initialize() must
// be called on the main thread: it registers the caller as a helper context so wait() on the
// main thread runs jobs instead of blocking.
export class JobSystem final
{
public:

    void initialize(const JobSystemDesc& desc = {});
    void shutdown(); // idempotent; joins workers, drops leftovers. Runs from ~JobSystem via init_seg ordering
    ~JobSystem() { shutdown(); } // atexit runs on the main thread, so the helper-context teardown is valid

    uint32 getNumWorkers() const { return m_numWorkers; }
    JobSystemStats getStats() const;

    // Index of the calling thread's scheduler context: 0 = the registered main thread,
    // 1..getNumWorkers() = workers. Only valid on registered threads and inside jobs. NEVER cache
    // it across a wait() - the fiber may resume on a different worker (same reason as /GT).
    uint32 getWorkerIndex() const;
    uint32 getNumContexts() const { return m_numContexts; } // sizes PerWorker arrays

    // Manually decrement a counter, resuming waiters at zero - the building block for JobEvent
    // and external completion sources. Pair every signal with a preceding add().
    void signal(JobCounter& counter) { signalCounter(counter); }

    // Fire-and-forget or counter-tracked async execution. The callable must fit Job::StorageSize.
    // counter (optional) is incremented now and decremented when the job finishes: submit
    // everything, then wait once. `profile` is REQUIRED: execute() opens that scope around the
    // job's body, so nothing a worker runs is ever invisible on the profiler (a job body may still
    // open finer scopes of its own inside it).
    template<typename Func>
    void submit(Func&& func, JobProfile profile, EJobPriority priority = EJobPriority::Normal, JobCounter* counter = nullptr)
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
        job->name = profile.name;
        job->profileCategory = profile.category;
        if (counter)
        {
            counter->add(1);
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
            job->signal = counter;
        }
        queueTimed(job, delaySec);
    }

    // Waits until the counter reaches zero. On a job fiber this parks the fiber (the worker keeps
    // running other jobs); on the main thread it runs jobs while waiting; on any other thread it
    // blocks on the atomic.
    void wait(JobCounter& counter);

    // Registered non-worker threads (main) can pump one ready job manually, e.g. to burn a stall.
    bool tryRunOneJob();

    // Splits [begin, end) into grainSize chunks pulled from a shared atomic cursor by
    // getNumWorkers() helper jobs plus the calling thread. func(chunkBegin, chunkEnd) must be
    // safe to run concurrently with itself. Returns when the whole range is done. `profile` is
    // REQUIRED and covers each participant's WHOLE run (helper jobs through execute(), the calling
    // thread wrapped here) — one span per participant, not per chunk.
    template<typename Func>
    void parallelFor(uint32 begin, uint32 end, uint32 grainSize, JobProfile profile, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        parallelForImpl(begin, end, grainSize, nullptr, profile, oc::forward<Func>(func), priority);
    }

    // Auto-grain: the chunk size is derived from the tracker's measured ns/item to hit
    // JobSystemDesc::parallelForTargetChunkNs, and this run's timings feed back into it.
    template<typename Func>
    void parallelFor(uint32 begin, uint32 end, JobCost& cost, JobProfile profile, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        const uint64 grain = oc::max<uint64>(1, m_targetChunkNs / cost.nsPerItem());
        parallelForImpl(begin, end, uint32(oc::min<uint64>(grain, UINT32_MAX)), &cost, profile, oc::forward<Func>(func), priority);
    }

    // Scheduler plumbing shared with JobGraph - not for gameplay code.
    Job* allocatePooledJob();
    void submitReady(Job* job);                        // job->pending must already be zero
    void submitReadyBatch(oc::span<Job* const> jobs); // fan-out path: straight to the shared queues

private:

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
        };
        Shared shared{ begin, end, grainSize, cost, &func };
        auto runner = [&shared]()
        {
            const Clock::time_point start = shared.cost ? Clock::now() : Clock::time_point{};
            uint32 numItems = 0;
            for (;;)
            {
                const uint64 chunkBegin = shared.next.fetch_add(shared.grain, oc::memory_order_relaxed);
                if (chunkBegin >= shared.end)
                    break;
                const uint32 chunkEnd = uint32(oc::min(chunkBegin + shared.grain, shared.end));
                (*shared.func)(uint32(chunkBegin), chunkEnd);
                numItems += chunkEnd - uint32(chunkBegin);
            }
            if (shared.cost && numItems) // includes the cursor overhead: overhead-dominated loops drive the grain up
                shared.cost->addSample(uint64(std::chrono::nanoseconds(Clock::now() - start).count()), numItems);
        };
        const uint32 numChunks = (count + grainSize - 1) / grainSize;
        const uint32 numHelpers = oc::min(m_numWorkers, numChunks - 1);
        JobCounter counter;
        for (uint32 i = 0; i < numHelpers; ++i)
            submit(runner, profile, priority, &counter); // helpers scoped by execute()
        {
            ProfileScope scope(profile.name, profile.category); // the calling thread's share
            runner();
        }
        wait(counter);
    }

    friend class JobGraph;
    friend class JobMutex;
    void lockJobMutexSlow(JobMutex& mutex);
    void unlockJobMutexSlow(JobMutex& mutex);

    static void __stdcall fiberEntry(void* param);

    void workerMain(uint32 contextIndex);
    void timerMain();
    void fiberMain(Fiber& fiber);
    void runFiber(WorkerContext& ctx, Fiber* fiber);
    void execute(Job& job);
    Job* getWork(WorkerContext& ctx);
    Job* trySteal(WorkerContext& ctx);
    void pushReadyJob(Job* job);
    void idle(WorkerContext& ctx);
    bool anyWorkForWorker() const;
    void wakeMany(uint32 count);
    void wakeOne() { wakeMany(1); }
    void signalCounter(JobCounter& counter);
    void fiberWait(JobCounter& counter, WorkerContext& ctx);
    void helpWait(JobCounter& counter, WorkerContext& ctx);
    void releasePooledJob(Job* job);
    void queueTimed(Job* job, double delaySec);
    void dropJob(Job* job); // shutdown path: destroy the capture, release counters/pool slot

    struct TimedJob
    {
        Clock::time_point due;
        Job* job;
    };
    struct TimedJobLater
    {
        bool operator()(const TimedJob& a, const TimedJob& b) const { return a.due > b.due; }
    };

    oc::unique_ptr<Job[]> m_jobPool;
    oc::unique_ptr<Fiber[]> m_fibers;
    oc::unique_ptr<WorkerContext[]> m_contexts; // [0] = main thread, [1..numWorkers] = workers
    oc::vector<std::thread> m_threads;
    TaggedIndexStack m_freeJobs;
    TaggedIndexStack m_freeFibers;
    MPMCQueue<Fiber*> m_resumeQueue;
    MPMCQueue<Job*> m_readyQueues[NumJobPriorities];
    uint32 m_numWorkers = 0;
    uint32 m_numContexts = 0;
    uint32 m_numFibers = 0;
    uint32 m_jobPoolCapacity = 0;
    uint32 m_targetChunkNs = 25000;
    oc::atomic<bool> m_running = false;
    oc::atomic<uint64> m_numInlineFallbacks = 0;
    oc::atomic<int32> m_pooledInFlight = 0;

    alignas(64) oc::atomic<uint32> m_wakeEpoch = 0;
    oc::atomic<uint32> m_numSleepers = 0;

    std::thread m_timerThread;
    std::mutex m_timedMutex;
    std::condition_variable m_timedCv;
    oc::priority_queue<TimedJob, oc::vector<TimedJob>, TimedJobLater> m_timedJobs;
};

export namespace Globals
{
// Teardown ordering lives in InitSeg.h. Every EntityPtr holder destructs BEFORE this: destroying an
// entity reaches PerWorker::local(), which needs the main thread's live worker context. ~JobSystem
// then joins the workers before the renderer and the plain-seg globals they might touch go down.
OC_INIT_SEG(OC_SEG_JOB_SYSTEM)
    JobSystem jobSystem;
}
