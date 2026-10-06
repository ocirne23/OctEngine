export module Settings.Scatter;

import Core;

// "Scatter": Procedural's ScatterSystem (trees, rocks and grass from the scatter rules).
export struct ScatterSettings
{
	bool  enabled = false;
	int   seed = 777;
	float cellSize = 64.0f;
	float densityScale = 1.0f;     // global multiplier on every rule's density
	int   maxGenJobs = 2;
	float viewScale = 1.0f;        // global multiplier on every rule's view distance
	int   maxSpawnsPerFrame = 768; // instance node spawns per frame (group activations spread out)
};

export namespace Settings
{
	void registerScatter(ScatterSettings& s);
}
