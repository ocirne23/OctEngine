export module Procedural:TerrainChunk;

import Core;
import Core.glm;

export namespace Procedural
{
	// The deepest edge stitch: a neighbour up to this many LODs coarser (TerrainChunkMesh::stitch.xyz).
	constexpr uint32 TERRAIN_STITCH_LEVELS = 3;

	// CPU output of one generated chunk. Geometry is chunk-LOCAL in X/Z (0..chunkSize) with world-space Y,
	// so a consumer places it with a pure XZ translation to the chunk origin (keeps float precision high).
	// No skirt and no tangents (the terrain FS builds its own bases): the vertex's tangent slot carries the
	// EDGE STITCH instead - xyz = this vertex's height when its edge is snapped to a neighbour 1 / 2 / 3 LODs
	// coarser (the coarse edge's straight line; the own height off the edges and on the coarse lattice),
	// w = the chunk's LOD. The terrain VS picks one per edge (instanced_indirect_terrain.vs.glsl).
	struct TerrainChunkMesh
	{
		oc::vector<glm::vec3> positions;   // local X/Z, world Y
		oc::vector<glm::vec3> normals;
		oc::vector<glm::vec3> texCoords;   // xy in [0,1] across the chunk, z unused
		oc::vector<glm::vec4> stitch;      // see above: MeshVertex::tangent
		oc::vector<uint32>    indices;

		void clear()
		{
			positions.clear(); normals.clear(); texCoords.clear(); stitch.clear(); indices.clear();
		}
	};
}
