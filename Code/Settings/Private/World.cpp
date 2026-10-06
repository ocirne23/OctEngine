module Settings.World;

import Core;
import Settings.Tweaks;

void Settings::registerWorld(WorldSettings& s)
{
    Tweak::boolean("Editor", "Light debug geometry", &s.lightDebugGeometry);

    // Neither Saved nor Synced: the code defaults rule every run (a stale tweaks.cfg must not override
    // a tuning change), and the LOD is a per-process performance setting.
    SimLodConfig& c = s.simLod;
    Tweak::boolean("Game/Sim LOD", "Enabled", &c.enabled);
    Tweak::boolean("Game/Sim LOD", "Horizontal distance", &c.horizontal);
    Tweak::floatVar("Game/Sim LOD", "Full rate within (m)", &c.radius[0], 0.0f, 2000.0f, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Tier 1 within (m)", &c.radius[1], 0.0f, 2000.0f, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Tier 1 interval (s)", &c.intervalSec[0], 0.0f, 5.0f, 0.01f);
    Tweak::intVar("Game/Sim LOD", "Tier 1 min frames", &c.minFrames[0], 1, 60, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Tier 2 within (m)", &c.radius[2], 0.0f, 2000.0f, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Tier 2 interval (s)", &c.intervalSec[1], 0.0f, 5.0f, 0.01f);
    Tweak::intVar("Game/Sim LOD", "Tier 2 min frames", &c.minFrames[1], 1, 60, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Dormant interval (s, 0 = never)", &c.intervalSec[2], 0.0f, 60.0f, 0.1f);
    Tweak::intVar("Game/Sim LOD", "Dormant min frames", &c.minFrames[2], 1, 600, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Interval jitter", &c.intervalJitter, 0.0f, 0.5f, 0.01f);
    Tweak::intVar("Game/Sim LOD", "Visible max tier", &c.visibleMaxTier, 0, 2, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Max catch-up (s)", &c.maxCatchUpSec, 0.01f, 10.0f, 0.05f);
    Tweak::floatVar("Game/Sim LOD", "Query margin (m)", &c.queryMargin, 0.0f, 100.0f, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Zone margin (m)", &c.zoneMargin, 0.0f, 100.0f, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Zone tier 2 band (m)", &c.zoneTier2Band, 0.0f, 200.0f, 1.0f);
    Tweak::floatVar("Game/Sim LOD", "Selection interval (s)", &c.selectionIntervalSec, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("Game/Sim LOD", "Force bubbles max tier (3 = always)", &c.forceMaxTier, 0, 3, 1.0f);
    Tweak::intVar("Game/Sim LOD", "Buoyancy max tier (3 = always)", &c.buoyancyMaxTier, 0, 3, 1.0f);
    Tweak::boolean("Game/Sim LOD", "Dormant disables physics body", &c.dormantDisableBody);
    Tweak::boolean("Game/Sim LOD/Follows", "Units", &c.units);
    Tweak::boolean("Game/Sim LOD/Follows", "Structures", &c.structures);
    Tweak::boolean("Game/Sim LOD/Follows", "Projectiles", &c.projectiles);
    Tweak::boolean("Game/Sim LOD/Follows", "Scripts", &c.scripts);
    Tweak::boolean("Game/Sim LOD/Follows", "Animators", &c.animators);

    // Live readouts (overwritten every pass; edits are meaningless) - deliberately not Saved.
    Tweak::intVar("Game/Sim LOD/Stats", "Full rate", &s.simLodStats[0], 0, 1 << 20, 0.0f);
    Tweak::intVar("Game/Sim LOD/Stats", "Tier 1", &s.simLodStats[1], 0, 1 << 20, 0.0f);
    Tweak::intVar("Game/Sim LOD/Stats", "Tier 2", &s.simLodStats[2], 0, 1 << 20, 0.0f);
    Tweak::intVar("Game/Sim LOD/Stats", "Dormant", &s.simLodStats[3], 0, 1 << 20, 0.0f);
}
