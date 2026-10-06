export module Procedural:RockType;

import Core;
import Core.glm;
export import Settings.Rocks; // RockWorldDesc

// One rock type, authored as a `.rock` text asset (Assets/Rocks/). The type sets the SHAPE only: a signed distance
// field built per variant from its seed. The colour comes from the climate at the rock (the terrain's bedrock
// materials), never from the type. See Docs/RockRenderingPlan.md.
export namespace Procedural
{
	enum class ERockShape : uint8
	{
		Boulder, // superellipsoid, default squareness 2.2 (near an ellipsoid)
		Block,   // superellipsoid, default squareness 3 (a rounded block)
		Pillar,  // a STANDING body whose width follows a profile over its height (a spire, a top-heavy monolith), on a flat floor
	};

	// A rock's largest size (m, its longest axis: `Scale` is clamped to it): the far volume's layer is 22 m high
	// (RendererVK "Far height"), and a taller rock would stick out of it.
	constexpr float ROCK_MAX_SIZE = 22.0f;
	constexpr int ROCK_MAX_PILE = 4;       // blocks per pile / pillars per group (RockShape's block arrays)
	constexpr int ROCK_PROFILE_POINTS = 5; // a Pillar's widths, base to top, evenly spaced

	// One world placement RULE (Docs/RockRenderingPlan.md 6): TreeWorld's rock records. A type has any number of them
	// (`Placement` blocks) and their densities ADD - a rule for everywhere plus one for a climate it is common in, each
	// with its own ground.
	struct RockPlacementDesc
	{
		float density = 0.0f;                         // per ha at full fit; 0 = the rule places nothing
		glm::vec2 temperature{ -1000.0f, 1000.0f };   // C, the ideal range (default: any climate)
		glm::vec2 precipitation{ 0.0f, 100000.0f };   // mm/yr
		float climateWidth = 0.06f;
		// The slope UNDER the rock (rise / run), a soft band: none below .x, full over .y .. .z, none above .w (a hard
		// edge where two are equal). A boulder rolls off a slope: `Slope 0 0 0.12 0.5`.
		glm::vec4 slope{ 0.0f, 0.0f, 2.0f, 2.0f };
		float crag = 0.0f;                            // affinity to exposed bedrock, 0..1
		float talus = 0.0f;                           // affinity to the foot of steep slopes, 0..1
		float clusterSize = 0.0f;                     // m, 0 = no patches
		float clusterCoverage = 0.5f;
		glm::vec2 altitude{ 1.5f, 100000.0f };        // m above the local water level
		// THE GROUND AROUND THE ROCK - density multipliers (TreeWorld's placeChunk has the measures):
		//   plains          flat ground: nothing steep within ~20 m (RockWorldDesc::ruggedSlope)
		//   rugged.x / .y   rugged ground, at the LOW end of the heights within ~20 m (a cliff's bottom, the foot of a
		//                   hillside) / at their HIGH end (a cliff's top, a crest)
		//   valley          low ground with higher ground within ~160 m (a valley's floor, the foot of a mountain):
		//                   there the multiplier IS this, whatever the three above say. < 0 = no valley rule.
		float plains = 1.0f;
		glm::vec2 rugged{ 1.0f };
		float valley = -1.0f;
	};

	// RockWorldDesc (the WORLD's rules over every rock type, "Rocks/World" tweaks) lives in Settings.Rocks.

	struct RockTypeDesc
	{
		oc::string name;
		oc::string path;
		uint32 seed = 1;
		glm::vec2 scale{ 2.0f, 4.0f }; // m, the longest axis
		int variantCount = 6;
		float sink = 0.15f;            // fraction of the height below the ground
		float align = 1.0f;            // 0 upright .. 1 follows the ground normal (RockSystem::groundTransform)

		// --- Shape (generated at a nominal size of 1 = the longest axis; every length below is a fraction of it) ---
		ERockShape shape = ERockShape::Boulder;
		glm::vec3 aspect{ 1.0f, 0.7f, 0.8f };    // axis ratios x y z (the largest is normalized to 1)
		glm::vec3 aspectVar{ 0.15f, 0.15f, 0.15f };
		float round = 0.05f;                     // edge rounding (fracture planes)
		float squareness = 0.0f;                 // the superellipsoid exponent (2 ellipsoid .. 8 near a box; a Pillar's section); 0 = the shape's default
		float erosion = 0.08f;                   // weathering: every convex edge and corner rounded to this radius
		float warpAmplitude = 0.04f;             // low-frequency domain warp: flat faces bulge, straight edges bend
		float warpFrequency = 1.5f;
		// Pillar: its width at ROCK_PROFILE_POINTS heights, base to top (the widest = the Aspect's width), each x
		// (1 +- profileVar) per variant; and up to `group` of them standing together, each later one ~x groupShrink.
		float profile[ROCK_PROFILE_POINTS] = { 1.0f, 0.8f, 0.6f, 0.4f, 0.15f };
		float profileVar = 0.15f;
		int group = 1;
		float groupShrink = 0.7f;
		int pile = 1;                          // Boulder / Block: blocks heaped together; 1 = one body
		float pileShrink = 0.75f;                // each block's size x ~this of the one before
		int fractureCount = 0;                   // random plane cuts: flat faces with edges
		float fractureDepth = 0.25f;             // how deep a cut reaches, fraction of the support distance
		float strataSpacing = 0.0f;              // horizontal layers; 0 = none
		float strataDepth = 0.02f;               // groove depth between layers
		float strataVar = 0.5f;                  // per-layer in / out step, x depth
		float noiseAmplitude = 0.04f;
		float noiseFrequency = 3.0f;             // cycles over the nominal size
		int noiseOctaves = 4;
		float noiseStretch = 1.0f;               // the noise's features x this along Y: > 1 vertical flutes, < 1 flat layers
		float ridged = 0.0f;                     // 0 smooth fbm .. 1 ridged fbm
		int pitCount = 0;                        // dents, mostly on the up-facing side
		float pitSize = 0.06f;
		float pitDepth = 0.02f;
		float splitChance = 0.0f;                // probability of one crack through the rock
		float splitGap = 0.02f;

		// --- Meshes ---
		int lodTriangles = 2000;                 // LOD 0's triangle count; each further level a quarter of the one before
		float resolution = 1.0f;                 // x "Rocks/Grid resolution": a type with thin features (strata, a spire's tip) asks for more

		oc::vector<RockPlacementDesc> placements; // the rules with a density; empty = never placed in the world
	};

	// Parses an Assets/Rocks/*.rock file (AssetParser syntax, a `RockType <name>` root). Clamps what would break
	// the generator. MAIN THREAD under FileSystem::AllowMainThreadIO, or a job.
	bool loadRockType(const oc::string& path, RockTypeDesc& out, oc::string& outError);
}
