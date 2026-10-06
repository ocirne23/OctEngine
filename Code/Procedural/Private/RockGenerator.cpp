module;

#include <meshoptimizer/meshoptimizer.h> // the base mesh: the dense surface simplified

module Procedural;

import Core;
import Core.glm;

import :RockType;
import :RockGenerator;
import :TreeGenerator; // treeHash

namespace
{
	using namespace Procedural;

	float smin(float a, float b, float k)
	{
		if (k <= 0.0f)
			return glm::min(a, b);
		const float h = glm::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
		return glm::mix(b, a, h) - k * h * (1.0f - h);
	}
	float smax(float a, float b, float k) { return -smin(-a, -b, k); }

	// 3D gradient noise, integer-hashed (treeHash): the GPU copy reproduces it. ~[-1, 1].
	float gradientNoise(const glm::vec3& p, uint32 seed)
	{
		const glm::vec3 fl = glm::floor(p);
		const glm::ivec3 c(fl);
		const glm::vec3 f = p - fl;
		const glm::vec3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
		auto grad = [&](int dx, int dy, int dz)
		{
			const uint32 h = treeHash(seed ^ treeHash((uint32)(c.x + dx) + treeHash((uint32)(c.y + dy) + treeHash((uint32)(c.z + dz)))));
			// 12 edge directions of a cube.
			static constexpr float G[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 },
				{ 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
			const float* g = G[h % 12u];
			return g[0] * (f.x - (float)dx) + g[1] * (f.y - (float)dy) + g[2] * (f.z - (float)dz);
		};
		const float x00 = glm::mix(grad(0, 0, 0), grad(1, 0, 0), u.x);
		const float x10 = glm::mix(grad(0, 1, 0), grad(1, 1, 0), u.x);
		const float x01 = glm::mix(grad(0, 0, 1), grad(1, 0, 1), u.x);
		const float x11 = glm::mix(grad(0, 1, 1), grad(1, 1, 1), u.x);
		return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);
	}

	// Superellipsoid |x/a|^n + |y/b|^n + |z/c|^n = 1 (n = squareness: 2 ellipsoid, 3 a rounded block, 8 near a box):
	// the Boulder / Block primitive (a Pillar is pillarSdf). k = the n-norm of q / h; the distance to first
	// order is (k - 1) / |grad k|.
	float superellipsoidSdf(const glm::vec3& q, const glm::vec3& h, float n)
	{
		const glm::vec3 a = glm::abs(q) / h;
		const glm::vec3 an1(std::pow(a.x, n - 1.0f), std::pow(a.y, n - 1.0f), std::pow(a.z, n - 1.0f));
		const float sum = an1.x * a.x + an1.y * a.y + an1.z * a.z;
		if (sum < 1e-12f)
			return -glm::min(h.x, glm::min(h.y, h.z));
		const float k = std::pow(sum, 1.0f / n);
		const float gradLen = glm::length(an1 / h) * std::pow(k, 1.0f - n);
		return gradLen > 1e-8f ? (k - 1.0f) / gradLen : -glm::min(h.x, glm::min(h.y, h.z));
	}

	// The low-frequency domain warp: flat fracture faces bulge and straight edges bend (weathered, not cut).
	glm::vec3 warpPoint(const RockShape& s, const glm::vec3& p)
	{
		if (s.warpAmplitude <= 0.0f)
			return p;
		const glm::vec3 q = p * s.warpFrequency;
		return p + s.warpAmplitude * glm::vec3(gradientNoise(q, s.noiseSeed ^ 0xA341u),
			gradientNoise(q + glm::vec3(5.2f), s.noiseSeed ^ 0xB517u), gradientNoise(q + glm::vec3(9.7f), s.noiseSeed ^ 0xC623u));
	}

