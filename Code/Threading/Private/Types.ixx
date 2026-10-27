export module Threading:Types;

import Core;

export class JobSystem;
struct Fiber;

// -- Submit parameters --

// Required on every submit: the scheduler scopes the job with it. `name` must be a string literal.
export struct JobProfile
{
    const char* name = nullptr;
    EProfileCategory category = EProfileCategory::Threading;
};

export enum class EJobPriority : uint8
{
    High = 0,
    Normal = 1,
    Low = 2,
};
export constexpr uint32 NumJobPriorities = 3;

export enum EJobFlags : uint8
{
    EJobFlag_Pooled = 1,  // owned by the JobSystem's ad-hoc pool, recycled after execution
    EJobFlag_Untimed = 2, // no wall-time measurement: only for nanosecond-scale jobs
    // REQUIRED when the body waits on a counter it did not create: a non-fiber stack never runs it
    // inline inside another job (deadlock). See "ForeignWait" in Threading CONTEXT.md.
    EJobFlag_ForeignWait = 4,
};

// -- Completion --

// On the waiting fiber's stack: the signaler must read `next` before making `fiber` resumable.
export struct FiberWaitNode
{
    Fiber* fiber = nullptr;
    FiberWaitNode* next = nullptr;
};

// Completion counter. A waiter may free it the moment it reads zero, so the zero transition never
// touches its memory after that (the invariants: "JobCounter" in Threading CONTEXT.md). Must outlive
// every job signaling it and every wait on it; re-add() only after the previous batch's waits returned.
export class alignas(64) JobCounter final
{
public:

    static constexpr uint32 CountMask = 0x00ffffff; // low 24 bits: outstanding jobs
    static constexpr uint32 WaiterInc = 0x01000000; // high 8 bits: registered fiber waiters

    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;
    ~JobCounter()
    {
        assert(isDone());
        [[maybe_unused]] FiberWaitNode* head = m_waiterHead.load(oc::memory_order_relaxed);
        assert(!head || head == takenSentinel());
    }

    void add(uint32 count = 1)
    {
        // a consumed wait list stays claimed until the counter is reused for a new batch
        if (m_waiterHead.load(oc::memory_order_relaxed) == takenSentinel())
            m_waiterHead.store(nullptr, oc::memory_order_relaxed);
        [[maybe_unused]] const uint32 old = m_count.fetch_add(count, oc::memory_order_relaxed);
        assert((old & CountMask) + count <= CountMask);
    }
    bool isDone() const { return m_count.load(oc::memory_order_acquire) == 0; }
    uint32 pending() const { return m_count.load(oc::memory_order_relaxed) & CountMask; }

    const char* label = nullptr; // the last submitted job's profile name: wait() names its stall scope after it

private:

    friend class JobSystem;
    static FiberWaitNode* takenSentinel()
    {
        static FiberWaitNode sentinel;
        return &sentinel;
    }

    oc::atomic<uint32> m_count = 0;
    oc::atomic<FiberWaitNode*> m_waiterHead = nullptr;
};

// -- Jobs --

export using JobFunc = void(*)(void*);

// Two cache lines, callable stored inline. Graph jobs carry successor edges; pooled jobs have none.
export struct alignas(64) Job
{
    JobFunc invoke = nullptr;
    JobFunc destroy = nullptr;          // only set for non-trivially-destructible captures
    JobCounter* signal = nullptr;
    Job* const* successors = nullptr;
    uint32 numSuccessors = 0;
    oc::atomic<int32> pending = 0;     // unsatisfied predecessors; last one to decrement pushes us
    uint32 initialPending = 0;          // reset value for re-running graphs
    uint32 costEmaUs = 0;               // EMA of measured wall time (Timed jobs; min 1us per run)
    uint32 rankUs = 0;                  // critical path: costEmaUs + longest successor chain (graph)
    EJobPriority priority = EJobPriority::Normal;          // what the user asked for
    EJobPriority effectivePriority = EJobPriority::Normal; // what the queues use (rank promotion)
    uint8 flags = 0;
    EProfileCategory profileCategory = EProfileCategory::Threading; // execute() opens name+this
    const char* name = nullptr;

