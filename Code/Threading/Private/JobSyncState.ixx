export module Threading:JobSyncState;

import Core;

import :JobTypes;

export class JobSystemState; // declared before their friend lines

// Everything of JobMutex but its public API (JobSync.ixx). The scheduler's lock / unlock slow paths read and write it directly.
export class JobMutexState
{
protected:

    friend class JobSystemState;

    static constexpr uint32 LockedBit = 1;
    static constexpr uint32 WaitersBit = 2; // parked fibers present; set/cleared under m_listLock

    oc::atomic<uint32> m_state = 0;
    oc::atomic<uint32> m_listLock = 0;
    oc::atomic<uint32> m_numBlockedThreads = 0; // unregistered threads blocked on m_state
    FiberWaitNode* m_waiters = nullptr;          // guarded by m_listLock
};

// Everything of JobEvent but its public API (JobSync.ixx).
export class JobEventState
{
protected:

    JobCounter m_counter;
    oc::atomic<bool> m_signaled = false;
};

// Everything of PerWorker but its public API (JobSync.ixx).
export template<typename T>
class PerWorkerState
{
protected:

    struct alignas(64) Slot
    {
        T value{};
    };

    oc::unique_ptr<Slot[]> m_slots;
    uint32 m_numSlots = 0;
};
