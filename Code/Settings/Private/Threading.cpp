module Settings.Threading;

import Core;
import Settings.Tweaks;

static constexpr oc::string_view modeNames[] = { "Off", "Empty jobs", "ParallelFor", "Graph", "Fiber wait" };

void Settings::registerThreading(ThreadingSettings& s)
{
    JobStressSettings& stress = s.stress;
    Tweak::enumVar("Threading/Stress", "Mode", &stress.mode, modeNames);
    Tweak::intVar("Threading/Stress", "Jobs per batch", &stress.batchSize, 1, 16000);
    Tweak::intVar("Threading/Stress", "ParallelFor size", &stress.parallelSize, 1, 1 << 24, 4096.0f);
    Tweak::intVar("Threading/Stress", "Grain size", &stress.grainSize, 1, 65536, 64.0f);
    Tweak::boolean("Threading/Stress", "Run self test", &stress.runSelfTest);
    Tweak::floatVar("Threading/Stress", "Batch ms", &stress.lastMs, 0.0f, FLT_MAX, 0.0f);
    Tweak::intVar("Threading/Stress", "Jobs per ms", &stress.jobsPerMs, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stress", "Hazard violations", &stress.violations, 0, INT32_MAX, 0.0f);

    JobStatsSettings& stats = s.stats;
    Tweak::intVar("Threading/Stats", "Executed/s", &stats.executedPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Stolen/s", &stats.stolenPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Parked/s", &stats.parkedPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Resumed/s", &stats.resumedPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Sleeps/s", &stats.sleepsPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Pre-empted/s", &stats.preemptedPerSec, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Inline fallbacks", &stats.inlineFallbacks, 0, INT32_MAX, 0.0f);
    Tweak::intVar("Threading/Stats", "Busy %", &stats.busyPercent, 0, 100, 0.0f);
}
