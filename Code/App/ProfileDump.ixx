export module App.ProfileDump;

import Core;
import Core.Log;
import File;

// Writes Profiler::buildReport to disk (relative paths resolve against Assets/). Main-thread IO is
// allowed explicitly: a dump is a one-shot user/automation action, never per-frame.
export bool writeProfileReport(const oc::string& path, const ProfileReportOptions& options)
{
    const oc::string report = Globals::profiler.buildReport(options);
    const oc::string dir = FileSystem::parentPath(path);
    if (!dir.empty())
        FileSystem::createDirectories(dir, /*allowMainThread*/ true);
    if (!FileSystem::writeFileStr(path, report, /*allowMainThread*/ true))
    {
        Log::error("Profile report: could not write " + path);
        return false;
    }
    Log::info("Profile report written to " + path + " (" + oc::to_string(report.size() / 1024) + " KB)");
    return true;
}
