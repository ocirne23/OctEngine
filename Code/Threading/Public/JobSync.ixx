export module Threading:JobSync;

import Core;

import :JobTypes;
import :JobSystem;
import :JobSyncState; // the state

// The synchronization types job code uses: the fiber-parking mutex, the manual-reset event, the per-context slots and the thread-local pin. The state is
// in JobSyncState.ixx; the multi-line bodies are below the classes.

// Fiber-parking mutex: a contended lock() in a job parks the fiber, not the worker. Barging, not fair.
export class JobMutex final : public JobMutexState
{
public:

    JobMutex() = default;
    ~JobMutex() { assert(m_state.load(oc::memory_order_relaxed) == 0 && !m_waiters); }

    bool tryLock();
    void lock()
    {
        if (!tryLock())
            Globals::jobSystem.lockJobMutexSlow(*this);
    }
    void unlock();

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

    JobMutex(const JobMutex&) = delete;
    JobMutex& operator=(const JobMutex&) = delete;
};

// Manual-reset event: signal() releases every waiter until reset().
export class JobEvent final : public JobEventState
{
public:

    JobEvent() { m_counter.add(1); }
    ~JobEvent();

    void wait() { Globals::jobSystem.wait(m_counter); }
    void signal();
    bool isSignaled() const { return m_signaled.load(oc::memory_order_acquire); }
    void reset(); // only between batches: no concurrent wait/signal

    JobEvent(const JobEvent&) = delete;
    JobEvent& operator=(const JobEvent&) = delete;
};

// One cacheline-aligned T per scheduler context, drained serially with forEach. Re-read local() after any wait().
export template<typename T>
class PerWorker final : public PerWorkerState<T>
{
public:

    void initialize();
    bool isInitialized() const { return this->m_numSlots != 0; }
    uint32 size() const { return this->m_numSlots; }

    T& local() { return this->m_slots[Globals::jobSystem.getWorkerIndex()].value; }
    T& at(uint32 index) { return this->m_slots[index].value; }
    const T& at(uint32 index) const { return this->m_slots[index].value; }

    template<typename Func>
    void forEach(Func&& func)
    {
        for (uint32 i = 0; i < this->m_numSlots; ++i)
            func(this->m_slots[i].value);
    }
};

// Debug pin on the current thread's TLS (a thread_local, a PerWorker::local() slot): while alive, a fiber park or an inline job on this thread asserts.
// Nests; no-op in non-debug.
export class ThreadLocalScope final
{
public:

    ThreadLocalScope() { JobSystemState::debugThreadLocalPin(+1); }
    ~ThreadLocalScope() { JobSystemState::debugThreadLocalPin(-1); }

    ThreadLocalScope(const ThreadLocalScope&) = delete;
    ThreadLocalScope& operator=(const ThreadLocalScope&) = delete;
};

// ---- Inline bodies ----

inline bool JobMutex::tryLock()
{
    uint32 state = m_state.load(oc::memory_order_relaxed);
    return !(state & LockedBit) && m_state.compare_exchange_strong(state, state | LockedBit, oc::memory_order_acquire, oc::memory_order_relaxed);
}

inline void JobMutex::unlock()
{
    // seq_cst pairs with the blocked-thread announce in the lock slow path (Dekker): either we see the blocked count here, or the blocker sees the cleared
    // lock bit before waiting
    const uint32 old = m_state.fetch_and(~LockedBit, oc::memory_order_seq_cst);
    assert(old & LockedBit);
    if (old & WaitersBit)
        Globals::jobSystem.unlockJobMutexSlow(*this);
    else if (m_numBlockedThreads.load(oc::memory_order_seq_cst) != 0)
        m_state.notify_all();
}

inline JobEvent::~JobEvent()
{
    if (!isSignaled()) // no waiters can legally exist here; settle the counter for its dtor assert
        Globals::jobSystem.signalCounter(m_counter);
}

inline void JobEvent::signal()
{
    if (!m_signaled.exchange(true, oc::memory_order_acq_rel))
        Globals::jobSystem.signalCounter(m_counter);
}

inline void JobEvent::reset()
{
    assert(isSignaled());
    m_signaled.store(false, oc::memory_order_relaxed);
    m_counter.add(1);
}

template<typename T>
void PerWorker<T>::initialize()
{
    this->m_numSlots = Globals::jobSystem.getNumContexts();
    assert(this->m_numSlots != 0 && "initialize the JobSystem first");
    this->m_slots = oc::make_unique<typename PerWorkerState<T>::Slot[]>(this->m_numSlots);
}
