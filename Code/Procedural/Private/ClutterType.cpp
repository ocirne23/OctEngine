module Procedural;

import Core;
import Core.glm;

import File;

import :ClutterType;
import :RockType;

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
	// `a b` (one value: both).
	void readPair(const AssetNode& node, const char* key, glm::vec2& value)
	{
		if (const AssetNode* n = node.find(key))
		{
			value.x = n->asFloat(0, value.x);
			value.y = n->asFloat(1, value.x);
		}
	}
	void readColor(const AssetNode& node, const char* key, glm::vec3& value)
	{
		if (const AssetNode* n = node.find(key))
			value = glm::clamp(glm::vec3(n->asFloat(0, value.x), n->asFloat(1, value.y), n->asFloat(2, value.z)), glm::vec3(0.0f), glm::vec3(1.0f));
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
}

namespace Procedural
{
	bool loadClutterType(const oc::string& path, ClutterTypeDesc& out, oc::string& outError)
	{
		AssetNode root;
		if (!loadAssetFile(path, root, outError))
			return false;
		const AssetNode* type = root.find("ClutterType");
		if (!type)
		{
			outError = "missing 'ClutterType' root";
			return false;
		}

		out = ClutterTypeDesc{};
		out.name = type->asString();
		out.path = path;
		if (const AssetNode* n = type->find("Kind"))
		{
			const oc::string kind = n->asString();
			out.kind = iequals(kind, "Branch") ? EClutterKind::Branch : iequals(kind, "Mushroom") ? EClutterKind::Mushroom
				: iequals(kind, "Flower") ? EClutterKind::Flower : EClutterKind::Pebble;
		}
		if (const AssetNode* n = type->find("Seed"))
			out.seed = (uint32)n->asInt(0, (int)out.seed);
		readInt(*type, "Variants", out.variantCount);
		readPair(*type, "Scale", out.scale);
		readFloat(*type, "Range", out.range);
		readFloat(*type, "Sink", out.sink);
		readFloat(*type, "Align", out.align);
		readColor(*type, "Color", out.color);
		readColor(*type, "Color2", out.color2);
		readFloat(*type, "Roughness", out.roughness);
		readInt(*type, "Lod", out.lodTriangles);

		// Pebble: the rock shape keys (a small rock - the defaults of a smooth river stone).
		out.rock.name = out.name;
		out.rock.seed = out.seed;
		out.rock.aspect = glm::vec3(1.0f, 0.55f, 0.75f);
		out.rock.erosion = 0.2f;
		out.rock.noiseAmplitude = 0.025f;
		out.rock.noiseOctaves = 3;
		readRockShapeKeys(*type, out.rock);
		// Branch.
		readFloat(*type, "Thickness", out.thickness);
		readInt(*type, "Twigs", out.twigs);
		readFloat(*type, "Bend", out.bend);
		// Mushroom.
		if (const AssetNode* n = type->find("Cap"))
		{
			const oc::string cap = n->asString();
			out.cap = iequals(cap, "Flat") ? EMushroomCap::Flat : iequals(cap, "Cone") ? EMushroomCap::Cone
				: iequals(cap, "Funnel") ? EMushroomCap::Funnel : EMushroomCap::Dome;
		}
		readPair(*type, "CapSize", out.capSize);
		readFloat(*type, "StemRadius", out.stemRadius);
		readInt(*type, "Group", out.group);
		readFloat(*type, "Spots", out.spots);
		// Flower.
		if (const AssetNode* n = type->find("Head"))
		{
			const oc::string head = n->asString();
			out.head = iequals(head, "Spike") ? EFlowerHead::Spike : iequals(head, "Umbel") ? EFlowerHead::Umbel
				: iequals(head, "Bell") ? EFlowerHead::Bell : iequals(head, "Reed") ? EFlowerHead::Reed
				: iequals(head, "Tuft") ? EFlowerHead::Tuft : EFlowerHead::Radial;
		}
		readInt(*type, "Petals", out.petals);
		readFloat(*type, "Open", out.open);
		readFloat(*type, "PetalWidth", out.petalWidth);
		readFloat(*type, "Stem", out.stemHeight);
		readFloat(*type, "HeadSize", out.headSize);

		for (const AssetNode* placement : type->findAll("Placement"))
		{
			ClutterPlacementDesc p;
			readFloat(*placement, "Density", p.density);
			readPair(*placement, "Temperature", p.temperature);
			readPair(*placement, "Precipitation", p.precipitation);
			readFloat(*placement, "ClimateWidth", p.climateWidth);
			if (const AssetNode* n = placement->find("Cluster"))
			{
				p.clusterSize = n->asFloat(0, p.clusterSize);
				p.clusterCoverage = n->asFloat(1, p.clusterCoverage);
			}
			readFloat(*placement, "Slope", p.maxSlope);
			readFloat(*placement, "Altitude", p.minAltitude);
			readPair(*placement, "Grass", p.grass);
			readPair(*placement, "Crag", p.crag);
			readPair(*placement, "Beach", p.beach);
			readPair(*placement, "Canopy", p.canopy);
			readPair(*placement, "Trunk", p.trunk);
			readPair(*placement, "Rock", p.rock);
			readPair(*placement, "Wet", p.wet);
			readPair(*placement, "River", p.river);
			if (const AssetNode* n = placement->find("River"))
				p.riverCurve = glm::clamp(n->asFloat(2, 1.0f), 0.1f, 16.0f); // River none full [curve]
			readPair(*placement, "Flow", p.flow);
			readPair(*placement, "Water", p.water);
			if (const AssetNode* n = placement->find("Ring"))
				p.ring = glm::vec4(n->asFloat(0, p.ring.x), n->asFloat(1, p.ring.y), n->asFloat(2, p.ring.z), n->asFloat(3, p.ring.w));
			if (p.density <= 0.0f)
				continue;
			p.climateWidth = glm::max(p.climateWidth, 0.01f);
			p.clusterCoverage = glm::clamp(p.clusterCoverage, 0.0f, 1.0f);
			p.maxSlope = glm::max(p.maxSlope, 0.0f);
			for (glm::vec2* term : { &p.grass, &p.crag, &p.beach, &p.canopy, &p.trunk, &p.rock, &p.wet, &p.river, &p.flow, &p.water })
				*term = glm::max(*term, glm::vec2(0.0f));
			p.ring = glm::vec4(glm::max(p.ring.x, 0.0f), glm::max(p.ring.y, 0.05f), glm::max(p.ring.z, 2.0f * p.ring.x + 1.0f), glm::clamp(p.ring.w, 0.0f, 1.0f));
			out.placements.push_back(p);
		}

		// Clamp what would break the generators; everything else is the author's call.
		out.variantCount = glm::clamp(out.variantCount, 1, 16);
		out.scale = glm::clamp(out.scale, glm::vec2(0.005f), glm::vec2(4.0f));
		out.scale.y = glm::max(out.scale.y, out.scale.x);
		out.range = glm::clamp(out.range, 5.0f, 500.0f);
		out.sink = glm::clamp(out.sink, 0.0f, 0.9f);
		out.align = glm::clamp(out.align, 0.0f, 1.0f);
		out.roughness = glm::clamp(out.roughness, 0.05f, 1.0f);
		out.lodTriangles = glm::clamp(out.lodTriangles, 32, 4000);
		out.rock.lodTriangles = out.lodTriangles;
		clampRockShape(out.rock);
		out.rock.lodTriangles = out.lodTriangles; // a pebble: far fewer triangles than a rock's minimum
		out.thickness = glm::clamp(out.thickness, 0.005f, 0.2f);
		out.twigs = glm::clamp(out.twigs, 0, 6);
		out.bend = glm::clamp(out.bend, 0.0f, 90.0f);
		out.capSize = glm::clamp(out.capSize, glm::vec2(0.05f), glm::vec2(1.5f, 0.8f));
		out.stemRadius = glm::clamp(out.stemRadius, 0.01f, 0.5f);
		out.group = glm::clamp(out.group, 1, 5);
		out.spots = glm::clamp(out.spots, 0.0f, 1.0f);
		out.petals = glm::clamp(out.petals, 1, 48);
		out.open = glm::clamp(out.open, -80.0f, 85.0f);
		out.petalWidth = glm::clamp(out.petalWidth, 0.02f, 1.0f);
		out.stemHeight = glm::clamp(out.stemHeight, 0.01f, 2.0f);
		out.headSize = glm::clamp(out.headSize, 0.002f, 0.5f);
		return true;
	}
}
