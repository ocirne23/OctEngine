module Settings.Nav;

import Core;
import Settings.Tweaks;

void Settings::registerNav(NavSettings& s)
{
    Tweak::boolean("Nav", "Enabled", &s.enabled);
    Tweak::floatVar("Nav", "Flow half-life (s)", &s.flowHalfLife, 0.05f, 60.0f, 0.05f);
    Tweak::floatVar("Nav", "Pressure diffusion", &s.pressureDiffusion, 0.0f, 0.25f, 0.005f);
    Tweak::floatVar("Nav", "Pressure half-life (s)", &s.pressureHalfLife, 0.02f, 30.0f, 0.02f);
    Tweak::floatVar("Nav", "Pressure floor", &s.pressureFloor, 0.0f, 5.0f, 0.02f);
    Tweak::floatVar("Nav", "Pressure flow gain 10^x", &s.pressureFlowGainExp, -2.0f, 3.0f, 0.02f);
    Tweak::floatVar("Nav", "Flow max (m/s)", &s.flowMaxSpeed, 0.0f, 40.0f, 0.5f);
    Tweak::floatVar("Nav", "Seed area (m)", &s.seedArea, 2.0f, 64.0f, 1.0f);
    Tweak::floatVar("Nav", "Seed cooldown (s)", &s.seedCooldown, 0.0f, 30.0f, 0.25f);
    Tweak::intVar("Nav", "Seed max/frame", &s.seedMaxPerFrame, 0, 16);
    Tweak::floatVar("Nav", "Seed trough", &s.seedTrough, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Nav", "Seed trough squeeze", &s.seedSqueeze, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Nav", "Seed range (m)", &s.seedRange, 0.0f, 400.0f, 2.0f); // 0 = the whole path
    Tweak::floatVar("Nav", "Field radius", &s.fieldRadius, 10.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Nav", "Rebuild interval", &s.rebuildInterval, 0.05f, 5.0f, 0.05f);
    Tweak::floatVar("Nav", "Build spread (s)", &s.buildSpread, 0.0f, 5.0f, 0.05f);
    Tweak::intVar("Nav", "Clearance cost", &s.clearanceCost, 0, 32);
    Tweak::intVar("Nav", "Chunk keep frames", &s.keepFrames, 1, 2000);
    Tweak::intVar("Nav", "Debug draw", &s.debugMode, 0, 2); // 1 = chunks + team field, 2 = flow + pressure
    Tweak::intVar("Nav", "Debug team", &s.debugTeam, 0, 8); // 0..7 = team fields (Nav::MaxTeams = 8), 8 = the player's goal field
    Tweak::floatVar("Nav", "Debug radius", &s.debugRadius, 5.0f, 400.0f, 1.0f);
    Tweak::floatVar("Nav", "Debug flow min (m/s)", &s.debugFlowMin, 0.0f, 8.0f, 0.05f);
}
