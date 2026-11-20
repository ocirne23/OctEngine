module Procedural;

import Core;
import Core.glm;

import :TreeGenerator;
import :TreeImpostor;

namespace
{
	using namespace Procedural;

	constexpr uint32 SUPERSAMPLE = 2; // per axis; the cover comes from the 2x2 subsamples
	// Bump when a bake OR the tree generator's geometry changes: the cached masks and voxel grids re-bake (treeBakeHash
	// names them by their inputs, not by the geometry).
	constexpr uint32 BAKE_VERSION = 27;

	glm::vec3 sampleWrap(const TreeBakeImage& image, glm::vec2 uv, float& outAlpha)
	{
		const float u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
		const uint32 x = glm::min((uint32)(u * (float)image.size), image.size - 1);
		const uint32 y = glm::min((uint32)(v * (float)image.size), image.size - 1);
		const uint8* p = &image.rgba[((size_t)y * image.size + x) * 4];
		outAlpha = (float)p[3] * (1.0f / 255.0f);
		return glm::vec3(p[0], p[1], p[2]) * (1.0f / 255.0f);
	}

	// One orthographic view being baked (supersampled): `centre` + right/up/dir basis, `halfExtent` along
	// right / up mapped onto the width / height. Per subsample the nearest surface: its depth and whether it is a leaf.
	struct FrameTarget
	{
		uint32 width = 0;
		uint32 height = 0;
		oc::vector<float> depth; // -FLT_MAX = nothing
		oc::vector<uint8> leaf;

		void clear(uint32 w, uint32 h)
		{
			width = w;
			height = h;
			depth.assign((size_t)w * h, -FLT_MAX);
			leaf.assign((size_t)w * h, 0);
		}
	};

	struct ViewBasis
	{
		glm::vec3 centre;
		glm::vec3 right;
		glm::vec3 up;
		glm::vec3 dir;       // toward the viewer
		glm::vec2 halfExtent; // along right / up
	};

