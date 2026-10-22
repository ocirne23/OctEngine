module Procedural;

import Core;
import Core.glm;

import File;

import :TreeSpecies;

namespace
{
	using namespace Procedural;

	void readFloat(const AssetNode& node, const char* key, float& value)
	{
		if (const AssetNode* n = node.find(key))
			value = n->asFloat(0, value);
	}
	void readInt(const AssetNode& node, const char* key, int& value)
	{
		if (const AssetNode* n = node.find(key))
			value = n->asInt(0, value);
	}
	void readRange(const AssetNode& node, const char* key, glm::vec2& value)
	{
		if (const AssetNode* n = node.find(key))
		{
			value.x = n->asFloat(0, value.x);
			value.y = n->asFloat(1, value.x);
		}
	}
	// "Key value variation": the variation is optional and keeps its default when absent.
	void readValueVar(const AssetNode& node, const char* key, float& value, float& var)
	{
		if (const AssetNode* n = node.find(key))
		{
			value = n->asFloat(0, value);
			var = n->asFloat(1, var);
		}
	}
	void readShape(const AssetNode& node, TreeBranchShape& shape)
	{
		readValueVar(node, "Curve", shape.curve, shape.curveVar);
		readFloat(node, "UpAttract", shape.upAttract);
		readFloat(node, "Droop", shape.droop);
		readFloat(node, "Wobble", shape.wobble);
		readFloat(node, "Elbows", shape.elbows);
		readValueVar(node, "ElbowAngle", shape.elbowAngle, shape.elbowAngleVar);
		readFloat(node, "ElbowUpBias", shape.elbowUpBias);
		readFloat(node, "ElbowMinElevation", shape.elbowMinElevation);
		readRange(node, "ElbowRange", shape.elbowRange);
		readFloat(node, "Stubs", shape.stubs);
		readInt(node, "Rings", shape.rings);
		readInt(node, "Sides", shape.sides);
		readRange(node, "Section", shape.section);
		if (const AssetNode* n = node.find("Section"))
		{
			shape.channel = n->asFloat(2, shape.channel);
			shape.sectionVar = n->asFloat(3, shape.sectionVar);
		}
		readFloat(node, "Swell", shape.swell);
		readFloat(node, "Cup", shape.cup);
		readFloat(node, "Tip", shape.tip);
	}

	void clampShape(TreeBranchShape& shape, int maxRings, int maxSides)
	{
		shape.droop = glm::clamp(shape.droop, 0.0f, 720.0f);
		shape.rings = glm::clamp(shape.rings, 2, maxRings);
		shape.sides = glm::clamp(shape.sides, 3, maxSides);
		shape.elbows = glm::clamp(shape.elbows, 0.0f, 8.0f);
		shape.elbowRange.x = glm::clamp(shape.elbowRange.x, 0.0f, 1.0f);
		shape.elbowRange.y = glm::clamp(shape.elbowRange.y, shape.elbowRange.x, 1.0f);
		shape.stubs = glm::clamp(shape.stubs, 0.0f, 1.0f);
		shape.section = glm::clamp(shape.section, glm::vec2(0.1f), glm::vec2(4.0f));
		shape.tip = glm::clamp(shape.tip, 0.0f, 0.8f);
		shape.channel = glm::clamp(shape.channel, 0.0f, 2.0f);
		shape.sectionVar = glm::clamp(shape.sectionVar, 0.0f, 0.8f);
		shape.swell = glm::clamp(shape.swell, -0.9f, 3.0f);
		shape.cup = glm::clamp(shape.cup, -180.0f, 180.0f);
	}

	void readColor(const AssetNode& node, const char* key, glm::vec3& value)
	{
		if (const AssetNode* n = node.find(key))
			value = glm::vec3(n->asFloat(0, value.x), n->asFloat(1, value.y), n->asFloat(2, value.z));
	}

