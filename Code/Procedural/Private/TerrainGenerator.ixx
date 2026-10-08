export module Procedural:TerrainGenerator;

import Core;
import Core.glm;
import :TerrainSampler;
import :TerrainChunk;

export namespace Procedural
{
	// One QUADTREE NODE of the terrain (TerrainStreamer): at `lod` it covers 2^lod x 2^lod base chunks with the same
	// lod0Res quad grid, so each level doubles the quad size (and halves the density).
	struct ChunkParams
	{
		glm::ivec2 coord{ 0, 0 }; // the node's coordinate AT ITS LOD (world origin = coord * chunkSize * 2^lod on X/Z)
		uint32 lod = 0;           // 0 = a base chunk
		float  chunkSize = 128.0f; // the BASE chunk's size (m)
		uint32 lod0Res = 64;      // quads per side of every node (a power of two: the edge stitch lattices)
		float nodeSize() const { return chunkSize * (float)(1u << lod); }
	};

	// Generates one chunk's surface mesh (geometry only) from any generator's fields. Pure and thread-safe
	// given a shared sampler. The height field is sampled in world space (so heights agree across chunk/LOD
	// boundaries) and includes the seabed below sea level - the OceanGenerator draws the water over it and
	// bakes its shore-depth map from the same field. No skirt: the terrain VS stitches every edge to the
	// coarser side from TerrainChunkMesh::stitch (see "Edge stitching" in the Procedural CONTEXT).
	void generateChunk(const ITerrainSampler& maps, const ChunkParams& params, TerrainChunkMesh& out);
}
