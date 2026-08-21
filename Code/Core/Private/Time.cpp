module;

#include <intrin.h> // _mm_pause for the limiter's final spin

module Core.Time;

import <queue>;

import Core;
import Core.Windows;
import Core.Window;
import Core.Tweaks;

static Clock::duration secondsToDuration(double sec)
{
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sec));
}

void Time::registerTweaks()
{
    Tweak::intVar("Time", "Max FPS", &m_maxFps, 0, 1000, 1.0f, {}, ETweakFlags::Saved);
    Tweak::intVar("Time", "Inactive max FPS", &m_inactiveMaxFps, 0, 240, 1.0f, {}, ETweakFlags::Saved);
    Tweak::floatVar("Time", "Busy-wait window (ms)", &m_busyWaitMs, 0.0f, 8.0f, 0.1f, {}, ETweakFlags::Saved);
    Tweak::boolean("Time", "Stable frame time", &m_stableFrameTime, {}, ETweakFlags::Saved);
    Tweak::floatVar("Time", "Input pump lead (ms)", &m_pumpLeadMs, 0.0f, 8.0f, 0.1f, {}, ETweakFlags::Saved);
}

void Time::beginFrame(bool windowFocused, bool vr, bool vsync, float displayRefreshHz, Window* pumpWindow, bool (*waitFence)(uint64))
{
    const Clock::time_point lastFrameStart = m_currentTime;
    const int targetFps = vr ? 0 : (windowFocused || m_inactiveMaxFps <= 0 ? m_maxFps : m_inactiveMaxFps);
    const bool capped = targetFps > 0;

    // The frame starts at whichever comes LAST: the limiter's desired end (capped) or the fence signal. Kick the pump a lead before the earliest of the two we can predict — the desired end is exact, the fence is predicted as raw last start + running MIN interval (FIFO unblocks early on alternate frames)
    const Clock::time_point desiredFrameEnd = capped ? lastFrameStart + secondsToDuration(1.0 / targetFps) : Clock::time_point::max();
    const Clock::time_point predictedFence = m_lastRawFrameStart + secondsToDuration(m_minFramePeriodSec);
    const Clock::time_point pumpAt = oc::min(desiredFrameEnd, predictedFence) - secondsToDuration(m_pumpLeadMs * 0.001);
    {
        // Sleep/spin to pumpAt OURSELVES, polling the fence (timeout 0): a driver's finite fence timeout may run "substantially longer than requested" (spec) — NVIDIA's ran to the signal and the kick landed at frame start
        ProfileScope scope("Pump kick wait", EProfileCategory::Wait);
        const Clock::duration spinWindow = secondsToDuration(0.0015);
        for (Clock::time_point now = Clock::now(); now < pumpAt; now = Clock::now())
        {
            if (!capped && waitFence && waitFence(0))
                break; // uncapped: the frame starts the moment the fence signals, so an early signal means kick now
            if (pumpAt - now > spinWindow)
                Sleep(1);
            else
                _mm_pause();
        }
    }
    if (pumpWindow)
        pumpWindow->requestPump();
    if (waitFence)
        waitFence(UINT64_MAX); // present-queue / GPU slot: nothing proceeds without it

    if (capped)
    {
        limitFrameRate(targetFps); // waits out the rest to the desired end; attributes it when it waited
        return;
    }
    // Stable time uncapped: under FIFO frames land on whole refresh periods (only the CPU's view jitters), so snap the start to the nearest multiple (min 1) of the display period; no vsync = raw clock
    const Clock::time_point signaled = Clock::now();
    Clock::time_point frameStart = signaled;
    const double vsyncPeriod = displayRefreshHz > 0.0f ? 1.0 / double(displayRefreshHz) : m_framePeriodSec;
    if (m_stableFrameTime && vsync && vsyncPeriod > 0.0)
    {
        const double intervals = std::chrono::duration<double>(signaled - lastFrameStart).count() / vsyncPeriod;
        const double rounded = glm::max(1.0, std::round(intervals));
        frameStart = lastFrameStart + secondsToDuration(rounded * vsyncPeriod);
    }
    update(frameStart);
    trackRawPeriod();
}

void Time::trackRawPeriod()
{
    // Period estimates track the RAW clock (snapped starts would only confirm themselves)
    const Clock::time_point rawNow = Clock::now();
    const double rawPeriod = std::chrono::duration<double>(rawNow - m_lastRawFrameStart).count();
    if (rawPeriod > 0.0 && rawPeriod < 0.25)
    {
        m_framePeriodSec = m_framePeriodSec * 0.9 + rawPeriod * 0.1;
        // running MIN, relaxing ~2%/frame so a pace change is followed within a few dozen frames
        m_minFramePeriodSec = glm::min(rawPeriod, m_minFramePeriodSec * 1.02);
    }
    m_lastRawFrameStart = rawNow;
}