	// The body without strata, noise and pits: blocks, fracture planes, the split. In the SHAPE frame.
	// EROSION is a Minkowski rounding: the body is built `erosion` smaller (blocks and planes pulled in), then the field
	// is grown by it - every convex edge and corner becomes a radius-`erosion` round, the size stays. The pile's
	// joints blend wider with it too (weathering fills the concave seams a little).
	// A PILLAR's side, in its block's frame: a superellipse section (n = squareness, the block's x / z half sizes)
	// whose width follows the block's PROFILE over its height - a Catmull-Rom spline through ROCK_PROFILE_POINTS
	// widths, base to top. It runs on below the block (the group's floor cuts it: bodySdf) and above it (the caller
	// cuts the top). First-order distance: (r - width(y)) / |its gradient|. `e`: the erosion shrink.
	float pillarSdf(const glm::vec3& q, const glm::vec3& half, const float* profile, float n, float e)
	{
		const float radius = glm::min(half.x, half.z);
		const glm::vec2 a = glm::abs(glm::vec2(q.x / half.x, q.z / half.z));
		const float r = std::pow(std::pow(a.x, n) + std::pow(a.y, n), 1.0f / n) * radius;
		constexpr int LAST = ROCK_PROFILE_POINTS - 1;
		const float t = glm::clamp((q.y + half.y) / (2.0f * half.y), 0.0f, 1.0f) * (float)LAST;
		const int i = glm::min((int)t, LAST - 1);
		const float f = t - (float)i;
		const float p0 = profile[glm::max(i - 1, 0)], p1 = profile[i], p2 = profile[i + 1], p3 = profile[glm::min(i + 2, LAST)];
		const float c1 = p2 - p0, c2 = 2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3, c3 = 3.0f * (p1 - p2) + p3 - p0;
		const float width = 0.5f * (2.0f * p1 + f * (c1 + f * (c2 + f * c3)));
		const float slope = 0.5f * (c1 + f * (2.0f * c2 + f * 3.0f * c3)) * (float)LAST / (2.0f * half.y) * radius;
		return (r - glm::max(radius * width - e, 0.004f)) / std::sqrt(1.0f + slope * slope);
	}

	// The union of the first `count` blocks, each shrunk by `e` (unwarped: the pile placement reads it too).
	float blocksSdf(const RockShape& s, const glm::vec3& p, uint32 count, float e)
	{
		float d = 1e9f;
		for (uint32 b = 0; b < count; ++b)
		{
			const glm::vec3 q = s.blockRot[b] * (p - s.blockCentre[b]);
			if (s.kind == ERockShape::Pillar)
			{
				// The side, cut at the block's top; the pillars of a group merge at their bases.
				const float side = pillarSdf(q, s.blockHalf[b], s.blockProfile[b], s.squareness, e);
				d = smin(d, smax(side, q.y - (s.blockHalf[b].y - e), 0.01f + e * 0.5f), 0.03f + e * 0.5f);
				continue;
			}
			const glm::vec3 half = glm::max(s.blockHalf[b] - e, glm::vec3(0.02f));
			d = smin(d, superellipsoidSdf(q, half, s.squareness), 0.015f + e * 0.5f);
		}
		return d;
	}

	float bodySdf(const RockShape& s, const glm::vec3& pIn)
	{
		const glm::vec3 p = warpPoint(s, pIn);
		const float e = s.erosion;
		float d = blocksSdf(s, p, s.blockCount, e);
		for (uint32 i = 0; i < s.planeCount; ++i)
			d = smax(d, glm::dot(p, glm::vec3(s.planes[i])) - (s.planes[i].w - e), s.round * 0.5f);
		if (s.split)
			d = glm::max(d, -(glm::abs(glm::dot(p, glm::vec3(s.splitPlane)) - s.splitPlane.w) - s.splitGap * 0.5f - e));
		// A group of pillars stands on ONE flat floor: the UNWARPED point, so the warp cannot wave it (the sink must
		// cover whatever of the base shows).
		if (s.kind == ERockShape::Pillar)
			d = glm::max(d, s.floorY + e - pIn.y);
		return d - e;
	}

