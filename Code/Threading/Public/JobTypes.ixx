export module Threading:JobTypes;

import Core;

import :JobTypesState; // the state

// The value types of the JobSystem's API (JobSystem.ixx). The state is in JobTypesState.ixx.

// ---- Submit parameters ----

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
    // REQUIRED when the body waits on a counter it did not create: a non-fiber stack never runs it inline inside another job (deadlock). See
    // "ForeignWait" in Threading CONTEXT.md.
    EJobFlag_ForeignWait = 4,
};

// Post-update jobs: kicked before present, joined in the next frame. Frame: joined first thing in the next frame. Sim: joined before the entity-change drains.
export enum class EPostUpdateBatch : uint8 { Frame, Sim, Count };

// ---- Completion ----

// Completion counter. A waiter may free it the moment it reads zero, so the zero transition never touches its memory after that (the invariants:
// "JobCounter" in Threading CONTEXT.md). Must outlive every job signaling it and every wait on it; re-add() only after the previous batch's waits returned.
export class alignas(64) JobCounter final : public JobCounterState
{
public:

    JobCounter() = default;
    ~JobCounter();

    void add(uint32 count = 1);
    bool isDone() const { return m_count.load(oc::memory_order_acquire) == 0; }

    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;
};

// ---- Cost estimates ----

// Per-item cost estimate of one parallelFor call site, passed instead of a grain size. A racy EMA on purpose.
export class JobCost final : public JobCostState
{
public:

    explicit JobCost(uint32 initialNsPerItem = 250) : JobCostState(initialNsPerItem) {}

    uint64 nsPerItem() const { return m_nsPerItem.load(oc::memory_order_relaxed); }
    void addSample(uint64 totalNs, uint32 numItems); // the parallelFor runners feed it; a caller may feed its own measurements (World's update batches)

    JobCost(const JobCost&) = delete;
    JobCost& operator=(const JobCost&) = delete;
};

// ---- Setup and stats ----

export struct JobSystemDesc
{
    uint32 numWorkers = 0;                   // 0 = cores - 2 (main + the window thread), min 1; see "Worker sizing" in Threading CONTEXT.md
    uint32 numFibers = 64;                   // max concurrently started-but-unfinished jobs (parked waits hold one)
    uint32 fiberStackCommit = 64 * 1024;
    uint32 fiberStackReserve = 512 * 1024;
    uint32 jobPoolCapacity = 65536;          // ad-hoc jobs in flight (rounded up to a power of two): 8 MB of Jobs
    uint32 queueCapacity = 65536;            // per priority class (rounded up to a power of two): a whole pool can sit in one ring
    uint32 dequeCapacity = 4096;             // per worker (rounded up to a power of two)
    uint32 postUpdateQueueCapacity = 1024;   // submitPostUpdate jobs awaiting the frame's kick
    uint32 parallelForTargetChunkNs = 25000; // auto-grain (JobCost overload) aims chunks at this duration
};

export struct JobSystemStats
{
    uint32 numWorkers = 0;
    uint32 numFibers = 0;
    int32 numPooledInFlight = 0;   // ad-hoc jobs allocated but not yet recycled
    uint64 numExecuted = 0;
    uint64 numStolen = 0;
    uint64 numParked = 0;          // fiber waits that actually suspended
    uint64 numResumed = 0;
    uint64 numSleeps = 0;          // workers going through a kernel wait
    uint64 numInlineFallbacks = 0; // pool/queue exhaustion made a submit run on the calling thread
    uint64 numPreempted = 0;       // higher-priority jobs run inline at a pre-emption point
    uint64 busyNs = 0;             // summed measured job wall time across contexts
};

// ---- Inline bodies ----

inline JobCounter::~JobCounter()
{
    assert(isDone());
    [[maybe_unused]] FiberWaitNode* head = m_waiterHead.load(oc::memory_order_relaxed);
    assert(!head || head == takenSentinel());
}

inline void JobCounter::add(uint32 count)
{
    // a consumed wait list stays claimed until the counter is reused for a new batch
    if (m_waiterHead.load(oc::memory_order_relaxed) == takenSentinel())
        m_waiterHead.store(nullptr, oc::memory_order_relaxed);
    [[maybe_unused]] const uint32 old = m_count.fetch_add(count, oc::memory_order_relaxed);
    assert((old & CountMask) + count <= CountMask);
}

inline void JobCost::addSample(uint64 totalNs, uint32 numItems)
{
    if (numItems == 0)
        return;
    const uint64 sample = oc::max<uint64>(1, totalNs / numItems);
    const uint64 old = m_nsPerItem.load(oc::memory_order_relaxed);
    m_nsPerItem.store((old * 7 + sample) / 8, oc::memory_order_relaxed);
}
