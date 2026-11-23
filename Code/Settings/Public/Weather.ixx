export module Settings.Weather;

import Core;

// "Weather": the precipitation and the air temperature now, on top of the world's climate, and the SNOWPACK they build
// (the renderer's SnowPack: the depth on the ground per climate temperature, integrated on the CPU in game time). The
// state rows are Synced, so a client builds the same snow as the server; the co-op game writes them per night
// (Game/Weather). See "Snow" in Docs/Context/RendererVK/Snow.md.
export struct WeatherSettings
{
    float precipitation = 0.0f;      // 0..1: how hard it rains or snows (0 = dry)
    float temperatureOffset = 0.0f;  // C added to the climate's temperature everywhere (a cold spell < 0)
    float dayNightSwing = 4.0f;      // C: the air is this much warmer at noon and colder at midnight (x the sun height)
    float timeScale = 60.0f;         // game seconds per sim second for the snowpack (60: a game hour per real minute)
    float snowBelow = 0.5f;          // C of air: all snow below this ...
    float rainAbove = 2.0f;          // ... all rain above this (sleet between)
    float rainWetting = 0.15f;       // the terrain wetness rain at precipitation 1 (1/s; it settles at this x "Dry time")
    float stormCloudCover = 0.85f;   // the cloud coverage at least, at precipitation 1

    bool snowEnabled = true;         // the dynamic snowpack (off: only the climate's permanent snow)
    float snowfallRate = 0.02f;      // m of snow per game hour on open flat ground at precipitation 1
    float meltRate = 0.0025f;        // m per game hour per C of air over 0 (degree-hour melt)
    float sunMelt = 0.004f;          // m per game hour with the sun overhead (less in cold air, none below -8 C)
    float settling = 0.15f;          // share of the depth the pack settles per game day
    float maxDepth = 3.0f;           // m: the pack stops growing here
    float fullCoverDepth = 0.03f;    // m: the ground is fully white from this depth on (thinner snow shows it through)
    bool clearSnow = false;          // a button: the renderer empties the snowpack and resets this
    bool shelter = true;             // the site map: no snow under roofs and overhangs, less under trees (needs RT)
    float foliageBlock = 0.7f;       // the share of the snow one layer of foliage stops (the site map's alpha-tested cards)
    bool snowDepth = true;           // the ground rises by the snow's depth (the terrain VS); off: a texture layer only
    float buryDepth = 0.3f;          // m of snow that buries the grass and the clutter (taller blades poke through thinner)
    bool objectSnow = true;          // the snow layer on the objects (buildings, props, bark, leaves)
    float leafSnow = 0.6f;           // x the snow on the up-facing leaves (a crown sheds part of it)
    float voxelCrownSnowDepth = 0.05f; // how far into a voxel crown its leaves hold snow (0 = the surface only, 1 = to the core)
    bool tracks = true;              // the players, units, walkers and impacts press tracks and craters into the snow (needs Depth)
    float trackDetailRange = 20.0f;  // m: closer than this the tessellation refines over a track (footprints need ~6 cm vertices); the tracks' shape fades out to twice it
    float bermShare = 0.1f;          // the share of the pressed snow pushed up into a rim around a track or a crater
    float trackWallSlope = 2.0f;     // m of rise per m: a deep track's wall slopes up to the snow surface (2 = 63 degrees)
    bool caps = true;                // the snow's volume on roofs and other object tops (the site map's caps; needs Depth, desktop)
    float capRange = 100.0f;         // m: caps out to here (the site map's data ends ~110 m from the focus)
    float capDetail = 40.0f;         // m: full-resolution caps out to here, half to twice it, a quarter beyond
    float capFromDepth = 0.03f;      // m: no cap under this depth (the texture layer alone; it is fully white from Full cover depth)
    float capRounding = 1.5f;        // m: a cap's edge rounds off over at most this (and at most twice the depth)
    float capSink = 0.05f;           // m: a cap's edge sinks this far into the top it lies on (no visible rim line)
    float capMaxStep = 0.3f;         // m: a drop larger than this between two site texels ends a cap (a roof's edge)
    bool rockShell = true;           // rocks, dead wood and cacti swell by the snow on their tops (needs Depth)
    float rockShellRange = 150.0f;   // m: the swelling fades out over the last 20 % of this

    // ---- The snow's state (SnowPack: per table cell, visual only): its grains, its meltwater and its grime ----
    float freshGrain = 0.1f;         // mm: the grain size of new snow (the albedo, the sparkle density)
    float grainGrowth = 0.05f;       // mm^2 per game day: dry snow's grains grow (slower in deep cold)
    float wetGrainGrowth = 0.2f;     // mm^3 per game hour at full meltwater: wet snow's grains grow fast (corn snow)
    float surfaceLayer = 0.02f;      // m of new snow that renews the surface (fresh grains, no grime, dry)
    float meltHold = 0.005f;         // m of melt that saturates the surface with meltwater
    float meltDrain = 6.0f;          // game hours: the meltwater drains (above 0 C) ...
    float refreeze = 2.0f;           // ... or freezes (below) by 1/e in this
    float grimePerDay = 0.01f;       // the dust that settles on the snow per game day (0..1 of full grime)
    float meltGrime = 0.5f;          // melt concentrates the grime: x the melt per surface layer
    float meltWetting = 0.05f;       // the terrain wetness rain at full meltwater (1/s), so the ground stays wet as it melts out

    // ---- The snow's look (snow_brdf.inc.glsl, snow.inc.glsl) ----
    float grainRough = 0.6f;         // the GGX alpha of dry snow's ice facets
    float wetRough = 0.15f;          // ... of wet snow (a film of meltwater)
    float softTerminator = 1.0f;     // the light that scatters through the snow across its relief (a soft terminator, blue on the shaded flank)
    float forwardScatter = 1.0f;     // the sheen with the sun ahead at grazing angles (the grains' forward scattering)
    bool brightCavities = true;      // the snow's cavities are lit by the snow around them (bounces, red lost first)
    bool sparkle = true;             // the sun glints on single ice grains
    float sparkleDensity = 0.3f;     // the share of the grains that have a mirror facet
    float sparkleSpread = 0.15f;     // deg: how far off the mirror direction a facet still glints (larger = more, dimmer glints)
    float sparkleStrength = 3.0f;    // x the ice facets' reflection, the glints and the smooth lobe alike (the light reflected inside the grains)
    float sparkleMax = 200.0f;       // x the white reflection: the brightest glint
    float cavityBounces = 1.5f;      // wall bounces per 10 cm of a track's depth (each takes the snow's colour again: blue)
    bool trackShadow = true;         // a track's walls shade its floor from the sun
    float wallGlow = 0.5f;           // the sun that shines through a track's thin wall (blue)
    float wetSnowReflect = 0.5f;     // wet snow mirrors the sky like wet ground
};

export namespace Settings
{
    void registerWeather(WeatherSettings& s);
}