	float fullSdf(const RockShape& s, const glm::vec3& p)
	{
		float d = bodySdf(s, p);
		if (s.strataSpacing > 0.0f)
		{
			const float t = p.y / s.strataSpacing + s.strataPhase + 0.15f * gradientNoise(p * 1.5f, s.noiseSeed ^ 0x5157u);
			const float layer = std::floor(t);
			const float f = t - layer;
			const float groove = 1.0f - glm::smoothstep(0.0f, 0.18f, glm::min(f, 1.0f - f));
			const float step = (treeHash01(treeHash(s.noiseSeed, (uint32)(int32)layer)) * 2.0f - 1.0f) * s.strataVar;
			d += s.strataDepth * (groove + step);
		}
		if (s.noiseOctaves > 0 && s.noiseAmplitude > 0.0f)
		{
			float fbm = 0.0f, ridge = 0.0f, amp = 0.5f, freq = s.noiseFrequency, norm = 0.0f;
			for (uint32 o = 0; o < s.noiseOctaves; ++o)
			{
				const float n = gradientNoise(p * s.noiseScale * freq + glm::vec3((float)o * 17.31f), s.noiseSeed + o);
				fbm += amp * n;
				const float r = 1.0f - glm::abs(n);
				ridge += amp * r * r;
				norm += amp;
				amp *= 0.5f;
				freq *= 2.0f;
			}
			fbm /= norm;
			ridge = ridge / norm * 2.0f - 1.0f;
			d -= s.noiseAmplitude * glm::mix(fbm, ridge, s.ridged);
		}
		for (uint32 i = 0; i < s.pitCount; ++i)
			d = smax(d, s.pits[i].w - glm::length(p - glm::vec3(s.pits[i])), s.pits[i].w * 0.3f);
		return d;
	}

	void addTangentFrame(RockMesh& m)
	{
		m.tangents.resize(m.positions.size());
		m.bitangents.resize(m.positions.size());
		for (size_t i = 0; i < m.positions.size(); ++i)
		{
			const glm::vec3 n = m.normals[i];
			const glm::vec3 ref = glm::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			const glm::vec3 t = glm::normalize(glm::cross(ref, n));
			m.tangents[i] = t;
			m.bitangents[i] = glm::cross(n, t);
		}
	}

	// Outward faces must wind counter-clockwise (right-handed): flips every triangle when the signed volume is negative.
	void orientOutward(RockMesh& m)
	{
		double volume = 0.0;
		for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
		{
			const glm::vec3& a = m.positions[m.indices[i]];
			const glm::vec3& b = m.positions[m.indices[i + 1]];
			const glm::vec3& c = m.positions[m.indices[i + 2]];
			volume += (double)glm::dot(a, glm::cross(b, c));
		}
		if (volume < 0.0)
			for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
				oc::swap(m.indices[i + 1], m.indices[i + 2]);
	}