	bool iequals(oc::string_view a, oc::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? char(a[i] + 32) : a[i];
			const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? char(b[i] + 32) : b[i];
			if (ca != cb)
				return false;
		}
		return true;
	}

	ETreeCrownShape parseCrownShape(const oc::string& name, ETreeCrownShape fallback)
	{
		static constexpr struct { const char* name; ETreeCrownShape shape; } shapes[] =
		{
			{ "Ellipsoid", ETreeCrownShape::Ellipsoid },
			{ "Cone", ETreeCrownShape::Cone },
			{ "Umbrella", ETreeCrownShape::Umbrella },
			{ "Column", ETreeCrownShape::Column },
			{ "Dome", ETreeCrownShape::Dome },
		};
		for (const auto& s : shapes)
			if (iequals(name, s.name))
				return s.shape;
		return fallback;
	}
}

namespace Procedural
{
	bool loadTreeSpecies(const oc::string& path, TreeSpeciesDesc& out, oc::string& outError)
	{
		AssetNode root;
		if (!loadAssetFile(path, root, outError))
			return false;
		const AssetNode* species = root.find("TreeSpecies");
		if (!species)
		{
			outError = "missing 'TreeSpecies' root";
			return false;
		}

		out = TreeSpeciesDesc{};
		out.name = species->asString();
		out.path = path;
		if (const AssetNode* n = species->find("Seed"))
			out.seed = (uint32)n->asInt(0, (int)out.seed);
		readRange(*species, "Scale", out.scale);
		if (const AssetNode* n = species->find("Kind"))
			out.bush = iequals(n->asString(), "Bush");
		if (const AssetNode* n = species->find("Climate"))
			for (size_t i = 0; i < n->numValues(); ++i) // a bush may list several (space-separated)
				out.climate += (i > 0 ? " " : "") + n->asString(i);
		readInt(*species, "Weight", out.weight);
		out.weight = glm::clamp(out.weight, 1, 8);
		if (const AssetNode* undergrowth = species->find("Undergrowth"))
		{
			readFloat(*undergrowth, "Bushes", out.undergrowthBushes);
			readFloat(*undergrowth, "Radius", out.undergrowthRadius);
		}
		if (const AssetNode* wind = species->find("Wind"))
		{
			readFloat(*wind, "Trunk", out.windTrunk);
			readFloat(*wind, "Branch", out.windBranch);
			out.windTrunk = glm::clamp(out.windTrunk, 0.0f, 4.0f);
			out.windBranch = glm::clamp(out.windBranch, 0.0f, 4.0f);
		}

		if (const AssetNode* trunk = species->find("Trunk"))
		{
			readInt(*trunk, "Count", out.trunkCount);
			readRange(*trunk, "Height", out.trunkHeight);
			readFloat(*trunk, "Radius", out.trunkRadius);
			readFloat(*trunk, "Taper", out.trunkTaper);
			readFloat(*trunk, "Flare", out.trunkFlare);
			readFloat(*trunk, "Sink", out.trunkSink);
			readInt(*trunk, "Lobes", out.trunkLobes);
			readRange(*trunk, "LobeDepth", out.trunkLobeDepth);
			readFloat(*trunk, "LobeHeight", out.trunkLobeHeight);
			readFloat(*trunk, "Twist", out.trunkTwist);
			if (const AssetNode* n = trunk->find("Stems"))
			{
				out.trunkStems.x = n->asInt(0, out.trunkStems.x);
				out.trunkStems.y = n->asInt(1, out.trunkStems.x);
			}
			readFloat(*trunk, "StemLean", out.trunkStemLean);
			readFloat(*trunk, "StemSpread", out.trunkStemSpread);
			readFloat(*trunk, "Fork", out.trunkFork);
			readFloat(*trunk, "LeaderLean", out.trunkLeaderLean);
			if (const AssetNode* n = trunk->find("Split"))
			{
				out.trunkSplitCount = n->asInt(0, out.trunkSplitCount);
				out.trunkSplitDepth = n->asInt(1, out.trunkSplitDepth);
				out.trunkSplitAngle = n->asFloat(2, out.trunkSplitAngle);
				out.trunkSplitSpan = n->asFloat(3, out.trunkSplitSpan);
				out.trunkSplitTipCurve = n->asFloat(4, out.trunkSplitTipCurve);
				out.trunkSplitFill = n->asFloat(5, out.trunkSplitFill);
				out.trunkSplitFillHeight = glm::clamp(n->asFloat(6, out.trunkSplitFillHeight), 0.3f, 3.0f);
				out.trunkSplitShoots = glm::clamp(n->asInt(7, out.trunkSplitShoots), 0, 8);
			}
			readShape(*trunk, out.trunkShape);
		}

		if (const AssetNode* crown = species->find("Crown"))
		{
			if (const AssetNode* n = crown->find("Shape"))
				out.crownShape = parseCrownShape(n->asString(), out.crownShape);
			readFloat(*crown, "Start", out.crownStart);
			readFloat(*crown, "Radius", out.crownRadius);
			readInt(*crown, "Slots", out.slots);
			if (const AssetNode* n = crown->find("Tiers"))
			{
				out.crownTiers = n->asInt(0, out.crownTiers);
				out.crownTierSpread = n->asFloat(1, out.crownTierSpread);
				out.crownTierJitter = n->asFloat(2, out.crownTierJitter);
			}
			readFloat(*crown, "Fill", out.slotFill);
			readRange(*crown, "Angle", out.slotAngle);
			readFloat(*crown, "AngleVar", out.slotAngleVar);
			readFloat(*crown, "BranchRadius", out.branchRadius);
			if (const AssetNode* n = crown->find("Leader"))
				out.leader = n->asBool(0, out.leader);
			if (const AssetNode* n = crown->find("SplitLimbs"))
			{
				out.crownSplitLimbs = n->asInt(0, out.crownSplitLimbs);
				out.crownSplitLimbAngle = n->asFloat(1, out.crownSplitLimbAngle);
				out.crownSplitLimbLength = n->asFloat(2, out.crownSplitLimbLength);
			}
			if (const AssetNode* n = crown->find("Top"))
			{
				out.crownTopSlots = n->asInt(0, out.crownTopSlots);
				out.crownTopReach = n->asFloat(1, out.crownTopReach);
				out.crownTopAngle = n->asFloat(2, out.crownTopAngle);
			}
		}

		if (const AssetNode* module = species->find("Module"))
		{
			readInt(*module, "Count", out.moduleCount);
			readFloat(*module, "Length", out.moduleLength);
			readFloat(*module, "Radius", out.moduleRadius);
			readShape(*module, out.moduleShape);
			for (const AssetNode* levelNode : module->findAll("Level"))
			{
				TreeBranchLevel level;
				readInt(*levelNode, "Count", level.count);
				readFloat(*levelNode, "Start", level.start);
				readFloat(*levelNode, "Length", level.length);
				readFloat(*levelNode, "LengthTaper", level.lengthTaper);
				readFloat(*levelNode, "LengthVar", level.lengthVar);
				readFloat(*levelNode, "Rise", level.rise);
				readFloat(*levelNode, "Flat", level.flat);
				readValueVar(*levelNode, "Angle", level.angle, level.angleVar);
				readFloat(*levelNode, "Radius", level.radius);
				readShape(*levelNode, level.shape);
				out.levels.push_back(level);
			}
		}

		if (const AssetNode* leaves = species->find("Leaves"))
		{
			readFloat(*leaves, "Size", out.leafSize);
			readFloat(*leaves, "Aspect", out.leafAspect);
			readInt(*leaves, "PerBranch", out.leavesPerBranch);
			readInt(*leaves, "Levels", out.leafLevels);
			if (const AssetNode* n = leaves->find("Type"))
				out.leafType = iequals(n->asString(), "Cluster") ? ETreeLeafType::Cluster : ETreeLeafType::Single;
			if (const AssetNode* n = leaves->find("Cross"))
				out.leafCross = n->asBool(0, out.leafCross);
			if (const AssetNode* n = leaves->find("Style"))
				out.clusterStyle = iequals(n->asString(), "Needles") ? ETreeClusterStyle::Needles
					: iequals(n->asString(), "Pinnate") ? ETreeClusterStyle::Pinnate
					: iequals(n->asString(), "Mat") ? ETreeClusterStyle::Mat : ETreeClusterStyle::Leaves;
			readInt(*leaves, "ClusterLeaves", out.clusterLeaves);
			readInt(*leaves, "Shoots", out.clusterShoots);
			readFloat(*leaves, "NormalBend", out.leafNormalBend);
			readFloat(*leaves, "Align", out.leafAlign);
			readFloat(*leaves, "Top", out.leafTop);
			readRange(*leaves, "Lift", out.leafLift);
			readFloat(*leaves, "FaceUp", out.leafFaceUp);
			if (const AssetNode* n = leaves->find("Pad"))
			{
				out.padCount = n->asInt(0, out.padCount);
				out.padWidth = n->asFloat(1, out.padWidth);
				out.padHeight = n->asFloat(2, out.padHeight);
				out.padTilt = n->asFloat(3, out.padTilt);
				out.padStart = n->asFloat(4, out.padStart);
			}
			readFloat(*leaves, "ClusterLeafSize", out.clusterLeafSize);
			readFloat(*leaves, "ClusterLeafWidth", out.clusterLeafWidth);
			readFloat(*leaves, "Cup", out.leafCup);
			readFloat(*leaves, "Veins", out.leafVeins);
			readFloat(*leaves, "Wrinkle", out.leafWrinkle);
		}

		if (const AssetNode* bark = species->find("Bark"))
		{
			readRange(*bark, "Plates", out.barkPlates);
			readFloat(*bark, "Crack", out.barkCrack);
			readFloat(*bark, "Breakup", out.barkBreakup);
			readFloat(*bark, "Relief", out.barkRelief);
			readFloat(*bark, "Lichen", out.barkLichen);
			readFloat(*bark, "Lenticels", out.barkLenticels);
			readFloat(*bark, "LenticelSize", out.barkLenticelSize);
			readFloat(*bark, "Scars", out.barkScars);
			readFloat(*bark, "Patches", out.barkPatches);
			readFloat(*bark, "Sections", out.barkSections);
		}

		if (const AssetNode* lod = species->find("Lod"))
			readFloat(*lod, "ErrorScale", out.lodErrorScale);
		if (const AssetNode* bake = species->find("Bake"))
			readInt(*bake, "Variants", out.variantCount);

		if (const AssetNode* billboard = species->find("Billboard"))
		{
			readFloat(*billboard, "Distance", out.billboardDistance);
			readInt(*billboard, "Resolution", out.billboardSize);
			readFloat(*billboard, "NormalBend", out.billboardNormalBend);
			readFloat(*billboard, "FadeWidth", out.billboardFadeWidth);
			readFloat(*billboard, "AlbedoScale", out.billboardAlbedoScale);
			readFloat(*billboard, "FarAlbedoScale", out.farAlbedoScale);
			readFloat(*billboard, "TopCardHeight", out.billboardTopCardHeight);
		}

		if (const AssetNode* color = species->find("Color"))
		{
			readColor(*color, "Bark", out.barkColor);
			readColor(*color, "Leaf", out.leafColor);
			readColor(*color, "Mark", out.markColor);
			readColor(*color, "Section", out.sectionColor);
			if (color->find("Twig"))
			{
				out.twigColor = glm::vec3(0.0f);
				readColor(*color, "Twig", out.twigColor);
			}
			if (color->find("Far"))
			{
				out.farColor = glm::vec3(0.0f);
				readColor(*color, "Far", out.farColor);
			}
			readFloat(*color, "LeafVariation", out.leafVariation);
			readColor(*color, "Accent", out.accentColor);
			readFloat(*color, "AccentShare", out.accentShare);
			out.leafVariation = glm::clamp(out.leafVariation, 0.0f, 3.0f);
			out.accentShare = glm::clamp(out.accentShare, 0.0f, 1.0f);
		}

		if (const AssetNode* placement = species->find("Placement"))
		{
			TreePlacementDesc& p = out.placement;
			readFloat(*placement, "Density", p.density);
			readRange(*placement, "Temperature", p.temperature);     // min max (one value: exactly that)
			readRange(*placement, "Precipitation", p.precipitation); // min max
			readFloat(*placement, "ClimateWidth", p.climateWidth);
			readValueVar(*placement, "Cluster", p.clusterSize, p.clusterCoverage);
			readFloat(*placement, "MaxSlope", p.maxSlope);
			readRange(*placement, "Altitude", p.altitude);
			readFloat(*placement, "Spacing", p.spacing);
		}

		// Clamp what would break the generator; everything else is the author's call.
		out.trunkCount = glm::clamp(out.trunkCount, 1, 64);
		out.moduleCount = glm::clamp(out.moduleCount, 1, 256);
		out.slots = glm::clamp(out.slots, 0, 128);
		out.crownTopSlots = glm::clamp(out.crownTopSlots, 0, 16);
		out.crownTopReach = glm::max(out.crownTopReach, 0.0f);
		out.crownTopAngle = glm::clamp(out.crownTopAngle, 0.0f, 150.0f);
		out.crownSplitLimbs = glm::clamp(out.crownSplitLimbs, 0, 16);
		out.crownSplitLimbAngle = glm::clamp(out.crownSplitLimbAngle, 0.0f, 120.0f);
		out.crownSplitLimbLength = glm::clamp(out.crownSplitLimbLength, 0.02f, 1.0f);
		out.crownTiers = glm::clamp(out.crownTiers, 1, glm::max(out.slots, 1));
		out.crownTierSpread = glm::clamp(out.crownTierSpread, 0.0f, 1.0f);
		out.crownTierJitter = glm::clamp(out.crownTierJitter, 0.0f, 1.0f);
		clampShape(out.trunkShape, 64, 32);
		out.trunkLobes = glm::clamp(out.trunkLobes, 0, 12);
		out.trunkLobeHeight = glm::max(out.trunkLobeHeight, 0.01f);
		out.trunkStems.x = glm::clamp(out.trunkStems.x, 1, 8);
		out.trunkStems.y = glm::clamp(out.trunkStems.y, out.trunkStems.x, 8);
		out.trunkStemLean = glm::clamp(out.trunkStemLean, 0.0f, 60.0f);
		out.trunkStemSpread = glm::max(out.trunkStemSpread, 0.0f);
		out.trunkFork = glm::clamp(out.trunkFork, 0.0f, 0.9f);
		out.trunkLeaderLean = glm::clamp(out.trunkLeaderLean, 0.0f, 1.0f);
		out.trunkSplitCount = glm::clamp(out.trunkSplitCount, 0, 4);
		out.trunkSplitDepth = glm::clamp(out.trunkSplitDepth, 1, 5);
		out.trunkSplitAngle = glm::clamp(out.trunkSplitAngle, 0.0f, 80.0f);
		out.trunkSplitSpan = glm::clamp(out.trunkSplitSpan, 0.05f, 0.8f);
		out.trunkSplitTipCurve = glm::clamp(out.trunkSplitTipCurve, -1.0f, 2.0f);
		clampShape(out.moduleShape, 64, 32);
		if (out.levels.size() > 3)
			out.levels.resize(3);
		for (TreeBranchLevel& level : out.levels)
		{
			level.count = glm::clamp(level.count, 0, 64);
			level.lengthVar = glm::clamp(level.lengthVar, 0.0f, 0.95f);
			level.rise = glm::clamp(level.rise, 0.0f, 1.0f);
			level.flat = glm::clamp(level.flat, 0.0f, 1.0f);
			clampShape(level.shape, 32, 16);
		}
		out.leavesPerBranch = glm::clamp(out.leavesPerBranch, 0, 128);
		out.leafLevels = glm::clamp(out.leafLevels, 1, 4);
		out.clusterLeaves = glm::clamp(out.clusterLeaves, out.clusterStyle == ETreeClusterStyle::Needles ? 0 : 1, 64);
		out.clusterShoots = glm::clamp(out.clusterShoots, 1, 5);
		out.leafNormalBend = glm::clamp(out.leafNormalBend, 0.0f, 1.0f);
		out.leafAlign = glm::clamp(out.leafAlign, 0.0f, 1.0f);
		out.leafTop = glm::clamp(out.leafTop, 0.0f, 1.0f);
		out.leafLift = glm::max(out.leafLift, glm::vec2(0.0f));
		out.leafFaceUp = glm::clamp(out.leafFaceUp, 0.0f, 1.0f);
		out.padCount = glm::clamp(out.padCount, 0, 256);
		out.padWidth = glm::max(out.padWidth, 0.0f);
		out.padHeight = glm::max(out.padHeight, 0.0f);
		out.padStart = glm::clamp(out.padStart, 0.0f, 0.95f);
		out.barkPlates = glm::clamp(glm::round(out.barkPlates), glm::vec2(1.0f), glm::vec2(64.0f));
		out.barkCrack = glm::clamp(out.barkCrack, 0.01f, 0.5f);
		out.barkBreakup = glm::clamp(out.barkBreakup, 0.0f, 1.0f);
		out.barkLenticels = glm::clamp(out.barkLenticels, 0.0f, 1.0f);
		out.barkLenticelSize = glm::clamp(out.barkLenticelSize, 0.5f, 4.0f);
		out.barkScars = glm::clamp(out.barkScars, 0.0f, 1.0f);
		out.barkPatches = glm::clamp(out.barkPatches, 0.0f, 1.0f);
		out.barkSections = glm::clamp(out.barkSections, 0.0f, 1.0f);
		out.lodErrorScale = glm::max(out.lodErrorScale, 0.0f);
		out.variantCount = glm::clamp(out.variantCount, 1, 64);
		out.billboardDistance = glm::max(out.billboardDistance, 0.0f);
		out.billboardNormalBend = glm::clamp(out.billboardNormalBend, 0.0f, 1.0f);
		out.billboardFadeWidth = glm::clamp(out.billboardFadeWidth, 1.0f, 1023.0f); // the material packs 10 bits
		out.billboardAlbedoScale = glm::clamp(out.billboardAlbedoScale, 0.0f, 8.0f);
		out.farAlbedoScale = glm::clamp(out.farAlbedoScale, 0.0f, 8.0f);
		if (out.farColor.x >= 0.0f)
			out.farColor = glm::clamp(out.farColor, glm::vec3(0.0f), glm::vec3(1.0f));
		out.billboardTopCardHeight = glm::clamp(out.billboardTopCardHeight, 0.0f, 1.0f);
		int billboardSize = 16;
		while (billboardSize < out.billboardSize && billboardSize < 2048)
			billboardSize *= 2;
		out.billboardSize = billboardSize;
		out.clusterLeafSize = glm::clamp(out.clusterLeafSize, 0.02f, 0.6f); // Pinnate leaflets are tiny
		out.clusterLeafWidth = glm::clamp(out.clusterLeafWidth, 0.1f, 0.6f);
		out.leafCup = glm::clamp(out.leafCup, 0.0f, 1.0f);
		out.leafVeins = glm::clamp(out.leafVeins, 0.0f, 1.0f);
		out.leafWrinkle = glm::clamp(out.leafWrinkle, 0.0f, 1.0f);
		out.placement.density = glm::max(out.placement.density, 0.0f);
		out.placement.climateWidth = glm::max(out.placement.climateWidth, 0.01f);
		out.placement.temperature.y = glm::max(out.placement.temperature.y, out.placement.temperature.x);
		out.placement.precipitation.y = glm::max(out.placement.precipitation.y, out.placement.precipitation.x);
		out.placement.clusterCoverage = glm::clamp(out.placement.clusterCoverage, 0.0f, 1.0f);
		out.placement.maxSlope = glm::max(out.placement.maxSlope, 0.0f);
		out.placement.spacing = glm::clamp(out.placement.spacing, 0.0f, 30.0f);
		return true;
	}
}
