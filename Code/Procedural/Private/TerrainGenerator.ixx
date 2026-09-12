export module Procedural:TerrainGenerator;

import Core;
import Core.glm;
import :TerrainSampler;
import :TerrainChunk;

export namespace Procedural
{
	struct ChunkParams
	{
		glm::ivec2 coord{ 0, 0 }; // chunk grid coordinate (world origin = coord * chunkSize on X/Z)
		uint32 lod = 0;           // 0 = finest; each level halves the grid resolution
		float  chunkSize = 128.0f;
		uint32 lod0Res = 64;      // quads per side at LOD0 (must be a power of two for clean LOD subsetting)
		float  skirtDepth = 8.0f; // how far border skirt walls drop below the surface
	};

	// Generates one chunk's surface mesh (geometry only) from any generator's fields. Pure and thread-safe
	// given a shared sampler. The height field is sampled in world space (so heights agree across chunk/LOD
	// boundaries) and includes the seabed below sea level - the OceanGenerator draws the water over it and
	// bakes its shore-depth map from the same field.
	void generateChunk(const ITerrainSampler& maps, const ChunkParams& params, TerrainChunkMesh& out);
}