	// Surface nets over `s`'s bounds: one vertex per cell the surface crosses (the mean of its edge crossings), one quad
	// per grid edge it crosses. Positions only (+ indices); the caller projects and shades.
	void surfaceNets(const RockShape& s, uint32 resolution, RockMesh& out, float& outCell)
	{
		const glm::vec3 extent = s.boundsMax - s.boundsMin;
		const float cell = glm::max(extent.x, glm::max(extent.y, extent.z)) / (float)resolution;
		const glm::ivec3 cells = glm::max(glm::ivec3(glm::ceil(extent / cell)), glm::ivec3(2));
		const glm::ivec3 corners = cells + 1;
		auto cornerIdx = [&](int x, int y, int z) { return ((size_t)z * (size_t)corners.y + (size_t)y) * (size_t)corners.x + (size_t)x; };
		auto cellIdx = [&](int x, int y, int z) { return ((size_t)z * (size_t)cells.y + (size_t)y) * (size_t)cells.x + (size_t)x; };
		auto cornerPos = [&](int x, int y, int z) { return s.boundsMin + glm::vec3((float)x, (float)y, (float)z) * cell; };

		oc::vector<float> field((size_t)corners.x * corners.y * corners.z);
		for (int z = 0; z < corners.z; ++z)
			for (int y = 0; y < corners.y; ++y)
				for (int x = 0; x < corners.x; ++x)
					field[cornerIdx(x, y, z)] = rockSdf(s, cornerPos(x, y, z));

		static constexpr int EDGES[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
		oc::vector<int32> vertexOf((size_t)cells.x * cells.y * cells.z, -1);
		for (int z = 0; z < cells.z; ++z)
			for (int y = 0; y < cells.y; ++y)
				for (int x = 0; x < cells.x; ++x)
				{
					float v[8];
					uint32 inside = 0;
					for (int c = 0; c < 8; ++c)
					{
						v[c] = field[cornerIdx(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1))];
						inside += v[c] < 0.0f ? 1u : 0u;
					}
					if (inside == 0 || inside == 8)
						continue;
					glm::vec3 sum(0.0f);
					uint32 count = 0;
					for (const auto& e : EDGES)
					{
						const float a = v[e[0]], b = v[e[1]];
						if ((a < 0.0f) == (b < 0.0f))
							continue;
						const float t = a / (a - b);
						const glm::vec3 pa((float)(e[0] & 1), (float)((e[0] >> 1) & 1), (float)((e[0] >> 2) & 1));
						const glm::vec3 pb((float)(e[1] & 1), (float)((e[1] >> 1) & 1), (float)((e[1] >> 2) & 1));
						sum += glm::mix(pa, pb, t);
						++count;
					}
					vertexOf[cellIdx(x, y, z)] = (int32)out.positions.size();
					out.positions.push_back(cornerPos(x, y, z) + sum / (float)count * cell);
				}

		// A quad per crossed grid edge, between the 4 cells around it. The winding is fixed afterwards (orientOutward).
		auto quad = [&](int32 a, int32 b, int32 c, int32 d, bool flip)
		{
			if (a < 0 || b < 0 || c < 0 || d < 0)
				return;
			if (flip)
				oc::swap(b, d);
			out.indices.insert(out.indices.end(), { (uint32)a, (uint32)b, (uint32)c, (uint32)a, (uint32)c, (uint32)d });
		};
		for (int z = 1; z < cells.z; ++z)
			for (int y = 1; y < cells.y; ++y)
				for (int x = 0; x < cells.x; ++x)
				{
					const bool in0 = field[cornerIdx(x, y, z)] < 0.0f;
					if (in0 != (field[cornerIdx(x + 1, y, z)] < 0.0f))
						quad(vertexOf[cellIdx(x, y - 1, z - 1)], vertexOf[cellIdx(x, y, z - 1)], vertexOf[cellIdx(x, y, z)], vertexOf[cellIdx(x, y - 1, z)], in0);
				}
		for (int z = 1; z < cells.z; ++z)
			for (int y = 0; y < cells.y; ++y)
				for (int x = 1; x < cells.x; ++x)
				{
					const bool in0 = field[cornerIdx(x, y, z)] < 0.0f;
					if (in0 != (field[cornerIdx(x, y + 1, z)] < 0.0f))
						quad(vertexOf[cellIdx(x - 1, y, z - 1)], vertexOf[cellIdx(x - 1, y, z)], vertexOf[cellIdx(x, y, z)], vertexOf[cellIdx(x, y, z - 1)], in0);
				}
		for (int z = 0; z < cells.z; ++z)
			for (int y = 1; y < cells.y; ++y)
				for (int x = 1; x < cells.x; ++x)
				{
					const bool in0 = field[cornerIdx(x, y, z)] < 0.0f;
					if (in0 != (field[cornerIdx(x, y, z + 1)] < 0.0f))
						quad(vertexOf[cellIdx(x - 1, y - 1, z)], vertexOf[cellIdx(x, y - 1, z)], vertexOf[cellIdx(x, y, z)], vertexOf[cellIdx(x - 1, y, z)], in0);
				}
		outCell = cell;
	}

	// `src` simplified to about `triangles` (positions, normals and the cavity; unused vertices dropped, the index
	// order optimized for the vertex cache). Returns the simplification's error in `src`'s own units (meshopt reports
	// it relative to the mesh extent).
	float simplifyRockMesh(const RockMesh& src, uint32 triangles, RockMesh& out)
	{
		oc::vector<uint32> simplified(src.indices.size());
		float error = 0.0f;
		const size_t count = meshopt_simplify(simplified.data(), src.indices.data(), src.indices.size(), &src.positions[0].x,
			src.positions.size(), sizeof(glm::vec3), (size_t)triangles * 3, 1.0f, 0, &error);
		simplified.resize(count);
		meshopt_optimizeVertexCache(simplified.data(), simplified.data(), simplified.size(), src.positions.size());
		oc::vector<uint32> remap(src.positions.size(), UINT32_MAX);
		for (uint32 idx : simplified)
		{
			if (remap[idx] == UINT32_MAX)
			{
				remap[idx] = (uint32)out.positions.size();
				out.positions.push_back(src.positions[idx]);
				out.normals.push_back(src.normals[idx]);
				out.texCoords.push_back(src.texCoords[idx]);
			}
			out.indices.push_back(remap[idx]);
		}
		return error * meshopt_simplifyScale(&src.positions[0].x, src.positions.size(), sizeof(glm::vec3));
	}

