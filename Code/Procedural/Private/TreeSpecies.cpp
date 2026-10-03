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
		readFloat(node, "Wobble", shape.wobble);
		readFloat(node, "Elbows", shape.elbows);
		readValueVar(node, "ElbowAngle", shape.elbowAngle, shape.elbowAngleVar);
		readFloat(node, "ElbowUpBias", shape.elbowUpBias);
		readFloat(node, "ElbowMinElevation", shape.elbowMinElevation);
		readRange(node, "ElbowRange", shape.elbowRange);
		readFloat(node, "Stubs", shape.stubs);
		readInt(node, "Rings", shape.rings);
		readInt(node, "Sides", shape.sides);
	}

	void clampShape(TreeBranchShape& shape, int maxRings, int maxSides)
	{
		shape.rings = glm::clamp(shape.rings, 2, maxRings);
		shape.sides = glm::clamp(shape.sides, 3, maxSides);
		shape.elbows = glm::clamp(shape.elbows, 0.0f, 8.0f);
		shape.elbowRange.x = glm::clamp(shape.elbowRange.x, 0.0f, 1.0f);
		shape.elbowRange.y = glm::clamp(shape.elbowRange.y, shape.elbowRange.x, 1.0f);
		shape.stubs = glm::clamp(shape.stubs, 0.0f, 1.0f);
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
			out.climate = n->asString();

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
			readShape(*trunk, out.trunkShape);
		}

		if (const AssetNode* crown = species->find("Crown"))
		{
			if (const AssetNode* n = crown->find("Shape"))
				out.crownShape = parseCrownShape(n->asString(), out.crownShape);
			readFloat(*crown, "Start", out.crownStart);
			readFloat(*crown, "Radius", out.crownRadius);
			readInt(*crown, "Slots", out.slots);
			readFloat(*crown, "Fill", out.slotFill);
			readRange(*crown, "Angle", out.slotAngle);
			readFloat(*crown, "AngleVar", out.slotAngleVar);
			readFloat(*crown, "BranchRadius", out.branchRadius);
			if (const AssetNode* n = crown->find("Leader"))
				out.leader = n->asBool(0, out.leader);
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
					: iequals(n->asString(), "Pinnate") ? ETreeClusterStyle::Pinnate : ETreeClusterStyle::Leaves;
			readInt(*leaves, "ClusterLeaves", out.clusterLeaves);
			readInt(*leaves, "Shoots", out.clusterShoots);
			readFloat(*leaves, "NormalBend", out.leafNormalBend);
			readFloat(*leaves, "ClusterLeafSize", out.clusterLeafSize);
		}

		if (const AssetNode* bark = species->find("Bark"))
		{
			readRange(*bark, "Plates", out.barkPlates);
			readFloat(*bark, "Crack", out.barkCrack);
			readFloat(*bark, "Breakup", out.barkBreakup);
			readFloat(*bark, "Relief", out.barkRelief);
			readFloat(*bark, "Lichen", out.barkLichen);
		}

		if (const AssetNode* lod = species->find("Lod"))
			readFloat(*lod, "ErrorScale", out.lodErrorScale);
		if (const AssetNode* bake = species->find("Bake"))
			readInt(*bake, "Variants", out.variantCount);

		if (const AssetNode* impostor = species->find("Impostor"))
		{
			readFloat(*impostor, "Distance", out.impostorDistance);
			readInt(*impostor, "Frames", out.impostorFrames);
			readInt(*impostor, "Resolution", out.impostorFrameSize);
		}

		if (const AssetNode* billboard = species->find("Billboard"))
		{
			readFloat(*billboard, "Distance", out.billboardDistance);
			readInt(*billboard, "Resolution", out.billboardSize);
			readFloat(*billboard, "NormalBend", out.billboardNormalBend);
			readFloat(*billboard, "FadeWidth", out.billboardFadeWidth);		}

		if (const AssetNode* color = species->find("Color"))
		{
			readColor(*color, "Bark", out.barkColor);
			readColor(*color, "Leaf", out.leafColor);
		}

		// Clamp what would break the generator; everything else is the author's call.
		out.trunkCount = glm::clamp(out.trunkCount, 1, 64);
		out.moduleCount = glm::clamp(out.moduleCount, 1, 256);
		out.slots = glm::clamp(out.slots, 0, 64);
		clampShape(out.trunkShape, 64, 32);
		out.trunkLobes = glm::clamp(out.trunkLobes, 0, 12);
		out.trunkLobeHeight = glm::max(out.trunkLobeHeight, 0.01f);
		clampShape(out.moduleShape, 64, 32);
		if (out.levels.size() > 3)
			out.levels.resize(3);
		for (TreeBranchLevel& level : out.levels)
		{
			level.count = glm::clamp(level.count, 0, 64);
			clampShape(level.shape, 32, 16);
		}
		out.leavesPerBranch = glm::clamp(out.leavesPerBranch, 0, 128);
		out.leafLevels = glm::clamp(out.leafLevels, 1, 4);
		out.clusterLeaves = glm::clamp(out.clusterLeaves, out.clusterStyle == ETreeClusterStyle::Needles ? 0 : 1, 64);
		out.clusterShoots = glm::clamp(out.clusterShoots, 1, 5);
		out.leafNormalBend = glm::clamp(out.leafNormalBend, 0.0f, 1.0f);
		out.barkPlates = glm::clamp(glm::round(out.barkPlates), glm::vec2(1.0f), glm::vec2(64.0f));
		out.barkCrack = glm::clamp(out.barkCrack, 0.01f, 0.5f);
		out.barkBreakup = glm::clamp(out.barkBreakup, 0.0f, 1.0f);
		out.lodErrorScale = glm::max(out.lodErrorScale, 0.0f);
		out.variantCount = glm::clamp(out.variantCount, 1, 64);
		out.impostorDistance = glm::max(out.impostorDistance, 0.0f);
		out.impostorFrames = glm::clamp(out.impostorFrames, 2, 32);
		// A power of two: the atlas (frames x frameSize) gets a mip chain down to 4 px per frame.
		int frameSize = 8;
		while (frameSize < out.impostorFrameSize && frameSize < 512)
			frameSize *= 2;
		out.impostorFrameSize = frameSize;
		out.billboardDistance = glm::max(out.billboardDistance, 0.0f);
		out.billboardNormalBend = glm::clamp(out.billboardNormalBend, 0.0f, 1.0f);
		out.billboardFadeWidth = glm::clamp(out.billboardFadeWidth, 1.0f, 1023.0f); // the material packs 10 bits
		int billboardSize = 16;
		while (billboardSize < out.billboardSize && billboardSize < 2048)
			billboardSize *= 2;
		out.billboardSize = billboardSize;
		out.clusterLeafSize = glm::clamp(out.clusterLeafSize, 0.02f, 0.6f); // Pinnate leaflets are tiny
		return true;
	}
}
