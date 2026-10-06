module Procedural;

import Core;
import Core.glm;

import File;

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
	void readRange(const AssetNode& node, const char* key, glm::vec2& value)
	{
		if (const AssetNode* n = node.find(key))
		{
			value.x = n->asFloat(0, value.x);
			value.y = n->asFloat(1, value.x);
		}
	}
	void readVec3(const AssetNode& node, const char* key, glm::vec3& value)
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
}

namespace Procedural
{
	bool loadRockType(const oc::string& path, RockTypeDesc& out, oc::string& outError)
	{
		AssetNode root;
		if (!loadAssetFile(path, root, outError))
			return false;
		const AssetNode* type = root.find("RockType");
		if (!type)
		{
			outError = "missing 'RockType' root";
			return false;
		}

		out = RockTypeDesc{};
		out.name = type->asString();
		out.path = path;
		if (const AssetNode* n = type->find("Seed"))
			out.seed = (uint32)n->asInt(0, (int)out.seed);
		readRange(*type, "Scale", out.scale);
		readInt(*type, "Variants", out.variantCount);
		readFloat(*type, "Sink", out.sink);
		readFloat(*type, "Align", out.align);

		if (const AssetNode* n = type->find("Shape"))
		{
			const oc::string shape = n->asString();
			out.shape = iequals(shape, "Block") ? ERockShape::Block : iequals(shape, "Pillar") ? ERockShape::Pillar : ERockShape::Boulder;
		}
		readVec3(*type, "Aspect", out.aspect);
		readVec3(*type, "AspectVar", out.aspectVar);
		readFloat(*type, "Round", out.round);
		readFloat(*type, "Squareness", out.squareness);
		readFloat(*type, "Erosion", out.erosion);
		if (const AssetNode* n = type->find("Warp"))
		{
			out.warpAmplitude = n->asFloat(0, out.warpAmplitude);
			out.warpFrequency = n->asFloat(1, out.warpFrequency);
		}
		if (const AssetNode* n = type->find("Profile"))
			for (int i = 0; i < ROCK_PROFILE_POINTS; ++i)
				out.profile[i] = n->asFloat((size_t)i, out.profile[i]);
		readFloat(*type, "ProfileVar", out.profileVar);
		if (const AssetNode* n = type->find("Group"))
		{
			out.group = n->asInt(0, out.group);
			out.groupShrink = n->asFloat(1, out.groupShrink);
		}
		if (const AssetNode* n = type->find("Pile"))
		{
			out.pile = n->asInt(0, out.pile);
			out.pileShrink = n->asFloat(1, out.pileShrink);
		}
		if (const AssetNode* n = type->find("Fracture"))
		{
			out.fractureCount = n->asInt(0, out.fractureCount);
			out.fractureDepth = n->asFloat(1, out.fractureDepth);
		}
		if (const AssetNode* n = type->find("Strata"))
		{
			out.strataSpacing = n->asFloat(0, out.strataSpacing);
			out.strataDepth = n->asFloat(1, out.strataDepth);
			out.strataVar = n->asFloat(2, out.strataVar);
		}
		if (const AssetNode* n = type->find("Noise"))
		{
			out.noiseAmplitude = n->asFloat(0, out.noiseAmplitude);
			out.noiseFrequency = n->asFloat(1, out.noiseFrequency);
			out.noiseOctaves = n->asInt(2, out.noiseOctaves);
		}
		readFloat(*type, "NoiseStretch", out.noiseStretch);
		readFloat(*type, "Ridged", out.ridged);
		if (const AssetNode* n = type->find("Pits"))
		{
			out.pitCount = n->asInt(0, out.pitCount);
			out.pitSize = n->asFloat(1, out.pitSize);
			out.pitDepth = n->asFloat(2, out.pitDepth);
		}
		if (const AssetNode* n = type->find("Split"))
		{
			out.splitChance = n->asFloat(0, out.splitChance);
			out.splitGap = n->asFloat(1, out.splitGap);
		}
		readInt(*type, "Lod", out.lodTriangles);
		readFloat(*type, "Resolution", out.resolution);

		for (const AssetNode* placement : type->findAll("Placement"))
		{
			RockPlacementDesc p;
			readFloat(*placement, "Density", p.density);
			readRange(*placement, "Temperature", p.temperature);
			readRange(*placement, "Precipitation", p.precipitation);
			readFloat(*placement, "ClimateWidth", p.climateWidth);
			if (const AssetNode* n = placement->find("Slope"))
			{
				// `min max`: a hard band. `none full full none`: soft edges.
				const float a = n->asFloat(0, 0.0f), b = n->asFloat(1, a);
				p.slope = n->numValues() >= 4 ? glm::vec4(a, b, n->asFloat(2, b), n->asFloat(3, b)) : glm::vec4(a, a, b, b);
				p.slope.y = glm::max(p.slope.y, p.slope.x);
				p.slope.z = glm::max(p.slope.z, p.slope.y);
				p.slope.w = glm::max(p.slope.w, p.slope.z);
			}
			readFloat(*placement, "Crag", p.crag);
			readFloat(*placement, "Talus", p.talus);
			if (const AssetNode* n = placement->find("Cluster"))
			{
				p.clusterSize = n->asFloat(0, p.clusterSize);
				p.clusterCoverage = n->asFloat(1, p.clusterCoverage);
			}
			readRange(*placement, "Altitude", p.altitude);
			readFloat(*placement, "Plains", p.plains);
			readRange(*placement, "Rugged", p.rugged); // low end, high end (one value: both)
			readFloat(*placement, "Valley", p.valley);
			if (p.density <= 0.0f)
				continue;
			p.climateWidth = glm::max(p.climateWidth, 0.01f);
			p.crag = glm::clamp(p.crag, 0.0f, 1.0f);
			p.talus = glm::clamp(p.talus, 0.0f, 1.0f);
			p.clusterCoverage = glm::clamp(p.clusterCoverage, 0.0f, 1.0f);
			p.plains = glm::max(p.plains, 0.0f);
			p.rugged = glm::max(p.rugged, glm::vec2(0.0f));
			out.placements.push_back(p);
		}

		// Clamp what would break the generator; everything else is the author's call.
		out.scale = glm::clamp(out.scale, glm::vec2(0.1f), glm::vec2(ROCK_MAX_SIZE));
		out.scale.y = glm::max(out.scale.y, out.scale.x);
		out.variantCount = glm::clamp(out.variantCount, 1, 32);
		out.sink = glm::clamp(out.sink, 0.0f, 0.9f);
		out.aspect = glm::max(out.aspect, glm::vec3(0.05f));
		out.aspectVar = glm::clamp(out.aspectVar, glm::vec3(0.0f), glm::vec3(0.9f));
		out.round = glm::clamp(out.round, 0.0f, 0.4f);
		if (out.squareness <= 0.0f)
			out.squareness = out.shape == ERockShape::Block ? 3.0f : 2.2f;
		out.squareness = glm::clamp(out.squareness, 1.5f, 12.0f);
		out.erosion = glm::clamp(out.erosion, 0.0f, 0.3f);
		out.warpAmplitude = glm::clamp(out.warpAmplitude, 0.0f, 0.2f);
		out.warpFrequency = glm::max(out.warpFrequency, 0.0f);
		out.pile = glm::clamp(out.pile, 1, ROCK_MAX_PILE);
		out.pileShrink = glm::clamp(out.pileShrink, 0.2f, 1.0f);
		for (float& width : out.profile)
			width = glm::clamp(width, 0.03f, 4.0f);
		out.profileVar = glm::clamp(out.profileVar, 0.0f, 0.8f);
		out.group = glm::clamp(out.group, 1, ROCK_MAX_PILE);
		out.groupShrink = glm::clamp(out.groupShrink, 0.2f, 1.0f);
		out.noiseStretch = glm::clamp(out.noiseStretch, 0.1f, 10.0f);
		out.resolution = glm::clamp(out.resolution, 0.5f, 4.0f);
		out.fractureCount = glm::clamp(out.fractureCount, 0, 16);
		out.fractureDepth = glm::clamp(out.fractureDepth, 0.0f, 0.9f);
		out.strataSpacing = glm::max(out.strataSpacing, 0.0f);
		out.noiseOctaves = glm::clamp(out.noiseOctaves, 0, 8);
		out.ridged = glm::clamp(out.ridged, 0.0f, 1.0f);
		out.pitCount = glm::clamp(out.pitCount, 0, 32);
		out.splitChance = glm::clamp(out.splitChance, 0.0f, 1.0f);
		out.lodTriangles = glm::clamp(out.lodTriangles, 64, 50000);
		out.align = glm::clamp(out.align, 0.0f, 1.0f);
		return true;
	}
}