	// The CAVITY at a surface point (1 = open, 0 = deep in a crevice): an ambient occlusion from the field itself -
	// five taps out along the normal, each counting how much nearer the surface is than the tap's own distance
	// (Quilez' SDF occlusion). A flat face gives 1; a right-angle inner corner about 0.5.
	float rockCavity(const RockShape& s, const glm::vec3& p, const glm::vec3& n)
	{
		float occlusion = 0.0f, weight = 1.0f;
		for (int i = 1; i <= 5; ++i)
		{
			const float h = 0.012f + 0.03f * (float)i;
			occlusion += glm::max(h - rockSdf(s, p + n * h), 0.0f) * weight;
			weight *= 0.7f;
		}
		return glm::clamp(1.0f - 8.0f * occlusion, 0.0f, 1.0f);
	}

	// Moves every vertex onto the field's zero set (Newton steps along the gradient), then shades it from the gradient
	// and takes its cavity (texCoords.x). In the SHAPE frame.
	void projectAndShade(const RockShape& s, RockMesh& m, float cell, uint32 steps)
	{
		m.normals.resize(m.positions.size());
		m.texCoords.resize(m.positions.size());
		for (size_t i = 0; i < m.positions.size(); ++i)
		{
			glm::vec3 p = m.positions[i];
			for (uint32 k = 0; k < steps; ++k)
			{
				const float d = rockSdf(s, p);
				const glm::vec3 g = rockSdfNormal(s, p, cell * 0.25f);
				p -= g * glm::clamp(d, -cell, cell);
			}
			m.positions[i] = p;
			m.normals[i] = rockSdfNormal(s, p, cell * 0.5f);
			m.texCoords[i] = glm::vec3(rockCavity(s, p, m.normals[i]), 0.0f, 0.0f);
		}
	}
}

namespace Procedural
{
	float rockSdf(const RockShape& shape, const glm::vec3& p)
	{
		return fullSdf(shape, p);
	}

	glm::vec3 rockSdfNormal(const RockShape& shape, const glm::vec3& p, float eps)
	{
		// Tetrahedron differences: 4 evaluations.
		const glm::vec3 k0(1.0f, -1.0f, -1.0f), k1(-1.0f, -1.0f, 1.0f), k2(-1.0f, 1.0f, -1.0f), k3(1.0f, 1.0f, 1.0f);
		const glm::vec3 g = k0 * rockSdf(shape, p + k0 * eps) + k1 * rockSdf(shape, p + k1 * eps)
			+ k2 * rockSdf(shape, p + k2 * eps) + k3 * rockSdf(shape, p + k3 * eps);
		const float len = glm::length(g);
		return len > 1e-12f ? g / len : glm::vec3(0.0f, 1.0f, 0.0f);
	}

