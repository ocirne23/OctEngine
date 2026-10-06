module Settings.Scatter;

import Core;
import Settings.Tweaks;

// Seed, cell size and density scale regenerate every cell: ScatterSystem::initialize attaches that listener.
void Settings::registerScatter(ScatterSettings& s)
{
	Tweak::boolean("Scatter", "Enabled", &s.enabled);
	Tweak::intVar("Scatter", "Seed", &s.seed, 0, 1000000, 1.0f);
	Tweak::floatVar("Scatter", "Cell size (m)", &s.cellSize, 16.0f, 256.0f, 1.0f);
	Tweak::floatVar("Scatter", "Density scale", &s.densityScale, 0.0f, 8.0f, 0.05f);
	Tweak::intVar("Scatter", "Gen jobs", &s.maxGenJobs, 1, 16, 1.0f);
	Tweak::floatVar("Scatter", "View distance scale", &s.viewScale, 0.1f, 4.0f, 0.05f); // live: spawn range only, no regen
	Tweak::intVar("Scatter", "Spawns/frame", &s.maxSpawnsPerFrame, 32, 8192, 1.0f);
}
