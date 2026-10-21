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

	constexpr float PI = 3.14159265f;

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

	// A profile's width at t (0 = the base, 1 = the top): pillarSdf's spline, without the slope.
	float profileAt(const float* profile, float t)
	{
		constexpr int LAST = ROCK_PROFILE_POINTS - 1;
		const float x = glm::clamp(t, 0.0f, 1.0f) * (float)LAST;
		const int i = glm::min((int)x, LAST - 1);
		const float f = x - (float)i;
		const float p0 = profile[glm::max(i - 1, 0)], p1 = profile[i], p2 = profile[i + 1], p3 = profile[glm::min(i + 2, LAST)];
		return 0.5f * (2.0f * p1 + f * ((p2 - p0) + f * ((2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) + f * (3.0f * (p1 - p2) + p3 - p0))));
	}

	// A cone with rounded ends from a (radius ra) to b (radius rb) - a branch stub, a root (iq's sdRoundCone).
	float roundConeSdf(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, float ra, float rb)
	{
		const glm::vec3 ba = b - a;
		const float l2 = glm::dot(ba, ba);
		const float rr = ra - rb;
		const float a2 = l2 - rr * rr;
		const float il2 = 1.0f / l2;
		const glm::vec3 pa = p - a;
		const float y = glm::dot(pa, ba);
		const float z = y - l2;
		const glm::vec3 xv = pa * l2 - ba * y;
		const float x2 = glm::dot(xv, xv);
		const float y2 = y * y * l2;
		const float z2 = z * z * l2;
		const float k = glm::sign(rr) * rr * rr * x2;
		if (glm::sign(z) * a2 * z2 > k)
			return std::sqrt(x2 + z2) * il2 - rb;
		if (glm::sign(y) * a2 * y2 < k)
			return std::sqrt(x2 + y2) * il2 - ra;
		return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - ra;
	}

	// A limb's frame: its axis (unit, a -> b) and two unit vectors across it - u its `side` made perpendicular to the axis,
	// or (no side) picked from the axis. The angle around a limb - the wood coordinates' (woodCoords), a cactus's ribs
	// (ribIndent) - is measured in it.
	struct LimbFrame
	{
		glm::vec3 dir, u, v;
		float length;
	};
	LimbFrame limbFrame(const RockShape::Limb& l)
	{
		LimbFrame f;
		const glm::vec3 axis = l.b - l.a;
		f.length = glm::length(axis);
		f.dir = axis / glm::max(f.length, 1e-6f);
		const glm::vec3 side = l.side - f.dir * glm::dot(l.side, f.dir);
		f.u = glm::dot(side, side) > 1e-8f ? glm::normalize(side)
			: glm::normalize(glm::cross(f.dir, glm::abs(f.dir.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)));
		f.v = glm::cross(f.dir, f.u);
		return f;
	}

	// A cactus segment's RIBS: the surface pushed in by up to ribDepth x the segment's radius between two crests - broad
	// crests, narrow valleys (((1 - cos) / 2)^2 of the angle). The crests lie where cos(ribs x (angle + pi)) = 1, the angle
	// in the segment's LimbFrame: the shaders find them from woodCoords' coordinates (cactus.inc.glsl). Faded out toward
	// the axis, so a dome's apex closes.
	float ribIndent(const RockShape& s, const RockShape::Limb& l, const glm::vec3& p)
	{
		const LimbFrame f = limbFrame(l);
		const float along = glm::dot(p - l.a, f.dir);
		const glm::vec3 off = p - l.a - f.dir * along;
		const float r = glm::mix(l.ra, l.rb, glm::clamp(along / glm::max(f.length, 1e-6f), 0.0f, 1.0f));
		const float valley = 0.5f - 0.5f * std::cos((float)s.ribCount * (std::atan2(glm::dot(off, f.v), glm::dot(off, f.u)) + PI));
		return s.ribDepth * r * valley * valley * glm::smoothstep(0.15f * r, 0.6f * r, glm::length(off));
	}

	// THE CACTUS: its segments (the limbs: the stem, each arm's elbow and its upright part) - round cones, ribbed around
	// their own axes - blended into one body with fillets half a segment's radius wide, cut flat at the ground line. A
	// segment's ribs only push its surface in, so a segment already past the fillet's reach skips them.
	float cactusSdf(const RockShape& s, const glm::vec3& p, float e)
	{
		float d = 1e9f;
		for (uint32 i = 0; i < s.limbCount; ++i)
		{
			const RockShape::Limb& l = s.limbs[i];
			float segment = roundConeSdf(p, l.a, l.b, glm::max(l.ra - e, 0.002f), glm::max(l.rb - e, 0.002f));
			if (s.ribCount > 0 && segment < d + 0.5f * l.ra)
				segment += ribIndent(s, l, p);
			d = smin(d, segment, 0.5f * l.ra);
		}
		return glm::max(d, s.groundY + e - p.y);
	}

	// A BROKEN END over the trunk's section (u: the section point / the end's radius): 1 where the wood reaches the end
	// (a splinter's tip), 0 at the deepest point of the break - long ridged splinters, the break leaning across (`slant`).
	float splinter(glm::vec2 u, uint32 seed, glm::vec2 slant)
	{
		const float ridge = glm::clamp(1.0f - glm::abs(gradientNoise(glm::vec3(u * 2.3f, 0.37f), seed) * 1.6f), 0.0f, 1.0f);
		const float fine = gradientNoise(glm::vec3(u * 7.0f, 1.71f), seed ^ 0x5A17u);
		return glm::clamp(0.2f + glm::dot(u, slant) + 0.6f * ridge * ridge * ridge + 0.15f * fine, 0.0f, 1.0f);
	}

	// THE TRUNK (block 0, its axis the block's Y): the profile's side, the two ends and the hollow core. An end is BROKEN
	// - the surface `breakDepth` x the end's radius deep where splinter() is 0, at the end where it is 1 - or, with no
	// depth, worn round by the erosion; a standing trunk's base is cut flat (it goes into the ground). The ends' terms
	// are scaled down by their steepness: the field stays near a distance for the projection.
	float trunkSdf(const RockShape& s, const glm::vec3& p, float e)
	{
		const glm::vec3 q = s.blockRot[0] * (p - s.blockCentre[0]);
		const glm::vec3& half = s.blockHalf[0];
		const float radius = glm::min(half.x, half.z);
		const glm::vec2 section(q.x, q.z);
		const float k = 0.004f + 0.5f * e;
		float d = pillarSdf(q, half, s.blockProfile[0], s.squareness, e);
		const float rTop = glm::max(radius * s.blockProfile[0][ROCK_PROFILE_POINTS - 1], 1e-4f);
		const float top = half.y - e - s.breakDepth.x * rTop * (1.0f - splinter(section / rTop, s.noiseSeed ^ 0x7A11u, s.breakSlant[0]));
		d = smax(d, (q.y - top) / (1.0f + 3.0f * s.breakDepth.x), k);
		if (s.standing)
			d = glm::max(d, -half.y + e - q.y);
		else
		{
			const float rBase = glm::max(radius * s.blockProfile[0][0], 1e-4f);
			const float base = -half.y + e + s.breakDepth.y * rBase * (1.0f - splinter(section / rBase, s.noiseSeed ^ 0x3B5Du, s.breakSlant[1]));
			d = smax(d, (base - q.y) / (1.0f + 3.0f * s.breakDepth.y), k);
		}
		// The hollow core runs the whole axis (open at the broken ends; a standing trunk's base is in the ground). The
		// cavity grows by the erosion (the Minkowski shrink).
		if (s.hollow > 0.0f)
		{
			const float core = s.hollow * radius * profileAt(s.blockProfile[0], (q.y + half.y) / (2.0f * half.y));
			d = smax(d, core + e - glm::length(section), k);
		}
		return d;
	}

	// The whole dead tree: the trunk, its root plate (a lumpy disc across the base) and its limbs (stubs, roots), each
	// blended in with a fillet as wide as its own size.
	float trunkBodySdf(const RockShape& s, const glm::vec3& p, float e)
	{
		float d = trunkSdf(s, p, e);
		if (s.plate.w > 0.0f)
		{
			const glm::vec3 h = glm::max(glm::vec3(0.5f * s.plateThickness, s.plate.w, s.plate.w) - e, glm::vec3(0.004f));
			d = smin(d, superellipsoidSdf(p - glm::vec3(s.plate), h, 2.6f), 0.15f * s.plate.w);
		}
		for (uint32 i = 0; i < s.limbCount; ++i)
		{
			const RockShape::Limb& l = s.limbs[i];
			d = smin(d, roundConeSdf(p, l.a, l.b, glm::max(l.ra - e, 0.002f), glm::max(l.rb - e, 0.002f)), 0.6f * l.ra);
		}
		return d;
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
		float d = s.kind == ERockShape::Trunk ? trunkBodySdf(s, p, e) : s.kind == ERockShape::Cactus ? cactusSdf(s, p, e)
			: blocksSdf(s, p, s.blockCount, e);
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

	// A wood mesh keeps its tangents (its wood coordinates); the bitangent only sets the stored sign, unread.
	void addTangentFrame(RockMesh& m)
	{
		const bool wood = !m.tangents.empty();
		m.tangents.resize(m.positions.size());
		m.bitangents.resize(m.positions.size());
		for (size_t i = 0; i < m.positions.size(); ++i)
		{
			const glm::vec3 n = m.normals[i];
			const glm::vec3 ref = glm::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			const glm::vec3 t = glm::normalize(glm::cross(ref, n));
			if (!wood)
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
	// per grid edge it crosses. Positions only (+ indices); the caller projects and shades. False when the body reaches
	// the grid's outer border: the mesh is then OPEN there (no cell outside to close it) - the caller grows the bounds.
	bool surfaceNets(const RockShape& s, uint32 resolution, RockMesh& out, float& outCell)
	{
		const glm::vec3 extent = s.boundsMax - s.boundsMin;
		const float cell = glm::max(extent.x, glm::max(extent.y, extent.z)) / (float)resolution;
		const glm::ivec3 cells = glm::max(glm::ivec3(glm::ceil(extent / cell)), glm::ivec3(2));
		const glm::ivec3 corners = cells + 1;
		auto cornerIdx = [&](int x, int y, int z) { return ((size_t)z * (size_t)corners.y + (size_t)y) * (size_t)corners.x + (size_t)x; };
		auto cellIdx = [&](int x, int y, int z) { return ((size_t)z * (size_t)cells.y + (size_t)y) * (size_t)cells.x + (size_t)x; };
		auto cornerPos = [&](int x, int y, int z) { return s.boundsMin + glm::vec3((float)x, (float)y, (float)z) * cell; };

		oc::vector<float> field((size_t)corners.x * corners.y * corners.z);
		bool closed = true;
		for (int z = 0; z < corners.z; ++z)
			for (int y = 0; y < corners.y; ++y)
				for (int x = 0; x < corners.x; ++x)
				{
					const float d = rockSdf(s, cornerPos(x, y, z));
					field[cornerIdx(x, y, z)] = d;
					const bool border = x == 0 || y == 0 || z == 0 || x == corners.x - 1 || y == corners.y - 1 || z == corners.z - 1;
					closed &= !(border && d < 0.0f);
				}

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
		return closed;
	}

	// `src` simplified to about `triangles` (positions, normals, the cavity / bark and a wood mesh's coordinates; unused
	// vertices dropped, the index order optimized for the vertex cache). Returns the simplification's error in `src`'s
	// own units (meshopt reports it relative to the mesh extent).
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
				if (!src.tangents.empty())
					out.tangents.push_back(src.tangents[idx]);
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

	// A WOOD vertex (RockMesh): the part it lies on - the trunk, the nearest limb, or the root plate (the trunk's frame) -
	// by the smallest distance; its coordinates along and across that part, and its BARK cover: the outer skin is bark,
	// anything inside the part's radius (a break, the hollow, a splinter's inner face) bare wood. Along is in units of the
	// part's base CIRCUMFERENCE - the bark texture's v, as on a tree branch (the shader's u is the angle around). A
	// CACTUS: its nearest segment, the same way (no cut faces). Anything else: the shape frame, all bark.
	void woodCoords(const RockShape& s, const glm::vec3& pIn, glm::vec3& coords, float& bark)
	{
		constexpr float TWO_PI = 6.28318531f;
		const glm::vec3 p = warpPoint(s, pIn);
		bark = 1.0f;
		if (s.kind != ERockShape::Trunk && s.kind != ERockShape::Cactus)
		{
			coords = glm::vec3(p.y / (TWO_PI * 0.25f), p.x, p.z);
			return;
		}
		float best = 1e9f;
		float outer = 0.0f;
		if (s.kind == ERockShape::Trunk)
		{
			const glm::vec3 q = s.blockRot[0] * (p - s.blockCentre[0]);
			const glm::vec3& half = s.blockHalf[0];
			best = pillarSdf(q, half, s.blockProfile[0], s.squareness, 0.0f);
			const float baseRadius = glm::max(glm::min(half.x, half.z) * s.blockProfile[0][0], 1e-3f);
			coords = glm::vec3((q.y + half.y) / (TWO_PI * baseRadius), q.x, q.z);
			outer = glm::min(half.x, half.z) * profileAt(s.blockProfile[0], (q.y + half.y) / (2.0f * half.y));
			if (s.plate.w > 0.0f)
			{
				const float plate = superellipsoidSdf(p - glm::vec3(s.plate), glm::vec3(0.5f * s.plateThickness, s.plate.w, s.plate.w), 2.6f);
				if (plate < best)
				{
					best = plate;
					outer = 0.0f; // the torn root mass: no cut face
				}
			}
		}
		for (uint32 i = 0; i < s.limbCount; ++i)
		{
			const RockShape::Limb& l = s.limbs[i];
			const float d = roundConeSdf(p, l.a, l.b, l.ra, l.rb);
			if (d >= best)
				continue;
			best = d;
			const LimbFrame f = limbFrame(l);
			const float along = glm::dot(p - l.a, f.dir);
			const glm::vec3 off = p - l.a - f.dir * along;
			coords = glm::vec3(along / (TWO_PI * glm::max(l.ra, 1e-3f)) + l.along0, glm::dot(off, f.u), glm::dot(off, f.v));
			outer = s.kind == ERockShape::Cactus ? 0.0f : glm::mix(l.ra, l.rb, glm::clamp(along / glm::max(f.length, 1e-6f), 0.0f, 1.0f));
		}
		// Below ~70 % of the radius: a cut. The surface noise moves the skin in and out by a few % - still bark.
		if (outer > 0.0f)
			bark = glm::smoothstep(0.68f, 0.86f, glm::length(glm::vec2(coords.y, coords.z)) / outer);
	}

	// Moves every vertex onto the field's zero set (Newton steps along the gradient), then shades it from the gradient
	// and takes its cavity (texCoords.x) - and for WOOD its bark cover (texCoords.y) and wood coordinates (the tangent); a
	// CACTUS skin the same coordinates and texCoords.y = 3 + its rib count / 100. In the SHAPE frame.
	void projectAndShade(const RockShape& s, RockMesh& m, float cell, uint32 steps)
	{
		m.normals.resize(m.positions.size());
		m.texCoords.resize(m.positions.size());
		if (s.wood)
			m.tangents.resize(m.positions.size());
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
			if (s.wood)
			{
				float bark = 1.0f;
				woodCoords(s, p, m.tangents[i], bark);
				m.texCoords[i].y = s.cactusSkin ? 3.0f + 0.01f * (float)s.ribCount : 1.0f + bark;
			}
		}
	}

	// THE TRUNK's random decisions (buildRockShape; before the normalization, in the aspect's units): block 0 turned so
	// its axis runs along the shape's X when it lies (shape Y -> block -X: the block's x half is the vertical
	// thickness), a slight lean when it stands; its profile; the breaks and the hollow; the limbs - roots out of a
	// standing trunk's base, branch stubs along the upper trunk (a lying one's never into the ground) - and a lying
	// trunk's root plate.
	void buildTrunk(const RockTypeDesc& type, RockShape& s, auto&& rnd)
	{
		const auto rnds = [&](uint32 salt) { return rnd(salt) * 2.0f - 1.0f; };
		const glm::vec3 aspectHalf = s.blockHalf[0];
		s.standing = !type.lying;
		if (type.lying)
		{
			s.blockRot[0] = glm::mat3(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			s.blockHalf[0] = glm::vec3(aspectHalf.y, aspectHalf.x, aspectHalf.z);
		}
		else
		{
			const float leanAxis = rnd(60) * 6.28318531f; // up to ~5 degrees
			const glm::quat lean = glm::angleAxis(rnd(50) * 0.09f, glm::vec3(std::cos(leanAxis), 0.0f, std::sin(leanAxis)));
			s.blockRot[0] = glm::transpose(glm::mat3_cast(lean));
		}
		float widest = 0.0f;
		for (int i = 0; i < ROCK_PROFILE_POINTS; ++i)
		{
			s.blockProfile[0][i] = type.profile[i] * (1.0f + type.profileVar * rnds(600 + (uint32)i));
			widest = glm::max(widest, s.blockProfile[0][i]);
		}
		for (float& width : s.blockProfile[0])
			width /= widest;
		const glm::vec3 half = s.blockHalf[0];
		const float radius = glm::min(half.x, half.z);
		const float rBase = radius * s.blockProfile[0][0];
		const bool plate = type.lying && rnd(700) < type.rootPlateChance;
		s.breakDepth = glm::vec2(type.breakDepth.x * (0.6f + 0.6f * rnd(701)),
			plate || s.standing ? 0.0f : type.breakDepth.y * (0.6f + 0.6f * rnd(702)));
		s.breakSlant[0] = 0.35f * glm::vec2(rnds(703), rnds(704));
		s.breakSlant[1] = 0.35f * glm::vec2(rnds(705), rnds(706));
		s.hollow = rnd(707) < type.hollowChance ? type.hollowSize * (0.8f + 0.3f * rnd(708)) : 0.0f;

		const glm::mat3 toShape = glm::transpose(s.blockRot[0]);
		const auto addLimb = [&](glm::vec3 a, glm::vec3 b, float ra, float rb) {
			if (s.limbCount >= ROCK_MAX_LIMBS)
				return;
			RockShape::Limb& l = s.limbs[s.limbCount];
			l = { toShape * a + s.blockCentre[0], toShape * b + s.blockCentre[0], ra, rb };
			l.along0 = 0.37f * (float)++s.limbCount; // each limb its own stretch of bark
		};
		// Roots: out of a standing trunk's base and down into the ground, thick at the trunk.
		const int roots = s.standing ? type.rootCount : 0;
		for (int i = 0; i < roots; ++i)
		{
			const float phi = ((float)i + 0.6f * rnd(720 + (uint32)i)) * 6.28318531f / (float)roots;
			const glm::vec3 out(std::cos(phi), 0.0f, std::sin(phi));
			const float reach = type.rootSpread * rBase * (0.75f + 0.5f * rnd(740 + (uint32)i));
			addLimb(out * (0.55f * rBase) + glm::vec3(0.0f, -half.y + 0.6f * rBase, 0.0f), out * reach + glm::vec3(0.0f, -half.y - 0.35f * rBase, 0.0f),
				rBase * (0.35f + 0.15f * rnd(760 + (uint32)i)), rBase * 0.1f);
		}
		// Branch stubs, leaning toward the top; a lying trunk's within 126 degrees of up (block -X is up).
		for (int i = 0; i < type.stubCount; ++i)
		{
			const uint32 k = 800u + 8u * (uint32)i;
			const float t = s.standing ? 0.35f + 0.6f * rnd(k) : 0.2f + 0.72f * rnd(k);
			const float phi = s.standing ? rnd(k + 1) * 6.28318531f : 3.14159265f + 2.2f * rnds(k + 1);
			const glm::vec3 radial(std::cos(phi), 0.0f, std::sin(phi));
			const float r = radius * profileAt(s.blockProfile[0], t);
			const glm::vec3 a = radial * (0.85f * r) + glm::vec3(0.0f, -half.y + 2.0f * half.y * t, 0.0f);
			const glm::vec3 dir = glm::normalize(radial + glm::vec3(0.0f, 0.3f + 0.5f * rnd(k + 2), 0.0f));
			const float ra = r * (0.25f + 0.2f * rnd(k + 3));
			addLimb(a, a + dir * (r + type.stubLength * (0.5f + 0.5f * rnd(k + 4))), ra, ra * (0.5f + 0.2f * rnd(k + 5)));
		}
		// The ROOT PLATE: a lumpy disc across a lying trunk's base, the trunk entering its upper part (the lower part was
		// the root ball: it goes into the ground below the ground line).
		if (plate)
		{
			const float plateR = type.rootPlateSize * rBase * (0.85f + 0.3f * rnd(709));
			s.plateThickness = 1.1f * rBase;
			s.plate = glm::vec4(toShape * glm::vec3(0.0f, -half.y + 0.3f * s.plateThickness, 0.0f) + s.blockCentre[0]
				+ glm::vec3(0.0f, -0.25f * plateR, 0.0f), plateR);
		}
	}

	// THE CACTUS's random decisions (buildRockShape; before the normalization, in the aspect's units, the ground line at
	// y = 0). Block 0 is only the stem's box (the bounds, the erosion cap, the ground line). Every part is a CHAIN of limbs
	// (cactusSdf) sharing one `side` (its ribs line up across the joints) and continuing its along coordinate:
	//   THE STEM - two segments from below the ground to its domed top, bent a little at mid height, narrower toward the
	//              top (limbs 0 and 1);
	//   THE ARMS - armCount.x..y at even angles around the stem, jittered, each leaving it at armHeight: a NECK out of the
	//              stem's axis, rising 15..40 degrees and thinner where it joins, an ARC of 3 chords turning it upright
	//              (radius 1.2..2 x its own), then the UPRIGHT part, leaning a little, to its domed top below the stem's.
	//              ArmSize's reach = how far the upright part stands out from the stem's side.
	void buildCactus(const RockTypeDesc& type, RockShape& s, auto&& rnd)
	{
		constexpr float TWO_PI = 2.0f * PI;
		const auto rnds = [&](uint32 salt) { return rnd(salt) * 2.0f - 1.0f; };
		const glm::vec3 Y(0.0f, 1.0f, 0.0f);
		const float R = s.blockHalf[0].x;
		const float H = 2.0f * s.blockHalf[0].y;
		s.blockHalf[0].z = R;
		s.blockCentre[0] = glm::vec3(0.0f, 0.5f * H, 0.0f);
		s.standing = true;
		s.ribCount = (uint32)type.ribCount;
		s.ribDepth = type.ribDepth;
		const auto addLimb = [&](const glm::vec3& a, const glm::vec3& b, float ra, float rb, const glm::vec3& side, float& along)
		{
			if (s.limbCount >= (uint32)ROCK_MAX_LIMBS)
				return;
			s.limbs[s.limbCount++] = { a, b, ra, rb, side, along };
			along += glm::length(b - a) / (TWO_PI * glm::max(ra, 1e-4f));
		};

		// The stem: leaning up to ~3 degrees, its upper half bent a little more.
		const float leanAxis = rnd(60) * TWO_PI;
		const glm::vec3 leanDir(std::cos(leanAxis), 0.0f, std::sin(leanAxis));
		const glm::vec3 stemSide(-leanDir.z, 0.0f, leanDir.x);
		const glm::vec3 up0 = glm::normalize(Y + 0.05f * rnd(50) * leanDir);
		const glm::vec3 up1 = glm::normalize(up0 + 0.04f * rnds(54) * leanDir + 0.03f * rnds(55) * stemSide);
		const glm::vec3 foot(0.0f, -R, 0.0f); // the ground line cuts the stem's lower dome off
		const float rMid = R * (0.96f + 0.04f * rnd(56));
		const float rTop = R * (0.86f + 0.08f * rnd(51));
		const glm::vec3 mid = foot + up0 * ((0.55f * H + R) / up0.y);
		const glm::vec3 top = mid + up1 * ((0.45f * H - rTop) / up1.y);
		float along = 0.0f;
		addLimb(foot, mid, R, rMid, stemSide, along);
		addLimb(mid, top, rMid, rTop, stemSide, along);
		const auto stemAt = [&](float y) { return y < mid.y ? foot + up0 * ((y - foot.y) / up0.y) : mid + up1 * ((y - mid.y) / up1.y); };

		const int count = glm::min(type.armCount.x + (int)(rnd(52) * (float)(type.armCount.y - type.armCount.x + 1)), type.armCount.y);
		const float phase = rnd(53) * TWO_PI;
		for (int i = 0; i < count && s.limbCount + 5 <= (uint32)ROCK_MAX_LIMBS; ++i)
		{
			const uint32 k = 1000u + 16u * (uint32)i;
			const float azimuth = phase + ((float)i + 0.35f * rnds(k)) * TWO_PI / (float)count;
			const glm::vec3 out(std::cos(azimuth), 0.0f, std::sin(azimuth));
			const glm::vec3 side(-out.z, 0.0f, out.x); // across the arm's plane (out, up)
			const float height = H * glm::mix(type.armHeight.x, type.armHeight.y, rnd(k + 1));
			const float ra = R * type.armThickness * (0.85f + 0.3f * rnd(k + 2));
			const float rise = glm::radians(15.0f + 25.0f * rnd(k + 3));
			const float bend = ra * (1.2f + 0.8f * rnd(k + 4)); // the arc's radius
			// The upright part's distance from the stem's axis = the neck's run + the arc's.
			const float reach = R + type.armReach * R * (0.7f + 0.6f * rnd(k + 5));
			const float neck = glm::max((reach - bend * (1.0f - std::sin(rise))) / std::cos(rise), 1.1f * R);
			float armAlong = 0.37f * (float)(i + 1); // each arm its own stretch of areoles
			glm::vec3 p = stemAt(height);
			glm::vec3 q = p + (out * std::cos(rise) + Y * std::sin(rise)) * neck;
			addLimb(p, q, 0.8f * ra, ra, side, armAlong);
			for (int j = 0; j < 3; ++j)
			{
				const float t0 = glm::mix(rise, 0.5f * PI, (float)j / 3.0f), t1 = glm::mix(rise, 0.5f * PI, (float)(j + 1) / 3.0f);
				p = q;
				q = p + bend * (out * (std::sin(t1) - std::sin(t0)) + Y * (std::cos(t0) - std::cos(t1)));
				addLimb(p, q, ra, ra, side, armAlong);
			}
			const float topY = glm::max(height + (H - height) * (0.45f + 0.5f * rnd(k + 6)), q.y + 2.5f * ra);
			const float rTip = ra * (0.85f + 0.1f * rnd(k + 7));
			const float rise2 = topY - rTip - q.y;
			const glm::vec3 tip = q + Y * rise2 + (out * (0.12f * rnds(k + 8)) + side * (0.06f * rnds(k + 9))) * rise2;
			addLimb(q, tip, ra, rTip, side, armAlong);
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
		const bool pillar = type.shape == ERockShape::Pillar;
		const bool trunk = type.shape == ERockShape::Trunk;
		const bool cactus = type.shape == ERockShape::Cactus;
		aspect = glm::max(aspect, glm::vec3(trunk || cactus ? 0.005f : 0.05f)); // a log, a cactus stem are thin
		glm::vec3 half = aspect * 0.5f;
		s.blockHalf[0] = half;
		s.blockCentre[0] = glm::vec3(0.0f);
		s.blockRot[0] = glm::mat3(1.0f);
		// PILLARS: 1..Group of them (the variant's draw), each with its own profile (the type's, jittered, its widest
		// point 1) and a lean of up to ~9 degrees; a later one is smaller and stands beside an earlier one, their bases
		// overlapping, on the same floor.
		s.blockCount = pillar ? 1u + glm::min((uint32)(rnd(5) * (float)type.group), (uint32)type.group - 1u) : trunk || cactus ? 1u : (uint32)type.pile;
		if (trunk)
			buildTrunk(type, s, rnd);
		if (cactus)
			buildCactus(type, s, rnd);
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
		for (uint32 b = 1; !pillar && !trunk && b < s.blockCount; ++b)
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
		// A trunk's limbs and root plate, a cactus's arms reach past its block.
		for (uint32 i = 0; i < s.limbCount; ++i)
		{
			const RockShape::Limb& l = s.limbs[i];
			lo = glm::min(lo, glm::min(l.a - l.ra, l.b - l.rb));
			hi = glm::max(hi, glm::max(l.a + l.ra, l.b + l.rb));
		}
		if (s.plate.w > 0.0f)
		{
			const glm::vec3 e(0.5f * s.plateThickness, s.plate.w, s.plate.w);
			lo = glm::min(lo, glm::vec3(s.plate) - e);
			hi = glm::max(hi, glm::vec3(s.plate) + e);
		}
		const glm::vec3 size = hi - lo;
		const float norm = 1.0f / glm::max(size.x, glm::max(size.y, size.z));
		const glm::vec3 centre = (lo + hi) * 0.5f;
		for (uint32 b = 0; b < s.blockCount; ++b)
		{
			s.blockCentre[b] = (s.blockCentre[b] - centre) * norm;
			s.blockHalf[b] *= norm;
		}
		for (uint32 i = 0; i < s.limbCount; ++i)
		{
			RockShape::Limb& l = s.limbs[i]; // its side and along0 are scale-free
			l.a = (l.a - centre) * norm;
			l.b = (l.b - centre) * norm;
			l.ra *= norm;
			l.rb *= norm;
		}
		s.plate = glm::vec4((glm::vec3(s.plate) - centre) * norm, s.plate.w * norm);
		s.plateThickness *= norm;
		const glm::vec3 boxHalf = size * 0.5f * norm;
		s.floorY = s.blockCentre[0].y - s.blockHalf[0].y;
		// A trunk's ground line: a log lies on its underside (the trunk's vertical half: block x), a stump / snag / cactus
		// stands on its flat-cut base.
		s.groundY = s.standing ? s.blockCentre[0].y - s.blockHalf[0].y : s.blockCentre[0].y - s.blockHalf[0].x;
		// EROSION shrinks every block by its radius before the field grows back: past the thinnest half-axis the block
		// turns inside out, its field negative out to the bounds - an OPEN mesh (the clutter's thin pebbles at Erosion
		// 0.2, 2026-10-07). Capped at 80 % of block 0's thinnest half-axis.
		s.erosion = glm::min(s.erosion, 0.8f * glm::min(s.blockHalf[0].x, glm::min(s.blockHalf[0].y, s.blockHalf[0].z)));

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
		// The stretch runs along a lying trunk (its X), else along Y.
		s.noiseScale = trunk && type.lying ? glm::vec3(1.0f / type.noiseStretch, 1.0f, 1.0f) : glm::vec3(1.0f, 1.0f / type.noiseStretch, 1.0f);
		s.ridged = type.ridged;
		s.wood = type.surface == ERockSurface::Wood || type.surface == ERockSurface::Cactus;
		s.cactusSkin = type.surface == ERockSurface::Cactus;

		// Pits: a sphere sunk into the body's surface from a random, mostly upward direction (the bisection finds the
		// surface along it on the body alone).
		// A trunk's rays start on its axis (the shape's origin may lie outside a log); a knot hole sits along it.
		const float reachMax = glm::length(boxHalf) * 1.5f;
		for (uint32 i = 0; i < (uint32)type.pitCount && s.pitCount < ROCK_MAX_PITS; ++i)
		{
			glm::vec3 dir = randomDir(500 + i * 3);
			dir.y = glm::abs(dir.y) + 0.3f;
			dir = glm::normalize(dir);
			const glm::vec3 from = trunk ? glm::transpose(s.blockRot[0]) * glm::vec3(0.0f, s.blockHalf[0].y * rnds(900 + i) * 0.8f, 0.0f) + s.blockCentre[0] : glm::vec3(0.0f);
			float t0 = 0.0f, t1 = reachMax;
			if (bodySdf(s, from + dir * t1) < 0.0f || bodySdf(s, from) > 0.0f)
				continue;
			for (int k = 0; k < 24; ++k)
			{
				const float t = (t0 + t1) * 0.5f;
				(bodySdf(s, from + dir * t) < 0.0f ? t0 : t1) = t;
			}
			const float r = type.pitSize * (0.6f + 0.8f * rnd(502 + i * 3));
			s.pits[s.pitCount++] = glm::vec4(from + dir * (t0 + r - type.pitDepth), r);
		}

		const float pad = type.noiseAmplitude * 1.5f + type.strataDepth * (1.0f + type.strataVar) + type.warpAmplitude * 1.8f + 0.03f;
		s.boundsMin = -boxHalf - pad;
		s.boundsMax = boxHalf + pad;
	}

	void generateRockVariant(const RockTypeDesc& type, uint32 seed, uint32 gridResolution, RockVariant& out)
	{
		out = RockVariant{};
		buildRockShape(type, seed, out.shape);

		// The FULL surface-nets mesh ("Grid resolution" x the type's `Resolution` cells along the longest axis) is CPU
		// working data only: every LOD
		// level is simplified from it.
		RockMesh full;
		float cell = 0.0f;
		// The bounds are a conservative guess, but not always: the superellipsoid field is no exact distance, so the
		// erosion's grow-back can carry a thin body past them (the clutter's pebbles, 2026-10-07) - the mesh was then
		// open at the grid's border. Grown by 15 % of the extent each side and meshed again, up to 4 times.
		for (int attempt = 0; ; ++attempt)
		{
			full = {};
			if (surfaceNets(out.shape, glm::max(gridResolution, 16u), full, cell) || attempt == 3)
				break;
			const glm::vec3 grow = (out.shape.boundsMax - out.shape.boundsMin) * 0.15f;
			out.shape.boundsMin -= grow;
			out.shape.boundsMax += grow;
		}
		if (full.indices.empty())
			return;
		projectAndShade(out.shape, full, cell, 2);

		// Rock-local origin: the lowest point at y = 0 - a trunk's ground line (its roots and root plate below it).
		// Shifting the BLOCKS, planes, pits and bounds would also move the strata and the noise (world-fixed in the
		// shape's frame), so the shape keeps its frame and every mesh shifts.
		float minY = 1e9f, maxY = -1e9f;
		for (const glm::vec3& p : full.positions)
		{
			minY = glm::min(minY, p.y);
			maxY = glm::max(maxY, p.y);
		}
		const bool groundLine = out.shape.kind == ERockShape::Trunk || out.shape.kind == ERockShape::Cactus;
		const float originY = groundLine ? glm::clamp(out.shape.groundY, minY, maxY) : minY;
		for (glm::vec3& p : full.positions)
			p.y -= originY;
		out.shape.originY = originY;
		// A lying trunk's height is its thickness (the sink buries a share of the log, not of its root plate).
		const bool lyingTrunk = out.shape.kind == ERockShape::Trunk && !out.shape.standing;
		out.height = lyingTrunk ? 2.0f * out.shape.blockHalf[0].x : maxY - originY;
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
					const float d = rockSdf(out.shape, local + glm::vec3(0.0f, originY, 0.0f)); // rock-local -> the shape frame
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
