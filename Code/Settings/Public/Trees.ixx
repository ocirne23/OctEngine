export module Settings.Trees;

import Core;

// "Trees": Procedural's TreeSystem (the species, the world set) and "Trees/World": its TreeWorld (every tree of the
// terrain ring as records). Docs/Context/Procedural/Trees.md.

export struct TreeSettings
{
	bool enabled = true;
	bool reload = false;             // button: re-read the .tree files, regenerate, respawn (TreeSystem clears it)
	bool respawn = false;            // button: build the world set again (TreeSystem clears it)
	bool regenerateTextures = false; // button: regenerate the species textures over the files on disk
	bool compressTextures = true;    // BC: bark BC1 + BC5 normal, leaves BC3, the shadow masks and the voxel opacity BC4 (reloads)
	float sizeVariation = 0.6f;       // extra per-tree scale on top of the species range: x 2^(+-this), log-uniform
	float bushesPerTree = 4.0f;       // `Kind Bush` species around each tree whose species has no `Undergrowth` block (the fraction by chance)
	float bushShadowDistance = 100.0f; // bushes cast no sun shadow beyond this from the cascades' centre (m); 0 = no limit
	int nearRadius = 3;               // chunks (Chebyshev) around the camera chunk whose trees are expanded
	int worldCapacity = 600000;       // the world set's piece slots (trees + bushes)
	int expandPerFrame = 2;           // expanded chunks added to the set per frame
	// COLLIDERS ("Trees/World/Colliders"): static bodies on the "Vegetation" layer for the expanded trees (a
	// cylinder each: the trunk) and rocks (their LOD chain's last level as a mesh) within colliderRadius of the camera.
	bool colliders = true;
	float colliderRadius = 80.0f;     // m (horizontal); a body goes again 10 % past it
	int collidersPerFrame = 64;       // bodies created per frame (removals are not budgeted)
	float farDistanceScale = 1.0f;    // x every species' `Billboard Distance` (the mesh -> voxels switch); 0 = the meshes at every distance
	float fadeWidthScale = 2.0f;      // x every species' `Billboard FadeWidth`: the mesh <-> voxels blend band (respawns)
	float voxelStart = 0.0f;          // m: the TREES' (not the bushes') mesh -> voxels blend starts here; 0 = each
	                                  // species' `Billboard Distance` x farDistanceScale - FadeWidth / 2 (respawns)
	// THE VOXEL TIER: past its species' band the main pass draws a baked variant's prefiltered voxel grid, marched per
	// pixel (LitTreeVoxel), in its meshes' place. Off: the meshes at every distance. The shadows and the rays take the
	// variant's shadow billboard either way. Reloads.
	bool voxelImpostors = true;
	int voxelResolution = 3;          // a tree's grid: 0 = 32^3, 1 = 64^3, 2 = 128^3, 3 = 256^3 voxels at level 0; a bush's: a quarter (reloads)
	bool forceFar = false;            // debug: every plant as its voxels
};

export struct TreeWorldSettings
{
	bool enabled = true;
	int seed = 1;
	float cellSize = 5.0f;
	float densityScale = 1.5f;
	float climateSharpness = 4.0f;  // the species pick by climate fit ^ this (0 = every fitting species alike)
	float climateFadeStart = 0.10f; // a species' climate fit fades to 0 between these (fractions of its peak): no tail
	float climateFadeEnd = 0.25f;
	// GRASSLANDS: open land with no trees, whatever the climate - about this share of the land, in patches of about
	// grasslandSize (m). The forests between them keep their full density (only a narrow edge thins).
	float grasslandShare = 0.5f;
	float grasslandSize = 800.0f;
	float grasslandTrees = 0.5f; // 0..1: the trees left IN the grasslands - copses, lone trees, a thinning forest edge
	// FOREST TYPES: stands of about forestTypeSize (m) where one species dominates; the others x forestMinority there.
	float forestTypeSize = 600.0f;
	float forestMinority = 0.05f;

	int maxGenJobs = 0;             // concurrent chunk placements (Low pump jobs, preemptible); 0 = one per job worker
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
