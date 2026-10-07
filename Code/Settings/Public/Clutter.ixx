export module Settings.Clutter;

import Core;

// "Clutter": GROUND CLUTTER (Docs/GroundClutterPlan.md) - pebbles, fallen branches, mushrooms and flowers. Procedural's
// ClutterSystem loads the Assets/Clutter/*.clutter types, generates their meshes and bakes the FOREST FLOOR MAP;
// RendererVK's ClutterPipeline places every object on the GPU each frame (clutter_cull.cs.glsl) and draws it. The
// renderer half is UBO-driven (u_clutter_*): live.
export struct ClutterSettings
{
	bool enabled = true;
	bool reload = false;            // button: re-read the .clutter files, regenerate the meshes (ClutterSystem clears it)
	float densityScale = 1.0f;      // x every type's density
	float rangeScale = 2.0f;        // x every type's `Range` (capped by the ground table: Renderer::groundRange)
	float patchSize = 4.0f;         // m: the candidate patches (CLUTTER_CANDIDATES ranked points each)
	float growBand = 0.3f;          // an object shrinks into the ground over this fraction of its keep range (no pops)
	float rangeFade = 0.25f;        // the last fraction of a type's range over which its density fades to none
	// LOD: the projected size (the object's radius / its distance) at which a mesh switches to level 1 / 2.
	float lod1Size = 0.015f;
	float lod2Size = 0.005f;
	// Flowers (grass_wind.inc.glsl stems): their LOD distances (m) - stem segments 4 / 2 / 1, petals full / half / a third.
	float flowerLod1Distance = 3.0f;
	float flowerLod2Distance = 15.0f;
	float flowerTransmission = 0.8f; // the sun through a petal from behind
	float flowerRoughness = 0.55f;
	float contactDarkening = 0.6f;   // pebbles, branches, mushrooms: the ambient at their foot
	float contactHeight = 0.08f;     // m: the band over which it fades out
	// THE FOREST FLOOR MAP (ClutterSystem: the trees' and rocks' records near the camera, splatted on a job): re-baked
	// when the camera leaves this far from the map's centre, or when the records around it change.
	float floorRebakeDistance = 40.0f;
	int meshResolution = 20;         // pebble SDF cells along the longest axis (regenerates the meshes)
};

export namespace Settings
{
	void registerClutter(ClutterSettings& s);
}