void Time::limitFrameRate(int targetFps)
{
    // m_currentTime is the attributed start of the frame that is ending (set by the previous call).
    const Clock::time_point desiredFrameEnd = m_currentTime + secondsToDuration(1.0 / targetFps);
    Clock::time_point frameEnd = Clock::now();
    if (frameEnd < desiredFrameEnd)
    {
        ProfileScope scope("Frame limit", EProfileCategory::Wait);
        const Clock::duration busyWindow = secondsToDuration(m_busyWaitMs * 0.001);
        while (Clock::now() + busyWindow < desiredFrameEnd)
            Sleep(1);
        while (Clock::now() < desiredFrameEnd)
            _mm_pause();
        frameEnd = m_stableFrameTime ? desiredFrameEnd : Clock::now();
    }
    update(frameEnd);
    trackRawPeriod();
}

struct TimerCompare
{
    bool operator()(const Timer* lhs, const Timer* rhs) const
    {
        return *lhs > *rhs;
    }
};

static oc::unordered_map<uint64, oc::vector<Timer*>> m_timerBuckets;
static uint64 m_lastCheckedBucket = 0;

void Time::addTimer(Timer* pTimer)
{
    ProfileScope scope("Timer::addTimer", EProfileCategory::Core);
    const auto currentTime = Globals::time.getCurrentTime();
    if (pTimer->m_endTime <= currentTime)
    {
        if (pTimer->m_onTimer)
        {
            if (pTimer->m_onTimer(*pTimer) == Timer::REPEAT)
            {
                pTimer->m_endTime = currentTime + (pTimer->m_endTime - pTimer->m_startTime);
                pTimer->m_startTime = currentTime;
                assert(pTimer->m_startTime != pTimer->m_endTime);
                addTimer(pTimer);
                return;
            }
        }
        pTimer->m_hasTriggered = true;
        return;
    }

    const uint64 bucket = (uint64)std::chrono::duration_cast<std::chrono::seconds>(pTimer->m_endTime.time_since_epoch()).count();
    if (m_lastCheckedBucket == 0)
        m_lastCheckedBucket = bucket - 1;

    m_timerBuckets[bucket].push_back(pTimer);
    std::push_heap(m_timerBuckets[bucket].begin(), m_timerBuckets[bucket].end(), TimerCompare());
    pTimer->m_hasTriggered = false;
}

void Time::removeTimer(Timer* pTimer) 
{
    ProfileScope scope("Timer::removeTimer", EProfileCategory::Core);
    const uint64 bucket = (uint64)std::chrono::duration_cast<std::chrono::seconds>(pTimer->m_endTime.time_since_epoch()).count();
    auto it = m_timerBuckets.find(bucket);
    if (it != m_timerBuckets.end())
    {
        oc::vector<Timer*>& timers = it->second; // REFERENCE: a copy here silently discarded the removal
        // swap and pop
        auto itTimer = oc::find(timers.begin(), timers.end(), pTimer);
        if (itTimer != timers.end())
        {
            oc::swap(*itTimer, timers.back());
            timers.pop_back();
            std::make_heap(timers.begin(), timers.end(), TimerCompare());
            if (timers.empty())
            {
                m_timerBuckets.erase(it);
            }
        }
    }
}

void Time::processTimers()
{
    ProfileScope scope("Timer::processTimers", EProfileCategory::Core);

    const auto currentTime = Globals::time.getCurrentTime();
    const uint64 currentBucket = (uint64)std::chrono::duration_cast<std::chrono::seconds>(currentTime.time_since_epoch()).count();
    for (uint64 bucket = m_lastCheckedBucket; bucket <= currentBucket; ++bucket)
    {
        while (true)
        {
            auto it = m_timerBuckets.find(bucket);
            if (it == m_timerBuckets.end())
                break;
            oc::vector<Timer*>& timers = it->second;
            if (timers.empty())
            {
                m_timerBuckets.erase(it);
                break;
            }
            Timer* pTimer = timers.front();
            if (!(pTimer->m_endTime <= currentTime))
                break;
            std::pop_heap(timers.begin(), timers.end(), TimerCompare());
            timers.pop_back();
            if (timers.empty())
                m_timerBuckets.erase(it);
            if (pTimer->m_onTimer)
            {
                if (pTimer->m_onTimer(*pTimer) == Timer::REPEAT)
                {
                    pTimer->m_endTime = currentTime + (pTimer->m_endTime - pTimer->m_startTime);
                    pTimer->m_startTime = currentTime;
                    assert(pTimer->m_startTime < pTimer->m_endTime);
                    addTimer(pTimer);
                }
            }
            pTimer->m_hasTriggered = true;
        }
        m_lastCheckedBucket = currentBucket;
    }
}