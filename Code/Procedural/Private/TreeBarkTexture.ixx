export module Procedural:TreeBarkTexture;

import Core;
import Core.glm;

import :TreeSpecies;

// The procedural bark texture of a species (placeholder until authored textures exist). Tiles in both
// directions, matching the bark UVs: u once around a branch, v along it in units of the base circumference.
// Furrows: two families of meandering fissure lines along the branch that cross and merge into braided
// ridges, rare horizontal breaks, rounded ridge crowns, fine grain and fibres, lichen patches. RGBA8 sRGB albedo + a LINEAR RGB tangent-space
// normal map from the same height field (x along u, y along v), each with its own box-filtered mip chain.
export namespace Procedural
{
	struct TreeBarkTexture
	{
		uint32 size = 0;
		oc::vector<oc::vector<uint8>> albedoMips;
		oc::vector<oc::vector<uint8>> normalMips;
	};

	// Level 0 of both maps as RGBA8 - what TreeSystem writes to / reads from Assets/Local/Trees/Textures.
	void generateBarkImages(const TreeSpeciesDesc& species, uint32 size, oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal);
	// Both full chains from a level 0 (generated or loaded from disk); normals renormalized per level.
	void buildBarkMips(oc::span<const uint8> albedo0, oc::span<const uint8> normal0, uint32 size, TreeBarkTexture& out);
	// A normal map's chain alone (box filter, renormalized, alpha 255), level 0 included: the billboards too.
	void buildNormalMips(oc::span<const uint8> normal0, uint32 size, oc::vector<oc::vector<uint8>>& outMips);
}
