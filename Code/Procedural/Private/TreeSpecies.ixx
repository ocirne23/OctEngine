export module Procedural:TreeSpecies;

import Core;
import Core.glm;

// One tree species, authored as a `.tree` text asset (Assets/Trees/). The species does not describe one
// tree: it describes a LIBRARY of pieces (trunks with attach slots, branch modules) and the rules the
// composite function uses to assemble a unique tree per seed from them. See Docs/Context/Procedural/Trees.md.
export namespace Procedural
{
	enum class ETreeCrownShape : uint8
	{
		Ellipsoid, // widest in the middle of the crown (broadleaf)
		Cone,      // widest at the bottom (conifer)
		Umbrella,  // widest at the top (acacia, umbrella pine)
		Column,    // narrow and even (poplar)
		Dome,      // wide from the bottom up, rounding in only near the top: a broad flat-topped crown (old cedar)
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
		Mat,     // a ragged patch of ClusterLeaves short needle shoots filling the cell (foliage pads: no single outline)
	};

	// How one branch grows - shared by the trunk, the module root and every sub-branch level.
	struct TreeBranchShape
	{
		float curve = 20.0f;       // total smooth bend over the length (degrees)
		float curveVar = 10.0f;
		float upAttract = 0.0f;    // bend toward world-up over the length; negative droops
		float droop = 0.0f;        // degrees turned toward straight DOWN over the length, never past it (weeping strands)
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
		float lengthVar = 0.0f;    // each child is a random 0..this fraction shorter (weeping strands end at layered heights)
		float angle = 45.0f;       // degrees from the parent direction
		float angleVar = 10.0f;
		float radius = 0.6f;       // child base radius / parent radius at the attach point
		float rise = 0.0f;         // 0..1: the child's start direction blended toward world-up (strands that arch over, then hang)
		float flat = 0.0f;         // 0..1: the child's side direction blended into the horizontal - flat sprays (cedar sheets)
		TreeBranchShape shape;
	};

