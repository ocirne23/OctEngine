export module Threading:Job;

import Core;

import :JobTypes;

// The scheduler's job record: pooled submits and graph nodes alike.

export using JobFunc = void(*)(void*);

export constexpr uint8 EJobFlag_Pooled = 1; // owned by the JobSystem's ad-hoc pool, recycled after execution (beside the public EJobFlags)

// Two cache lines, callable stored inline. Graph jobs carry successor edges; pooled jobs have none.
export struct alignas(64) Job
{
    JobFunc invoke = nullptr;
    JobFunc destroy = nullptr;          // only set for non-trivially-destructible captures
    JobCounter* signal = nullptr;
    Job* const* successors = nullptr;
    uint32 numSuccessors = 0;
    oc::atomic<int32> pending = 0;      // unsatisfied predecessors; last one to decrement pushes us
    uint32 initialPending = 0;          // reset value for re-running graphs
    uint32 costEmaUs = 0;               // EMA of measured wall time (min 1us per run)
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
