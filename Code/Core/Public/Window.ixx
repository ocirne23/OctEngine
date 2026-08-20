export module Core.Window;

import Core;
import Core.glm;
import Core.SDL;

export import Core.fwd;

// The WINDOW THREAD. Win32 fixes an HWND's message queue to the thread that CREATES it (no
// transfer API exists), and SDL3's "main thread" is the one that calls SDL_Init(SDL_INIT_VIDEO) -
// so initialize() spawns a dedicated thread that initializes SDL, creates the window, and OWNS the
// message pump for the process lifetime. WndProc dispatch (raw input, cursor, IME, and Windows'
// modal drag/resize loops - which used to freeze the whole engine) never touches the engine's main
// thread again.
//
// Per frame the ENGINE main thread calls requestPump() (loop top, before the frame fence wait);
// the window thread pumps + drains every SDL event into a buffer takeEvents() hands back after
// waitPumpDone(). Between pumps the thread is NOT spinning on the OS queue: it runs queued window
// ops, then HELPS THE JOB SYSTEM (the idle-work hook - App wires jobSystem.tryRunOneHighJob, High
// only: Normal/Low carry multi-second jobs that would stall the next frame's pump) until the next
// request parks it on the epoch atomic.
//
// Window-affine SDL calls from other threads go through runOnWindowThread() (executed at the next
// pump; wait=true blocks - init-time only: Vulkan surface creation, the ImGui SDL backend init).
// Cross-thread SDL READS (getWindowSize, GetKeyboardState) stay direct - cached-state reads.
export class Window
{
public:

    Window() = default;
    ~Window(); // stops + joins the thread (the window is destroyed ON it)
    Window(const Window&) = delete;
    Window(const Window&&) = delete;
    Window& operator=(const Window&) = delete;
    Window& operator=(const Window&&) = delete;

    bool initialize(oc::string_view windowTitle, glm::ivec2 pos, glm::ivec2 size); // spawns the thread, blocks until the window exists

    // -- engine main thread, once per frame --
    void requestPump();                          // kick a pump; the fence wait runs while it happens
    void waitPumpDone();                         // events for this frame are in the buffer after this
    // Direct access to the pumped event buffer, bracketed by the lock (the window thread appends
    // during a pump). Clear it before unlocking once processed - lockEvents() does not drain.
    oc::vector<SDL_Event>& lockEvents() { m_eventMutex.lock(); return m_events; }
    void unlockEvents() { m_eventMutex.unlock(); }

    // Marshal a window-affine SDL call onto the window thread. Runs at the next pump (async), or
    // immediately woken + waited when wait = true (init-time only - a wait can stall behind a
    // helped job, so never call it per frame).
    void runOnWindowThread(oc::function<void()> op, bool wait = false);

    // Idle behaviour between pumps (App wires the job system's High-priority helper):
    // work() runs one job (false = ring empty), wait() parks until work MIGHT exist (the job
    // system's helper eventcount), wake() unparks it (called from requestPump on MAIN, so a pump
    // request always interrupts the nap - it is stored main-side and never raced). work/wait are
    // installed via the op queue so they only ever run on the window thread.
    void setIdleWork(oc::function<bool()> work, oc::function<void()> wait, oc::function<void()> wake);

    void* getWindowHandle() const { return m_windowHandle; }
    void setTitle(oc::string_view title); // queued op
    void getWindowSize(glm::ivec2& size) const;

private:

    void threadMain(oc::string title, glm::ivec2 size);
    void servePump();

    void* m_windowHandle = nullptr;
    std::thread m_thread;
    oc::atomic<uint32> m_pumpRequested = 0; // epochs: requested > served = a pump is due
    oc::atomic<uint32> m_pumpServed = 0;
    oc::atomic<bool> m_running = false;
    oc::atomic<bool> m_ready = false;       // window created (or failed); initialize() waits on it
    std::mutex m_eventMutex;
    oc::vector<SDL_Event> m_events;         // pumped events, drained by takeEvents
    std::mutex m_opMutex;
    oc::vector<oc::function<void()>> m_ops; // marshaled window ops, run at pump time
    oc::atomic<uint32> m_opsDone = 0;       // epoch for runOnWindowThread(wait = true)
    oc::function<bool()> m_idleWork;        // window-thread-only (installed via the op queue)
    oc::function<void()> m_idleWait;        // window-thread-only: park until work might exist
    oc::function<void()> m_wakeIdle;        // MAIN-thread-only: requestPump interrupts the park
};