	// WORLD placement (`Placement` block; TreeWorld): the expected trees per hectare in the species' IDEAL climate - a
	// box of mean annual temperature x annual precipitation - falling off as a Gaussian of the distance OUTSIDE the box,
	// in the normalized (temperature, precipitation) space the clutter types and the terrain textures use, then gated by
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
		float spacing = 0.0f;          // m: no other tree within max(this, its spacing) of one with a higher priority
	};

	struct TreeSpeciesDesc
	{
		oc::string name;
		oc::string path;
		uint32 seed = 1;
		glm::vec2 scale{ 0.85f, 1.15f }; // uniform tree scale range
		bool bush = false;               // `Kind Bush`: scattered under the grove's trees, never one of its trees
		oc::string climate;              // `Climate <name>`: a bush grows around the trees of its climate only
		int weight = 1;                  // `Weight n` (bushes, 1..8): how often its climate picks it - n entries in each tree's bush list
		// `Undergrowth` block (trees): the bushes around each of its trees - `Bushes` per tree (the fraction by chance) out
		// to `Radius` m from it. < 0 = "Trees/Bushes per tree" / 0.75 x "Trees/Spacing (m)".
		float undergrowthBushes = -1.0f;
		float undergrowthRadius = -1.0f;
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
		// Multi-stem trunks (birch clumps, forked cedars): each trunk piece grows min..max STEMS from around one base,
		// each Radius thick, leaning out by StemLean, its base StemSpread m from the centre; the first stem is the
		// tallest. Every stem carries its own crown (Crown Slots split between them). 1 1 = one trunk.
		glm::ivec2 trunkStems{ 1, 1 };
		float trunkStemLean = 12.0f;  // degrees from vertical, outward
		float trunkStemSpread = 0.25f; // m
		float trunkFork = 0.0f;       // 0..0.9: > 0 = ONE trunk up to this fraction of the height, the stems fork from its top
		float trunkLeaderLean = 0.3f; // a fork's first stem (it continues the trunk): this x StemLean (0.3 = a central
		                              // leader; 1 = it leans like the others, no leader)
		// The stem's TOP splits (Trunk Split count depth angle span): its top `span` of the height becomes `depth` rounds of
		// `count` branches fanning out `angle` degrees off their parent (and pulled toward up), each thinner and shorter;
		// the Crown Top whorl at every final tip. count < 2 = off.
		int trunkSplitCount = 0;
		int trunkSplitDepth = 2;
		float trunkSplitAngle = 30.0f;
		float trunkSplitSpan = 0.3f;
		// 1 = the splits FILL a half sphere of Crown Radius over the stem's end: every split divides its parent's cone of
		// directions among its children, which grow toward points in their own sub-cones (Split angle unused); 0 = fan.
		float trunkSplitFill = 0.0f;
		float trunkSplitFillHeight = 1.0f;
		int trunkSplitShoots = 0; // fill: short side shoots along every inner split branch, each ending in a leafy whorl // fill: the dome's height / its radius (1 = a half sphere, > 1 = an egg)
		float trunkSplitTipCurve = 1.0f; // the final round bends toward its direction + this x up: 1 = up (level sheets on
		                                 // top, cedar), < 0 = out sideways (elm)
		TreeBranchShape trunkShape{ .curve = 8.0f, .curveVar = 4.0f, .rings = 12, .sides = 10 };

		// Crown: where and how long the attach slots are
		ETreeCrownShape crownShape = ETreeCrownShape::Ellipsoid;
		float crownStart = 0.35f;   // fraction of the trunk height where slots begin
		float crownRadius = 4.0f;   // horizontal reach at the widest point (m)
		int slots = 16;
		int crownTiers = 1;         // > 1: the slots grouped into this many whorls up the crown (layered crowns)
		float crownTierSpread = 0.35f; // the fraction of its band each tier's slots spread over
		float crownTierJitter = 0.0f;  // 0..1: each tier's height off its even spacing (x half a band) and its reach x 1 +- 0.35 this
		float slotFill = 0.9f;      // probability that a slot gets a module
		glm::vec2 slotAngle{ 70.0f, 40.0f }; // degrees from up at the crown bottom / top
		float slotAngleVar = 10.0f;
		bool leader = false;        // one extra slot at the top continues the trunk upward
		int crownTopSlots = 0;      // > 0: a WHORL of this many level limbs at every stem's tip - a flat sheet on top (cedar)
		float crownTopReach = 0.6f; // their length, x the crown radius
		float crownTopAngle = 90.0f; // their angle from up (deg; 90 = level sheets, less = rising limbs that arch over)
		// Crown SplitLimbs count angle length (needs Trunk Split): per stem `count` side limbs `angle` deg from up,
		// `length` x the stem height long, each splitting like the stem's top (one round fewer) with whorls at its tips.
		int crownSplitLimbs = 0;
		float crownSplitLimbAngle = 60.0f;
		float crownSplitLimbLength = 0.25f;
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
		float leafAlign = 0.0f;     // 0 = leaves stick out of the branch facing up, 1 = they lie along it (weeping curtains)
		float leafTop = 0.0f;       // 0..1: the leaves' side of the branch blended toward its TOP (sheets on top of the limbs)
		// Cluster cards raised along the module's up (free of their branch): x = a lift for every card (x the card size),
		// y = a DOME on top (x the module length) - highest along the limb, falling to 0 at the sheet's edges and its end.
		glm::vec2 leafLift{ 0.0f, 0.0f };
		// 0..1: the card's FACE turned toward up. With Align 1 a card lies along its branch, straddling it - facing up it
		// covers the branch from above (its drawn spray over the real twig); Align alone faces it out sideways (curtains).
		float leafFaceUp = 0.0f;
		// FOLIAGE PADS (Cluster; replaces the per-branch leaves): per module `padCount` big cards scattered through a flat
		// DOME over its limb - padWidth x the module length either side, padHeight x it at the dome's top - mostly level
		// (a cedar's tiers: clouds of foliage, the branches inside them). 0 = off.
		int padCount = 0;
		float padWidth = 0.35f;
		float padHeight = 0.08f;
		float padTilt = 0.25f;      // the cards' random tilt off level (a vector of up to this x unit, added to up)
		float padStart = 0.15f;     // where along the limb the pad begins; its cards denser toward the limb's end
		ETreeClusterStyle clusterStyle = ETreeClusterStyle::Leaves; // Cluster: what the texture draws
		int clusterLeaves = 12;     // Cluster: leaves drawn per texture variant (Needles: side shoots per branch;
		                            // Pinnate: pinna pairs per compound leaf)
		int clusterShoots = 1;      // Needles / Pinnate: branches / compound leaves fanned out from the card's base
		float clusterLeafSize = 0.3f; // Cluster: leaf (Needles: needle) length in the texture, a fraction of the card length
		float clusterLeafWidth = 0.24f; // Cluster Leaves: a texture leaf's half width / its length (0.24 narrow, 0.45 oval)
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
		// Birch-like MARKS in markColor: short dark horizontal dashes (lenticels) and dark rough patches.
		float barkLenticels = 0.0f; // 0..1: the share of the dash lattice's cells that hold one
		float barkPatches = 0.0f;   // 0..1: about this share of the bark is dark patches
		glm::vec3 markColor{ 0.09f, 0.08f, 0.07f };
		// Bark SECTIONS: about this share of the bark (soft-edged areas, longer along the branch) tinted to sectionColor
		// (a birch's brown, older bark), under the marks.
		float barkSections = 0.0f;
		glm::vec3 sectionColor{ 0.45f, 0.33f, 0.24f };

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
		glm::vec3 twigColor{ -1.0f }; // `Color Twig`: the twigs drawn in the leaf texture (< 0 = barkColor)
		glm::vec3 leafTwigColor() const { return twigColor.x >= 0.0f ? twigColor : barkColor; }
		// The FAR-TREE VOLUME's colour: `Color Far` (sRGB like Leaf; < 0 = the leaf texture's mean) x farAlbedoScale.
		glm::vec3 farColor{ -1.0f };
		float farAlbedoScale = 1.0f;
	};

	bool loadTreeSpecies(const oc::string& path, TreeSpeciesDesc& out, oc::string& outError);
}