	void buildRockShape(const RockTypeDesc& type, uint32 seed, RockShape& s)
	{
		s = RockShape{};
		const uint32 h = treeHash(type.seed, seed);
		auto rnd = [&](uint32 salt) { return treeHash01(treeHash(h, salt)); };
		auto rnds = [&](uint32 salt) { return rnd(salt) * 2.0f - 1.0f; };
		auto randomDir = [&](uint32 salt)
		{
			const float z = rnds(salt);
			const float a = rnd(salt + 1u) * 6.28318531f;
			const float r = std::sqrt(glm::max(1.0f - z * z, 0.0f));
			return glm::vec3(r * std::cos(a), z, r * std::sin(a));
		};

		s.kind = type.shape;
		s.round = type.round;
		s.squareness = type.squareness;
		s.erosion = type.erosion;
		s.warpAmplitude = type.warpAmplitude;
		s.warpFrequency = type.warpFrequency;
		s.noiseSeed = treeHash(h, 400u); // before the pits: their bisection reads the warped body

		// Blocks: the first one carries the variant's aspect, upright at the origin. A PILE adds smaller blocks, each with
		// its own aspect jitter, yaw and tilt, from a random direction (sideways to upward) pushed in until it LEANS into
		// the blocks before it - sunk ~35 % of its own height into them. A heap, not a column.
		glm::vec3 aspect = type.aspect * (1.0f + type.aspectVar * glm::vec3(rnds(1), rnds(2), rnds(3)));
		aspect = glm::max(aspect, glm::vec3(0.05f));
		const bool pillar = type.shape == ERockShape::Pillar;
		glm::vec3 half = aspect * 0.5f;
		s.blockHalf[0] = half;
		s.blockCentre[0] = glm::vec3(0.0f);
		s.blockRot[0] = glm::mat3(1.0f);
		// PILLARS: 1..Group of them (the variant's draw), each with its own profile (the type's, jittered, its widest
		// point 1) and a lean of up to ~9 degrees; a later one is smaller and stands beside an earlier one, their bases
		// overlapping, on the same floor.
		s.blockCount = pillar ? 1u + glm::min((uint32)(rnd(5) * (float)type.group), (uint32)type.group - 1u) : (uint32)type.pile;
		for (uint32 b = 0; pillar && b < s.blockCount; ++b)
		{
			if (b > 0)
			{
				half *= type.groupShrink * (0.8f + 0.4f * rnd(20 + b));
				s.blockHalf[b] = half * (1.0f + 0.2f * glm::vec3(rnds(10 + b * 4), rnds(11 + b * 4), rnds(12 + b * 4)));
			}
			float widest = 0.0f;
			for (int i = 0; i < ROCK_PROFILE_POINTS; ++i)
			{
				s.blockProfile[b][i] = type.profile[i] * (1.0f + type.profileVar * rnds(600 + b * 8 + (uint32)i));
				widest = glm::max(widest, s.blockProfile[b][i]);
			}
			for (float& width : s.blockProfile[b])
				width /= widest;
			const glm::quat yaw = glm::angleAxis(rnd(40 + b) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
			const float leanAxis = rnd(60 + b) * 6.28318531f;
			const glm::quat lean = glm::angleAxis(rnd(50 + b) * 0.16f, glm::vec3(std::cos(leanAxis), 0.0f, std::sin(leanAxis)));
			s.blockRot[b] = glm::transpose(glm::mat3_cast(lean * yaw));
			if (b > 0)
			{
				const uint32 parent = glm::min((uint32)(rnd(70 + b) * (float)b), b - 1u);
				const float azimuth = rnd(80 + b) * 6.28318531f;
				const float reach = (glm::min(s.blockHalf[parent].x, s.blockHalf[parent].z) + glm::min(s.blockHalf[b].x, s.blockHalf[b].z))
					* (0.55f + 0.4f * rnd(90 + b));
				s.blockCentre[b] = s.blockCentre[parent] + glm::vec3(std::cos(azimuth), 0.0f, std::sin(azimuth)) * reach;
				s.blockCentre[b].y = s.blockHalf[b].y - s.blockHalf[0].y; // its base on block 0's
			}
		}
		for (uint32 b = 1; !pillar && b < s.blockCount; ++b)
		{
			half *= type.pileShrink * (0.85f + 0.3f * rnd(20 + b));
			const glm::vec3 jitter = 1.0f + 0.25f * glm::vec3(rnds(10 + b * 4), rnds(11 + b * 4), rnds(12 + b * 4));
			s.blockHalf[b] = half * jitter;
			const glm::quat yaw = glm::angleAxis(rnd(40 + b) * 6.28318531f, glm::vec3(0.0f, 1.0f, 0.0f));
			const float tiltAngle = rnd(50 + b) * 0.6f;
			const float tiltAxis = rnd(60 + b) * 6.28318531f;
			const glm::quat tilt = glm::angleAxis(tiltAngle, glm::vec3(std::cos(tiltAxis), 0.0f, std::sin(tiltAxis)));
			s.blockRot[b] = glm::transpose(glm::mat3_cast(tilt * yaw)); // block -> shape is tilt * yaw; the field needs the inverse
			// The direction: any azimuth, 5..60 degrees up from horizontal.
			const float azimuth = rnd(70 + b) * 6.28318531f;
			const float elevation = glm::radians(5.0f + 55.0f * rnd(80 + b));
			const glm::vec3 dir(std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth));
			// Bisection along it for the centre where the earlier blocks' surface lies `lean` below it.
			const float lean = s.blockHalf[b].y * 0.65f;
			float t0 = 0.0f, t1 = 4.0f;
			for (int k = 0; k < 24; ++k)
			{
				const float t = (t0 + t1) * 0.5f;
				(blocksSdf(s, dir * t, b, 0.0f) < lean ? t0 : t1) = t;
			}
			s.blockCentre[b] = dir * t1;
		}

		// The union's box (a rotated block: the box of its rotated box), then normalize so the longest axis is 1.
		glm::vec3 lo(1e9f), hi(-1e9f);
		for (uint32 b = 0; b < s.blockCount; ++b)
		{
			const glm::mat3 toShape = glm::transpose(s.blockRot[b]);
			glm::vec3 e(0.0f);
			for (int axis = 0; axis < 3; ++axis)
				e += glm::abs(toShape[axis]) * s.blockHalf[b][axis];
			lo = glm::min(lo, s.blockCentre[b] - e);
			hi = glm::max(hi, s.blockCentre[b] + e);
		}
		const glm::vec3 size = hi - lo;
		const float norm = 1.0f / glm::max(size.x, glm::max(size.y, size.z));
		const glm::vec3 centre = (lo + hi) * 0.5f;
		for (uint32 b = 0; b < s.blockCount; ++b)
		{
			s.blockCentre[b] = (s.blockCentre[b] - centre) * norm;
			s.blockHalf[b] *= norm;
		}
		const glm::vec3 boxHalf = size * 0.5f * norm;
		s.floorY = s.blockCentre[0].y - s.blockHalf[0].y;

		// Fracture: planes cut into the body from random directions, each to a random fraction of its support distance.
		s.planeCount = (uint32)type.fractureCount;
		for (uint32 i = 0; i < s.planeCount; ++i)
		{
			glm::vec3 n = randomDir(100 + i * 3);
			if (pillar)
				n.y = glm::abs(n.y); // never from below: a pillar keeps its foot on the floor
			s.planes[i] = glm::vec4(n, glm::length(boxHalf * n) * (1.0f - type.fractureDepth * (0.3f + 0.7f * rnd(102 + i * 3))));
		}

		// The split: one crack, mostly vertical, near the centre.
		if (rnd(200) < type.splitChance)
		{
			const float a = rnd(201) * 6.28318531f;
			const glm::vec3 n = glm::normalize(glm::vec3(std::cos(a), rnds(202) * 0.3f, std::sin(a)));
			s.split = true;
			s.splitPlane = glm::vec4(n, rnds(203) * 0.15f * glm::min(boxHalf.x, glm::min(boxHalf.y, boxHalf.z)));
			s.splitGap = type.splitGap;
		}

		s.strataSpacing = type.strataSpacing;
		s.strataDepth = type.strataDepth;
		s.strataVar = type.strataVar;
		s.strataPhase = rnd(300);
		s.noiseAmplitude = type.noiseAmplitude;
		s.noiseFrequency = type.noiseFrequency;
		s.noiseOctaves = (uint32)type.noiseOctaves;
		s.noiseScale = glm::vec3(1.0f, 1.0f / type.noiseStretch, 1.0f);
		s.ridged = type.ridged;

		// Pits: a sphere sunk into the body's surface from a random, mostly upward direction (the bisection finds the
		// surface along it on the body alone).
		const float reachMax = glm::length(boxHalf) * 1.5f;
		for (uint32 i = 0; i < (uint32)type.pitCount && s.pitCount < ROCK_MAX_PITS; ++i)
		{
			glm::vec3 dir = randomDir(500 + i * 3);
			dir.y = glm::abs(dir.y) + 0.3f;
			dir = glm::normalize(dir);
			float t0 = 0.0f, t1 = reachMax;
			if (bodySdf(s, dir * t1) < 0.0f)
				continue;
			for (int k = 0; k < 24; ++k)
			{
				const float t = (t0 + t1) * 0.5f;
				(bodySdf(s, dir * t) < 0.0f ? t0 : t1) = t;
			}
			const float r = type.pitSize * (0.6f + 0.8f * rnd(502 + i * 3));
			s.pits[s.pitCount++] = glm::vec4(dir * (t0 + r - type.pitDepth), r);
		}

		const float pad = type.noiseAmplitude * 1.5f + type.strataDepth * (1.0f + type.strataVar) + type.warpAmplitude * 1.8f + 0.03f;
		s.boundsMin = -boxHalf - pad;
		s.boundsMax = boxHalf + pad;
	}

