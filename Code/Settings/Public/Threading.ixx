export module Settings.Threading;

import Core;

// "Threading/Stress": the JobSystemStress harness - controls plus its readouts.
export struct JobStressSettings
{
    int mode = 0; // Off, Empty jobs, ParallelFor, Graph, Fiber wait
    int batchSize = 10000;
    int parallelSize = 1 << 20;
    int grainSize = 4096;
    bool runSelfTest = false;
    float lastMs = 0.0f;
    int jobsPerMs = 0;
    int violations = 0;
};

// "Threading/Stats": scheduler rates, written by the JobSystemStress harness.
export struct JobStatsSettings
{
    int executedPerSec = 0;
    int stolenPerSec = 0;
    int parkedPerSec = 0;
    int resumedPerSec = 0;
    int sleepsPerSec = 0;
    int preemptedPerSec = 0;
    int inlineFallbacks = 0;
    int busyPercent = 0;
};

export struct ThreadingSettings
{
    JobStressSettings stress;
    JobStatsSettings stats;
};

export namespace Settings
{
    void registerThreading(ThreadingSettings& s);
}