    static constexpr uint32 StorageSize = 64;
    alignas(8) uint8 storage[StorageSize];
};
static_assert(sizeof(Job) == 128);

export template<typename F>
void setJobCallable(Job& job, F&& func)
{
    using Func = std::decay_t<F>;
    static_assert(sizeof(Func) <= Job::StorageSize, "capture list exceeds inline job storage - capture a pointer to bigger state instead");
    static_assert(alignof(Func) <= 8, "over-aligned captures not supported by inline job storage");
    new (static_cast<void*>(job.storage)) Func(oc::forward<F>(func));
    job.invoke = [](void* storage) { (*static_cast<Func*>(storage))(); };
    if constexpr (std::is_trivially_destructible_v<Func>)
        job.destroy = nullptr;
    else
        job.destroy = [](void* storage) { static_cast<Func*>(storage)->~Func(); };
}

// -- Graph resources and cost estimates --

// A process-unique id for data that graph jobs declare read/write access on. Create once, keep alive.
export class JobResource final
{
public:

    explicit JobResource(const char* name = nullptr) : m_id(s_nextId.fetch_add(1, oc::memory_order_relaxed)), m_name(name) {}
    JobResource(const JobResource&) = delete;
    JobResource& operator=(const JobResource&) = delete;

    uint32 id() const { return m_id; }
    const char* name() const { return m_name; }

private:

    static inline oc::atomic<uint32> s_nextId = 0;
    uint32 m_id;
    const char* m_name;
};

// Per-item cost estimate of one parallelFor call site, passed instead of a grain size. A racy EMA on purpose.
export class JobCost final
{
public:

    explicit JobCost(uint32 initialNsPerItem = 250, const char* name = nullptr)
        : m_nsPerItem(oc::max(1u, initialNsPerItem)), m_name(name) {}
    JobCost(const JobCost&) = delete;
    JobCost& operator=(const JobCost&) = delete;

    uint64 nsPerItem() const { return m_nsPerItem.load(oc::memory_order_relaxed); }
    const char* name() const { return m_name; }

    void addSample(uint64 totalNs, uint32 numItems)
    {
        if (numItems == 0)
            return;
        const uint64 sample = oc::max<uint64>(1, totalNs / numItems);
        const uint64 old = m_nsPerItem.load(oc::memory_order_relaxed);
        m_nsPerItem.store((old * 7 + sample) / 8, oc::memory_order_relaxed);
    }

private:

    oc::atomic<uint64> m_nsPerItem;
    const char* m_name;
};

// -- Setup and stats --

export struct JobSystemDesc
{
    uint32 numWorkers = 0;                  // 0 = PHYSICAL cores - 2 (main + headroom), min 1
    uint32 numFibers = 64;                 // max concurrently started-but-unfinished jobs (parked waits hold one)
    uint32 fiberStackCommit = 64 * 1024;
    uint32 fiberStackReserve = 512 * 1024;
    uint32 jobPoolCapacity = 65536;         // ad-hoc jobs in flight (rounded up to a power of two): 8 MB of Jobs
    uint32 queueCapacity = 65536;           // per priority class (rounded up to a power of two): a whole pool can sit in one ring
    uint32 dequeCapacity = 4096;            // per worker (rounded up to a power of two)
    uint32 postUpdateQueueCapacity = 1024;  // submitPostUpdate jobs awaiting the frame's kick
    uint32 parallelForTargetChunkNs = 25000; // auto-grain (JobCost overload) aims chunks at this duration
};

export struct JobSystemStats
{
    uint32 numWorkers = 0;
    uint32 numFibers = 0;
    int32 numPooledInFlight = 0; // ad-hoc jobs allocated but not yet recycled
    uint64 numExecuted = 0;
    uint64 numStolen = 0;
    uint64 numParked = 0;    // fiber waits that actually suspended
    uint64 numResumed = 0;
    uint64 numSleeps = 0;    // workers going through a kernel wait
    uint64 numInlineFallbacks = 0; // pool/queue exhaustion made a submit run on the calling thread
    uint64 numPreempted = 0; // higher-priority jobs run inline at a pre-emption point
    uint64 busyNs = 0;       // summed measured job wall time across contexts (timed jobs)
};
