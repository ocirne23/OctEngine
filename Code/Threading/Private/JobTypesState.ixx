export module Threading:JobTypesState;

import Core;

export class JobSystemState; // declared before their friend lines
export class JobSystem;
struct Fiber;

// On the waiting fiber's stack: the signaler must read `next` before making `fiber` resumable.
export struct FiberWaitNode
{
    Fiber* fiber = nullptr;
    FiberWaitNode* next = nullptr;
};

// Everything of JobCounter but its public API (JobTypes.ixx). The scheduler reads and writes it directly.
export class JobCounterState
{
protected:

    friend class JobSystemState;
    friend class JobSystem;

    static constexpr uint32 CountMask = 0x00ffffff; // low 24 bits: outstanding jobs
    static constexpr uint32 WaiterInc = 0x01000000; // high 8 bits: registered fiber waiters

    static FiberWaitNode* takenSentinel()
    {
        static FiberWaitNode sentinel;
        return &sentinel;
    }

    oc::atomic<uint32> m_count = 0;
    oc::atomic<FiberWaitNode*> m_waiterHead = nullptr;
    const char* m_label = nullptr; // the last submitted job's profile name: wait() names its stall scope after it
};

// Everything of JobCost but its public API (JobTypes.ixx).
export class JobCostState
{
protected:

    explicit JobCostState(uint32 initialNsPerItem) : m_nsPerItem(oc::max(1u, initialNsPerItem)) {}

    oc::atomic<uint64> m_nsPerItem;
};
