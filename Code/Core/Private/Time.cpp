module Core.Time;

import <queue>;

import Core;

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
        // Re-find the bucket EVERY iteration: a timer callback may add/remove timers (addTimer
        // inserts into the map — a rehash invalidates iterators and the vector reference), so
        // nothing map-related survives across the callback. An emptied bucket is erased BEFORE
        // the callback runs for the same reason.
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