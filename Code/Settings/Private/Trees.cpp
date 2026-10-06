module Settings.Trees;

import Core;
import Settings.Tweaks;

// "Far distance scale", "Branch card distance" and "Force far" rewrite the materials' fade bands: TreeSystem::initialize
// attaches that listener.
void Settings::registerTrees(TreeSettings& s)
{
	auto respawn = [&s]() { s.respawn = true; };
	auto reload = [&s]() { s.reload = true; };
	Tweak::boolean("Trees", "Enabled", &s.enabled);
	Tweak::boolean("Trees", "Reload species", &s.reload);
	Tweak::boolean("Trees", "Respawn preview", &s.respawn);
	Tweak::boolean("Trees", "Regenerate textures", &s.regenerateTextures, [&s]() { if (s.regenerateTextures) s.reload = true; });
	Tweak::boolean("Trees", "Show piece library", &s.showLibrary, reload);
	Tweak::boolean("Trees", "Compress textures", &s.compressTextures, reload);
	Tweak::intVar("Trees", "Grove size", &s.gridSize, 1, 512, 1.0f, respawn);
	Tweak::floatVar("Trees", "Spacing (m)", &s.spacing, 2.0f, 50.0f, 0.1f, respawn);
	Tweak::floatVar("Trees", "Position jitter", &s.positionJitter, 0.0f, 2.0f, 0.01f, respawn);
	Tweak::floatVar("Trees", "Size variation", &s.sizeVariation, 0.0f, 2.0f, 0.01f, respawn);
	Tweak::floatVar("Trees", "Bushes per tree", &s.bushesPerTree, 0.0f, 8.0f, 0.05f, respawn);
	// GPU path: a bush farther than this from the shadow cascades' centre casts no sun shadow. 0 = no limit.
	Tweak::floatVar("Trees", "Bush shadow distance (m)", &s.bushShadowDistance, 0.0f, 5000.0f, 1.0f, respawn);
	Tweak::intVar("Trees", "Seed", &s.seed, 0, 1000000, 1.0f, respawn);
	Tweak::enumVar("Trees", "Grove type", &s.groveType, TREE_GROVE_TYPES, respawn);
	// World mode ("Trees/World/Enabled" with the GPU expansion): the near chunks' records as trees + bushes.
	Tweak::intVar("Trees/World", "Near radius (chunks)", &s.nearRadius, 1, 32, 1.0f, respawn);
	Tweak::intVar("Trees/World", "Set capacity (pieces)", &s.worldCapacity, 10000, 8000000, 1000.0f, respawn);
	Tweak::intVar("Trees/World", "Expand per frame", &s.expandPerFrame, 1, 32, 1.0f);
	static constexpr oc::string_view FAR_MODES[] = { "Billboards", "None" };
	Tweak::enumVar("Trees", "Far mode", &s.farMode, FAR_MODES, reload);
	static constexpr oc::string_view BILLBOARD_VIEWS[] = { "2 (side + top)", "4 (+ other side + bottom)" };
	Tweak::enumVar("Trees", "Billboard views", &s.billboardViews, BILLBOARD_VIEWS, reload);
	Tweak::floatVar("Trees", "Far distance scale", &s.farDistanceScale, 0.0f, 10.0f, 0.01f);
	// The MID tier: from this fraction of each species' billboard distance the leaves mesh gives way to one billboard
	// card per branch module (the bark mesh stays). 0 = off.
	Tweak::floatVar("Trees", "Branch card distance", &s.branchCardDistance, 0.0f, 1.0f, 0.01f, respawn);
	Tweak::boolean("Trees", "Force far", &s.forceFar);
	Tweak::boolean("Trees", "GPU expansion", &s.gpuExpansion, respawn);
}

// Every one but "Gen jobs", "Upload KB per frame" and the two buttons starts a new generation, and "CPU keep radius"
// rescans the ring: TreeWorld::initialize attaches those listeners.
void Settings::registerTreeWorld(TreeWorldSettings& s)
{
	Tweak::boolean("Trees/World", "Enabled", &s.enabled);
	Tweak::intVar("Trees/World", "Seed", &s.seed, 0, 1000000, 1.0f);
	Tweak::floatVar("Trees/World", "Candidate cell (m)", &s.cellSize, 2.0f, 20.0f, 0.1f);
	Tweak::floatVar("Trees/World", "Density scale", &s.densityScale, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Trees/World", "Climate sharpness", &s.climateSharpness, 0.0f, 16.0f, 0.1f);
	Tweak::floatVar("Trees/World", "Climate fade start", &s.climateFadeStart, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Trees/World", "Climate fade end", &s.climateFadeEnd, 0.0f, 1.0f, 0.01f);
	Tweak::intVar("Trees/World", "Gen jobs", &s.maxGenJobs, 1, 8, 1.0f);
	Tweak::intVar("Trees/World", "GPU pool (MB)", &s.poolMB, 1, 1024, 1.0f);
	Tweak::intVar("Trees/World", "Upload KB per frame", &s.uploadKB, 16, 65536, 16.0f);
	Tweak::intVar("Trees/World", "CPU keep radius (chunks)", &s.keepRadius, 0, 128, 1.0f);
	Tweak::boolean("Trees/World", "Reload species", &s.reloadSpecies);
	Tweak::boolean("Trees/World", "Log stats", &s.logStats);
}
