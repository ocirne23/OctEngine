export module Threading:Types;

import Core;

export class JobSystem;
struct Fiber;

export enum class EJobPriority : uint8
{
    High = 0,
    Normal = 1,
    Low = 2,
};
export constexpr uint32 NumJobPriorities = 3;

// Intrusive wait-list node. Lives on the waiting fiber's stack, so it stays valid exactly as long
// as the fiber is parked; the signaler must read `next` before making `fiber` resumable.
export struct FiberWaitNode
{
    Fiber* fiber = nullptr;
    FiberWaitNode* next = nullptr;
};

// Completion counter jobs decrement when they finish. The decrement hot path is a single
// fetch_sub. A waiter may free the counter the instant it observes completion, so the whole
// design revolves around the zero transition never touching counter memory after the value
// observably reaches zero: parked-fiber registrations live in the HIGH BITS of the count atomic
// (the zero transition sees them in the same fetch_sub, and outstanding registrations keep the
// full value nonzero until their owner - the transition or a backing-out waiter - removes them),
// isDone() is "the FULL value is zero", the wait list is a lock-free push stack whose head the
// transition claims with ONE exchange (a taken sentinel tells late waiters to back out), and the
// only post-zero operation is an address-only notify (WaitOnAddress keys, no deref). Must
// outlive every job signaling it and every wait on it (stack lifetime is fine when the owner
// waits before returning, e.g. parallelFor); re-add() only after the previous batch's waits
// returned.
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

export using JobFunc = void(*)(void*);

export enum EJobFlags : uint8
{
    EJobFlag_Pooled = 1,  // owned by the JobSystem's ad-hoc pool, recycled after execution
    EJobFlag_Untimed = 2, // OPT-OUT of wall-time measurement (costEmaUs + worker busy-time stats).
                          // Timing is on by default; clear it only for nanosecond-scale spam jobs
                          // where two clock reads (~40-60ns) would dominate the job itself
};

// One schedulable unit: two cache lines, callable stored inline (no heap). Graph jobs live in
// their JobGraph and carry successor edges + a reset value for the dependency counter; ad-hoc
// jobs come from the JobSystem pool with no edges.
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

// Identity of a piece of data jobs declare read/write access on. Just a process-unique id;
// what it refers to is by convention (a system, a buffer, a component array). Create once at
// init next to the data it names and keep it alive (non-copyable, like the Tweak pattern).
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

// Live per-item cost estimate for a parallelFor call site. Create one next to the loop it
// measures (static/member, like a Tweak registration) and pass it instead of a grain size: the
// chunk size is derived from the measured ns/item to hit JobSystemDesc::parallelForTargetChunkNs,
// and every run's runner timings feed back into the estimate. initialNsPerItem seeds the first
// run. Lossy racy EMA on purpose - it is a heuristic, not a measurement.
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

export struct JobSystemDesc
{
    uint32 numWorkers = 0;                  // 0 = hardware threads - 2 (main + headroom), min 1
    uint32 numFibers = 128;                 // max concurrently started-but-unfinished jobs (parked waits hold one)
    uint32 fiberStackCommit = 64 * 1024;
    uint32 fiberStackReserve = 512 * 1024;
    uint32 jobPoolCapacity = 16384;         // ad-hoc jobs in flight (rounded up to a power of two)
    uint32 queueCapacity = 16384;           // per priority class (rounded up to a power of two)
    uint32 dequeCapacity = 4096;            // per worker (rounded up to a power of two)
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
    uint64 busyNs = 0;       // summed measured job wall time across contexts (timed jobs)
};
