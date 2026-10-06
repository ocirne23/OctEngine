module Settings.Time;

import Core;
import Core.Time;
import Settings.Tweaks;

void Settings::registerTime(TimeSettings& s)
{
    Tweak::intVar("Time", "Max FPS", &s.maxFps, 0, 1000, 1.0f, {}, ETweakFlags::Saved);
    Tweak::intVar("Time", "Inactive max FPS", &s.inactiveMaxFps, 0, 240, 1.0f, {}, ETweakFlags::Saved);
    Tweak::floatVar("Time", "Busy-wait window (ms)", &s.busyWaitMs, 0.0f, 8.0f, 0.1f, {}, ETweakFlags::Saved);
    Tweak::boolean("Time", "Stable frame time", &s.stableFrameTime, {}, ETweakFlags::Saved);
    // Synced, deliberately NOT Saved: the server's pause freezes clients too, but a pause must
    // never persist into the next run.
    Tweak::boolean("Time", "Paused", &s.paused, {}, ETweakFlags::Synced);
    Tweak::floatVar("Time", "Input pump lead (ms)", &s.pumpLeadMs, 0.0f, 8.0f, 0.1f, {}, ETweakFlags::Saved);
}
