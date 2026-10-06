export module Settings.Rocks;

import Core;
import Core.glm;

// "Rocks": Procedural's RockSystem (Docs/RockRenderingPlan.md).
export namespace Procedural
{
	// The WORLD's rules over every rock type ("Rocks/World" tweaks -> TreeWorld's placement): what the types' ground
	// keys (RockPlacementDesc) mean.
	struct RockWorldDesc
	{
		float densityScale = 1.0f;
		// PLAINS / RUGGED: the steepest ground within ~20 m of the rock (rise / run) - at or below .x plains, at or
		// above .y fully rugged.
		glm::vec2 ruggedSlope{ 0.08f, 0.4f };
		// VALLEY: the height range within ~160 m of the rock (m) - at or below .x no valley, at or above .y a full one.
		glm::vec2 valleyRelief{ 15.0f, 60.0f };
		bool operator==(const RockWorldDesc&) const = default;
	};
}

export struct RockSettings
{
	bool enabled = true;
	bool reload = false;       // button: re-read the .rock files, regenerate, respawn (RockSystem clears it)
	bool respawn = false;      // button: respawn the preview in front of the camera (RockSystem clears it)
	bool showPreview = false;  // the rows in front of the camera (off: the world's rocks only)
	bool worldEnabled = true;  // rock records in the world (TreeWorld) + rock types in TreeSystem's world set
	Procedural::RockWorldDesc worldRules;
	int previewShading = 0;    // 0 = the rock material (LitRock: the climate's bedrock), 1 = flat grey (the shape alone)
	int gridResolution = 32;   // surface-nets cells along the longest axis (remeshes)
	int seed = 1;
	float spacing = 1.4f;      // x the row's largest rock size
};

export namespace Settings
{
	void registerRocks(RockSettings& s);
}