	float edge(glm::vec2 a, glm::vec2 b, glm::vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

	// Rasterizes one mesh into the view: back faces culled against the view direction (every piece mesh is
	// closed or double-sided), z = distance toward the viewer (larger wins). `leaves`: the mesh is the leaves - `image` is
	// their atlas, its alpha tested; else the bark (opaque: no image read).
	void rasterMesh(FrameTarget& target, const TreeMesh& mesh, const ViewBasis& view, const TreeBakeImage& image, bool leaves)
	{
		if (leaves && image.size == 0)
			return;
		const glm::vec3& centre = view.centre;
		const glm::vec3& right = view.right;
		const glm::vec3& up = view.up;
		const glm::vec3& dir = view.dir;
		const glm::vec2 scale = glm::vec2((float)target.width, (float)target.height) / glm::max(view.halfExtent, glm::vec2(1e-4f));
		for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
		{
			const uint32 i0 = mesh.indices[t], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
			const glm::vec3& p0 = mesh.positions[i0];
			const glm::vec3& p1 = mesh.positions[i1];
			const glm::vec3& p2 = mesh.positions[i2];
			if (glm::dot(glm::cross(p1 - p0, p2 - p0), dir) <= 0.0f)
				continue;
			glm::vec2 sp[3];
			float z[3];
			const glm::vec3* pts[3] = { &p0, &p1, &p2 };
			for (int k = 0; k < 3; ++k)
			{
				const glm::vec3 d = *pts[k] - centre;
				sp[k] = glm::vec2((glm::dot(d, right) * 0.5f) * scale.x + (float)target.width * 0.5f,
					(float)target.height * 0.5f - (glm::dot(d, up) * 0.5f) * scale.y);
				z[k] = glm::dot(d, dir);
			}
			const float area = edge(sp[0], sp[1], sp[2]);
			if (glm::abs(area) < 1e-8f)
				continue;
			const float invArea = 1.0f / area;
			const int x0 = glm::max((int)std::floor(glm::min(sp[0].x, glm::min(sp[1].x, sp[2].x))), 0);
			const int y0 = glm::max((int)std::floor(glm::min(sp[0].y, glm::min(sp[1].y, sp[2].y))), 0);
			const int x1 = glm::min((int)std::ceil(glm::max(sp[0].x, glm::max(sp[1].x, sp[2].x))), (int)target.width - 1);
			const int y1 = glm::min((int)std::ceil(glm::max(sp[0].y, glm::max(sp[1].y, sp[2].y))), (int)target.height - 1);
			for (int y = y0; y <= y1; ++y)
			{
				for (int x = x0; x <= x1; ++x)
				{
					const glm::vec2 p((float)x + 0.5f, (float)y + 0.5f);
					const float w0 = edge(sp[1], sp[2], p) * invArea;
					const float w1 = edge(sp[2], sp[0], p) * invArea;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
						continue;
					const size_t idx = (size_t)y * target.width + (size_t)x;
					const float depth = w0 * z[0] + w1 * z[1] + w2 * z[2];
					if (depth <= target.depth[idx])
						continue;
					if (leaves)
					{
						const glm::vec2 uv = glm::vec2(mesh.texCoords[i0]) * w0 + glm::vec2(mesh.texCoords[i1]) * w1 + glm::vec2(mesh.texCoords[i2]) * w2;
						float alpha = 1.0f;
						sampleWrap(image, uv, alpha);
						if (alpha < 0.5f)
							continue;
					}
					target.depth[idx] = depth;
					target.leaf[idx] = leaves ? 1 : 0;
				}
			}
		}
	}

	uint8 toByte(float v) { return (uint8)(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

	// Resolves the supersampled view into rows [offsetY, offsetY + its height) of the mask's level 0 (`outWidth` texels
	// wide): per texel its COVER (the share of its subsamples a surface covers), the LEAF share of the covered ones and
	// their mean DEPTH, 0..1 of `depthRange` metres: how far the tree's surface stands off the card's plane, toward the
	// viewer. `back` (the same view from behind, or null): the depth is HALF THE THICKNESS between the two surfaces - a
	// vertical card is lit from either side, and one value serves both. No `back` (the card seen from above only): the
	// front surface's own height above the plane, never below it.
	void resolveView(const FrameTarget& target, const FrameTarget* back, float depthRange, oc::vector<float>& outCover, oc::vector<float>& outLeaf,
		oc::vector<float>& outDepth, uint32 outWidth, uint32 offsetY)
	{
		const uint32 w = target.width / SUPERSAMPLE, h = target.height / SUPERSAMPLE;
		const float invRange = 1.0f / glm::max(depthRange, 1e-3f);
		for (uint32 y = 0; y < h; ++y)
			for (uint32 x = 0; x < w; ++x)
			{
				uint32 covered = 0, leaves = 0;
				float depth = 0.0f;
				for (uint32 sy = 0; sy < SUPERSAMPLE; ++sy)
					for (uint32 sx = 0; sx < SUPERSAMPLE; ++sx)
					{
						const size_t k = (size_t)(y * SUPERSAMPLE + sy) * target.width + (x * SUPERSAMPLE + sx);
						if (target.depth[k] <= -FLT_MAX)
							continue;
						++covered;
						leaves += target.leaf[k];
						if (!back)
							depth += glm::max(target.depth[k], 0.0f);
						else if (back->depth[k] > -FLT_MAX)
							depth += glm::max(0.5f * (target.depth[k] + back->depth[k]), 0.0f);
					}
				const size_t out = ((size_t)offsetY + y) * outWidth + x;
				outCover[out] = (float)covered / (float)(SUPERSAMPLE * SUPERSAMPLE);
				outLeaf[out] = covered > 0 ? (float)leaves / (float)covered : 0.0f;
				outDepth[out] = covered > 0 ? glm::min(depth / (float)covered * invRange, 1.0f) : 0.0f;
			}
	}

	// Spreads a mask level's depth into its EMPTY texels (cover 0), a few texels out and never across a card's strip: a
	// filtered tap at the crown's edge mixes in its empty neighbours, and a BC block's two ends should not be spent on them.
	void dilateMaskDepth(oc::vector<float>& depth, const oc::vector<float>& cover, uint32 size, uint32 stripHeight)
	{
		constexpr int PASSES = 8;
		oc::vector<uint8> known(depth.size());
		for (size_t i = 0; i < depth.size(); ++i)
			known[i] = cover[i] > 0.0f ? 1 : 0;
		oc::vector<float> next;
		oc::vector<uint8> nextKnown;
		for (int pass = 0; pass < PASSES; ++pass)
		{
			next = depth;
			nextKnown = known;
			for (uint32 y = 0; y < size; ++y)
			{
				const uint32 strip = y / stripHeight;
				for (uint32 x = 0; x < size; ++x)
				{
					const size_t idx = (size_t)y * size + x;
					if (known[idx])
						continue;
					float sum = 0.0f;
					uint32 count = 0;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const int nx = (int)x + dx, ny = (int)y + dy;
							if (nx < 0 || ny < 0 || nx >= (int)size || ny >= (int)size || (uint32)ny / stripHeight != strip)
								continue;
							const size_t n = (size_t)ny * size + (size_t)nx;
							if (!known[n])
								continue;
							sum += depth[n];
							++count;
						}
					if (count > 0)
					{
						next[idx] = sum / (float)count;
						nextKnown[idx] = 1;
					}
				}
			}
			depth.swap(next);
			known.swap(nextKnown);
		}
	}

	// The share of a mask level's texels that pass the alpha test (0.5) with their cover x `scale`.
	float maskCoverage(const oc::vector<float>& cover, float scale)
	{
		size_t solid = 0;
		for (const float c : cover)
			solid += c * scale >= 0.5f ? 1 : 0;
		return cover.empty() ? 0.0f : (float)solid / (float)cover.size();
	}

	// A mask level's bytes, two per texel. THE FIRST IS THE (scaled) COVER up to `barkValue` - a texture filter blends it and
	// the alpha test (0.5) then runs on a continuous cover. From `barkValue` up a texel is solid for certain, and the value
	// says WHAT it is: `barkValue` = leaves .. 1 = bark. So leaves are the cover clamped (no step at all), and bark - opaque,
	// its cover near 1 anyway - is about its cover too. A step AT the test (a texel of cover 0.51 stored as 1) made the
	// filtered test solid over most of the way to a neighbour of cover 0.45: a speckled, too dense caster that flickered as
	// it moved. THE SECOND IS THE DEPTH (resolveView), spread into the empty texels around the crown.
	void encodeMaskLevel(const oc::vector<float>& cover, const oc::vector<float>& leaf, oc::vector<float> depth, float scale, float barkValue,
		uint32 size, uint32 stripHeight, oc::vector<uint8>& out)
	{
		dilateMaskDepth(depth, cover, size, stripHeight);
		out.resize(cover.size() * 2);
		for (size_t i = 0; i < cover.size(); ++i)
		{
			const float a = cover[i] * scale;
			out[i * 2] = toByte(a < barkValue ? a : glm::mix(1.0f, barkValue, leaf[i]));
			out[i * 2 + 1] = toByte(depth[i]);
		}
	}

	// THE INTERIOR of a res^3 grid of optical depths (`tau`: per crossing of `crossing` metres; x fastest, then y, then z):
	// per voxel the smallest optical depth from it out of the box along the six axis directions (half its own), over the
	// largest among the voxels a ray can see (an opacity above 2 %) - 0 on the crown's surface, 1 at its core.
	void interiorField(const oc::vector<float>& tau, uint32 res, const glm::vec3& voxel, float crossing, oc::vector<float>& out)
	{
		const size_t count = (size_t)res * res * res;
		const auto index = [res](uint32 x, uint32 y, uint32 z) { return (size_t)x + res * ((size_t)y + res * (size_t)z); };
		oc::vector<float> best(count, FLT_MAX);
		for (uint32 axis = 0; axis < 3; ++axis)
		{
			const float scale = voxel[(int)axis] / crossing; // tau per crossing -> per voxel length along this axis
			for (uint32 b = 0; b < res; ++b)
				for (uint32 a = 0; a < res; ++a)
				{
					const auto at = [&](uint32 s)
					{
						return axis == 0 ? index(s, a, b) : axis == 1 ? index(a, s, b) : index(a, b, s);
					};
					float sum = 0.0f;
					for (uint32 s = 0; s < res; ++s) // from the low side
					{
						const size_t idx = at(s);
						const float own = tau[idx] * scale;
						best[idx] = glm::min(best[idx], sum + 0.5f * own);
						sum += own;
					}
					sum = 0.0f;
					for (uint32 s = res; s-- > 0; ) // from the high side
					{
						const size_t idx = at(s);
						const float own = tau[idx] * scale;
						best[idx] = glm::min(best[idx], sum + 0.5f * own);
						sum += own;
					}
				}
		}
		constexpr float VISIBLE_TAU = 0.0202027f; // an opacity of 2 %: -ln(0.98)
		float core = 0.0f;
		for (size_t i = 0; i < count; ++i)
			if (tau[i] > VISIBLE_TAU)
				core = glm::max(core, best[i]);
		const float invCore = core > 0.0f ? 1.0f / core : 0.0f;
		out.resize(count);
		for (size_t i = 0; i < count; ++i)
			out[i] = glm::clamp(best[i] * invCore, 0.0f, 1.0f);
	}
}

namespace Procedural
{
	uint32 treeBakeHash(uint32 source, uint32 a, uint32 b)
	{
		return treeHash(BAKE_VERSION, treeHash(source, treeHash(a, b)));
	}

	void bakeTreeDensity(const TreePiece& piece, uint32 res, float leafCoverage, oc::vector<float>& out, TreeBillboardBox& outBox)
	{
		outBox = billboardBox(piece);
		out.assign((size_t)res * res * res, 0.0f);
		const glm::vec3 size = glm::max(outBox.max - outBox.min, glm::vec3(1e-3f));
		const glm::vec3 voxel = size / (float)res;
		const float invVoxelVolume = 1.0f / (voxel.x * voxel.y * voxel.z);
		const float minVoxel = glm::min(glm::min(voxel.x, voxel.y), voxel.z);
		auto splat = [&](const TreeMesh& mesh, float blockPerArea)
		{
			for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
			{
				const glm::vec3 a = mesh.positions[mesh.indices[t]];
				const glm::vec3 b = mesh.positions[mesh.indices[t + 1]];
				const glm::vec3 c = mesh.positions[mesh.indices[t + 2]];
				const float area = 0.5f * glm::length(glm::cross(b - a, c - a));
				if (area <= 0.0f)
					continue;
				// A barycentric grid of n(n+1)/2 points, fine enough that every voxel the triangle crosses gets some.
				const float longest = glm::max(glm::max(glm::length(b - a), glm::length(c - b)), glm::length(a - c));
				const uint32 n = glm::clamp((uint32)std::ceil(longest / (0.5f * minVoxel)), 1u, 32u);
				const float share = area * blockPerArea * invVoxelVolume / (float)(n * (n + 1) / 2);
				for (uint32 i = 0; i < n; ++i)
					for (uint32 j = 0; i + j < n; ++j)
					{
						const float u = ((float)i + 1.0f / 3.0f) / (float)n, v = ((float)j + 1.0f / 3.0f) / (float)n;
						const glm::vec3 p = a + (b - a) * u + (c - a) * v;
						const glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor((p - outBox.min) / voxel)), glm::ivec3(0), glm::ivec3((int)res - 1));
						out[(size_t)cell.x + res * ((size_t)cell.y + res * (size_t)cell.z)] += share;
					}
			}
		};
		// Leaves only: the bark (0.25 per area before) put a trunk's whole surface into one thin column of voxels - a dense
		// vertical line per tree, darkened by the volume's self-shadow and interior taps: dashed vertical streaks through
		// the far blobs. A trunk is far below a pixel at the volume's distances.
		// A piece WITHOUT leaves (a succulent: its leaves are bark blades, `Section`) splats its bark instead, at half a
		// leaf's weight per area: a closed blade's surface is both its faces.
		if (!piece.leaves[0].indices.empty())
			splat(piece.leaves[0], 0.5f * leafCoverage);
		else
			splat(piece.bark[0], 0.25f * leafCoverage);
	}

