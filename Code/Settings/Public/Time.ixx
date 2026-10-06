export module Settings.Time;

import Core;
import Core.Time;

// "Time": the frame pacing and the pause. The type is Core's (TimeSettings, Core.Time - Core sits below this library);
// main binds the instance with Globals::time.bindSettings.
export namespace Settings
{
    void registerTime(TimeSettings& s);
}
