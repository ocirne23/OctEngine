module Settings.Rocks;

import Core;
import Core.glm;
import Settings.Tweaks;

// "Grid resolution" remeshes the loaded types: RockSystem::initialize attaches that listener.
void Settings::registerRocks(RockSettings& s)
{
	auto respawn = [&s]() { s.respawn = true; };
	Tweak::boolean("Rocks", "Enabled", &s.enabled);
	Tweak::boolean("Rocks", "Reload types", &s.reload);
	Tweak::boolean("Rocks", "Respawn preview", &s.respawn);
	Tweak::boolean("Rocks", "Show preview", &s.showPreview, respawn);
	// The WORLD's rocks (TreeWorld's records + TreeSystem's world set; they need "Trees/World/Enabled" too). A
	// change regenerates every record chunk (the trees give way to the rocks).
	Tweak::boolean("Rocks/World", "Enabled", &s.worldEnabled);
	Tweak::floatVar("Rocks/World", "Density scale", &s.worldRules.densityScale, 0.0f, 8.0f, 0.01f);
	// What the types' `Plains` / `Rugged` (.rock Placement) mean: the steepest ground within ~20 m of a rock - at
	// or below "start" it lies on plains, at or above "full" on fully rugged ground.
	Tweak::floatVar("Rocks/World", "Rugged slope start", &s.worldRules.ruggedSlope.x, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Rocks/World", "Rugged slope full", &s.worldRules.ruggedSlope.y, 0.0f, 2.0f, 0.01f);
	// What their `Valley` means: low ground, where the heights within ~160 m of the rock range over "start" (no
	// valley) .. "full" metres.
	Tweak::floatVar("Rocks/World", "Valley relief start (m)", &s.worldRules.valleyRelief.x, 0.0f, 500.0f, 0.5f);
	Tweak::floatVar("Rocks/World", "Valley relief full (m)", &s.worldRules.valleyRelief.y, 0.0f, 500.0f, 0.5f);
	// (The LOD level each rock draws is the GPU's pick: the "LOD" tweaks - "Force LOD" shows one level.)
	// The rock material (RendererVK EPipelineIndex::LitRock, "Rocks/Material": the climate's terrain bedrock), or
	// flat grey on LitOpaque - the shape alone.
	static constexpr oc::string_view PREVIEW_SHADINGS[] = { "Rock material (climate)", "Flat grey" };
	Tweak::enumVar("Rocks", "Preview shading", &s.previewShading, PREVIEW_SHADINGS, respawn);
	// The meshes only: the types (and so TreeWorld's placement) stay as read.
	Tweak::intVar("Rocks", "Grid resolution", &s.gridResolution, 16, 256, 1.0f);
	Tweak::intVar("Rocks", "Seed", &s.seed, 0, 1000000, 1.0f, respawn);
	Tweak::floatVar("Rocks", "Spacing", &s.spacing, 1.0f, 4.0f, 0.01f, respawn);
}
