export module Procedural:TerrainChunk;

import Core;
import Core.glm;
import RendererVK;

export namespace Procedural
{
	// CPU output of one generated chunk. Geometry is chunk-LOCAL in X/Z (0..chunkSize) with world-space Y,
	// so a consumer places it with a pure XZ translation to the chunk origin (keeps float precision high).
	// No skirt and no tangents (the terrain FS builds its own bases). Every node of one lod0Res has the same
	// grid, so the indices are NOT here: terrainNodeIndices, one shared RenderMeshIndices. The terrain VS stitches
	// the edges from these vertices (instanced_indirect_terrain.vs.glsl).
	using TerrainChunkMesh = RenderTerrainMeshData;
}
