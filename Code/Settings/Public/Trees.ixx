export module Settings.Trees;

import Core;

// "Trees": Procedural's TreeSystem (the species, the preview grove, the world set) and "Trees/World": its TreeWorld
// (every tree of the terrain ring as records). Docs/TreeRenderingPlan.md.

// "Trees/Grove type": "Mixed" alternates every loaded species; the rest select one by its TreeSpecies name. The names
// are fixed here (a tweak enum registers before the species load) - add a new species' name.
export inline constexpr oc::string_view TREE_GROVE_TYPES[] = { "Mixed", "Oak", "Pine", "Acacia", "Willow" };

export struct TreeSettings
{
	bool enabled = true;
	bool reload = false;             // button: re-read the .tree files, regenerate, respawn (TreeSystem clears it)
	bool respawn = false;            // button: respawn the preview in front of the camera (TreeSystem clears it)
	bool regenerateTextures = false; // button: regenerate the species textures over the files on disk
	bool showLibrary = true;         // also reloads: without it the module / trunk billboards are never uploaded
	bool compressTextures = true;    // BC: bark BC1 + BC5 normal, leaves / billboards / card atlas BC3 (reloads)
	int gridSize = 1;
	float spacing = 11.0f;
	float positionJitter = 0.8f;      // random offset per tree, x spacing (1 = anywhere in its cell, > 1 overlaps)
	float sizeVariation = 0.6f;       // extra per-tree scale on top of the species range: x 2^(+-this), log-uniform
	float bushesPerTree = 4.0f;       // `Kind Bush` species scattered around each grove tree (the fraction by chance)
	float bushShadowDistance = 100.0f; // bushes cast no sun shadow beyond this from the cascades' centre (m); 0 = no limit
	int seed = 1;
	int groveType = 1;                // 0 = mixed (species alternate), else TREE_GROVE_TYPES[i] by species name (1 = Oak)
	// World mode.
	int nearRadius = 3;               // chunks (Chebyshev) around the camera chunk whose trees are expanded
	int worldCapacity = 600000;       // the dynamic set's piece slots (trees + bushes)
	int expandPerFrame = 2;           // expanded chunks added to the set per frame
	int farMode = 0;                  // 0 = billboards, 1 = none (reloads)
	int billboardViews = 0;           // 0 = 2 views (back faces show the front through the card), 1 = 4 (reloads)
	float farDistanceScale = 4.0f;    // x every species' billboard distance; 0 = off
	float branchCardDistance = 0.4f;  // the mid tier (GPU path): branch cards from this x the billboard distance; 0 = off
	bool forceFar = false;            // debug: every module as its far representation
	bool gpuExpansion = true;         // G4: pieces expanded on the GPU (one set) instead of a CPU push per node
};

export struct TreeWorldSettings
{
	bool enabled = true;
	int seed = 1;
	float cellSize = 5.0f;
	float densityScale = 1.0f;
	float climateSharpness = 4.0f;  // the species pick by climate fit ^ this (0 = every fitting species alike)
	float climateFadeStart = 0.10f; // a species' climate fit fades to 0 between these (fractions of its peak): no tail
	float climateFadeEnd = 0.25f;
	int maxGenJobs = 2;
	int poolMB = 128;               // the GPU record pool (MB; fixed - a change drains the GPU and regenerates)
	int uploadKB = 1024;            // record bytes uploaded per frame
	int keepRadius = 4;             // chunks (Chebyshev) whose records the CPU keeps
	bool reloadSpecies = false;     // button: re-read the Placement blocks, regenerate (TreeWorld clears it)
	bool logStats = false;          // button (TreeWorld clears it)
};

export namespace Settings
{
	void registerTrees(TreeSettings& s);
	void registerTreeWorld(TreeWorldSettings& s);
}
