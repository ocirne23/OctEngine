export module Procedural:TreeSpecies;

import Core;
import Core.glm;

// One tree species, authored as a `.tree` text asset (Assets/Trees/). The species does not describe one
// tree: it describes a LIBRARY of pieces (trunks with attach slots, branch modules) and the rules the
// composite function uses to assemble a unique tree per seed from them. See Docs/TreeRenderingPlan.md.
export namespace Procedural
{
	enum class ETreeCrownShape : uint8
	{
		Ellipsoid, // widest in the middle of the crown (broadleaf)
		Cone,      // widest at the bottom (conifer)
		Umbrella,  // widest at the top (acacia, umbrella pine)
		Column,    // narrow and even (poplar)
	};

	enum class ETreeLeafType : uint8
	{
		Single,  // one double-sided diamond per leaf, solid colour
		Cluster, // alpha-tested cards showing a twig with many leaves (procedural texture, TreeLeafTexture)
	};

	enum class ETreeClusterStyle : uint8
	{
		Leaves,  // a twig with broad leaves (ClusterLeaves of them)
		Needles, // a conifer shoot: a main shoot + ClusterLeaves side shoots, all densely set with needles
		Pinnate, // bipinnate compound leaves (acacia): a rachis with ClusterLeaves pairs of pinnae lined with leaflets
	};

	// How one branch grows - shared by the trunk, the module root and every sub-branch level.
	struct TreeBranchShape
	{
		float curve = 20.0f;       // total smooth bend over the length (degrees)
		float curveVar = 10.0f;
		float upAttract = 0.0f;    // bend toward world-up over the length; negative droops
		float wobble = 0.0f;       // random direction change per segment (degrees) - less straight
		float elbows = 0.0f;       // average number of sharp direction changes along the branch
		float elbowAngle = 30.0f;  // degrees per elbow
		float elbowAngleVar = 10.0f;
		float elbowUpBias = 0.5f;  // probability that an elbow turning DOWN is mirrored to turn up instead
		float elbowMinElevation = -15.0f; // degrees vs horizontal: an elbow never turns the branch lower than this
		glm::vec2 elbowRange{ 0.0f, 1.0f }; // fraction of the length where elbows (and their stubs) may sit
		float stubs = 0.0f;       // probability that an elbow carries a snapped-off branch stub
		int rings = 5;             // rings along the length (each elbow adds 2 more to round it)
		int sides = 5;
	};

	// One recursion level of sub-branches inside a branch module (level 1 = children of the module root).
	struct TreeBranchLevel
	{
		int count = 6;             // children per parent branch
		float start = 0.2f;        // first child at this fraction of the parent's length
		float length = 0.5f;       // child length / parent length
		float lengthTaper = 0.5f;  // children near the parent's tip are up to this fraction shorter
		float angle = 45.0f;       // degrees from the parent direction
		float angleVar = 10.0f;
		float radius = 0.6f;       // child base radius / parent radius at the attach point
		TreeBranchShape shape;
	};

	// WORLD placement (`Placement` block; TreeWorld): the expected trees per hectare in the species' IDEAL climate - a
	// box of mean annual temperature x annual precipitation - falling off as a Gaussian of the distance OUTSIDE the box,
	// in the normalized (temperature, precipitation) space the scatter rules and the terrain textures use, then gated by
	// cluster noise, slope and the height above the water. density 0 = never placed.
	struct TreePlacementDesc
	{
		float density = 0.0f;                    // per hectare inside the ideal climate
		glm::vec2 temperature{ 10.0f, 20.0f };   // the ideal range (C)
		glm::vec2 precipitation{ 800.0f, 2200.0f }; // the ideal range (mm/yr)
		float climateWidth = 0.06f;              // sigma of the falloff outside the box, in the normalized climate space
		float clusterSize = 0.0f;      // m: the size of the forest patches; 0 = no patches
		float clusterCoverage = 0.5f;  // ~the fraction of the land the patches cover
		float maxSlope = 0.6f;         // rise / run
		glm::vec2 altitude{ 1.5f, 1e9f }; // m above the local water level
	};

	struct TreeSpeciesDesc
	{
		oc::string name;
		oc::string path;
		uint32 seed = 1;
		glm::vec2 scale{ 0.85f, 1.15f }; // uniform tree scale range
		bool bush = false;               // `Kind Bush`: scattered under the grove's trees, never one of its trees
		oc::string climate;              // `Climate <name>`: a bush grows around the trees of its climate only
		TreePlacementDesc placement;

