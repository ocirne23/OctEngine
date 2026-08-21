module;

#include <intrin.h> // _mm_pause for waitPumpDone's spin

module Core.Window;

import Core;
import Core.SDL;
import Core.Profiler;
import Core.Windows; // timeBeginPeriod (real 1 ms sleeps for the frame limiter)

Window::~Window()
{
    if (!m_running.load(oc::memory_order_relaxed))
        return;
    m_running.store(false, oc::memory_order_release);
    requestPump(); // wakes every park site (epoch, job-system helper wait)
    m_thread.join();
    m_windowHandle = nullptr;
}

bool Window::initialize(oc::string_view windowTitle, glm::ivec2 pos, glm::ivec2 size)
{
    (void)pos;
    m_running.store(true, oc::memory_order_relaxed);
    m_thread = std::thread(&Window::threadMain, this, oc::string(windowTitle), size);
    m_ready.wait(false, oc::memory_order_acquire); // window created (or creation failed)
    return m_windowHandle != nullptr;
}

void Window::threadMain(oc::string title, glm::ivec2 size)
{
    // ProfileScopes run here (the pump marker, helped jobs), so register first thing.
    Globals::profiler.registerThread("Window", Profiler::SORT_KEY_BACKGROUND);
    timeBeginPeriod(1); // process-wide: the frame limiter's Sleep(1) steps (Time::limitFrameRate) are real milliseconds

    // SDL3's "main thread" is the one that calls SDL_Init(SDL_INIT_VIDEO): initializing HERE makes
    // this thread the legal owner of the pump, the window, and every window-affine SDL call.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        printf("SDL_Init Error: %s\n", SDL_GetError());
    m_windowHandle = SDL_CreateWindow(title.c_str(), size.x, size.y, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (m_windowHandle == nullptr)
        printf("SDL_CreateWindow Error: %s\n", SDL_GetError());
    else
    {
        SDL_SetWindowMinimumSize((SDL_Window*)m_windowHandle, 480, 360);
        queryDisplayRefresh();
    }
    m_ready.store(true, oc::memory_order_release);
    m_ready.notify_all();

    while (m_running.load(oc::memory_order_acquire))
    {
        if (int32(m_pumpRequested.load(oc::memory_order_acquire) - m_pumpServed.load(oc::memory_order_relaxed)) > 0)
        {
            servePump();
            continue;
        }
        // Between pumps: help the job system. High-priority only (see the idle-work wiring) - a
        // long job here would delay the NEXT frame's pump, which is exactly the stall this thread
        // exists to remove.
        if (m_idleWork && m_idleWork())
            continue; // ran a job; a pump request may have arrived meanwhile
        if (m_idleWait)
        {
            // Parks in the job system's helper eventcount: woken by High submissions (physics
            // fan-outs, spatial stamps) AND by requestPump via the wake hook.
            m_idleWait();
            continue;
        }
        // No hooks wired yet (pre-jobSystem init): park on the pump epoch alone.
        const uint32 seen = m_pumpRequested.load(oc::memory_order_acquire);
        if (int32(seen - m_pumpServed.load(oc::memory_order_relaxed)) > 0)
            continue;
        m_pumpRequested.wait(seen, oc::memory_order_acquire);
    }

    if (m_windowHandle != nullptr)
        SDL_DestroyWindow((SDL_Window*)m_windowHandle); // destroyed on its owning thread
    timeEndPeriod(1);
}

void Window::servePump()
{
    // One serve can cover SEVERAL queued requests (init-time blocking ops bump the epoch too), so
    // the served epoch SNAPS to the request epoch observed here - a +1 would fall behind forever
    // and deadlock waitPumpDone.
    const uint32 target = m_pumpRequested.load(oc::memory_order_acquire);
    // Marshaled window ops first (surface creation, backend init, title) - they may need to exist
    // before the events they cause.
    {
        oc::vector<oc::function<void()>> ops;
        {
            const std::lock_guard<std::mutex> lock(m_opMutex);
            ops.swap(m_ops);
        }
        for (const oc::function<void()>& op : ops)
            op();
        if (!ops.empty())
        {
            m_opsDone.fetch_add(uint32(ops.size()), oc::memory_order_release);
            m_opsDone.notify_all();
        }
    }
    {
        // The WndProc dispatch cost, on ITS thread: raw input, cursor, IME, drag/resize loops.
        ProfileScope pumpScope("Event pump", EProfileCategory::Input);
        SDL_PumpEvents();
        SDL_Event evt;
        const std::lock_guard<std::mutex> lock(m_eventMutex);
        while (SDL_PollEvent(&evt))
        {
            if (evt.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || evt.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
                queryDisplayRefresh(); // moved to another monitor / refresh rate changed
            m_events.push_back(evt);
        }
    }
    m_pumpServed.store(target, oc::memory_order_release); // no notify: waitPumpDone spins (main's critical path)
}

void Window::queryDisplayRefresh()
{
    float hz = 0.0f;
    if (const SDL_DisplayID display = SDL_GetDisplayForWindow((SDL_Window*)m_windowHandle))
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display))
            hz = mode->refresh_rate; // 0 when the platform does not report it
    m_displayRefreshHz.store(hz, oc::memory_order_relaxed);
}

void Window::requestPump()
{
    m_pumpRequested.fetch_add(1, oc::memory_order_release);
    m_pumpRequested.notify_all();
    if (m_wakeIdle)
        m_wakeIdle(); // the thread may be napping in the job system's helper wait
}

void Window::waitPumpDone()
{
    // BUSY wait, deliberately: this sits on the main thread's critical path at frame start, and the
    // pump is normally already done or within tens of microseconds of it - a kernel wait
    // (WaitOnAddress) would add a wake-up latency of the same order as the thing being waited for.
    const uint32 target = m_pumpRequested.load(oc::memory_order_acquire);
    while (int32(target - m_pumpServed.load(oc::memory_order_acquire)) > 0) // epoch compare, wrap-safe
        _mm_pause();
}

void Window::runOnWindowThread(oc::function<void()> op, bool wait)
{
    uint32 doneTarget;
    {
        const std::lock_guard<std::mutex> lock(m_opMutex);
        m_ops.push_back(oc::move(op));
        doneTarget = m_opsDone.load(oc::memory_order_relaxed) + uint32(m_ops.size());
    }
    if (!wait)
        return; // runs at the next pump
    requestPump(); // wake the thread so a blocking op never waits a whole frame
    uint32 done = m_opsDone.load(oc::memory_order_acquire);
    while (int32(doneTarget - done) > 0)
    {
        m_opsDone.wait(done, oc::memory_order_acquire);
        done = m_opsDone.load(oc::memory_order_acquire);
    }
}

void Window::setIdleWork(oc::function<bool()> work, oc::function<void()> wait, oc::function<void()> wake)
{
    m_wakeIdle = oc::move(wake); // main-side: requestPump (main) is its only reader
    // work/wait are installed on the window thread itself so they are never raced.
    runOnWindowThread([this, w = oc::move(work), p = oc::move(wait)]() mutable
    {
        m_idleWork = oc::move(w);
        m_idleWait = oc::move(p);
    }, true);
}

void Window::setTitle(oc::string_view title)
{
    runOnWindowThread([this, t = oc::string(title)] { SDL_SetWindowTitle((SDL_Window*)m_windowHandle, t.c_str()); });
}

void Window::getWindowSize(glm::ivec2& size) const
{
    SDL_GetWindowSizeInPixels((SDL_Window*)m_windowHandle, &size.x, &size.y); // cached-state read, cross-thread tolerated
}
