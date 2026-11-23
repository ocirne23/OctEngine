module Settings.Weather;

import Core;
import Core.glm;
import Settings.Tweaks;

void Settings::registerWeather(WeatherSettings& s)
{
    // The state the snowpack is built from is Synced: every peer integrates the same pack from the server's values.
    {
        const Tweak::ScopedFlags scoped(ETweakFlags::Synced);
        Tweak::floatVar("Weather", "Precipitation", &s.precipitation, 0.0f, 1.0f, 0.01f);
        Tweak::floatVar("Weather", "Temperature offset (C)", &s.temperatureOffset, -30.0f, 30.0f, 0.1f);
        Tweak::floatVar("Weather", "Day-night swing (C)", &s.dayNightSwing, 0.0f, 20.0f, 0.1f);
        Tweak::floatVar("Weather", "Time scale", &s.timeScale, 0.0f, 3600.0f, 1.0f);
        Tweak::floatVar("Weather", "Snow below (C)", &s.snowBelow, -10.0f, 10.0f, 0.1f,
            [&s] { s.rainAbove = glm::max(s.rainAbove, s.snowBelow + 0.1f); });
        Tweak::floatVar("Weather", "Rain above (C)", &s.rainAbove, -10.0f, 10.0f, 0.1f,
            [&s] { s.snowBelow = glm::min(s.snowBelow, s.rainAbove - 0.1f); });
        Tweak::floatVar("Weather/Snow", "Snowfall (m per hour)", &s.snowfallRate, 0.0f, 1.0f, 0.001f);
        Tweak::floatVar("Weather/Snow", "Melt (m per hour per C)", &s.meltRate, 0.0f, 0.1f, 0.0001f);
        Tweak::floatVar("Weather/Snow", "Sun melt (m per hour)", &s.sunMelt, 0.0f, 0.1f, 0.0001f);
        Tweak::floatVar("Weather/Snow", "Settling (per day)", &s.settling, 0.0f, 1.0f, 0.01f);
        Tweak::floatVar("Weather/Snow", "Max depth (m)", &s.maxDepth, 0.1f, 10.0f, 0.05f);
    }
    Tweak::floatVar("Weather", "Rain wetting (1/s)", &s.rainWetting, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Weather", "Storm cloud cover", &s.stormCloudCover, 0.0f, 1.0f, 0.01f);
    Tweak::boolean("Weather/Snow", "Enabled", &s.snowEnabled);
    Tweak::floatVar("Weather/Snow", "Full cover depth (m)", &s.fullCoverDepth, 0.001f, 0.5f, 0.001f);
    Tweak::boolean("Weather/Snow", "Clear snow", &s.clearSnow);
    Tweak::boolean("Weather/Snow", "Shelter", &s.shelter);
    Tweak::floatVar("Weather/Snow", "Foliage block", &s.foliageBlock, 0.0f, 1.0f, 0.01f);
    Tweak::boolean("Weather/Snow", "Depth", &s.snowDepth);
    Tweak::floatVar("Weather/Snow", "Bury depth (m)", &s.buryDepth, 0.01f, 2.0f, 0.01f);
    Tweak::boolean("Weather/Snow", "Objects", &s.objectSnow);
    Tweak::floatVar("Weather/Snow", "Leaf snow", &s.leafSnow, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Voxel crown snow depth", &s.voxelCrownSnowDepth, 0.0f, 1.0f, 0.01f);
    Tweak::boolean("Weather/Snow", "Tracks", &s.tracks);
    Tweak::floatVar("Weather/Snow", "Track detail range (m)", &s.trackDetailRange, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Weather/Snow", "Berm", &s.bermShare, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Track wall slope", &s.trackWallSlope, 0.5f, 10.0f, 0.05f);
    Tweak::boolean("Weather/Snow", "Caps", &s.caps);
    Tweak::floatVar("Weather/Snow", "Cap range (m)", &s.capRange, 10.0f, 105.0f, 1.0f);
    // >= 20 m: LOD 1's morph band must start a tile's diagonal (11.3 m) past LOD 0's end, or a coarser neighbour's edge cracks.
    Tweak::floatVar("Weather/Snow", "Cap full detail (m)", &s.capDetail, 20.0f, 105.0f, 1.0f);
    Tweak::floatVar("Weather/Snow", "Caps from depth (m)", &s.capFromDepth, 0.001f, 0.5f, 0.001f);
    Tweak::floatVar("Weather/Snow", "Cap edge rounding (m)", &s.capRounding, 0.05f, 1.75f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Cap edge sink (m)", &s.capSink, 0.0f, 0.3f, 0.005f);
    Tweak::floatVar("Weather/Snow", "Cap max step (m)", &s.capMaxStep, 0.05f, 2.0f, 0.01f);
    Tweak::boolean("Weather/Snow", "Rock shell", &s.rockShell);
    Tweak::floatVar("Weather/Snow", "Rock shell range (m)", &s.rockShellRange, 10.0f, 1000.0f, 1.0f);
    // The state (SnowPack, per table cell): visual only, so local - every peer integrates it from the Synced weather.
    Tweak::floatVar("Weather/Snow", "Fresh grain size (mm)", &s.freshGrain, 0.03f, 1.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Grain growth (mm2 per day)", &s.grainGrowth, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Weather/Snow", "Wet grain growth (mm3 per hour)", &s.wetGrainGrowth, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Surface layer (m)", &s.surfaceLayer, 0.002f, 0.1f, 0.001f);
    Tweak::floatVar("Weather/Snow", "Meltwater hold (m)", &s.meltHold, 0.0005f, 0.05f, 0.0005f);
    Tweak::floatVar("Weather/Snow", "Meltwater drain (hours)", &s.meltDrain, 0.1f, 48.0f, 0.1f);
    Tweak::floatVar("Weather/Snow", "Refreeze (hours)", &s.refreeze, 0.1f, 24.0f, 0.1f);
    Tweak::floatVar("Weather/Snow", "Grime (per day)", &s.grimePerDay, 0.0f, 1.0f, 0.001f);
    Tweak::floatVar("Weather/Snow", "Melt grime", &s.meltGrime, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Weather/Snow", "Melt wetting (1/s)", &s.meltWetting, 0.0f, 1.0f, 0.005f);
    // The look.
    Tweak::floatVar("Weather/Snow", "Grain roughness", &s.grainRough, 0.1f, 1.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Wet snow roughness", &s.wetRough, 0.02f, 1.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Soft terminator", &s.softTerminator, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Weather/Snow", "Forward scattering", &s.forwardScatter, 0.0f, 2.0f, 0.05f);
    Tweak::boolean("Weather/Snow", "Bright cavities", &s.brightCavities);
    Tweak::boolean("Weather/Snow", "Sparkle", &s.sparkle);
    Tweak::floatVar("Weather/Snow", "Sparkle density", &s.sparkleDensity, 0.01f, 1.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Sparkle spread (deg)", &s.sparkleSpread, 0.05f, 5.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Sparkle strength", &s.sparkleStrength, 1.0f, 50.0f, 0.1f);
    Tweak::floatVar("Weather/Snow", "Sparkle max (x white)", &s.sparkleMax, 1.0f, 2000.0f, 1.0f);
    Tweak::floatVar("Weather/Snow", "Cavity bounces (per 10 cm)", &s.cavityBounces, 0.0f, 10.0f, 0.05f);
    Tweak::boolean("Weather/Snow", "Track sun shadow", &s.trackShadow);
    Tweak::floatVar("Weather/Snow", "Wall glow", &s.wallGlow, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Weather/Snow", "Wet snow reflection", &s.wetSnowReflect, 0.0f, 1.0f, 0.01f);
}
