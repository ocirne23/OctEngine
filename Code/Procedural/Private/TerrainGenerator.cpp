module Procedural;

import Core;
import Core.glm;
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
		// neighbouring grid entries. The obvious version — sampleHeight at the vertex plus four more for a
		// central difference — costs 5 samples per vertex, which at LOD0 is 513*513*5 = 1.3M point queries
		// for ONE chunk. That is affordable for a noise field and ruinous for V3, where every query resolves
		// a diffusion tile block and takes the tile-cache lock. sampleGrid resolves the block once and then
		// fills lock-free, so this is ~5x fewer samples AND ~1.3M fewer lock round-trips.
		//
		// The halo is not overhead: the old code already sampled outside the chunk for the border vertices'
		// differences. It just did it one point at a time.
		const uint32 gpr = vpr + 2; // grid points per row: the vertex grid plus a 1-point halo
		oc::vector<TerrainPoint> field((size_t)gpr * gpr);
		maps.sampleGrid(ox - (double)step, oz - (double)step, (double)step, gpr, gpr, field);

		// Vertex coords -> grid entry. SIGNED on purpose: the border vertices ask for col/row -1, which the
		// halo holds. (With uint32 that underflows and only lands on the right entry by wrapping twice.)
		const auto at = [&](int32 col, int32 row) -> const TerrainPoint&
		{
			return field[(size_t)(row + 1) * gpr + (size_t)(col + 1)];
		};

		out.positions.reserve((size_t)vpr * vpr);
		out.normals.reserve((size_t)vpr * vpr);
		out.tangents.reserve((size_t)vpr * vpr);
		out.bitangents.reserve((size_t)vpr * vpr);
		out.texCoords.reserve((size_t)vpr * vpr);

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
				// the real bottom to hit — the old max(h, seaLevel) lid read as zero-depth water and
				// co-planed with it).
				const float y = at(col, row).height;

				// Central-difference TBN (matches the engine's existing terrain convention).
				const glm::vec3 dpdu(2.0f * eps, at(col + 1, row).height - at(col - 1, row).height, 0.0f);
				const glm::vec3 dpdv(0.0f, at(col, row + 1).height - at(col, row - 1).height, 2.0f * eps);
				const glm::vec3 normal = -glm::normalize(glm::cross(dpdu, dpdv));

				out.positions.push_back({ lx, y, lz });
				out.normals.push_back(normal);
				out.tangents.push_back(glm::normalize(dpdu));
				out.bitangents.push_back(glm::normalize(dpdv));
				out.texCoords.push_back({ (float)col / (float)res, (float)row / (float)res, 0.0f });
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

		// --- Skirt: a vertical border wall dropped by skirtDepth, hiding cracks against coarser neighbours.
		// Emitted double-sided (both windings) so it fills the gap regardless of view/backface-cull direction;
		// the extra triangles are negligible and this keeps the generator neighbour-independent.
		if (params.skirtDepth > 0.0f)
		{
			// Perimeter vertex indices, walked as a closed loop.
			oc::vector<uint32> perimeter;
			perimeter.reserve(4u * res);
			for (uint32 col = 0; col < res; ++col)           perimeter.push_back(0u * vpr + col);        // top row, L->R
			for (uint32 row = 0; row < res; ++row)           perimeter.push_back(row * vpr + res);        // right col, T->B
			for (uint32 col = res; col > 0; --col)           perimeter.push_back(res * vpr + col);        // bottom row, R->L
			for (uint32 row = res; row > 0; --row)           perimeter.push_back(row * vpr + 0u);         // left col, B->T

			for (size_t k = 0; k < perimeter.size(); ++k)
			{
				const uint32 iA = perimeter[k];
				const uint32 iB = perimeter[(k + 1) % perimeter.size()];

				const glm::vec3 lowA = out.positions[iA] - glm::vec3(0.0f, params.skirtDepth, 0.0f);
				const glm::vec3 lowB = out.positions[iB] - glm::vec3(0.0f, params.skirtDepth, 0.0f);

				const uint32 la = (uint32)out.positions.size();
				out.positions.push_back(lowA);
				out.normals.push_back(out.normals[iA]);
				out.tangents.push_back(out.tangents[iA]);
				out.bitangents.push_back(out.bitangents[iA]);
				out.texCoords.push_back(out.texCoords[iA]);

				const uint32 lb = (uint32)out.positions.size();
				out.positions.push_back(lowB);
				out.normals.push_back(out.normals[iB]);
				out.tangents.push_back(out.tangents[iB]);
				out.bitangents.push_back(out.bitangents[iB]);
				out.texCoords.push_back(out.texCoords[iB]);

				// Quad [iA, iB, lb, la] as two triangles, then reversed for the back side.
				out.indices.push_back(iA); out.indices.push_back(iB); out.indices.push_back(lb);
				out.indices.push_back(iA); out.indices.push_back(lb); out.indices.push_back(la);
				out.indices.push_back(iA); out.indices.push_back(lb); out.indices.push_back(iB);
				out.indices.push_back(iA); out.indices.push_back(la); out.indices.push_back(lb);
			}
		}
	}
}