		// Trunks
		int trunkCount = 4;
		glm::vec2 trunkHeight{ 8.0f, 12.0f };
		float trunkRadius = 0.3f;   // base radius (m)
		float trunkTaper = 0.25f;   // tip radius / base radius
		float trunkFlare = 0.3f;    // extra radius at the ground (fraction), fades over the lowest 15%
		float trunkSink = 0.5f;     // the trunk continues this far (m) below its base, into sloped ground
		int trunkLobes = 0;         // ridges (buttresses) around the trunk; 0 = round cross-section
		glm::vec2 trunkLobeDepth{ 0.25f, 0.04f }; // ridge depth as a radius fraction at the base / above LobeHeight
		float trunkLobeHeight = 0.3f; // fraction of the height over which the ridges fade to the top depth
		float trunkTwist = 25.0f;   // degrees the ridge pattern turns over the full height
		TreeBranchShape trunkShape{ .curve = 8.0f, .curveVar = 4.0f, .rings = 12, .sides = 10 };

		// Crown: where and how long the attach slots are
		ETreeCrownShape crownShape = ETreeCrownShape::Ellipsoid;
		float crownStart = 0.35f;   // fraction of the trunk height where slots begin
		float crownRadius = 4.0f;   // horizontal reach at the widest point (m)
		int slots = 16;
		float slotFill = 0.9f;      // probability that a slot gets a module
		glm::vec2 slotAngle{ 70.0f, 40.0f }; // degrees from up at the crown bottom / top
		float slotAngleVar = 10.0f;
		bool leader = false;        // one extra slot at the top continues the trunk upward
		float branchRadius = 0.7f;  // max module base radius / trunk radius at the slot (caps the module scale)

		// Branch modules
		int moduleCount = 12;
		float moduleLength = 0.0f;  // nominal module length (m); 0 = crownRadius
		float moduleRadius = 0.035f; // root base radius / module length
		TreeBranchShape moduleShape{ .curve = 20.0f, .curveVar = 10.0f, .upAttract = 0.1f, .rings = 8, .sides = 6 };
		oc::vector<TreeBranchLevel> levels; // sub-branch levels below the module root

		// Leaves, on the last level's branches. For Cluster, Size / Aspect are the CARD's, and PerBranch counts cards.
		ETreeLeafType leafType = ETreeLeafType::Single;
		bool leafCross = false;     // Cluster: a second card at 90 degrees about the card's stem axis
		float leafNormalBend = 0.7f; // Cluster: 0 = flat card normals, 1 = normals point straight out of the crown
		ETreeClusterStyle clusterStyle = ETreeClusterStyle::Leaves; // Cluster: what the texture draws
		int clusterLeaves = 12;     // Cluster: leaves drawn per texture variant (Needles: side shoots per branch;
		                            // Pinnate: pinna pairs per compound leaf)
		int clusterShoots = 1;      // Needles / Pinnate: branches / compound leaves fanned out from the card's base
		float clusterLeafSize = 0.3f; // Cluster: leaf (Needles: needle) length in the texture, a fraction of the card length
		float leafSize = 0.25f;     // length (m)
		float leafAspect = 0.6f;    // width / length
		int leavesPerBranch = 8;    // per average last-level branch; longer branches get proportionally more
		int leafLevels = 1;         // how many of the deepest tiers carry leaves (1 = the last level only)

		// Procedural bark texture (TreeBarkTexture): plates separated by fissures, tiling once around a branch
		// (u) and once per base circumference along it (v).
		glm::vec2 barkPlates{ 10.0f, 3.0f }; // plates around / along one texture tile (rounded to integers)
		float barkCrack = 0.12f;    // fissure width, as a fraction of a ridge
		float barkBreakup = 0.6f;   // 0 = continuous fissure lines, 1 = mostly short segments
		float barkRelief = 1.0f;    // normal-map strength
		float barkLichen = 0.3f;    // amount of grey-green lichen patches

		// Baked tree variants (bakeTreeVariant): the runtime draws whole merged trees, not composited pieces.
		int variantCount = 4;

		// Piece mesh LODs: the per-level error (a fraction of the piece length, TreeGenerator's PIECE_LODS)
		// is multiplied by this. > 1 switches to coarser levels nearer the camera.
		float lodErrorScale = 1.0f;

		// Branch-module billboards (the far representation, Trees/Far mode): beyond
		// `billboardDistance` (m, 0 = never) a module draws as two crossed cards baked into one
		// billboardSize-square texture: one strip per view (Trees/Billboard views: 2 or 4).
		float billboardDistance = 50.0f;
		int billboardSize = 512;
		float billboardNormalBend = 0.0f; // baked normals bent toward "out of the clump" (0..1): shades like a volume
		float billboardFadeWidth = 10.0f; // m: the mesh <-> billboard crossfade band, centred on billboardDistance
		float billboardAlbedoScale = 1.0f; // x the WHOLE-TREE billboards' colour (linear; applied at load, not baked)
		float billboardTopCardHeight = 0.5f; // the whole tree's horizontal card, as a fraction of its height (0 = bottom, 1 = top)
		glm::vec3 barkColor{ 0.32f, 0.24f, 0.17f };
		glm::vec3 leafColor{ 0.20f, 0.38f, 0.10f };
	};

	bool loadTreeSpecies(const oc::string& path, TreeSpeciesDesc& out, oc::string& outError);
}