	uint32 treeVoxelLevels(uint32 res)
	{
		uint32 levels = 1;
		while ((res >> levels) >= TREE_VOXEL_MIN_RES)
			++levels;
		return levels;
	}

	// A level's sampled span is n - 1 voxels (centre to centre), a brick's TREE_VOXEL_BRICK.
	uint32 treeVoxelBricks(uint32 n)
	{
		return glm::max((n - 1 + TREE_VOXEL_BRICK - 1) / TREE_VOXEL_BRICK, 1u);
	}

	TreeBillboardBox treeVoxelBox(const TreePiece& piece, uint32 res)
	{
		TreeBillboardBox box = billboardBox(piece);
		const glm::vec3 pad = (box.max - box.min) / (float)glm::max(res, 3u) * 1.5f;
		box.min -= pad;
		box.max += pad;
		return box;
	}

	void bakeTreeVoxels(const TreePiece& piece, const TreeBakeImage& bark, const TreeBakeImage& leaves, uint32 res, float albedoScale,
		TreeVoxelBake& out)
	{
		const auto toLinear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
		const auto toSrgb = [](float c) { c = glm::clamp(c, 0.0f, 1.0f); return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };

		const TreeBillboardBox box = treeVoxelBox(piece, res);
		const glm::vec3 voxel = (box.max - box.min) / (float)res;
		const float crossing = std::cbrt(voxel.x * voxel.y * voxel.z); // the length the stored opacity is for
		const float minVoxel = glm::min(glm::min(voxel.x, voxel.y), voxel.z);
		const size_t count = (size_t)res * res * res;
		const auto index = [res](uint32 x, uint32 y, uint32 z) { return (size_t)x + res * ((size_t)y + res * (size_t)z); };

		// ---- Level 0: the surfaces splatted by PROJECTED area (a two-sided leaf blocks half its area on average, an opaque
		// bark surface - closed, but far below a voxel thick - also counts half: a quarter, a convex body's mean, left the
		// trunks see-through) ----
		// The leaves and the bark apart: both block the ray (the opacity), but in a voxel with leaves the bark is mostly
		// hidden behind them (colour below).
		struct Surface
		{
			oc::vector<float> area;
			oc::vector<glm::vec3> colour;
			oc::vector<glm::vec3> normal;
		};
		Surface leafSurface{ oc::vector<float>(count, 0.0f), oc::vector<glm::vec3>(count, glm::vec3(0.0f)), oc::vector<glm::vec3>(count, glm::vec3(0.0f)) };
		Surface barkSurface{ oc::vector<float>(count, 0.0f), oc::vector<glm::vec3>(count, glm::vec3(0.0f)), oc::vector<glm::vec3>(count, glm::vec3(0.0f)) };
		// The leaf normals are turned OUT of the crown (a card's two faces are one surface): from the leaves' centre.
		glm::vec3 leafMin(FLT_MAX), leafMax(-FLT_MAX);
		for (const glm::vec3& p : piece.leaves[0].positions)
		{
			leafMin = glm::min(leafMin, p);
			leafMax = glm::max(leafMax, p);
		}
		const glm::vec3 crownCentre = leafMin.x <= leafMax.x ? (leafMin + leafMax) * 0.5f : (box.min + box.max) * 0.5f;
		const auto splat = [&](const TreeMesh& mesh, const TreeBakeImage& image, float projected, bool outward, Surface& into)
		{
			if (image.size == 0 || mesh.texCoords.size() != mesh.positions.size() || mesh.normals.size() != mesh.positions.size())
				return;
			for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
			{
				const uint32 i0 = mesh.indices[t], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
				const glm::vec3 a = mesh.positions[i0], b = mesh.positions[i1], c = mesh.positions[i2];
				const float triArea = 0.5f * glm::length(glm::cross(b - a, c - a));
				if (triArea <= 0.0f)
					continue;
				// A barycentric grid fine enough that every voxel the triangle crosses gets points (bakeTreeDensity's). The
				// limit is for a degenerate triangle only: a trunk triangle of 2 m needs ~120 points an edge at 256 voxels
				// (at 64 its points stood further apart than a voxel, and the trunk was a net of filled voxels with holes).
				const float longest = glm::max(glm::max(glm::length(b - a), glm::length(c - b)), glm::length(a - c));
				const uint32 n = glm::clamp((uint32)std::ceil(longest / (0.5f * minVoxel)), 1u, 1024u);
				const float share = triArea * projected / (float)(n * (n + 1) / 2);
				const glm::vec2 ta = glm::vec2(mesh.texCoords[i0]), tb = glm::vec2(mesh.texCoords[i1]), tc = glm::vec2(mesh.texCoords[i2]);
				const glm::vec3 na = mesh.normals[i0], nb = mesh.normals[i1], nc = mesh.normals[i2];
				for (uint32 i = 0; i < n; ++i)
					for (uint32 j = 0; i + j < n; ++j)
					{
						const float u = ((float)i + 1.0f / 3.0f) / (float)n, v = ((float)j + 1.0f / 3.0f) / (float)n;
						float alpha = 1.0f;
						const glm::vec3 rgb = sampleWrap(image, ta + (tb - ta) * u + (tc - ta) * v, alpha);
						if (alpha <= 0.0f)
							continue;
						const glm::vec3 p = a + (b - a) * u + (c - a) * v;
						glm::vec3 nrm = na + (nb - na) * u + (nc - na) * v;
						const float nLen = glm::length(nrm);
						nrm = nLen > 1e-6f ? nrm / nLen : glm::vec3(0.0f, 1.0f, 0.0f);
						if (outward && glm::dot(nrm, p - crownCentre) < 0.0f)
							nrm = -nrm;
						const glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor((p - box.min) / voxel)), glm::ivec3(0), glm::ivec3((int)res - 1));
						const size_t idx = index((uint32)cell.x, (uint32)cell.y, (uint32)cell.z);
						const float w = share * alpha;
						into.area[idx] += w;
						into.colour[idx] += glm::vec3(toLinear(rgb.x), toLinear(rgb.y), toLinear(rgb.z)) * w;
						into.normal[idx] += nrm * w;
					}
			}
		};
		splat(piece.leaves[0], leaves, 0.5f, true, leafSurface);
		splat(piece.bark[0], bark, 0.5f, false, barkSurface);

		// Opacity over one crossing: tau = sigma x crossing = (projected area / volume) x crossing - leaves and bark alike.
		// The COLOUR and the NORMAL weight the bark by how much of it the voxel's own leaves leave visible, exp(-their tau):
		// the branches inside a crown add cover but little colour (by area alone they greyed the crown). A voxel of bark
		// only (the trunk under the crown) keeps the bark. Then x `Billboard AlbedoScale`.
		oc::vector<glm::vec3> colour(count, glm::vec3(0.0f)), normal(count, glm::vec3(0.0f));
		oc::vector<float> opacity(count, 0.0f), tau(count, 0.0f), interior(count, 0.0f);
		// THE LEAF SHARE of a voxel's seen surface (1 = leaves only, 0 = bark only: the trunk): the shader's leaf-only terms
		// (the crown's colour patches) take it, so they stay off the bark. An empty voxel counts as leaves (the crown's air).
		oc::vector<float> leafShare(count, 1.0f);
		glm::vec3 meanColour(0.0f);
		float meanWeight = 0.0f;
		oc::vector<uint8> filled(count, 0);
		const float invCross2 = 1.0f / (crossing * crossing);
		for (size_t i = 0; i < count; ++i)
		{
			const float leafArea = leafSurface.area[i], barkArea = barkSurface.area[i];
			if (leafArea + barkArea <= 0.0f)
				continue;
			tau[i] = (leafArea + barkArea) * invCross2;
			opacity[i] = 1.0f - std::exp(-tau[i]);
			const float barkSeen = std::exp(-leafArea * invCross2);
			const float weight = leafArea + barkArea * barkSeen;
			const glm::vec3 c = leafSurface.colour[i] + barkSurface.colour[i] * barkSeen;
			meanColour += c;
			meanWeight += weight;
			colour[i] = c / weight * albedoScale;
			normal[i] = (leafSurface.normal[i] + barkSurface.normal[i] * barkSeen) / weight; // the MEAN normal: shorter where the leaves face every way
			leafShare[i] = leafArea / weight;
			filled[i] = 1;
		}
		meanColour = meanWeight > 0.0f ? meanColour / meanWeight * albedoScale : glm::vec3(0.05f, 0.1f, 0.03f);

		// THE BARK BODIES ARE SOLID. The law above is for a cloud of small surfaces (leaves, twigs below a voxel): one bark
		// WALL that crosses a whole voxel gets an opacity of ~0.4 from it, so a ray through a trunk - a hollow shell of two
		// such walls, many voxels apart at a fine grid - passed in ~40 % of the pixels. Where a voxel's bark normals AGREE
		// (a wall: the body is at least ~2.5 voxels thick; around a thinner twig they cancel), the voxel one step BEHIND
		// the wall lies inside the body: it becomes opaque, with the wall's colour and normal. The wall voxels keep their
		// own partial opacity (the silhouette does not grow).
		{
			constexpr float WALL_AGREE = 0.93f; // the mean normal's length: a 60 degree arc of a tube is 0.955, a half tube 0.64
			const glm::vec3 invVoxel = 1.0f / voxel;
			for (uint32 z = 0; z < res; ++z)
				for (uint32 y = 0; y < res; ++y)
					for (uint32 x = 0; x < res; ++x)
					{
						const size_t wall = index(x, y, z);
						const float barkArea = barkSurface.area[wall];
						if (barkArea <= 0.0f)
							continue;
						const glm::vec3 mean = barkSurface.normal[wall] / barkArea;
						if (glm::dot(mean, mean) < WALL_AGREE * WALL_AGREE)
							continue;
						// One voxel inward: the normal in voxel units, its largest component a whole step.
						const glm::vec3 inVoxels = mean * invVoxel;
						const float largest = glm::max(glm::max(glm::abs(inVoxels.x), glm::abs(inVoxels.y)), glm::abs(inVoxels.z));
						const glm::ivec3 behind = glm::ivec3((int)x, (int)y, (int)z) - glm::ivec3(glm::round(inVoxels / largest));
						if (behind.x < 0 || behind.y < 0 || behind.z < 0 || behind.x >= (int)res || behind.y >= (int)res || behind.z >= (int)res)
							continue;
						const size_t core = index((uint32)behind.x, (uint32)behind.y, (uint32)behind.z);
						opacity[core] = 1.0f;
						tau[core] = glm::max(tau[core], 1.0f); // the interior's optical depth: a moderate value, the crown sets its scale
						if (!filled[core])
						{
							colour[core] = colour[wall];
							normal[core] = normal[wall];
							leafShare[core] = leafShare[wall];
							filled[core] = 1;
						}
					}
		}
		leafSurface = {};
		barkSurface = {};

		// The colour and the normal DILATED into the empty voxels next to the crown: the filter blends a surface voxel with
		// its empty neighbours, which would pull its colour toward black. Two rings, then the mean colour.
		for (uint32 pass = 0; pass < 2; ++pass)
		{
			oc::vector<uint8> next = filled;
			for (uint32 z = 0; z < res; ++z)
				for (uint32 y = 0; y < res; ++y)
					for (uint32 x = 0; x < res; ++x)
					{
						const size_t idx = index(x, y, z);
						if (filled[idx])
							continue;
						glm::vec3 c(0.0f), nrm(0.0f);
						float leaf = 0.0f;
						uint32 k = 0;
						const int dx[6] = { 1, -1, 0, 0, 0, 0 }, dy[6] = { 0, 0, 1, -1, 0, 0 }, dz[6] = { 0, 0, 0, 0, 1, -1 };
						for (int d = 0; d < 6; ++d)
						{
							const int nx = (int)x + dx[d], ny = (int)y + dy[d], nz = (int)z + dz[d];
							if (nx < 0 || ny < 0 || nz < 0 || nx >= (int)res || ny >= (int)res || nz >= (int)res)
								continue;
							const size_t nIdx = index((uint32)nx, (uint32)ny, (uint32)nz);
							if (!filled[nIdx])
								continue;
							c += colour[nIdx];
							nrm += normal[nIdx];
							leaf += leafShare[nIdx];
							++k;
						}
						if (k == 0)
							continue;
						colour[idx] = c / (float)k;
						normal[idx] = nrm / (float)k;
						leafShare[idx] = leaf / (float)k;
						next[idx] = 1;
					}
			filled = oc::move(next);
		}
		for (size_t i = 0; i < count; ++i)
			if (!filled[i])
				colour[i] = meanColour;

		// THE INTERIOR (interiorField): 0 on the crown's surface, 1 at its core.
		interiorField(tau, res, voxel, crossing, interior);

		// ---- The levels, cut into BRICKS (TreeImpostor.ixx "The VOXEL tier") ----
		constexpr uint32 B = TREE_VOXEL_BRICK, S = TREE_VOXEL_BRICK_TEXELS, C = TREE_VOXEL_SURFACE_TEXELS;
		static_assert(2 * C == S && 4 * C * C == S * S, "the surface atlas is half the opacity atlas each way");
		constexpr size_t OPACITY_BYTES = (size_t)S * S * S;         // a brick's opacity: [z][y][x]
		constexpr size_t SURFACE_BYTES = (size_t)C * C * C * 4 * 2; // its surface: the colour set, then the normal set, RGBA [z][y][x]
		const uint32 levels = treeVoxelLevels(res);
		struct LevelIndex
		{
			uint32 bricks = 0;       // per axis
			oc::vector<uint32> slots; // per brick: its atlas slot, 0 = empty
		};
		oc::vector<LevelIndex> levelIndex(levels);
		oc::vector<uint8> opacityBricks(OPACITY_BYTES, 0), surfaceBricks(SURFACE_BYTES, 0); // slot 0: the header
		uint32 numSlots = 1;
		out = {};
		uint32 n = res;
		for (uint32 level = 0; level < levels; ++level)
		{
			if (level > 0)
			{
				// Halve: the coverage from the transmission through the children along each axis, the rest weighted by opacity.
				const uint32 m = n / 2;
				const auto childIdx = [n](uint32 x, uint32 y, uint32 z) { return (size_t)x + n * ((size_t)y + n * (size_t)z); };
				const size_t coarse = (size_t)m * m * m;
				oc::vector<float> cOpacity(coarse), cInterior(coarse), cLeafShare(coarse);
				oc::vector<glm::vec3> cColour(coarse), cNormal(coarse);
				for (uint32 z = 0; z < m; ++z)
					for (uint32 y = 0; y < m; ++y)
						for (uint32 x = 0; x < m; ++x)
						{
							float t[2][2][2];
							float weight = 0.0f, inter = 0.0f, leaf = 0.0f;
							glm::vec3 c(0.0f), nrm(0.0f);
							for (uint32 k = 0; k < 8; ++k)
							{
								const uint32 ox = k & 1u, oy = (k >> 1) & 1u, oz = k >> 2;
								const size_t idx = childIdx(2 * x + ox, 2 * y + oy, 2 * z + oz);
								t[ox][oy][oz] = 1.0f - opacity[idx];
								const float w = opacity[idx] + 1e-3f;
								weight += w;
								c += colour[idx] * w;
								nrm += normal[idx] * w;
								inter += interior[idx] * w;
								leaf += leafShare[idx] * w;
							}
							float transmit = 0.0f;
							for (uint32 i = 0; i < 2; ++i)
								for (uint32 j = 0; j < 2; ++j)
									transmit += t[0][i][j] * t[1][i][j] + t[i][0][j] * t[i][1][j] + t[i][j][0] * t[i][j][1];
							const size_t cIdx = (size_t)x + m * ((size_t)y + m * (size_t)z);
							cOpacity[cIdx] = 1.0f - transmit / 12.0f;
							cColour[cIdx] = c / weight;
							cNormal[cIdx] = nrm / weight;
							cInterior[cIdx] = inter / weight;
							cLeafShare[cIdx] = leaf / weight;
						}
				opacity = oc::move(cOpacity);
				colour = oc::move(cColour);
				normal = oc::move(cNormal);
				interior = oc::move(cInterior);
				leafShare = oc::move(cLeafShare);
				n = m;
			}
			// This level's bricks: brick k stores the voxels k B .. k B + B per axis (beyond the grid: empty), and is kept
			// only when one of them holds cover.
			const uint32 D = treeVoxelBricks(n);
			LevelIndex& levelBricks = levelIndex[level];
			levelBricks.bricks = D;
			levelBricks.slots.assign((size_t)D * D * D, 0u);
			out.bricksTotal += D * D * D;
			const auto voxelIdx = [n](uint32 x, uint32 y, uint32 z) { return (size_t)x + n * ((size_t)y + n * (size_t)z); };
			const float spacing = (float)B / (float)(C - 1); // voxels between two surface samples: the tent's radius too
			for (uint32 kz = 0; kz < D; ++kz)
				for (uint32 ky = 0; ky < D; ++ky)
					for (uint32 kx = 0; kx < D; ++kx)
					{
						const glm::uvec3 first(kx * B, ky * B, kz * B);
						uint8 block[OPACITY_BYTES];
						bool any = false;
						for (uint32 z = 0; z < S; ++z)
							for (uint32 y = 0; y < S; ++y)
								for (uint32 x = 0; x < S; ++x)
								{
									const glm::uvec3 v = first + glm::uvec3(x, y, z);
									const uint8 a = v.x < n && v.y < n && v.z < n ? toByte(opacity[voxelIdx(v.x, v.y, v.z)]) : (uint8)0;
									block[x + S * (y + S * z)] = a;
									any |= a != 0;
								}
						if (!any)
							continue;
						levelBricks.slots[kx + D * ((size_t)ky + D * (size_t)kz)] = numSlots++;
						opacityBricks.insert(opacityBricks.end(), block, block + OPACITY_BYTES);

						// THE SURFACE: C samples per axis over the brick's span, each the mean of the voxels under a tent of the
						// sample spacing, weighted by their opacity (an empty voxel's dilated colour counts only where nothing
						// else is). A voxel's centre is its index here.
						uint8 surface[SURFACE_BYTES];
						for (uint32 jz = 0; jz < C; ++jz)
							for (uint32 jy = 0; jy < C; ++jy)
								for (uint32 jx = 0; jx < C; ++jx)
								{
									const glm::vec3 centre = glm::min(glm::vec3(first) + glm::vec3((float)jx, (float)jy, (float)jz) * spacing, glm::vec3((float)(n - 1)));
									const glm::ivec3 lo = glm::max(glm::ivec3(glm::ceil(centre - spacing + 1e-4f)), glm::ivec3(0));
									const glm::ivec3 hi = glm::min(glm::ivec3(glm::floor(centre + spacing - 1e-4f)), glm::ivec3((int)n - 1));
									glm::vec3 c(0.0f), nrm(0.0f);
									float inter = 0.0f, leaf = 0.0f, weight = 0.0f;
									for (int z = lo.z; z <= hi.z; ++z)
									{
										const float wz = 1.0f - glm::abs((float)z - centre.z) / spacing;
										for (int y = lo.y; y <= hi.y; ++y)
										{
											const float wzy = wz * (1.0f - glm::abs((float)y - centre.y) / spacing);
											for (int x = lo.x; x <= hi.x; ++x)
											{
												const size_t idx = voxelIdx((uint32)x, (uint32)y, (uint32)z);
												const float w = wzy * (1.0f - glm::abs((float)x - centre.x) / spacing) * (opacity[idx] + 1e-4f);
												c += colour[idx] * w;
												nrm += normal[idx] * w;
												inter += interior[idx] * w;
												leaf += leafShare[idx] * w;
												weight += w;
											}
										}
									}
									const float inv = weight > 0.0f ? 1.0f / weight : 0.0f;
									const size_t texel = ((size_t)jx + C * ((size_t)jy + C * (size_t)jz)) * 4;
									uint8* colourTexel = surface + texel;
									uint8* normalTexel = surface + (size_t)C * C * C * 4 + texel;
									colourTexel[0] = toByte(toSrgb(c.x * inv));
									colourTexel[1] = toByte(toSrgb(c.y * inv));
									colourTexel[2] = toByte(toSrgb(c.z * inv));
									colourTexel[3] = toByte(inter * inv);
									const glm::vec3 meanNormal = glm::clamp(nrm * inv, glm::vec3(-1.0f), glm::vec3(1.0f));
									normalTexel[0] = toByte(meanNormal.x * 0.5f + 0.5f);
									normalTexel[1] = toByte(meanNormal.y * 0.5f + 0.5f);
									normalTexel[2] = toByte(meanNormal.z * 0.5f + 0.5f);
									normalTexel[3] = toByte(leaf * inv); // the leaf share
								}
						surfaceBricks.insert(surfaceBricks.end(), surface, surface + SURFACE_BYTES);
					}
		}
		out.bricks = numSlots - 1;

		// ---- The atlases: slot s at column s % cols, row s / cols; about square ----
		uint32 cols = 2, colsLog2 = 1;
		while (cols < TREE_VOXEL_MAX_COLS && (size_t)cols * cols * S < numSlots)
		{
			cols *= 2;
			++colsLog2;
		}
		const uint32 rows = (numSlots + cols - 1) / cols;
		out.width = cols * S * S;
		out.height = rows * S;
		const uint32 surfaceWidth = out.width / 2, surfaceHeight = out.height / 2;
		out.opacity.assign((size_t)out.width * out.height, 0);
		out.surface.assign((size_t)surfaceWidth * surfaceHeight * 4, 0);
		for (uint32 slot = 0; slot < numSlots; ++slot)
		{
			const uint32 col = slot % cols, row = slot / cols;
			const uint8* opacitySrc = &opacityBricks[(size_t)slot * OPACITY_BYTES];
			for (uint32 z = 0; z < S; ++z)
				for (uint32 y = 0; y < S; ++y)
					memcpy(&out.opacity[(size_t)(row * S + y) * out.width + (size_t)col * S * S + z * S], opacitySrc + S * (y + S * z), S);
			const uint8* surfaceSrc = &surfaceBricks[(size_t)slot * SURFACE_BYTES];
			for (uint32 set = 0; set < 2; ++set)
				for (uint32 z = 0; z < C; ++z)
					for (uint32 y = 0; y < C; ++y)
						memcpy(&out.surface[((size_t)(row * C + y) * surfaceWidth + (size_t)col * 2 * C * C + set * C * C + z * C) * 4],
							surfaceSrc + ((size_t)set * C * C * C + C * (y + C * z)) * 4, (size_t)C * 4);
		}

		// ---- The index: the header texel, then every level's bricks ----
		uint32 resLog2 = 0;
		while ((1u << resLog2) < res)
			++resLog2;
		out.indexWidth = levelIndex[0].bricks * levelIndex[0].bricks;
		out.indexHeight = 1;
		for (const LevelIndex& entry : levelIndex)
			out.indexHeight += entry.bricks;
		out.index.assign((size_t)out.indexWidth * out.indexHeight * 4, 0);
		out.index[0] = (uint8)resLog2;
		out.index[1] = (uint8)levels;
		out.index[2] = (uint8)colsLog2;
		out.index[3] = 255;
		uint32 baseY = 1;
		for (const LevelIndex& entry : levelIndex)
		{
			const uint32 D = entry.bricks;
			for (uint32 z = 0; z < D; ++z)
				for (uint32 y = 0; y < D; ++y)
					for (uint32 x = 0; x < D; ++x)
					{
						const uint32 slot = entry.slots[x + D * ((size_t)y + D * (size_t)z)];
						uint8* texel = &out.index[((size_t)(baseY + y) * out.indexWidth + x + z * D) * 4];
						texel[0] = (uint8)(slot & 255u);
						texel[1] = (uint8)((slot >> 8) & 255u);
						texel[2] = (uint8)((slot >> 16) & 255u);
						texel[3] = 255;
					}
			baseY += D;
		}
	}

	void treeVoxelBoxMesh(const TreeBillboardBox& box, TreeMesh& out)
	{
		out = {};
		const glm::vec3 size = box.max - box.min;
		for (uint32 k = 0; k < 8; ++k)
		{
			const glm::vec3 grid((float)(k & 1u), (float)((k >> 1) & 1u), (float)(k >> 2));
			out.positions.push_back(box.min + grid * size);
			out.normals.push_back(grid);
			out.tangents.push_back(size);
			out.bitangents.push_back(glm::vec3(0.0f, 0.0f, 1.0f));
			out.texCoords.push_back(glm::vec3(0.0f, 0.0f, TREE_VOXEL_BOX_WIND_CODE));
			out.bones.push_back(0);
		}
		// Corner k = x | y << 1 | z << 2. Counter-clockwise seen from outside (the engine's front faces).
		const uint32 faces[6][4] = {
			{ 0, 4, 6, 2 }, // -X
			{ 1, 3, 7, 5 }, // +X
			{ 0, 1, 5, 4 }, // -Y
			{ 2, 6, 7, 3 }, // +Y
			{ 0, 2, 3, 1 }, // -Z
			{ 4, 5, 7, 6 }, // +Z
		};
		for (const auto& f : faces)
		{
			out.indices.insert(out.indices.end(), { f[0], f[1], f[2], f[0], f[2], f[3] });
		}
	}

	TreeBillboardBox billboardBox(const TreePiece& piece)
	{
		glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
		for (const TreeMesh* mesh : { &piece.bark[0], &piece.leaves[0] })
			for (const glm::vec3& p : mesh->positions)
			{
				mn = glm::min(mn, p);
				mx = glm::max(mx, p);
			}
		if (mn.x > mx.x)
			return {};
		return { mn, glm::max(mx, mn + 1e-3f) };
	}

	TreeBillboardLayout billboardLayout(uint32 size)
	{
		constexpr uint32 NUM_STRIPS = 3;
		TreeBillboardLayout layout;
		// The strip height a multiple of the largest power of two p that leaves >= 4 px per strip at mip log2(p): every
		// strip boundary then falls on a texel boundary at every mip the material gets, so no mip mixes two cards (and a
		// BC block never spans two). 512 / 3 strips: p = 32, 160 rows each (32 spare rows at the bottom).
		const uint32 raw = size / NUM_STRIPS;
		uint32 p = 1;
		while (raw / (p * 2) >= 4)
			p *= 2;
		layout.stripHeight = glm::max(raw / p, 1u) * p;
		layout.numMips = 1;
		for (uint32 s = p; s > 1; s /= 2)
			++layout.numMips;
		return layout;
	}

	// One card: its centre and basis (image u along `right`, v down along -`up`, `normal` its front) and its half size.
	struct CardView
	{
		glm::vec3 centre;
		glm::vec3 right;
		glm::vec3 up;
		glm::vec3 normal;
		glm::vec2 halfExtent;
	};

	// The three cards in strip order. The vertical ones' PLANES pass through the trunk axis (x = 0 / z = 0), not through
	// the box centre, which the crown's spread pushes off the trunk; in-plane they cover the whole box. The bake projects
	// orthographically along the card normal, so a plane's offset never changes its image.
	static void billboardCards(const TreeBillboardBox& box, float topCardHeight, CardView (&views)[3])
	{
		const glm::vec3 centre = (box.min + box.max) * 0.5f;
		const glm::vec3 half = (box.max - box.min) * 0.5f;
		// Seen along X: image u along the trunk (+Y), v along +Z.
		views[0] = { glm::vec3(0.0f, centre.y, centre.z), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(half.y, half.z) };
		// Seen along Z: image u along the trunk (+Y), v along -X.
		views[1] = { glm::vec3(centre.x, centre.y, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(half.y, half.x) };
		// The horizontal card, centred ON the axis at `topCardHeight` of the box, seen from above: u along +Z, v along +X.
		const float maxX = glm::max(-box.min.x, box.max.x), maxZ = glm::max(-box.min.z, box.max.z);
		views[2] = { glm::vec3(0.0f, glm::mix(box.min.y, box.max.y, topCardHeight), 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(maxZ, maxX) };
	}

	// A card's DEPTH RANGE (m): what the mask's depth channel at 1 stands for, off the card's plane (resolveView). A
	// vertical card: the box's reach off the trunk axis along its normal (half a crown's thickness never passes it). The
	// horizontal one: from the card up to the box's top.
	static glm::vec3 billboardDepthRanges(const TreeBillboardBox& box, float topCardHeight)
	{
		return glm::max(glm::vec3(glm::max(-box.min.x, box.max.x), glm::max(-box.min.z, box.max.z),
			(box.max.y - box.min.y) * (1.0f - topCardHeight)), glm::vec3(1e-3f));
	}

	void billboardMesh(const TreeBillboardBox& box, uint32 size, float topCardHeight, TreeMesh& out)
	{
		CardView views[3];
		billboardCards(box, topCardHeight, views);
		const glm::vec3 depthRanges = billboardDepthRanges(box, topCardHeight);
		const float stripV = (float)billboardLayout(size).stripHeight / (float)size; // one strip in v
		// THE CROWN SHAPE for a ray hit's normal (tree_card.inc.glsl): its radii = the cards' half extents, in every
		// vertex's tangent (a card has no normal map). Its centre = the cards' bound centre, on the trunk axis.
		const glm::vec3 crownRadii(glm::max(-box.min.x, box.max.x), (box.max.y - box.min.y) * 0.5f, glm::max(-box.min.z, box.max.z));
		for (uint32 v = 0; v < 3; ++v)
		{
			const CardView& view = views[v];
			const glm::vec3 r = view.right * view.halfExtent.x;
			const glm::vec3 u = view.up * view.halfExtent.y;
			const glm::vec3 corners[4] = { view.centre - r - u, view.centre + r - u, view.centre + r + u, view.centre - r + u };
			// Image u along right; image v down from the card's top, into this card's strip of the texture.
			const glm::vec2 uvs[4] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
			// Both faces read the card's strip with the same u along `right`: the back face shows it through the card.
			// THE NORMAL'S LENGTH = the card's depth range (shadow_depth.vs.glsl: how far the mask's depth lifts the shadow
			// caster off the card); a ray hit takes the crown shape's normal, not this one.
			const glm::vec3 normal = view.normal * depthRanges[(int)v];
			for (int face = 0; face < 2; ++face)
			{
				const uint32 base = out.numVertices();
				for (int k = 0; k < 4; ++k)
				{
					out.positions.push_back(corners[k]);
					out.normals.push_back(face == 0 ? normal : -normal);
					out.tangents.push_back(crownRadii);
					out.bitangents.push_back(view.up);
					out.texCoords.push_back(glm::vec3(uvs[k].x, (uvs[k].y + (float)v) * stripV, 0.0f)); // z 0: a plain mesh (no wind payload)
					out.bones.push_back(0);
				}
				if (face == 0)
					out.indices.insert(out.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
				else
					out.indices.insert(out.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
			}
		}
	}

	void bakeBillboardMask(const TreePiece& piece, const TreeBakeImage& leaves, uint32 size, float barkValue, float topCardHeight,
		oc::vector<oc::vector<uint8>>& outMips)
	{
		const TreeBillboardLayout layout = billboardLayout(size);
		// LEVEL 0: each card's view into its strip (the spare rows below the last strip stay empty). A vertical card is
		// seen from behind too: its depth is half the thickness between the two views' surfaces.
		oc::vector<float> cover((size_t)size * size, 0.0f), leaf((size_t)size * size, 0.0f), depth((size_t)size * size, 0.0f);
		const TreeBillboardBox box = billboardBox(piece);
		CardView views[3];
		billboardCards(box, topCardHeight, views);
		const glm::vec3 depthRanges = billboardDepthRanges(box, topCardHeight);
		FrameTarget target, back;
		for (uint32 v = 0; v < 3; ++v)
		{
			const ViewBasis view{ views[v].centre, views[v].right, views[v].up, views[v].normal, views[v].halfExtent };
			target.clear(size * SUPERSAMPLE, layout.stripHeight * SUPERSAMPLE);
			rasterMesh(target, piece.bark[0], view, leaves, false);
			rasterMesh(target, piece.leaves[0], view, leaves, true);
			const bool vertical = v < 2;
			if (vertical)
			{
				// The same image (right / up as they are), the other faces: z = the distance behind the plane.
				const ViewBasis behind{ view.centre, view.right, view.up, -view.dir, view.halfExtent };
				back.clear(size * SUPERSAMPLE, layout.stripHeight * SUPERSAMPLE);
				rasterMesh(back, piece.bark[0], behind, leaves, false);
				rasterMesh(back, piece.leaves[0], behind, leaves, true);
			}
			resolveView(target, vertical ? &back : nullptr, depthRanges[(int)v], cover, leaf, depth, size, v * layout.stripHeight);
		}

		// THE LEVELS: the cover averaged (unscaled: it feeds the next level), the leaf share and the depth by the cover. A
		// level's test then runs on its cover x a scale that keeps level 0's solid share (binary search), as the leaf
		// clusters' mips.
		outMips.clear();
		outMips.resize(layout.numMips);
		const float targetCoverage = maskCoverage(cover, 1.0f);
		encodeMaskLevel(cover, leaf, depth, 1.0f, barkValue, size, layout.stripHeight, outMips[0]);
		uint32 levelSize = size;
		for (uint32 k = 1; k < layout.numMips; ++k)
		{
			const uint32 half = levelSize / 2;
			oc::vector<float> coverNext((size_t)half * half), leafNext((size_t)half * half), depthNext((size_t)half * half);
			for (uint32 y = 0; y < half; ++y)
				for (uint32 x = 0; x < half; ++x)
				{
					float c = 0.0f, l = 0.0f, d = 0.0f;
					for (uint32 s = 0; s < 4; ++s)
					{
						const size_t src = (size_t)(y * 2 + (s >> 1)) * levelSize + (x * 2 + (s & 1));
						c += cover[src];
						l += cover[src] * leaf[src];
						d += cover[src] * depth[src];
					}
					coverNext[(size_t)y * half + x] = c * 0.25f;
					leafNext[(size_t)y * half + x] = c > 0.0f ? l / c : 0.0f;
					depthNext[(size_t)y * half + x] = c > 0.0f ? d / c : 0.0f;
				}
			cover = oc::move(coverNext);
			leaf = oc::move(leafNext);
			depth = oc::move(depthNext);
			levelSize = half;
			float lo = 0.0f, hi = 8.0f;
			for (int i = 0; i < 16; ++i)
			{
				const float mid = (lo + hi) * 0.5f;
				(maskCoverage(cover, mid) < targetCoverage ? lo : hi) = mid;
			}
			encodeMaskLevel(cover, leaf, depth, hi, barkValue, levelSize, layout.stripHeight >> k, outMips[k]);
		}
	}
}
