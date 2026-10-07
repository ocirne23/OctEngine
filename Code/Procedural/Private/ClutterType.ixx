export module Procedural:ClutterType;

import Core;
import Core.glm;
import RendererVK; // RendererVKLayout::EClutterKind / EFlowerHead

import :RockType;

// One GROUND CLUTTER type, authored as a `.clutter` text asset (Assets/Clutter/; Docs/GroundClutterPlan.md 5): what it
// is (the SHAPE - a pebble, a fallen branch, a mushroom, a flower) and where it grows (its Placement blocks: the
// climate, the terrain, and the FOREST FLOOR - canopy, trunks, rocks). Placement is evaluated on the GPU every frame
// (clutter_cull.cs.glsl); every Placement block is its own GPU type over the same meshes, so their densities ADD.
export namespace Procedural
{
	using EClutterKind = RendererVKLayout::EClutterKind;
	using EFlowerHead = RendererVKLayout::EFlowerHead;

	enum class EMushroomCap : uint8
	{
		Dome,   // a round cap (bolete, fly agaric)
		Flat,   // a wide flat cap on a tall stem (parasol)
		Cone,   // a pointed cap (ink cap, liberty cap)
		Funnel, // a cap curved up into a funnel (chanterelle)
	};

	// One placement rule. The density (per m^2) x the climate fit x each term mix(a, b, measure 0..1). Default
	// terms (1, 1): the measure does not matter.
	struct ClutterPlacementDesc
	{
		float density = 0.0f;                       // per m^2 at full fit; 0 = the block places nothing
		glm::vec2 temperature{ -1000.0f, 1000.0f }; // C, the ideal range (default: any climate)
		glm::vec2 precipitation{ 0.0f, 100000.0f }; // mm/yr
		float climateWidth = 0.06f;                 // the falloff outside the box (normalized climate space)
		float clusterSize = 0.0f;                   // m, 0 = no patches
		float clusterCoverage = 0.5f;
		float maxSlope = 1.0f;                      // rise / run under the object
		float minAltitude = 0.2f;                   // m above the local water
		glm::vec2 grass{ 1.0f };                    // the terrain's grass cover: bare .. full
		glm::vec2 crag{ 1.0f };                     // bedrock showing: none .. full
		glm::vec2 beach{ 1.0f };                    // the beach band: none .. full
		glm::vec2 canopy{ 1.0f };                   // tree crowns overhead: open .. full shade
		glm::vec2 trunk{ 1.0f };                    // a trunk's foot: far .. at it
		glm::vec2 rock{ 1.0f };                     // a rock's foot: far .. at it
		glm::vec2 wet{ 1.0f };                      // the climate's humidity: dry .. wet
		glm::vec4 ring{ 0.0f, 0.5f, 30.0f, 0.3f };  // fairy rings: radius (0 = none), width, cell (m), chance per cell
	};

	struct ClutterTypeDesc
	{
		oc::string name;
		oc::string path;
		EClutterKind kind = EClutterKind::Pebble;
		uint32 seed = 1;
		int variantCount = 4;          // meshes (not flowers)
		glm::vec2 scale{ 0.1f, 0.2f }; // m: the mesh's nominal size 1 x this (a flower: x its stem and head)
		float range = 60.0f;           // m from the camera (x "Clutter/Range scale")
		float sink = 0.1f;             // fraction of the height below the ground
		float align = 1.0f;            // 0 upright .. 1 follows the ground's normal
		glm::vec3 color{ 1.0f };       // sRGB: pebble = a tint on the climate's bedrock; branch = bark; mushroom = cap; flower = petals
		glm::vec3 color2{ 0.5f };      // sRGB: branch = end grain; mushroom = stem + gills; flower = its centre
		float roughness = 0.8f;
		int lodTriangles = 160;        // level 0 (pebble, mushroom); each further level about a quarter

		// Pebble: a small RockType (every .rock shape key; Scale / Variants / Sink / Align are the clutter type's).
		RockTypeDesc rock;
		// Branch (a fallen stick along mesh X, length 1): radius / length at the thick end, side twigs, bend.
		float thickness = 0.035f;
		int twigs = 2;
		float bend = 20.0f;            // degrees over the length
		// Mushroom (height 1 = the tallest of its group): the cap, the stem, a group, the spots.
		EMushroomCap cap = EMushroomCap::Dome;
		glm::vec2 capSize{ 0.45f, 0.3f }; // radius, height (x the mushroom's height)
		float stemRadius = 0.09f;
		int group = 1;                 // 1..5 mushrooms per variant, each smaller, around the first
		float spots = 0.0f;            // 0..1: white spots on the cap
		// Flower (no mesh: built in the vertex shader).
		EFlowerHead head = EFlowerHead::Radial;
		int petals = 6;
		float open = 10.0f;            // degrees: the petals' tilt up from the head's plane (negative: swept back)
		float petalWidth = 0.45f;      // x the head size
		float stemHeight = 0.35f;      // m at scale 1
		float headSize = 0.03f;        // m at scale 1 (a petal's length)

		oc::vector<ClutterPlacementDesc> placements; // the blocks with a density
	};

	// Parses an Assets/Clutter/*.clutter file (AssetParser syntax, a `ClutterType <name>` root). Clamps what would break
	// the generators. MAIN THREAD under FileSystem::AllowMainThreadIO, or a job.
	bool loadClutterType(const oc::string& path, ClutterTypeDesc& out, oc::string& outError);
}