	void generateRockVariant(const RockTypeDesc& type, uint32 seed, uint32 gridResolution, RockVariant& out)
	{
		out = RockVariant{};
		buildRockShape(type, seed, out.shape);

		// The FULL surface-nets mesh (tens of thousands of vertices at 96 cells) is CPU working data only: every LOD
		// level is simplified from it.
		RockMesh full;
		float cell = 0.0f;
		surfaceNets(out.shape, glm::max(gridResolution, 16u), full, cell);
		if (full.indices.empty())
			return;
		projectAndShade(out.shape, full, cell, 2);

		// Rock-local origin: the lowest point at y = 0. Shifting the BLOCKS, planes, pits and bounds would also move the
		// strata and the noise (world-fixed in the shape's frame), so the shape keeps its frame and every mesh shifts.
		float minY = 1e9f, maxY = -1e9f;
		for (const glm::vec3& p : full.positions)
		{
			minY = glm::min(minY, p.y);
			maxY = glm::max(maxY, p.y);
		}
		for (glm::vec3& p : full.positions)
			p.y -= minY;
		out.shape.originY = minY;
		out.height = maxY - minY;
		orientOutward(full);

		// THE FAR VOLUME's occupancy: the field at each voxel centre, soft over one voxel (a surface through the
		// voxel's middle = 0.5), over the full mesh's rock-local box.
		glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
		for (const glm::vec3& p : full.positions)
		{
			lo = glm::min(lo, p);
			hi = glm::max(hi, p);
		}
		const glm::vec3 voxel = glm::max(hi - lo, glm::vec3(1e-3f)) / (float)ROCK_DENSITY_RES;
		const float voxelSize = glm::max(voxel.x, glm::max(voxel.y, voxel.z));
		out.densityMin = lo;
		out.densityMax = hi;
		out.density.resize((size_t)ROCK_DENSITY_RES * ROCK_DENSITY_RES * ROCK_DENSITY_RES);
		for (uint32 z = 0; z < ROCK_DENSITY_RES; ++z)
			for (uint32 y = 0; y < ROCK_DENSITY_RES; ++y)
				for (uint32 x = 0; x < ROCK_DENSITY_RES; ++x)
				{
					const glm::vec3 local = lo + (glm::vec3((float)x, (float)y, (float)z) + 0.5f) * voxel;
					const float d = rockSdf(out.shape, local + glm::vec3(0.0f, minY, 0.0f)); // rock-local -> the shape frame
					out.density[x + ROCK_DENSITY_RES * (y + ROCK_DENSITY_RES * z)] = glm::clamp(0.5f - d / voxelSize, 0.0f, 1.0f);
				}

		// THE LOD CHAIN: level 0 at the type's `Lod` triangles, each further level a quarter of the one before, every
		// level simplified from the FULL mesh (its error is then against the true surface, not accumulated). A level
		// that no longer gets clearly smaller ends the chain.
		uint32 target = glm::min((uint32)type.lodTriangles, (uint32)(full.indices.size() / 3));
		size_t previousIndices = SIZE_MAX;
		for (uint32 level = 0; level < ROCK_MAX_LODS && target >= ROCK_LOD_MIN_TRIANGLES; ++level, target /= 4)
		{
			RockMesh mesh;
			const float error = simplifyRockMesh(full, target, mesh);
			if (mesh.indices.size() < 12 || (level > 0 && mesh.indices.size() * 10 >= previousIndices * 9))
				break;
			previousIndices = mesh.indices.size();
			addTangentFrame(mesh);
			out.lodError[out.lodCount] = level == 0 ? 0.0f : error;
			out.lods[out.lodCount++] = oc::move(mesh);
		}
	}
}
