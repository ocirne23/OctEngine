module Settings.Trees;

import Core;
import Settings.Tweaks;

// "Far distance scale", "Fade width scale", "Voxel start" and "Force far" rewrite the materials' fade bands:
// TreeSystem::initialize attaches that listener.
void Settings::registerTrees(TreeSettings& s)
{
	auto respawn = [&s]() { s.respawn = true; };
	auto reload = [&s]() { s.reload = true; };
	Tweak::boolean("Trees", "Enabled", &s.enabled);
	Tweak::boolean("Trees", "Reload species", &s.reload);
	Tweak::boolean("Trees", "Respawn", &s.respawn);
	Tweak::boolean("Trees", "Regenerate textures", &s.regenerateTextures, [&s]() { if (s.regenerateTextures) s.reload = true; });
	Tweak::boolean("Trees", "Compress textures", &s.compressTextures, reload);
	Tweak::floatVar("Trees", "Size variation", &s.sizeVariation, 0.0f, 2.0f, 0.01f, respawn);
	Tweak::floatVar("Trees", "Bushes per tree", &s.bushesPerTree, 0.0f, 8.0f, 0.05f, respawn);
	// A bush farther than this from the shadow cascades' centre casts no sun shadow. 0 = no limit.
	Tweak::floatVar("Trees", "Bush shadow distance (m)", &s.bushShadowDistance, 0.0f, 5000.0f, 1.0f, respawn);
	// The world set ("Trees/World/Enabled"): the near chunks' records as trees + bushes.
	Tweak::intVar("Trees/World", "Near radius (chunks)", &s.nearRadius, 1, 32, 1.0f, respawn);
	Tweak::intVar("Trees/World", "Set capacity (pieces)", &s.worldCapacity, 10000, 8000000, 1000.0f, respawn);
	Tweak::intVar("Trees/World", "Expand per frame", &s.expandPerFrame, 1, 32, 1.0f);
	Tweak::boolean("Trees/World/Colliders", "Enabled", &s.colliders);
	Tweak::floatVar("Trees/World/Colliders", "Radius (m)", &s.colliderRadius, 4.0f, 512.0f, 1.0f);
	Tweak::intVar("Trees/World/Colliders", "Bodies per frame", &s.collidersPerFrame, 1, 4096, 1.0f);
	Tweak::floatVar("Trees", "Far distance scale", &s.farDistanceScale, 0.0f, 10.0f, 0.01f);
	// x every species' `Billboard FadeWidth` (m): how wide the mesh <-> voxels blend is. The GPU set keeps the width per
	// piece type, so a change respawns.
	Tweak::floatVar("Trees", "Fade width scale", &s.fadeWidthScale, 0.1f, 20.0f, 0.01f, respawn);
	// Where the TREES' mesh -> voxels blend starts (m; bushes keep their own); 0 = each species' Billboard Distance x
	// "Far distance scale". The GPU set keeps the distance per piece type, so a change respawns.
	Tweak::floatVar("Trees", "Voxel start (m)", &s.voxelStart, 0.0f, 4000.0f, 1.0f, respawn);
	// The VOXEL tier: the main pass past a species' band draws the variant's marched voxel grid. Off: its meshes.
	Tweak::boolean("Trees", "Voxel impostors", &s.voxelImpostors, reload);
	static constexpr oc::string_view VOXEL_RESOLUTIONS[] = { "32", "64", "128", "256" };
	Tweak::enumVar("Trees", "Voxel resolution", &s.voxelResolution, VOXEL_RESOLUTIONS, reload);
	Tweak::boolean("Trees", "Force far", &s.forceFar);
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
	Tweak::floatVar("Trees/World", "Grassland share", &s.grasslandShare, 0.0f, 0.9f, 0.01f);
	Tweak::floatVar("Trees/World", "Grassland size (m)", &s.grasslandSize, 50.0f, 5000.0f, 10.0f);
	Tweak::floatVar("Trees/World", "Grassland trees", &s.grasslandTrees, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Trees/World", "Forest type size (m)", &s.forestTypeSize, 50.0f, 5000.0f, 10.0f);
	Tweak::floatVar("Trees/World", "Forest minority", &s.forestMinority, 0.0f, 1.0f, 0.01f);
	Tweak::intVar("Trees/World", "Gen jobs", &s.maxGenJobs, 0, 64, 1.0f); // 0 = one per job worker
	Tweak::intVar("Trees/World", "GPU pool (MB)", &s.poolMB, 1, 1024, 1.0f);
	Tweak::intVar("Trees/World", "Upload KB per frame", &s.uploadKB, 16, 65536, 16.0f);
	Tweak::intVar("Trees/World", "CPU keep radius (chunks)", &s.keepRadius, 0, 128, 1.0f);
	Tweak::boolean("Trees/World", "Reload species", &s.reloadSpecies);
	Tweak::boolean("Trees/World", "Log stats", &s.logStats);
}
