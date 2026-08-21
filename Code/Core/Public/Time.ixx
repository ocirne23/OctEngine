export module Core.Time;

import <queue>;

import Core;
import Core.Window;

export class Timer;
export class Time
{
public:

    void update() { update(Clock::now()); }
    // `now` may be an ATTRIBUTED time rather than the wall clock: the frame limiter passes the
    // desired frame end when it waited, so tail jitter (OS scheduler lateness after the wait) is
    // charged to the next frame instead of producing a volatile delta - see main.cpp's pacing.
    void update(Clock::time_point now)
    {
        m_currentTime = now;
        m_deltaSec = std::chrono::duration<double>(m_currentTime - m_lastTime).count();
        m_elapsedSec = std::chrono::duration<double>(m_currentTime - m_startTime).count();
        m_lastTime = m_currentTime;
        processTimers();
    }

    // FRAME PACING ("Time" tweaks, registerTweaks()). One call
    // at the loop top does the whole frame boundary: the present-queue (fence) wait, the frame-rate
    // limit, the window thread's event-pump kick, and the start of the next frame's clock.
    //  * Limit: waits until the desired end (last frame start + 1/target; target = "Max FPS", or
    //    "Inactive max FPS" when unfocused, 0 = uncapped; never in VR - xrWaitFrame owns pacing),
    //    sleeping 1 ms steps while more than "Busy-wait window" remains (real milliseconds: the
    //    window thread sets timeBeginPeriod(1)) then spinning the last stretch.
    //  * Stable time: when it waited, the frame's end is ATTRIBUTED as the desired end rather than
    //    the clock after the wait - the OS's post-target lateness is charged to the NEXT frame as
    //    processing time, which keeps the delta flat instead of jittering with scheduler tails.
    //    Already late = the frame ends now. UNCAPPED + VSYNC (FIFO present) every frame lands on a
    //    whole number of refresh periods BY CONSTRUCTION - only the CPU's view of it jitters,
    //    because the throttle lands either in the loop-top fence or in the end-of-frame acquire -
    //    so the start is QUANTIZED to the nearest whole number of periods (min 1; a dropped frame
    //    is 2) using the display's reported refresh rate, or the measured period EMA when the
    //    platform reports none. Rounding against the real clock each frame bounds the drift to
    //    half a period. Without vsync intervals are arbitrary, so the raw clock is used.
    //  * Pump kick: requestPump() fires "Pump lead" BEFORE the frame starts, so the window thread
    //    pumps while we still wait and the events are at most a lead old when sampled. Capped, the
    //    end is exact and the limiter kicks inside its own wait; uncapped, the fence decides, so the
    //    kick targets the EARLIEST plausible unblock (raw last start + a decayed running MINIMUM of
    //    the raw interval - under FIFO the CPU unblocks early on alternate frames, and a mean-based
    //    prediction kicked too late on those), the fence is waited (bounded) until a lead before
    //    it, then kicked, then waited out (an earlier fence just ends the slice and kicks now).
    // waitFence(timeoutNs) = the frame-fence wait: true once signaled (or already waited), false on
    // timeout; UINT64_MAX blocks. May be null (no renderer). pumpWindow may be null. vsync = the
    // renderer presents FIFO; displayRefreshHz = the display's reported rate (0 = unknown).
    void registerTweaks();
    void beginFrame(bool windowFocused, bool vr, bool vsync, float displayRefreshHz, Window* pumpWindow, bool (*waitFence)(uint64 timeoutNs));

    double getDeltaSec() const { return m_deltaSec; }
    double getElapsedSec() const { return m_elapsedSec; }
    Clock::time_point getCurrentTime() const { return m_currentTime; }

private:

    friend class Timer;
    static void addTimer(Timer* pTimer);
    static void removeTimer(Timer* pTimer);
    static void processTimers();

private:

    void limitFrameRate(int targetFps); // capped: wait to the desired end + attributed update()
    void trackRawPeriod();              // period EMA + running min of the RAW frame interval (the pump predictor)

    Clock::time_point m_currentTime = Clock::now();
    double m_deltaSec = 0.0;
    double m_elapsedSec = 0.0;

    // Frame pacing (see beginFrame)
    int   m_maxFps = 0;            // 0 = uncapped
    int   m_inactiveMaxFps = 30;   // 0 = no extra cap when unfocused
    float m_busyWaitMs = 2.5f;
    bool  m_stableFrameTime = true;
    float m_pumpLeadMs = 2.0f;
    double m_framePeriodSec = 1.0 / 60.0;    // EMA of RAW frame starts (seed only: converges in ~20 frames) - the snap grid when the display reports no refresh rate
    double m_minFramePeriodSec = 1.0 / 60.0; // decayed running MIN of the raw interval: the uncapped pump predictor targets the EARLIEST unblock
    Clock::time_point m_lastRawFrameStart = Clock::now();

    const Clock::time_point m_startTime = Clock::now();
    Clock::time_point m_lastTime = Clock::now();
};

export namespace Globals
{
    Time time;
}

export class Timer
{
public:

    enum OnTrigger
    {
        REPEAT,
        DONE
    };

    Timer(Clock::duration duration, oc::function<OnTrigger(Timer&)> onTimer = nullptr)
        : m_onTimer(onTimer)
    {
        m_startTime = Globals::time.getCurrentTime();
        m_endTime = m_startTime + duration;
        assert(duration >= std::chrono::milliseconds(1) && "Timer duration < 1ms");
        Globals::time.addTimer(this);
    }

    ~Timer() { if (!m_hasTriggered) Globals::time.removeTimer(this); }

    Timer(const Timer&) = delete;
    Timer(const Timer&&) = delete;

    bool operator<(const Timer& other) const { return m_endTime < other.m_endTime; }
    bool operator>(const Timer& other) const { return m_endTime > other.m_endTime; }

    double getElapsedSec() const
    {
        return std::chrono::duration<double>(Globals::time.getCurrentTime() - m_startTime).count();
    }

    double getRemainingSec() const
    {
        return std::chrono::duration<double>(m_endTime - Globals::time.getCurrentTime()).count();
    }

    bool hasTriggered() const { return m_hasTriggered; }

    void reset(Clock::duration duration)
    {
        if (!m_hasTriggered) Globals::time.removeTimer(this);
        m_startTime = Globals::time.getCurrentTime();
        m_endTime = m_startTime + duration;
        Globals::time.addTimer(this);
    }

    Clock::time_point getStartTime() const { return m_startTime; }
    Clock::time_point getEndTime() const { return m_endTime; }

private:

    friend class Time;
    Clock::time_point m_startTime;
    Clock::time_point m_endTime;
    oc::function<OnTrigger(Timer&)> m_onTimer;
    bool m_hasTriggered = false;
};