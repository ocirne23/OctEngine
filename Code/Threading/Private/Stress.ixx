export module Threading:Stress;

import Core;
import :Types;
import :JobSystem;
import :JobGraph;

// Benchmark + correctness harness for the JobSystem, driven from the Threading/Stress tweaks:
// pick a mode and it runs one measured batch per frame (empty-job throughput, parallelFor,
// a 128-job hazard-checked graph, or fiber park/resume), publishing timings and scheduler rates
// under Threading/Stats. "Run self test" fires a one-shot correctness pass (submit/wait sums,
// parallelFor coverage, read/write hazard ordering, fiber waits, delayed jobs) into the log.
export class JobSystemStress final
{
public:

    void initialize(); // registers the Threading/Stress + Threading/Stats tweaks
    void update();     // call once per frame from the main loop
    void selfTest();   // one-shot correctness pass into the log (also fired by the tweak)

private:

    void benchEmptyJobs();
    void benchParallelFor();
    void benchGraph();
    void benchFiberWait();
    void buildBenchGraph();
    void updateStatsDisplay();

    // hazard validation: writers must be exclusive, readers must never overlap a writer
    struct alignas(64) ResourceGuard
    {
        oc::atomic<int32> writers = 0;
        oc::atomic<int32> readers = 0;
    };
    static constexpr uint32 NumBenchResources = 16;
    static constexpr uint32 NumBenchJobs = 128;

    JobGraph m_benchGraph;
    oc::unique_ptr<JobResource[]> m_benchResources;
    oc::unique_ptr<ResourceGuard[]> m_guards;
    oc::vector<float> m_parallelData;
    oc::atomic<uint32> m_violations = 0;
    oc::atomic<int32> m_workSink = 0;
    oc::atomic<uint32> m_delayedFlag = 0;
    oc::atomic<int64> m_delayedMicros = 0;

    int m_mode = 0;
    int m_batchSize = 10000;
    int m_parallelSize = 1 << 20;
    int m_grainSize = 4096;
    float m_lastMs = 0.0f;
    int m_jobsPerMs = 0;
    int m_violationsDisplay = 0;
    bool m_runSelfTest = false;

    JobSystemStats m_lastStats;
    Clock::time_point m_lastStatsTime = {};
    int m_executedPerSec = 0;
    int m_stolenPerSec = 0;
    int m_parkedPerSec = 0;
    int m_resumedPerSec = 0;
    int m_sleepsPerSec = 0;
    int m_inlineFallbacks = 0;
    int m_busyPercent = 0;
};

export namespace Globals
{
    JobSystemStress jobSystemStress;
}
