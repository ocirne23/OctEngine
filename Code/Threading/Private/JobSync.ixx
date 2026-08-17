export module Threading:JobSync;

import Core;
import :Types;
import :JobSystem;

// Fiber-parking mutex: contended lock() inside a job parks the fiber (the worker keeps running
// other jobs) instead of blocking the thread; the registered main thread helps run jobs while
// waiting; unregistered threads block on the state atomic. Barging (no FIFO handoff): unlock
// wakes one waiter which re-races fresh lockers - simpler and higher throughput; don't use it
// where fairness matters. Standard mutex lifetime contract: never destroy while anyone can be
// locking or waiting. For long exclusive sections (V3 tile inference) this is what keeps pooled
// workers from starving - the ~seconds-long waits hold a fiber, not a worker thread.
export class JobMutex final
{
public:

    JobMutex() = default;
    JobMutex(const JobMutex&) = delete;
    JobMutex& operator=(const JobMutex&) = delete;
    ~JobMutex() { assert(m_state.load(oc::memory_order_relaxed) == 0 && !m_waiters); }

    bool tryLock()
    {
        uint32 state = m_state.load(oc::memory_order_relaxed);
        return !(state & LockedBit) && m_state.compare_exchange_strong(state, state | LockedBit, oc::memory_order_acquire, oc::memory_order_relaxed);
    }

    void lock()
    {
        if (!tryLock())
            Globals::jobSystem.lockJobMutexSlow(*this);
    }

    void unlock()
    {
        // seq_cst pairs with the blocked-thread announce in the lock slow path (Dekker): either
        // we see the blocked count here, or the blocker sees the cleared lock bit before waiting
        const uint32 old = m_state.fetch_and(~LockedBit, oc::memory_order_seq_cst);
        assert(old & LockedBit);
        if (old & WaitersBit)
            Globals::jobSystem.unlockJobMutexSlow(*this);
        else if (m_numBlockedThreads.load(oc::memory_order_seq_cst) != 0)
            m_state.notify_all();
    }

    // RAII guard, usable exactly like std::lock_guard
    class Scope final
    {
    public:
        explicit Scope(JobMutex& mutex) : m_mutex(mutex) { m_mutex.lock(); }
        ~Scope() { m_mutex.unlock(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        JobMutex& m_mutex;
    };

private:

    friend class JobSystem;
    static constexpr uint32 LockedBit = 1;
    static constexpr uint32 WaitersBit = 2; // parked fibers present; set/cleared under m_listLock

    oc::atomic<uint32> m_state = 0;
    oc::atomic<uint32> m_listLock = 0;
    oc::atomic<uint32> m_numBlockedThreads = 0; // unregistered threads blocked on m_state
    FiberWaitNode* m_waiters = nullptr;          // guarded by m_listLock
};

// Manual-reset event over a JobCounter: signal() releases all current and future waiters until
// reset(). Idempotent signal; reset() only when no waits/signals are in flight. Waiting inside a
// job parks the fiber. The building block for "wait for that tile/bake/upload to exist" handoffs.
export class JobEvent final
{
public:

    JobEvent() { m_counter.add(1); }
    JobEvent(const JobEvent&) = delete;
    JobEvent& operator=(const JobEvent&) = delete;
    ~JobEvent()
    {
        if (!isSignaled()) // no waiters can legally exist here; settle the counter for its dtor assert
            Globals::jobSystem.signal(m_counter);
    }

    void wait() { Globals::jobSystem.wait(m_counter); }
    void signal()
    {
        if (!m_signaled.exchange(true, oc::memory_order_acq_rel))
            Globals::jobSystem.signal(m_counter);
    }
    bool isSignaled() const { return m_signaled.load(oc::memory_order_acquire); }
    void reset() // only between batches: no concurrent wait/signal
    {
        assert(isSignaled());
        m_signaled.store(false, oc::memory_order_relaxed);
        m_counter.add(1);
    }

private:

    JobCounter m_counter;
    oc::atomic<bool> m_signaled = false;
};

// One cacheline-aligned T per scheduler context (main helper + workers): the substrate for
// per-worker command queues, dirty lists, and scratch. Each slot is single-writer within a
// parallel phase; a downstream serial node drains them with forEach. Re-read local() after any
// wait() - fibers migrate workers.
export template<typename T>
class PerWorker final
{
public:

    void initialize()
    {
        m_numSlots = Globals::jobSystem.getNumContexts();
        assert(m_numSlots != 0 && "initialize the JobSystem first");
        m_slots = oc::make_unique<Slot[]>(m_numSlots);
    }

    bool isInitialized() const { return m_numSlots != 0; }
    uint32 size() const { return m_numSlots; }

    T& local() { return m_slots[Globals::jobSystem.getWorkerIndex()].value; }
    T& at(uint32 index) { return m_slots[index].value; }
    const T& at(uint32 index) const { return m_slots[index].value; }

    template<typename Func>
    void forEach(Func&& func)
    {
        for (uint32 i = 0; i < m_numSlots; ++i)
            func(m_slots[i].value);
    }

private:

    struct alignas(64) Slot
    {
        T value{};
    };

    oc::unique_ptr<Slot[]> m_slots;
    uint32 m_numSlots = 0;
};
