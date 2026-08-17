export module Core.Time;

import <queue>;

import Core;

export class Timer;
export class Time
{
public:

    void update()
    {
        m_currentTime = Clock::now();
        m_deltaSec = std::chrono::duration<double>(m_currentTime - m_lastTime).count();
        m_elapsedSec = std::chrono::duration<double>(m_currentTime - m_startTime).count();
        m_lastTime = m_currentTime;
        processTimers();
    }

    double getDeltaSec() const { return m_deltaSec; }
    double getElapsedSec() const { return m_elapsedSec; }
    Clock::time_point getCurrentTime() const { return m_currentTime; }

private:

    friend class Timer;
    static void addTimer(Timer* pTimer);
    static void removeTimer(Timer* pTimer);
    static void processTimers();

private:

    Clock::time_point m_currentTime = Clock::now();
    double m_deltaSec = 0.0;
    double m_elapsedSec = 0.0;

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