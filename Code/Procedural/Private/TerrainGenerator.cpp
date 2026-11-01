module Procedural;

import Core;
import Core.glm;
import Threading; // pre-emption points between the chunk's stages and rows
import :TerrainGenerator;
import :TerrainSampler;
import :TerrainChunk;

namespace Procedural
{
	void generateChunk(const ITerrainSampler& maps, const ChunkParams& params, TerrainChunkMesh& out, oc::vector<TerrainPoint>* outField)
	{
		out.vertices.clear();

		const uint32 res = glm::max(1u, params.lod0Res); // every node: the same grid, over 2^lod base chunks
		assert(res <= 255 && "TerrainVertex holds its grid position in 8 bits");
		const uint32 vpr = res + 1; // vertices per row
		const float  step = params.nodeSize() / (float)res;
		const double ox = (double)params.coord.x * (double)params.nodeSize();
		const double oz = (double)params.coord.y * (double)params.nodeSize();

		// Sample the field ONCE per point, into a grid with a one-vertex halo, and take the normals from
		// neighbouring grid entries. The obvious version - sampleHeight at the vertex plus four more for a
		// central difference - costs 5 samples per vertex, which at LOD0 is 513*513*5 = 1.3M point queries
		// for ONE chunk. That is affordable for a noise field and ruinous for V3, where every query resolves
		// a diffusion tile block and takes the tile-cache lock. sampleGrid resolves the block once and then
		// fills lock-free, so this is ~5x fewer samples AND ~1.3M fewer lock round-trips.
		//
		// The halo is not overhead: the old code already sampled outside the chunk for the border vertices'
		// differences. It just did it one point at a time.
		const uint32 gpr = vpr + 2; // grid points per row: the vertex grid plus a 1-point halo
		oc::vector<TerrainPoint> field((size_t)gpr * gpr);
		maps.sampleGrid(ox - (double)step, oz - (double)step, (double)step, gpr, gpr, field);
		// This runs on a Low pump job (or the Normal collider job): let higher-priority work through
		// between the stages and between the vertex rows below. The V3 lock is NOT held here - the
		// grid was resolved above - so a pre-empting job that samples terrain cannot deadlock on it.
		Globals::jobSystem.preemptionPoint();

		// Vertex coords -> grid entry. SIGNED on purpose: the border vertices ask for col/row -1, which the
		// halo holds. (With uint32 that underflows and only lands on the right entry by wrapping twice.)
		const auto at = [&](int32 col, int32 row) -> const TerrainPoint&
		{
			return field[(size_t)(row + 1) * gpr + (size_t)(col + 1)];
		};

		// The vertices, then the node's bricks (TerrainRayFrameGpu's comment) in the same allocation, in whole vertices.
		const uint32 bricksPerSide = glm::max(res >> RendererVKLayout::TERRAIN_BRICK_SHIFT, 1u);
		const size_t numBrickVertices = ((size_t)bricksPerSide * bricksPerSide * sizeof(glm::vec2) + sizeof(RendererVKLayout::TerrainVertex) - 1) / sizeof(RendererVKLayout::TerrainVertex);
		out.vertices.reserve((size_t)vpr * vpr + numBrickVertices);
		glm::vec3 boundsMin(FLT_MAX), boundsMax(-FLT_MAX);

		// One cell, not the old half cell: the difference is now taken between the actual mesh neighbours,
		// so the normals describe the triangles being drawn rather than a sub-cell slope the geometry never
		// shows. Slightly smoother on the finest LOD; the detail layer is still resolved by the vertices.
		const float eps = step;

		for (int32 row = 0; row <= (int32)res; ++row)
		{
			for (int32 col = 0; col <= (int32)res; ++col)
			{
				const float lx = (float)col * step;
				const float lz = (float)row * step;
				// True surface height, INCLUDING the seabed below sea level: water is the OceanGenerator's
				// job now (its shore-depth bake samples this same field, and its ray-traced refraction needs
				// the real bottom to hit - the old max(h, seaLevel) lid read as zero-depth water and
				// co-planed with it).
				const float y = at(col, row).height;

				// Central-difference normal (matches the engine's existing terrain convention).
				const glm::vec3 dpdu(2.0f * eps, at(col + 1, row).height - at(col - 1, row).height, 0.0f);
				const glm::vec3 dpdv(0.0f, at(col, row + 1).height - at(col, row - 1).height, 2.0f * eps);
				const glm::vec3 normal = -glm::normalize(glm::cross(dpdu, dpdv));

				// The river data: the terrain VS and the grass / clutter culls read the influence (terrain_splat.inc.glsl: the bed
				// takes the beach layer), the clutter cull the flow speed and the water (its `Flow` and `Water` terms: reeds by slow
				// water at its edge, stones in fast; grassGroundRiverData).
				const TerrainPoint& p = at(col, row);
				const bool underWater = p.waterKind == ETerrainWater::River || p.waterKind == ETerrainWater::Lake;
				RendererVKLayout::TerrainVertex& vertex = out.vertices.emplace_back();
				vertex.height = y;
				vertex.setNormalLod(normal, underWater, params.lod);
				vertex.riverInfluence = (uint8)glm::round(glm::clamp(p.river, 0.0f, 1.0f) * 255.0f);
				vertex.riverSpeed = (uint8)glm::round(glm::clamp(p.riverSpeed / RendererVKLayout::TERRAIN_VERTEX_SPEED_MAX, 0.0f, 1.0f) * 255.0f);
				vertex.col = (uint8)col;
				vertex.row = (uint8)row;
				const glm::vec3 position(lx, y, lz);
				boundsMin = glm::min(boundsMin, position);
				boundsMax = glm::max(boundsMax, position);
			}
			Globals::jobSystem.preemptionPoint(); // nothing half-done between rows
		}
		out.bounds.pos = (boundsMin + boundsMax) * 0.5f;
		out.bounds.radius = glm::length(boundsMax - boundsMin) * 0.5f;
		out.minHeight = boundsMin.y;
		out.maxHeight = boundsMax.y;

		// The bricks: each over its cells' corner vertices, so a brick's bounds hold every triangle in it.
		const uint32 brickCells = glm::min(RendererVKLayout::TERRAIN_BRICK_CELLS, res);
		oc::vector<glm::vec2> bricks((size_t)bricksPerSide * bricksPerSide);
		for (uint32 brickRow = 0; brickRow < bricksPerSide; ++brickRow)
		{
			for (uint32 brickCol = 0; brickCol < bricksPerSide; ++brickCol)
			{
				glm::vec2 bound(FLT_MAX, -FLT_MAX);
				for (uint32 row = brickRow * brickCells; row <= glm::min((brickRow + 1) * brickCells, res); ++row)
				{
					for (uint32 col = brickCol * brickCells; col <= glm::min((brickCol + 1) * brickCells, res); ++col)
					{
						const float height = out.vertices[(size_t)row * vpr + col].height;
						bound = glm::vec2(glm::min(bound.x, height), glm::max(bound.y, height));
					}
				}
				bricks[(size_t)brickRow * bricksPerSide + brickCol] = bound;
			}
		}
		out.vertices.resize((size_t)vpr * vpr + numBrickVertices);
		memcpy(out.vertices.data() + (size_t)vpr * vpr, bricks.data(), bricks.size() * sizeof(glm::vec2));

		if (outField)
			*outField = oc::move(field);
	}

	oc::vector<uint32> terrainNodeIndices(uint32 lod0Res, uint32 stride)
	{
		const uint32 res = glm::max(1u, lod0Res);
		const uint32 vpr = res + 1;
		oc::vector<uint32> indices;
		indices.reserve((size_t)(res / stride) * (res / stride) * 6);
		for (uint32 row = 0; row + stride <= res; row += stride)
		{
			for (uint32 col = 0; col + stride <= res; col += stride)
			{
				const uint32 a = row * vpr + col;
				const uint32 b = a + stride;
				const uint32 c = (row + stride) * vpr + col;
				const uint32 d = c + stride;
				indices.push_back(a); indices.push_back(c); indices.push_back(b);
				indices.push_back(b); indices.push_back(c); indices.push_back(d);
			}
		}
		return indices;
	}
}
