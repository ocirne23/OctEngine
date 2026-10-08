module Procedural;

import Core;
import Core.glm;
import Threading; // pre-emption points between the chunk's stages and rows
import :TerrainGenerator;
import :TerrainSampler;
import :TerrainChunk;

namespace Procedural
{
	void generateChunk(const ITerrainSampler& maps, const ChunkParams& params, TerrainChunkMesh& out)
	{
		out.clear();

		const uint32 res = glm::max(1u, params.lod0Res >> params.lod);
		const uint32 vpr = res + 1; // vertices per row
		const float  step = params.chunkSize / (float)res;
		const double ox = (double)params.coord.x * (double)params.chunkSize;
		const double oz = (double)params.coord.y * (double)params.chunkSize;

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

		out.positions.reserve((size_t)vpr * vpr);
		out.normals.reserve((size_t)vpr * vpr);
		out.texCoords.reserve((size_t)vpr * vpr);
		out.stitch.reserve((size_t)vpr * vpr);

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

				out.positions.push_back({ lx, y, lz });
				out.normals.push_back(normal);
				out.texCoords.push_back({ (float)col / (float)res, (float)row / (float)res, 0.0f });
			}
			Globals::jobSystem.preemptionPoint(); // nothing half-done between rows
		}

		// The EDGE STITCH (TerrainChunkMesh::stitch): per vertex, its height on the straight edge of a neighbour
		// 1 / 2 / 3 LODs coarser - that neighbour's vertices sit on every (1 << d)-th of ours, at the same world
		// points, so it draws the edge as the line between those heights. Off the edges, and on the coarse lattice,
		// it is the vertex's own height. A neighbour coarser than this chunk's single quad clamps to the corners.
		const auto heightAt = [&](uint32 col, uint32 row) { return out.positions[(size_t)row * vpr + col].y; };
		const auto edgeHeight = [&](uint32 i, uint32 d, auto&& along) -> float
		{
			const uint32 s = glm::min(1u << d, res);
			const uint32 i0 = i / s * s;
			if (i0 == i)
				return along(i);
			const uint32 i1 = glm::min(i0 + s, res);
			return glm::mix(along(i0), along(i1), (float)(i - i0) / (float)(i1 - i0));
		};
		for (uint32 row = 0; row <= res; ++row)
		{
			for (uint32 col = 0; col <= res; ++col)
			{
				glm::vec4 s(heightAt(col, row));
				s.w = (float)params.lod;
				const bool rowEdge = row == 0 || row == res, colEdge = col == 0 || col == res;
				if (rowEdge != colEdge) // on one edge (a corner is on every lattice)
				{
					for (uint32 d = 1; d <= TERRAIN_STITCH_LEVELS; ++d)
						s[d - 1] = rowEdge
							? edgeHeight(col, d, [&](uint32 c) { return heightAt(c, row); })
							: edgeHeight(row, d, [&](uint32 r) { return heightAt(col, r); });
				}
				out.stitch.push_back(s);
			}
		}

		out.indices.reserve((size_t)res * res * 6);
		for (uint32 row = 0; row < res; ++row)
		{
			for (uint32 col = 0; col < res; ++col)
			{
				const uint32 a = row * vpr + col;
				const uint32 b = a + 1;
				const uint32 c = (row + 1) * vpr + col;
				const uint32 d = c + 1;
				out.indices.push_back(a); out.indices.push_back(c); out.indices.push_back(b);
				out.indices.push_back(b); out.indices.push_back(c); out.indices.push_back(d);
			}
		}
	}
}
