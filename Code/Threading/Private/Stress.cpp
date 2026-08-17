module Threading;

import Core;
import Core.Log;
import Core.Tweaks;

static constexpr oc::string_view modeNames[] = { "Off", "Empty jobs", "ParallelFor", "Graph", "Fiber wait" };

void JobSystemStress::initialize()
{
    Tweak::enumVar("Threading/Stress", "Mode", &m_mode, modeNames);
    Tweak::intVar("Threading/Stress", "Jobs per batch", &m_batchSize, 1, 16000);
    Tweak::intVar("Threading/Stress", "ParallelFor size", &m_parallelSize, 1, 1 << 24, 4096.0f);
    Tweak::intVar("Threading/Stress", "Grain size", &m_grainSize, 1, 65536, 64.0f);
    Tweak::boolean("Threading/Stress", "Run self test", &m_runSelfTest);
    Tweak::floatVar("Threading/Stress", "Batch ms", &m_lastMs, 0.0f, FLT_MAX, 0.0f);
    Tweak::intVar("Threading/Stress", "Jobs per ms", &m_jobsPerMs, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stress", "Hazard violations", &m_violationsDisplay, 0, INT32_MAX, 0.0f);

    Tweak::intVar("Threading/Stats", "Executed/s", &m_executedPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Stolen/s", &m_stolenPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Parked/s", &m_parkedPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Resumed/s", &m_resumedPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Sleeps/s", &m_sleepsPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Inline fallbacks", &m_inlineFallbacks, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Busy %", &m_busyPercent, 0, 100, 0.0f);
}

void JobSystemStress::update()
{
    if (!Globals::jobSystem.isInitialized())
        return;

    if (m_runSelfTest)
    {
        m_runSelfTest = false;
        selfTest();
    }

    if (m_delayedFlag.exchange(0, oc::memory_order_relaxed))
    {
        char buf[128];
        sprintf_s(buf, "JobSystem self test: delayed job fired after %.1f ms (asked for 250 ms) - %s",
            double(m_delayedMicros.load(oc::memory_order_relaxed)) / 1000.0,
            m_delayedMicros.load(oc::memory_order_relaxed) >= 240000 ? "PASS" : "FAIL");
        m_delayedMicros >= 240000 ? Log::info(buf) : Log::error(buf);
    }

    switch (m_mode)
    {
    case 1: benchEmptyJobs(); break;
    case 2: benchParallelFor(); break;
    case 3: benchGraph(); break;
    case 4: benchFiberWait(); break;
    default: break;
    }

    updateStatsDisplay();
}

void JobSystemStress::benchEmptyJobs()
{
    JobSystem& jobSystem = Globals::jobSystem;
    const uint32 count = uint32(m_batchSize);
    const Clock::time_point start = Clock::now();
    JobCounter counter;
    for (uint32 i = 0; i < count; ++i)
        jobSystem.submit([] {}, EJobPriority::Normal, &counter, "stressEmpty");
    jobSystem.wait(counter);
    m_lastMs = float(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    m_jobsPerMs = m_lastMs > 0.0f ? int(float(count) / m_lastMs) : 0;
}

void JobSystemStress::benchParallelFor()
{
    const uint32 count = uint32(m_parallelSize);
    m_parallelData.resize(count);
    float* data = m_parallelData.data();
    const Clock::time_point start = Clock::now();
    Globals::jobSystem.parallelFor(0, count, uint32(m_grainSize), [data](uint32 begin, uint32 end)
        {
            for (uint32 i = begin; i < end; ++i)
                data[i] = std::sqrt(float(i) * 1.618f) + float(i) * 0.001f;
        });
    m_lastMs = float(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    m_jobsPerMs = m_lastMs > 0.0f ? int(float(count) / m_lastMs) : 0;
}

void JobSystemStress::buildBenchGraph()
{
    if (m_benchGraph.getNumJobs() != 0)
        return;
    m_benchResources = oc::make_unique<JobResource[]>(NumBenchResources);
    m_guards = oc::make_unique<ResourceGuard[]>(NumBenchResources);
    uint32 rng = 0x2545f491u;
    auto nextRandom = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (uint32 j = 0; j < NumBenchJobs; ++j)
    {
        const int32 r0 = int32(nextRandom() % NumBenchResources);
        int32 r1 = int32(nextRandom() % NumBenchResources);
        int32 w = (nextRandom() & 1) ? int32(nextRandom() % NumBenchResources) : -1;
        if (r1 == r0)
            r1 = -1;
        while (w >= 0 && (w == r0 || w == r1)) // per-job read/write sets kept disjoint so the guard math stays simple
            w = (w + 1) % int32(NumBenchResources);
        JobGraphBuilder builder = m_benchGraph.addJob("stressGraph", [this, r0, r1, w]
            {
                if (m_guards[r0].writers.load(oc::memory_order_relaxed) != 0)
                    m_violations.fetch_add(1, oc::memory_order_relaxed);
                m_guards[r0].readers.fetch_add(1, oc::memory_order_relaxed);
                if (r1 >= 0)
                {
                    if (m_guards[r1].writers.load(oc::memory_order_relaxed) != 0)
                        m_violations.fetch_add(1, oc::memory_order_relaxed);
                    m_guards[r1].readers.fetch_add(1, oc::memory_order_relaxed);
                }
                if (w >= 0)
                {
                    if (m_guards[w].writers.fetch_add(1, oc::memory_order_relaxed) != 0 ||
                        m_guards[w].readers.load(oc::memory_order_relaxed) != 0)
                        m_violations.fetch_add(1, oc::memory_order_relaxed);
                }
                float f = float(r0 + 1);
                for (uint32 i = 0; i < 400; ++i) // enough work for overlap windows to be real
                    f = f * 1.0001f + 0.5f;
                m_workSink.fetch_add(int32(f) & 1, oc::memory_order_relaxed);
                if (w >= 0)
                    m_guards[w].writers.fetch_sub(1, oc::memory_order_relaxed);
                if (r1 >= 0)
                    m_guards[r1].readers.fetch_sub(1, oc::memory_order_relaxed);
                m_guards[r0].readers.fetch_sub(1, oc::memory_order_relaxed);
            });
        builder.reads(m_benchResources[r0]);
        if (r1 >= 0)
            builder.reads(m_benchResources[r1]);
        if (w >= 0)
            builder.writes(m_benchResources[w]);
    }
    m_benchGraph.compile();
}

void JobSystemStress::benchGraph()
{
    buildBenchGraph();
    const Clock::time_point start = Clock::now();
    m_benchGraph.runAndWait();
    m_lastMs = float(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    m_jobsPerMs = m_lastMs > 0.0f ? int(float(NumBenchJobs) / m_lastMs) : 0;
    m_violationsDisplay = int(m_violations.load(oc::memory_order_relaxed));
}

void JobSystemStress::benchFiberWait()
{
    JobSystem& jobSystem = Globals::jobSystem;
    const Clock::time_point start = Clock::now();
    JobCounter outer;
    for (uint32 i = 0; i < 64; ++i)
        jobSystem.submit([this]
            {
                JobSystem& js = Globals::jobSystem;
                oc::atomic<uint32> innerDone = 0;
                JobCounter inner;
                for (uint32 k = 0; k < 16; ++k)
                    js.submit([&innerDone] { innerDone.fetch_add(1, oc::memory_order_relaxed); }, EJobPriority::Normal, &inner, "stressInner");
                js.wait(inner); // parks this fiber; the counter + flag live on its stack
                if (innerDone.load(oc::memory_order_relaxed) != 16)
                    m_violations.fetch_add(1, oc::memory_order_relaxed);
            }, EJobPriority::Normal, &outer, "stressOuter");
    jobSystem.wait(outer);
    m_lastMs = float(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    m_jobsPerMs = m_lastMs > 0.0f ? int((64.0f * 17.0f) / m_lastMs) : 0;
    m_violationsDisplay = int(m_violations.load(oc::memory_order_relaxed));
}

void JobSystemStress::updateStatsDisplay()
{
    const Clock::time_point now = Clock::now();
    const double interval = std::chrono::duration<double>(now - m_lastStatsTime).count();
    if (interval < 0.5)
        return;
    const JobSystemStats stats = Globals::jobSystem.getStats();
    if (m_lastStatsTime != Clock::time_point{})
    {
        m_executedPerSec = int(double(stats.numExecuted - m_lastStats.numExecuted) / interval);
        m_stolenPerSec = int(double(stats.numStolen - m_lastStats.numStolen) / interval);
        m_parkedPerSec = int(double(stats.numParked - m_lastStats.numParked) / interval);
        m_resumedPerSec = int(double(stats.numResumed - m_lastStats.numResumed) / interval);
        m_sleepsPerSec = int(double(stats.numSleeps - m_lastStats.numSleeps) / interval);
        m_inlineFallbacks = int(stats.numInlineFallbacks);
        m_busyPercent = int(100.0 * double(stats.busyNs - m_lastStats.busyNs) / (interval * 1e9 * double(Globals::jobSystem.getNumContexts())));
    }
    m_lastStats = stats;
    m_lastStatsTime = now;
}

void JobSystemStress::selfTest()
{
    JobSystem& jobSystem = Globals::jobSystem;
    char buf[192];
    uint32 numFailed = 0;

    { // submit + counter wait
        oc::atomic<uint64> sum = 0;
        JobCounter counter;
        for (uint32 i = 0; i < 4096; ++i)
            jobSystem.submit([&sum, i] { sum.fetch_add(i, oc::memory_order_relaxed); }, EJobPriority::Normal, &counter, "testSum");
        jobSystem.wait(counter);
        const bool pass = sum.load() == 4096ull * 4095ull / 2ull;
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: submit/wait sum - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // parallelFor covers every index exactly once
        oc::vector<uint32> values(100000, 0);
        uint32* data = values.data();
        jobSystem.parallelFor(0, uint32(values.size()), 1024, [data](uint32 begin, uint32 end)
            {
                for (uint32 i = begin; i < end; ++i)
                    data[i] += i * 3u;
            });
        bool pass = true;
        for (uint32 i = 0; i < uint32(values.size()); ++i)
            pass &= values[i] == i * 3u;
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: parallelFor coverage - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // JobCost auto-grain: coverage stays exact across runs while the estimate converges
        JobCost cost(10, "testAutoGrain");
        oc::vector<uint32> values(200000, 0);
        uint32* data = values.data();
        for (uint32 run = 0; run < 8; ++run)
            jobSystem.parallelFor(0, uint32(values.size()), cost, [data](uint32 begin, uint32 end)
                {
                    for (uint32 i = begin; i < end; ++i)
                        data[i] += 1;
                });
        bool pass = cost.nsPerItem() > 0;
        for (uint32 i = 0; i < uint32(values.size()); ++i)
            pass &= values[i] == 8;
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: JobCost auto-grain coverage x8 (%llu ns/item) - %s", cost.nsPerItem(), pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // graph hazard ordering: write -> reads -> write -> read must observe each stage
        JobResource resource("testResource");
        oc::atomic<int32> value = 0;
        oc::atomic<uint32> wrongOrder = 0;
        JobGraph graph;
        const JobId write1 = graph.addJob("write1", [&value] { value.store(1, oc::memory_order_relaxed); }).writes(resource);
        graph.addJob("readA", [&value, &wrongOrder] { if (value.load(oc::memory_order_relaxed) != 1) wrongOrder.fetch_add(1); }).reads(resource);
        graph.addJob("readB", [&value, &wrongOrder] { if (value.load(oc::memory_order_relaxed) != 1) wrongOrder.fetch_add(1); }).reads(resource);
        graph.addJob("write2", [&value, &wrongOrder] { if (value.load(oc::memory_order_relaxed) != 1) wrongOrder.fetch_add(1); value.store(2, oc::memory_order_relaxed); }).writes(resource);
        const JobId readC = graph.addJob("readC", [&value, &wrongOrder] { if (value.load(oc::memory_order_relaxed) != 2) wrongOrder.fetch_add(1); }).reads(resource);
        graph.compile();
        for (uint32 run = 0; run < 64; ++run) // re-run the same compiled graph, resets must hold up
        {
            value.store(0, oc::memory_order_relaxed);
            graph.runAndWait();
        }
        // cost/rank plumbing: every node ran (cost >= 1us floor), and the chain head outranks the tail
        bool pass = wrongOrder.load() == 0;
        pass &= graph.getJobCostUs(write1) >= 1 && graph.getJobCostUs(readC) >= 1;
        pass &= graph.getJobRankUs(write1) > graph.getJobRankUs(readC);
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: graph hazard ordering x64 - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // hazard-checked random graph
        buildBenchGraph();
        const uint32 before = m_violations.load(oc::memory_order_relaxed);
        for (uint32 run = 0; run < 16; ++run)
            m_benchGraph.runAndWait();
        const bool pass = m_violations.load(oc::memory_order_relaxed) == before;
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: random graph (128 jobs, 16 resources) x16 - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // fiber wait: jobs waiting on sub-jobs mid-execution
        const uint32 before = m_violations.load(oc::memory_order_relaxed);
        benchFiberWait();
        const bool pass = m_violations.load(oc::memory_order_relaxed) == before;
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: fiber wait (64 outer x 16 inner) - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // JobMutex: exclusion + fiber parking under heavy contention, from jobs AND external threads
        JobMutex mutex;
        uint32 shared = 0; // non-atomic on purpose: the mutex is what protects it
        oc::atomic<int32> inside = 0;
        JobCounter counter;
        for (uint32 i = 0; i < 512; ++i)
            jobSystem.submit([this, &mutex, &shared, &inside]
                {
                    JobMutex::Scope lock(mutex);
                    if (inside.fetch_add(1, oc::memory_order_relaxed) != 0)
                        m_violations.fetch_add(1, oc::memory_order_relaxed);
                    shared += 1;
                    for (volatile int spin = 0; spin < 50; ++spin) {}
                    inside.fetch_sub(1, oc::memory_order_relaxed);
                }, EJobPriority::Normal, &counter, "testMutex");
        std::thread external([&mutex, &shared, &inside, this]
            {
                for (uint32 i = 0; i < 64; ++i)
                {
                    JobMutex::Scope lock(mutex);
                    if (inside.fetch_add(1, oc::memory_order_relaxed) != 0)
                        m_violations.fetch_add(1, oc::memory_order_relaxed);
                    shared += 1;
                    inside.fetch_sub(1, oc::memory_order_relaxed);
                }
            });
        jobSystem.wait(counter);
        external.join();
        const bool pass = shared == 512 + 64 && m_violations.load(oc::memory_order_relaxed) == 0;
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: JobMutex exclusion (512 jobs + external thread) - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // JobEvent: one producer signals, many parked consumers release exactly once
        JobEvent event;
        oc::atomic<uint32> released = 0;
        JobCounter counter;
        for (uint32 i = 0; i < 64; ++i)
            jobSystem.submit([&event, &released]
                {
                    event.wait();
                    released.fetch_add(1, oc::memory_order_relaxed);
                }, EJobPriority::Normal, &counter, "testEventWait");
        jobSystem.submitDelayed(0.02, [&event] { event.signal(); });
        jobSystem.wait(counter);
        const bool pass = released.load() == 64 && event.isSignaled();
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: JobEvent (64 parked waiters, delayed signal) - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // PerWorker + getWorkerIndex: per-slot sums with no cross-talk; nested graph->parallelFor
        PerWorker<uint64> sums;
        sums.initialize();
        JobGraph graph;
        graph.addParallelJob("perWorkerSum", [] { return 100000u; }, 256, [&sums](uint32 begin, uint32 end)
            {
                sums.local() += end - begin; // single writer per slot within the phase
            });
        graph.compile();
        graph.runAndWait();
        uint64 total = 0;
        sums.forEach([&total](uint64& value) { total += value; });
        const bool pass = total == 100000u;
        numFailed += !pass;
        sprintf_s(buf, "JobSystem self test: PerWorker sums via graph-nested parallelFor - %s", pass ? "PASS" : "FAIL");
        pass ? Log::info(buf) : Log::error(buf);
    }

    { // delayed job; result is logged from update() when it lands
        const Clock::time_point start = Clock::now();
        jobSystem.submitDelayed(0.25, [this, start]
            {
                m_delayedMicros.store(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count(), oc::memory_order_relaxed);
                m_delayedFlag.store(1, oc::memory_order_relaxed);
            }, EJobPriority::Normal, nullptr, "testDelayed");
        Log::info("JobSystem self test: delayed job submitted (250 ms), result follows...");
    }

    sprintf_s(buf, "JobSystem self test: %u failures", numFailed);
    numFailed == 0 ? Log::info(buf) : Log::error(buf);
}
