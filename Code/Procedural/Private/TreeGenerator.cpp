module Procedural;

import Core;
import Core.glm;
import Core.Transform;

import :TreeSpecies;
import :TreeGenerator;

namespace
{
	using namespace Procedural;

	constexpr float TWO_PI = 6.28318531f;
	constexpr float GOLDEN_ANGLE = 2.39996323f;
	constexpr glm::vec3 WORLD_UP{ 0.0f, 1.0f, 0.0f };

	// Generation randomness (CPU only, the library is generated once): a treeHash chain.
	struct Rng
	{
		uint32 state;
		explicit Rng(uint32 seed) : state(treeHash(seed)) {}
		uint32 next() { state = treeHash(state); return state; }
		float next01() { return treeHash01(next()); }
		float range(float lo, float hi) { return lo + (hi - lo) * next01(); }
		float signed1() { return next01() * 2.0f - 1.0f; }
	};

	glm::vec3 anyPerpendicular(const glm::vec3& d)
	{
		const glm::vec3 ref = glm::abs(d.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		return glm::normalize(glm::cross(d, ref));
	}

	glm::vec3 randomPerpendicular(const glm::vec3& d, Rng& rng)
	{
		return glm::angleAxis(rng.next01() * TWO_PI, d) * anyPerpendicular(d);
	}

	// A branch centreline: points with their tangents and arc-length fraction `u`, and a linear radius
	// taper over `u`. Elbows insert extra points, so everything along the branch is addressed by `u`.
	struct BranchPath
	{
		oc::vector<glm::vec3> points;
		oc::vector<glm::vec3> tangents;
		oc::vector<float> u;
		float length = 1.0f;
		float r0 = 0.1f;
		float r1 = 0.01f;

		float radiusAt(float t) const { return glm::mix(r0, r1, t); }

		struct Sample { glm::vec3 pos; glm::vec3 tangent; float radius; };
		Sample sample(float t) const
		{
			t = glm::clamp(t, 0.0f, 1.0f);
			size_t i = 1;
			while (i + 1 < u.size() && u[i] < t)
				++i;
			const float w = glm::clamp((t - u[i - 1]) / glm::max(u[i] - u[i - 1], 1e-6f), 0.0f, 1.0f);
			return { glm::mix(points[i - 1], points[i], w), glm::normalize(glm::mix(tangents[i - 1], tangents[i], w)), radiusAt(t) };
		}

		// Tangents from the neighbours, u from the arc length.
		void finish()
		{
			const size_t n = points.size();
			tangents.resize(n);
			u.resize(n);
			float acc = 0.0f;
			for (size_t k = 0; k < n; ++k)
			{
				tangents[k] = glm::normalize(points[glm::min(k + 1, n - 1)] - points[k > 0 ? k - 1 : 0]);
				if (k > 0)
					acc += glm::distance(points[k], points[k - 1]);
				u[k] = acc;
			}
			for (float& v : u)
				v /= glm::max(acc, 1e-6f);
		}
	};

	// Where a branch turned sharply: the rounded corner's middle point, the direction the branch had
	// BEFORE the turn (a snapped-off stub continues it), and its arc-length fraction.
	struct Elbow
	{
		glm::vec3 pos;
		glm::vec3 incomingDir;
		float u;
	};

	// Smooth bend (`curve` about one random axis), up-attraction, a per-segment random walk (`wobble`) and
	// sharp `elbows`, each rounded by replacing its corner point with three points of a quadratic Bezier.
	void growBranch(BranchPath& b, const glm::vec3& origin, const glm::vec3& dir, float length, float r0, float r1,
		const TreeBranchShape& shape, const glm::vec3& up, Rng& rng, oc::vector<Elbow>* outElbows = nullptr,
		float spacingPower = 1.0f)
	{
		const int n = glm::max(shape.rings, 2);
		b.points.clear();
		b.length = length;
		b.r0 = r0;
		b.r1 = r1;

		// Node k sits at length fraction (k / (n-1))^spacingPower: > 1 packs the rings toward the base (the
		// trunk's flare and buttresses change fastest there).
		oc::vector<float> nodeAt((size_t)n);
		for (int k = 0; k < n; ++k)
			nodeAt[(size_t)k] = std::pow((float)k / (float)(n - 1), spacingPower);

		// Stochastic rounding of the average elbow count; elbows sit on interior nodes only.
		const float elbowsF = shape.elbows * rng.range(0.5f, 1.5f);
		int numElbows = (int)elbowsF + (rng.next01() < elbowsF - std::floor(elbowsF) ? 1 : 0);
		numElbows = glm::min(numElbows, n - 2);
		oc::vector<uint8> elbowAt((size_t)n, 0);
		for (int e = 0; e < numElbows; ++e)
		{
			const float f = ((float)e + 0.5f + rng.signed1() * 0.3f) / (float)numElbows;
			const float along = glm::mix(shape.elbowRange.x, shape.elbowRange.y, glm::clamp(f, 0.0f, 1.0f));
			int best = 1;
			for (int k = 2; k <= n - 2; ++k)
				if (glm::abs(nodeAt[(size_t)k] - along) < glm::abs(nodeAt[(size_t)best] - along))
					best = k;
			elbowAt[(size_t)best] = 1;
		}

		const float curveRad = glm::radians(shape.curve + rng.signed1() * shape.curveVar);
		glm::vec3 d = glm::normalize(dir);
		glm::vec3 axis = randomPerpendicular(d, rng);
		const float wobble = glm::radians(shape.wobble);
		const float droop = glm::radians(shape.droop);

		oc::vector<glm::vec3> raw;
		oc::vector<glm::vec3> incoming((size_t)n, glm::vec3(0.0f));
		glm::vec3 p = origin;
		for (int k = 0; k < n; ++k)
		{
			raw.push_back(p);
			if (k == n - 1)
				break;
			// Per-segment shares of the totals; the wobble is a random walk, so it scales with sqrt(share).
			const float frac = nodeAt[(size_t)k + 1] - nodeAt[(size_t)k];
			d = glm::normalize(glm::angleAxis(curveRad * frac, axis) * d + up * (shape.upAttract * frac));
			if (wobble > 0.0f)
				d = glm::normalize(glm::angleAxis(wobble * std::sqrt(frac * (float)(n - 1)) * rng.signed1(), randomPerpendicular(d, rng)) * d);
			// Droop: a fixed turn per length toward straight down that stops there - the branch arcs over, then hangs.
			if (droop > 0.0f)
			{
				const glm::vec3 fall = glm::cross(d, -up);
				const float toDown = std::acos(glm::clamp(-glm::dot(d, up), -1.0f, 1.0f));
				const float turn = glm::min(droop * frac, toDown);
				if (turn > 1e-5f && glm::dot(fall, fall) > 1e-10f)
					d = glm::normalize(glm::angleAxis(turn, glm::normalize(fall)) * d);
			}
			if (elbowAt[(size_t)k])
			{
				incoming[(size_t)k] = d;
				const float turn = glm::radians(shape.elbowAngle + rng.signed1() * shape.elbowAngleVar);
				const glm::vec3 turnAxis = randomPerpendicular(d, rng);
				glm::vec3 turned = glm::angleAxis(turn, turnAxis) * d;
				// Mirroring the turn about the same axis mirrors its change in height to first order.
				if (glm::dot(turned, up) < glm::dot(d, up) && rng.next01() < shape.elbowUpBias)
					turned = glm::angleAxis(-turn, turnAxis) * d;
				// Floor the elevation after the turn - but never above where the branch already pointed, so
				// an intentionally drooping branch is not lifted by its own elbow.
				const float incomingElev = std::asin(glm::clamp(glm::dot(d, up), -1.0f, 1.0f));
				const float minElev = glm::min(glm::radians(shape.elbowMinElevation), incomingElev);
				const float elev = std::asin(glm::clamp(glm::dot(turned, up), -1.0f, 1.0f));
				const glm::vec3 liftAxis = glm::cross(turned, up);
				if (elev < minElev && glm::dot(liftAxis, liftAxis) > 1e-8f)
					turned = glm::angleAxis(minElev - elev, glm::normalize(liftAxis)) * turned;
				d = glm::normalize(turned);
			}
			const glm::vec3 a = axis - d * glm::dot(axis, d);
			axis = glm::dot(a, a) > 1e-8f ? glm::normalize(a) : anyPerpendicular(d);
			p += d * (length * frac);
		}

		oc::vector<size_t> elbowPoints;
		for (int k = 0; k < n; ++k)
		{
			if (!elbowAt[(size_t)k])
			{
				b.points.push_back(raw[(size_t)k]);
				continue;
			}
			const glm::vec3& P = raw[(size_t)k];
			const glm::vec3 pa = P + (raw[(size_t)k - 1] - P) * 0.3f;
			const glm::vec3 pb = P + (raw[(size_t)k + 1] - P) * 0.3f;
			b.points.push_back(pa);
			elbowPoints.push_back(b.points.size());
			b.points.push_back(pa * 0.25f + P * 0.5f + pb * 0.25f);
			b.points.push_back(pb);
		}
		b.finish();

		if (outElbows)
		{
			size_t e = 0;
			for (int k = 0; k < n; ++k)
				if (elbowAt[(size_t)k])
				{
					const size_t idx = elbowPoints[e++];
					outElbows->push_back({ b.points[idx], incoming[(size_t)k], b.u[idx] });
				}
		}
	}

	// A branch that STARTS along `dir0` and bends to `dir1` over its first `bendEnd` of the length (smoothstep), then runs
	// straight on: its first ring is square to `dir0`, so it continues a parent ending along `dir0` with no wedge between
	// the two rings (a split's continuing branch).
	// `wobble` (radians per segment, a random walk like growBranch's) starts after the bend's first half, so the first
	// ring still meets the parent square.
	void bentPath(BranchPath& b, const glm::vec3& origin, const glm::vec3& dir0, const glm::vec3& dir1, float length, float r0,
		float r1, int rings, float bendEnd, float wobble = 0.0f, Rng* rng = nullptr)
	{
		const int n = glm::max(rings, 3);
		b.points.clear();
		b.length = length;
		b.r0 = r0;
		b.r1 = r1;
		glm::vec3 p = origin;
		b.points.push_back(p);
		glm::quat walk(1.0f, 0.0f, 0.0f, 0.0f);
		for (int k = 0; k < n - 1; ++k)
		{
			const float u = ((float)k + 0.5f) / (float)(n - 1);
			glm::vec3 d = glm::normalize(glm::mix(dir0, dir1, glm::smoothstep(0.0f, bendEnd, u)));
			if (wobble > 0.0f && rng && u > bendEnd * 0.5f)
			{
				walk = glm::normalize(glm::angleAxis(wobble * rng->signed1(), randomPerpendicular(d, *rng)) * walk);
				d = glm::normalize(walk * d);
			}
			p += d * (length / (float)(n - 1));
			b.points.push_back(p);
		}
		b.finish();
		b.tangents.front() = glm::normalize(dir0); // finish takes it from the first segment: exactly the parent's end
	}

	// A lobed, twisting cross-section (trunk buttresses): radius x (1 + depth(u) * (ridges(theta + twist u) -
	// mean)). Ridges are irregular (random phase jitter and height), deepest at the base, fading over
	// `fadeHeight` to a small leftover irregularity.
	struct TubeProfile
	{
		static constexpr int MAX_LOBES = 12;
		int lobes = 0;
		float phase[MAX_LOBES] = {};
		float amp[MAX_LOBES] = {};
		float mean = 0.0f;
		glm::vec2 depth{ 0.0f };  // base, top
		float fadeHeight = 0.3f;
		float twist = 0.0f;       // radians over the full length

		// Gaussian ridges, each ~1/3 of the lobe spacing wide: separate peaks with real valleys between them
		// (a raised-cosine bump per lobe overlapped its neighbours into an almost round section).
		float ridges(float phi) const
		{
			const float sigma = 0.33f * TWO_PI / (float)glm::max(lobes, 1);
			const float invS2 = 1.0f / (sigma * sigma);
			float s = 0.0f;
			for (int i = 0; i < lobes; ++i)
			{
				const float d = std::remainder(phi - phase[i], TWO_PI); // wrapped to [-pi, pi]
				s += amp[i] * std::exp(-d * d * invS2);
			}
			return s;
		}

		float scale(float theta, float u) const
		{
			const float fade = 1.0f - glm::smoothstep(0.0f, fadeHeight, u);
			const float d = glm::mix(depth.y, depth.x, fade * fade);
			return 1.0f + d * (ridges(theta + twist * u) - mean);
		}

		void build(int numLobes, glm::vec2 lobeDepth, float lobeHeight, float twistDeg, Rng& rng)
		{
			lobes = glm::min(numLobes, MAX_LOBES);
			for (int i = 0; i < lobes; ++i)
			{
				phase[i] = TWO_PI * ((float)i + rng.signed1() * 0.3f) / (float)lobes;
				amp[i] = rng.range(0.7f, 1.0f);
			}
			mean = 0.0f;
			for (int s = 0; s < 64; ++s)
				mean += ridges(TWO_PI * (float)s / 64.0f);
			mean /= 64.0f;
			depth = lobeDepth;
			fadeHeight = lobeHeight;
			twist = glm::radians(twistDeg) * rng.range(0.6f, 1.4f) * (rng.next01() < 0.5f ? -1.0f : 1.0f);
		}
	};

	// Swept tube along the path (parallel-transport frames). UV: u around (0..1), v along the length in
	// units of the base circumference, so bark texels stay square at the base. `cap` closes the end with a
	// flat disc (the broken face of a stub). A `profile` deforms the cross-section; the normals then come
	// from the deformed grid instead of the radial direction. `extendBase` adds one ring that continues the
	// base backward by that many metres with the base's own cross-section (a trunk sunk into sloped ground).
	// `tip` (a fraction of the length): over that last part the radius narrows to TUBE_TIP_RADIUS of the path's - a closed
	// point, not an open cut (the trunk's end). The mesh only: BranchPath::sample keeps the linear radius (slots, children).
	// `section` != 1 1 (TreeBranchShape::section): a flat BLADE - each ring an ellipse, `section.x` x the radius ACROSS
	// (cross(tangent, faceUp): the module's X) and `section.y` x it toward `faceUp` (the module's up side, +Z), bent
	// into a gutter by `channel` (its middle that x the radius away from faceUp: hollow on the up side); its normals from
	// the deformed grid, as a profile's. `swell`: the radius x (1 + swell sin(pi t)) - widest near the middle.
	constexpr float TUBE_TIP_RADIUS = 0.12f;
	void appendTube(TreeMesh& m, const BranchPath& b, int sides, uint8 bone, float flare = 0.0f, bool cap = false,
		const TubeProfile* profile = nullptr, float extendBase = 0.0f, float tip = 0.0f, glm::vec2 section = glm::vec2(1.0f),
		float channel = 0.0f, float swell = 0.0f, const glm::vec3& faceUp = glm::vec3(0.0f, 0.0f, 1.0f))
	{
		const bool blade = section != glm::vec2(1.0f) || channel != 0.0f;
		const int extra = extendBase > 0.0f ? 1 : 0;
		const int n = (int)b.points.size() + extra; // rings
		const uint32 base = m.numVertices();
		glm::vec3 frameN = anyPerpendicular(b.tangents[0]);
		glm::vec3 frameB = glm::cross(b.tangents[0], frameN);
		const float vScale = 1.0f / (TWO_PI * glm::max(b.r0, 1e-3f));
		float r = b.r0;
		for (int ring = 0; ring < n; ++ring)
		{
			const int k = glm::max(ring - extra, 0);
			const bool below = ring < extra;
			const glm::vec3 tan = b.tangents[k];
			if (k > 0)
			{
				const glm::vec3 projected = frameN - tan * glm::dot(frameN, tan);
				frameN = glm::dot(projected, projected) > 1e-8f ? glm::normalize(projected) : anyPerpendicular(tan);
			}
			frameB = glm::cross(tan, frameN);
			const float t = b.u[k];
			const float vAcc = below ? -extendBase : t * b.length;
			const glm::vec3 centre = below ? b.points[0] - tan * extendBase : b.points[k];
			r = b.radiusAt(t);
			if (flare > 0.0f)
			{
				const float f = 1.0f - glm::smoothstep(0.0f, 0.15f, t); // eases into the trunk, no step
				r *= 1.0f + flare * f * f;
			}
			if (swell != 0.0f)
				r *= 1.0f + swell * std::sin(0.5f * TWO_PI * t);
			if (tip > 0.0f)
				r *= glm::mix(1.0f, TUBE_TIP_RADIUS, glm::smoothstep(1.0f - tip, 1.0f, t));
			// A blade's axes: across it and through its face (a branch along faceUp keeps the transported frame). The pair
			// keeps the round frame's handedness (frameB = tan x frameN): the normals and the winding stay outward.
			glm::vec3 across = frameN, toFace = frameB;
			if (blade)
			{
				const glm::vec3 w = glm::cross(tan, faceUp);
				if (glm::dot(w, w) > 1e-8f)
				{
					across = glm::normalize(w);
					toFace = glm::cross(tan, across);
				}
			}
			for (int j = 0; j <= sides; ++j)
			{
				const float theta = TWO_PI * (float)j / (float)sides;
				const float c = std::cos(theta), s = std::sin(theta);
				const glm::vec3 offset = c * frameN + s * frameB;
				const float rj = profile ? r * profile->scale(theta, t) : r;
				// toFace points AWAY from faceUp: + channel lowers the blade's middle (a gutter, hollow on its up side).
				m.positions.push_back(centre + (blade ? (c * section.x * across + (s * section.y + channel * (1.0f - c * c)) * toFace) * r : offset * rj));
				m.normals.push_back(offset);
				m.tangents.push_back(-s * frameN + c * frameB);
				m.bitangents.push_back(tan);
				m.texCoords.push_back(glm::vec3((float)j / (float)sides, vAcc * vScale, 0.0f));
				m.bones.push_back(bone);
			}
		}
		const uint32 stride = (uint32)sides + 1;

		if (profile || blade)
		{
			// Normals (and the around-tangent) from central differences on the deformed (ring, side) grid.
			// The seam column (j = 0 and j = sides share a position) uses the wrapped neighbours.
			auto at = [&](int k, int j) { return m.positions[base + (uint32)k * stride + (uint32)j]; };
			for (int k = 0; k < n; ++k)
			{
				for (int j = 0; j <= sides; ++j)
				{
					const int jPrev = j == 0 || j == sides ? sides - 1 : j - 1;
					const int jNext = j == 0 || j == sides ? 1 : j + 1;
					const glm::vec3 dTheta = at(k, jNext) - at(k, jPrev);
					const glm::vec3 dU = at(glm::min(k + 1, n - 1), j) - at(glm::max(k - 1, 0), j);
					const glm::vec3 normal = glm::cross(dTheta, dU);
					const uint32 idx = base + (uint32)k * stride + (uint32)j;
					if (glm::dot(normal, normal) > 1e-12f)
						m.normals[idx] = glm::normalize(normal);
					if (glm::dot(dTheta, dTheta) > 1e-12f)
						m.tangents[idx] = glm::normalize(dTheta);
				}
			}
		}
		for (int k = 0; k < n - 1; ++k)
		{
			for (int j = 0; j < sides; ++j)
			{
				const uint32 a = base + (uint32)k * stride + (uint32)j;
				const uint32 b1 = a + 1;
				const uint32 c = a + stride;
				const uint32 d = c + 1;
				m.indices.insert(m.indices.end(), { a, b1, c, b1, d, c });
			}
		}

		if (cap)
		{
			const glm::vec3 tan = b.tangents.back();
			const glm::vec3 centre = b.points.back();
			const uint32 c = m.numVertices();
			for (int j = -1; j < sides; ++j)
			{
				const float theta = TWO_PI * (float)glm::max(j, 0) / (float)sides;
				const float cs = j < 0 ? 0.0f : std::cos(theta), sn = j < 0 ? 0.0f : std::sin(theta);
				m.positions.push_back(centre + (cs * frameN + sn * frameB) * r);
				m.normals.push_back(tan);
				m.tangents.push_back(frameN);
				m.bitangents.push_back(frameB);
				m.texCoords.push_back(glm::vec3(0.5f + 0.5f * cs, 0.5f + 0.5f * sn, 0.0f));
				m.bones.push_back(bone);
			}
			for (uint32 j = 0; j < (uint32)sides; ++j)
				m.indices.insert(m.indices.end(), { c, c + 1 + j, c + 1 + (j + 1) % (uint32)sides });
		}
	}

	// --- The piece skeleton: everything random is decided once, into plans; each LOD meshes the plans. ---

	struct TubePlan
	{
		BranchPath path;
		int sides = 3;
		uint8 bone = 0;
		int level = 0;          // 0 = trunk / module root, i = sub-branch level i
		float flare = 0.0f;
		bool lobed = false;     // use the plan's TubeProfile
		float sink = 0.0f;
		float tip = 0.0f;       // appendTube's closing tip (trunks; a blade's point)
		bool stub = false;      // a snapped-off stub (capped; dropped at the coarse levels)
		glm::vec2 section{ 1.0f }; // appendTube's blade section (TreeBranchShape::section / channel / swell)
		float channel = 0.0f;
		float swell = 0.0f;
	};

	struct LeafPlan
	{
		glm::vec3 stem;
		glm::vec3 along;
		glm::vec3 normal;
		float length;
		float width;
		uint32 cell;
		uint8 bone;
	};

	struct PiecePlan
	{
		oc::vector<TubePlan> tubes;
		oc::vector<LeafPlan> leaves;
		TubeProfile profile;
		int maxLevel = 0;
	};

	// A snapped-off branch at an elbow: a short, thick, straight stub continuing the direction the branch
	// had before it turned, ending in a flat broken face.
	void planStub(PiecePlan& plan, const BranchPath& parent, const Elbow& elbow, int sides, uint8 bone, int level, Rng& rng)
	{
		const float r = parent.radiusAt(elbow.u);
		const float r0 = r * rng.range(0.55f, 0.85f);
		const float length = r * rng.range(1.5f, 4.0f);
		const glm::vec3 dir = glm::normalize(elbow.incomingDir + randomPerpendicular(elbow.incomingDir, rng) * 0.25f);
		TubePlan& stub = plan.tubes.emplace_back();
		stub.path.length = length;
		stub.path.r0 = r0;
		stub.path.r1 = r0 * 0.85f;
		stub.path.points = { elbow.pos, elbow.pos + dir * (length * 0.5f), elbow.pos + dir * length };
		stub.path.finish();
		stub.sides = sides;
		stub.bone = bone;
		stub.level = level;
		stub.stub = true;
	}

	void planStubs(PiecePlan& plan, const BranchPath& parent, oc::span<const Elbow> elbows, const TreeBranchShape& shape, uint8 bone, int level, Rng& rng)
	{
		for (const Elbow& elbow : elbows)
			if (rng.next01() < shape.stubs)
				planStub(plan, parent, elbow, shape.sides, bone, level, rng);
	}

	// What each LOD keeps: ring / side fractions, how many of the deepest branch levels it drops (never
	// below level 1), the leaf stride (every Nth leaf, scaled by sqrt(N) so the total area holds), stubs.
	struct LodSpec
	{
		float rings;
		float sides;
		int levelDrop;
		uint32 leafStride;
		bool stubs;
		float error; // x piece length x the species' Lod ErrorScale
	};
	constexpr LodSpec PIECE_LODS[TREE_PIECE_LODS] =
	{
		{ 1.0f,  1.0f,  0, 1, true,  0.0f },
		{ 0.6f,  0.6f,  0, 2, true,  0.001f },
		{ 0.4f,  0.45f, 1, 4, false, 0.0025f },
		{ 0.25f, 0.34f, 2, 8, false, 0.005f },
	};

	// Every ~(1/frac)th point, both ends kept (u re-derived from the kept chord lengths).
	BranchPath subsamplePath(const BranchPath& b, float frac)
	{
		const int n = (int)b.points.size();
		const int m = glm::clamp((int)std::round((float)n * frac), 2, n);
		if (m == n)
			return b;
		BranchPath out;
		out.length = b.length;
		out.r0 = b.r0;
		out.r1 = b.r1;
		for (int i = 0; i < m; ++i)
			out.points.push_back(b.points[(size_t)std::round((float)i * (float)(n - 1) / (float)(m - 1))]);
		out.finish();
		return out;
	}

	// THE WIND PAYLOAD (RendererVK tree_wind.inc.glsl, the layout there): texCoords.z = 1 + payload / 2^22 rides the
	// tangent's w magnitude to the vertex shader (RenderMeshData). The module weight runs along the module's root bone
	// (a sub-branch's vertices take it at their bone's pivot, so the sub-branch moves with the module where it attaches),
	// the sub-branch weight along its level-1 bone; the module placement's phase is added by bakeTreeVariant.
	constexpr uint32 TREE_WIND_BIT = 1u << 21;
	constexpr uint32 TREE_WIND_TIP_BIT = 1u << 20;
	constexpr uint32 TREE_WIND_MODULE_PHASE_SHIFT = 17;
	constexpr float TREE_WIND_PAYLOAD_SCALE = 4194304.0f; // 2^22

	float encodeWind(uint32 payload) { return 1.0f + (float)payload / TREE_WIND_PAYLOAD_SCALE; }

	uint32 windPayload(oc::span<const TreeBone> bones, const glm::vec3& p, uint8 bone, bool trunk)
	{
		if (trunk || bones.empty())
			return TREE_WIND_BIT; // the trunk bend only
		auto along = [](const TreeBone& b, const glm::vec3& q) { return glm::clamp(glm::dot(q - b.pivot, b.axis) / glm::max(b.length, 1e-3f), 0.0f, 1.0f); };
		if (bone == 0 || bone >= bones.size())
			return (uint32)std::round(along(bones[0], p) * 127.0f) | TREE_WIND_BIT;
		const uint32 rootW = (uint32)std::round(along(bones[0], bones[bone].pivot) * 127.0f);
		const uint32 boneW = (uint32)std::round(along(bones[bone], p) * 63.0f);
		return rootW | (boneW << 7) | ((treeHash(bone) & 15u) << 13) | TREE_WIND_BIT;
	}

	// A diamond leaf card from its stem point, both faces (no alpha test needed for the placeholder shape).
	void appendLeaf(TreeMesh& m, const glm::vec3& stem, const glm::vec3& along, const glm::vec3& normal, float length, float width, uint8 bone,
		uint32 wind)
	{
		const glm::vec3 side = glm::normalize(glm::cross(normal, along));
		const glm::vec3 corners[4] =
		{
			stem,
			stem + along * (length * 0.35f) + side * (width * 0.5f),
			stem + along * length,
			stem + along * (length * 0.35f) - side * (width * 0.5f),
		};
		const glm::vec3 uvs[4] = { { 0.5f, 0.0f, 0.0f }, { 1.0f, 0.35f, 0.0f }, { 0.5f, 1.0f, 0.0f }, { 0.0f, 0.35f, 0.0f } };
		for (int face = 0; face < 2; ++face)
		{
			const uint32 base = m.numVertices();
			const glm::vec3 n = face == 0 ? normal : -normal;
			for (int i = 0; i < 4; ++i)
			{
				m.positions.push_back(corners[i]);
				m.normals.push_back(n);
				m.tangents.push_back(side); // dPos/dU on BOTH faces (same UVs): the back face's handedness flips instead
				m.bitangents.push_back(along);
				m.texCoords.push_back(glm::vec3(glm::vec2(uvs[i]), encodeWind(i == 0 ? wind : wind | TREE_WIND_TIP_BIT))); // all but the stem flutter
				m.bones.push_back(bone);
			}
			if (face == 0)
				m.indices.insert(m.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
			else
				m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}
	}

	// A leaf-cluster card from its stem point: a quad mapped to one cell of the 2x2 leaf atlas (stem at v = 0),
	// both faces. Alpha-tested, so the outline comes from the texture. `normalBend` (0..1) bends each vertex
	// normal - on BOTH faces - toward the direction away from `crownCentre`, so the crown shades as one round
	// volume instead of a stack of flat cards, the same from either side of a card.
	void appendCard(TreeMesh& m, const glm::vec3& stem, const glm::vec3& along, const glm::vec3& normal, float length, float width,
		uint32 cell, uint8 bone, const glm::vec3& crownCentre, float normalBend, uint32 wind)
	{
		constexpr float CELL = 0.5f; // 1 / TREE_LEAF_ATLAS_CELLS
		const glm::vec2 cellOrigin((float)(cell & 1u) * CELL, (float)((cell >> 1) & 1u) * CELL);
		const glm::vec3 side = glm::normalize(glm::cross(normal, along));
		const glm::vec3 halfSide = side * (width * 0.5f);
		const glm::vec3 corners[4] = { stem - halfSide, stem + halfSide, stem + halfSide + along * length, stem - halfSide + along * length };
		const glm::vec2 uvs[4] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
		for (int face = 0; face < 2; ++face)
		{
			const uint32 base = m.numVertices();
			for (int i = 0; i < 4; ++i)
			{
				const glm::vec2 uv = cellOrigin + uvs[i] * CELL;
				const glm::vec3 faceNormal = face == 0 ? normal : -normal;
				const glm::vec3 away = corners[i] - crownCentre;
				glm::vec3 n = faceNormal;
				if (normalBend > 0.0f && glm::dot(away, away) > 1e-8f)
				{
					const glm::vec3 bent = glm::mix(faceNormal, glm::normalize(away), normalBend);
					if (glm::dot(bent, bent) > 1e-6f)
						n = glm::normalize(bent);
				}
				// The tangent is dPos/dU on BOTH faces (they share the UVs) - the back face's handedness flips instead
				// (RenderMesh's sign from n, t, b). A back face with -side mirrored the normal map's X: its bumps faced
				// away from the light, and half the leaves shaded too dark.
				const glm::vec3 t = side - n * glm::dot(side, n);
				m.positions.push_back(corners[i]);
				m.normals.push_back(n);
				m.tangents.push_back(glm::dot(t, t) > 1e-8f ? glm::normalize(t) : side);
				m.bitangents.push_back(along);
				m.texCoords.push_back(glm::vec3(uv, encodeWind(i >= 2 ? wind | TREE_WIND_TIP_BIT : wind))); // corners 2, 3: the tip
				m.bones.push_back(bone);
			}
			if (face == 0)
				m.indices.insert(m.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
			else
				m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}
	}

	// The LOD levels meshPiece builds: LEVEL 0 ONLY - the trees switch through their own tiers (mid tier, billboard, far
	// volume) and upload no mesh LOD chains (TreeSystem uploadLodChain). Levels 1.. stay empty; PIECE_LODS keeps their
	// specs should mesh LODs come back.
	constexpr uint32 MESHED_LODS = 1;

	// Meshes the LODs of a piece from its plan (MESHED_LODS of them). `trunk`: its vertices take the wind's trunk bend only.
	void meshPiece(const TreeSpeciesDesc& sp, const PiecePlan& plan, TreePiece& out, bool trunk)
	{
		// The module root is the attach point on the trunk axis - inside the crown, the best centre a shared
		// module knows (the composited tree's own centre differs per tree).
		const glm::vec3 crownCentre(0.0f);
		for (uint32 lod = 0; lod < MESHED_LODS; ++lod)
		{
			const LodSpec& spec = PIECE_LODS[lod];
			const int keepLevel = glm::max(plan.maxLevel - spec.levelDrop, 1);
			TreeMesh& bark = out.bark[lod];
			for (const TubePlan& tube : plan.tubes)
			{
				if (tube.level > keepLevel || (tube.stub && !spec.stubs))
					continue;
				const int sides = glm::max(3, (int)std::round((float)tube.sides * spec.sides));
				appendTube(bark, subsamplePath(tube.path, spec.rings), sides, tube.bone, tube.flare, tube.stub,
					tube.lobed ? &plan.profile : nullptr, tube.sink, tube.tip, tube.section, tube.channel, tube.swell);
			}
			for (uint32 v = 0; v < bark.numVertices(); ++v)
				bark.texCoords[v].z = encodeWind(windPayload(out.bones, bark.positions[v], bark.bones[v], trunk));

			const float grow = std::sqrt((float)spec.leafStride);
			for (size_t i = 0; i < plan.leaves.size(); i += spec.leafStride)
			{
				const LeafPlan& leaf = plan.leaves[i];
				const float length = leaf.length * grow, width = leaf.width * grow;
				const uint32 wind = windPayload(out.bones, leaf.stem, leaf.bone, trunk); // the stem's: one phase per card
				if (sp.leafType == ETreeLeafType::Single)
				{
					appendLeaf(out.leaves[lod], leaf.stem, leaf.along, leaf.normal, length, width, leaf.bone, wind);
					continue;
				}
				appendCard(out.leaves[lod], leaf.stem, leaf.along, leaf.normal, length, width, leaf.cell, leaf.bone, crownCentre, sp.leafNormalBend, wind);
				if (sp.leafCross)
					appendCard(out.leaves[lod], leaf.stem, leaf.along, glm::normalize(glm::cross(leaf.along, leaf.normal)), length, width,
						leaf.cell ^ 1u, leaf.bone, crownCentre, sp.leafNormalBend, wind);
			}
			out.lodError[lod] = spec.error * out.length * sp.lodErrorScale;
		}
	}

	// Any branch with Droop: its modules must know world-down exactly (generateTreeLibrary / compositeTree).
	bool speciesHangs(const TreeSpeciesDesc& sp)
	{
		if (sp.moduleShape.droop > 0.0f)
			return true;
		for (const TreeBranchLevel& level : sp.levels)
			if (level.shape.droop > 0.0f)
				return true;
		return false;
	}

	float crownEnvelope(ETreeCrownShape shape, float h)
	{
		switch (shape)
		{
		case ETreeCrownShape::Ellipsoid:
		{
			const float x = (h - 0.4f) / 0.6f;
			return glm::max(std::sqrt(glm::max(0.0f, 1.0f - x * x)), 0.25f);
		}
		case ETreeCrownShape::Cone:     return glm::max(1.0f - h, 0.08f);
		case ETreeCrownShape::Umbrella: return 0.3f + 0.7f * std::pow(h, 1.5f);
		case ETreeCrownShape::Column:   return 0.35f + 0.1f * std::sin(h * 3.14159265f);
		case ETreeCrownShape::Dome:     return glm::max(std::sqrt(glm::max(0.0f, 1.0f - h * h * h)), 0.3f);
		}
		return 1.0f;
	}

	// A trunk piece: one stem, or (Trunk Stems) several from around one base. The single-stem path draws its randomness
	// in the original order, so a one-stem species' trunks are unchanged.
	void generateTrunk(const TreeSpeciesDesc& sp, uint32 seed, TreePiece& out)
	{
		Rng rng(seed);
		const float height = rng.range(sp.trunkHeight.x, sp.trunkHeight.y);
		out.length = height;
		out.baseRadius = sp.trunkRadius;

		const int stems = sp.trunkStems.y > sp.trunkStems.x
			? sp.trunkStems.x + (int)(rng.next() % (uint32)(sp.trunkStems.y - sp.trunkStems.x + 1)) : sp.trunkStems.x;
		const float stemAzimuth = stems > 1 ? rng.next01() * TWO_PI : 0.0f;
		if (stems > 1)
			out.baseRadius = sp.trunkRadius + sp.trunkStemSpread;

		PiecePlan plan;
		oc::vector<Elbow> elbows;
		float crownStart = glm::clamp(sp.crownStart, 0.0f, 0.98f);

		// FORK (several stems, Trunk Fork > 0): ONE trunk from the ground to the fork height - flare, ridges, sink - and
		// the stems grow from its top, each thinner (the cross-section shared: base top radius x 1.15 / sqrt(stems), at
		// most 0.85 of it), starting a tip radius down inside it so the joint is hidden. The crown starts at the same
		// tree height, on the stems.
		const float fork = stems > 1 ? glm::clamp(sp.trunkFork, 0.0f, 0.9f) : 0.0f;
		const float treeCrownStart = crownStart;
		glm::vec3 forkPoint(0.0f), forkTangent = WORLD_UP;
		float stemRadius = sp.trunkRadius;
		BranchPath basePath;
		if (fork > 0.0f)
		{
			// The lower trunk STRAIGHTER than the stems: a third of their curve and wobble, no elbows (the stems' bend made
			// it look bent).
			TreeBranchShape baseShape = sp.trunkShape;
			baseShape.curve *= 0.33f;
			baseShape.curveVar *= 0.33f;
			baseShape.wobble *= 0.33f;
			baseShape.elbows = 0.0f;
			TubePlan& base = plan.tubes.emplace_back();
			growBranch(base.path, glm::vec3(0.0f), WORLD_UP, height * fork, sp.trunkRadius,
				sp.trunkRadius * glm::mix(1.0f, sp.trunkTaper, fork), baseShape, WORLD_UP, rng, &elbows, 1.6f);
			base.sides = sp.trunkShape.sides;
			base.flare = sp.trunkFlare;
			base.lobed = sp.trunkLobes > 0;
			base.sink = glm::max(sp.trunkSink, 0.0f);
			if (base.lobed)
				plan.profile.build(sp.trunkLobes, sp.trunkLobeDepth, sp.trunkLobeHeight, sp.trunkTwist, rng);
			basePath = base.path;
			planStubs(plan, basePath, elbows, sp.trunkShape, 0, 0, rng);
			stemRadius = glm::min(basePath.r1 * 0.85f, basePath.r1 * 1.15f / std::sqrt((float)stems));
			forkPoint = basePath.points.back() - basePath.tangents.back() * basePath.r1;
			forkTangent = basePath.tangents.back();
			crownStart = glm::clamp((crownStart - fork) / (1.0f - fork), 0.0f, 0.95f);
			out.bones.push_back({ -1, glm::vec3(0.0f), WORLD_UP, height });
			out.baseRadius = sp.trunkRadius;
		}

		// The stems: at even azimuths around the base (jittered), leaning out by StemLean x 0.8..1.1, the first the
		// tallest. Their slots are split by LENGTH (an even split left the tallest stem's layers sparse).
		// With a fork the BASE trunk carries limbs too (the long low ones), its share of the slots by the length of it inside
		// the crown, as each stem's.
		struct StemPlan { glm::vec3 origin{ 0.0f }, dir = WORLD_UP, outward{ 0.0f }; float height = 0.0f; int slots = 0; };
		oc::small_vector<StemPlan, 8> stemPlans((size_t)stems);
		int baseSlots = 0;
		if (stems > 1)
		{
			const float baseBearing = fork > 0.0f ? glm::max(fork - treeCrownStart, 0.0f) * height : 0.0f;
			float total = baseBearing;
			for (int stem = 0; stem < stems; ++stem)
			{
				StemPlan& p = stemPlans[(size_t)stem];
				const float az = stemAzimuth + TWO_PI * (float)stem / (float)stems + rng.signed1() * 0.4f;
				p.outward = glm::vec3(std::cos(az), 0.0f, std::sin(az));
				p.origin = forkPoint + p.outward * (sp.trunkStemSpread * rng.range(0.6f, 1.0f));
				const float lean = glm::radians(sp.trunkStemLean * rng.range(0.8f, 1.1f));
				p.dir = glm::normalize(std::cos(lean) * forkTangent + std::sin(lean) * p.outward);
				// The first stem the tallest - on the ground (a clump's leader); a forked crown's stems all alike (one full-
				// height stem stood out of the crown).
				p.height = height * (1.0f - fork) * (stem == 0 && fork <= 0.0f ? 1.0f : rng.range(0.7f, 0.95f));
				total += p.height * (1.0f - crownStart);
			}
			float acc = baseBearing;
			baseSlots = (int)std::round((float)sp.slots * acc / glm::max(total, 1e-3f));
			for (StemPlan& p : stemPlans)
			{
				const int before = (int)std::round((float)sp.slots * acc / glm::max(total, 1e-3f));
				acc += p.height * (1.0f - crownStart);
				p.slots = (int)std::round((float)sp.slots * acc / glm::max(total, 1e-3f)) - before;
			}
		}
		else
		{
			stemPlans[0].height = height;
			stemPlans[0].slots = sp.slots;
		}

		// The limbs along one trunk path from `pathStart` (a fraction of it) to its end. With a fork the envelope and the
		// angle follow the limb's height in the WHOLE tree (one crown: the longest limbs low), not along its own stem
		// (each stem its own small crown, the stems' tops made an inverted pyramid); a forked tree's limbs take their angle
		// from world up. `outward` shortens a stem's limbs into the clump.
		auto placeSlots = [&](const BranchPath& path, int slots, float pathStart, const glm::vec3& outward)
		{
			// Tier jitter: each tier's height off its even spacing (at most half a band, the order kept) and its own reach -
			// sheets at uneven heights, some wider than others. Drawn only with a jitter: other species are unchanged.
			const int tiers = glm::min(sp.crownTiers, glm::max(slots, 1));
			oc::small_vector<float, 64> tierCentre, tierReach;
			if (tiers > 1 && sp.crownTierJitter > 0.0f)
			{
				for (int t = 0; t < tiers; ++t)
				{
					tierCentre.push_back(((float)t + 0.5f + rng.signed1() * 0.5f * sp.crownTierJitter) / (float)tiers);
					tierReach.push_back(1.0f + rng.signed1() * 0.35f * sp.crownTierJitter);
				}
			}

			for (int i = 0; i < slots; ++i)
			{
				const float jitter = rng.signed1() * 0.3f / (float)slots;
				float h = glm::clamp(((float)i + 0.5f) / (float)slots + jitter, 0.0f, 1.0f);
				float reach = 1.0f;
				if (sp.crownTiers > 1)
				{
					// Tiers: slot i goes to whorl i * tiers / slots, spread over `crownTierSpread` of that whorl's band.
					const int tier = i * tiers / slots;
					const int first = (tier * slots + tiers - 1) / tiers;
					const int count = ((tier + 1) * slots + tiers - 1) / tiers - first;
					const float local = ((float)(i - first) + 0.5f) / (float)glm::max(count, 1) - 0.5f + jitter;
					if (!tierCentre.empty())
					{
						h = glm::clamp(tierCentre[(size_t)tier] + local * sp.crownTierSpread / (float)tiers, 0.0f, 1.0f);
						reach = tierReach[(size_t)tier];
					}
					else
						h = glm::clamp(((float)tier + 0.5f + local * sp.crownTierSpread) / (float)tiers, 0.0f, 1.0f);
				}
				const BranchPath::Sample s = path.sample(pathStart + (1.0f - pathStart) * h);
				const float shapeH = fork > 0.0f
					? glm::clamp((s.pos.y / height - treeCrownStart) / glm::max(1.0f - treeCrownStart, 1e-3f), 0.0f, 1.0f) : h;
				const float pitch = glm::radians(glm::mix(sp.slotAngle.x, sp.slotAngle.y, shapeH) + rng.signed1() * sp.slotAngleVar);
				float az = (float)i * GOLDEN_ANGLE + rng.signed1() * 0.3f;
				// A forked stem fills its own SECTOR of the clump: centred on its outward direction, 1.2 x its share of the
				// circle, an even golden-ratio fill of it - the stems' limbs fan out around the tree, not over each other
				// (each stem's own full golden circle put limbs of neighbouring stems in the same directions). A clump from the
				// ground (no fork) takes 1.3 x its share, at most 0.65 of the circle: a few limbs past the side of its stem
				// into the clump's middle (exactly the share left a gap between two stems). Its sector opens to the full circle
				// over the upper half of its crown: the stems lean apart there and the limbs are small.
				if (stems > 1 && glm::dot(outward, outward) > 0.0f)
				{
					const float share = TWO_PI / (float)stems;
					const float sector = fork > 0.0f ? share * 1.2f
						: glm::mix(glm::min(share * 1.3f, TWO_PI * 0.65f), TWO_PI, glm::smoothstep(0.5f, 1.0f, h));
					const float f = (float)i * 0.618034f;
					az = std::atan2(outward.z, outward.x) + (f - std::floor(f) - 0.5f) * sector + rng.signed1() * 0.05f;
				}
				const glm::vec3 axis = fork > 0.0f ? WORLD_UP : s.tangent;
				glm::vec3 horiz(std::cos(az), 0.0f, std::sin(az));
				horiz = glm::normalize(horiz - axis * glm::dot(horiz, axis));
				TreeSlot slot;
				slot.pos = s.pos;
				slot.dir = glm::normalize(std::cos(pitch) * axis + std::sin(pitch) * horiz);
				slot.length = crownEnvelope(sp.crownShape, shapeH) * sp.crownRadius * reach / glm::max(std::sin(pitch), 0.35f);
				// Several stems: the branches into the clump's middle are half as long (the crowns share it).
				if (stems > 1 && glm::dot(outward, outward) > 0.0f)
					slot.length *= 0.75f + 0.25f * glm::dot(glm::vec3(horiz.x, 0.0f, horiz.z), outward);
				slot.radius = s.radius;
				out.slots.push_back(slot);
			}
		};
		if (baseSlots > 0)
			placeSlots(basePath, baseSlots, glm::clamp(treeCrownStart / fork, 0.0f, 0.95f), glm::vec3(0.0f));

		for (int stem = 0; stem < stems; ++stem)
		{
			const StemPlan& stemPlan = stemPlans[(size_t)stem];
			const glm::vec3 origin = stemPlan.origin, dir = stemPlan.dir, outward = stemPlan.outward;
			const float stemHeight = stemPlan.height;
			const int slots = stemPlan.slots;

			// Split: the stem grows to (1 - span) of its height, as thick there as the full taper puts it; the splits take
			// the rest.
			const bool split = sp.trunkSplitCount >= 2;
			// A fork's central leader (stem 0, below) stands near vertical: 0.85 x as long, so it does not top the crown.
			const float leaderShorten = glm::mix(0.85f, 1.0f, glm::clamp((sp.trunkLeaderLean - 0.3f) / 0.7f, 0.0f, 1.0f));
			const float mainHeight = (split ? stemHeight * (1.0f - sp.trunkSplitSpan) : stemHeight) * (fork > 0.0f && stem == 0 ? leaderShorten : 1.0f);
			const size_t firstTube = plan.tubes.size(); // this stem's trunk tube (planStubs appends after it)
			TubePlan& trunk = plan.tubes.emplace_back();
			elbows.clear();
			// With a fork the FIRST stem CONTINUES the lower trunk: from its last ring, as thick, starting along it and
			// bending to its lean over its first 30 % - the lower trunk ends open with no cut showing (stems that all
			// started inside it left its open end in view).
			const bool continuesBase = fork > 0.0f && stem == 0;
			const float r0 = continuesBase ? basePath.r1 : fork > 0.0f ? stemRadius : sp.trunkRadius;
			const float fullR1 = glm::min(sp.trunkRadius * sp.trunkTaper, r0);
			const float r1 = split ? glm::mix(r0, fullR1, 1.0f - sp.trunkSplitSpan) : fullR1;
			// It stays near the middle (30 % of the stem lean): a central leader that fills the crown's centre (leaning out
			// like the others it left a V with an empty middle).
			if (continuesBase)
				bentPath(trunk.path, basePath.points.back(), basePath.tangents.back(),
					glm::normalize(glm::mix(basePath.tangents.back(), dir, sp.trunkLeaderLean)), mainHeight, r0, r1,
					glm::max(sp.trunkShape.rings, 8), 0.3f);
			else
			{
				// A FILLING crown's stem stands STRAIGHT (a third of the shape's curve and wobble, no elbows): the shape's
				// wobble is for its snaking branches.
				TreeBranchShape stemShape = sp.trunkShape;
				if (split && sp.trunkSplitFill > 0.0f)
				{
					stemShape.curve *= 0.33f;
					stemShape.curveVar *= 0.33f;
					stemShape.wobble *= 0.33f;
					stemShape.elbows = 0.0f;
				}
				growBranch(trunk.path, origin, dir, mainHeight, r0, r1, stemShape, WORLD_UP, rng, &elbows,
					fork > 0.0f ? 1.0f : 1.6f); // a trunk from the ground packs its rings toward it
			}
			trunk.sides = sp.trunkShape.sides;
			trunk.flare = fork > 0.0f ? 0.0f : sp.trunkFlare;
			trunk.lobed = fork > 0.0f ? false : sp.trunkLobes > 0;
			trunk.sink = fork > 0.0f ? 0.0f : glm::max(sp.trunkSink, 0.0f);
			// The end closes into a point over ~6 tip radii (a leader grows on out of it): never an open cut. A split stem
			// ends OPEN: its continuing branch carries on from its last ring.
			trunk.tip = split ? 0.0f : glm::clamp(6.0f * r1 / glm::max(mainHeight, 0.1f), 0.03f, 0.3f);
			if (trunk.lobed && stem == 0)
				plan.profile.build(sp.trunkLobes, sp.trunkLobeDepth, sp.trunkLobeHeight, sp.trunkTwist, rng);
			const BranchPath path = trunk.path; // planStubs grows plan.tubes: no reference into it past here
			planStubs(plan, path, elbows, sp.trunkShape, 0, 0, rng);
			if (stem == 0 && fork <= 0.0f)
				out.bones.push_back({ -1, glm::vec3(0.0f), WORLD_UP, height });

			placeSlots(path, slots, crownStart, outward);
			// Crown Top: a whorl of level limbs at `pos` - a flat sheet on top, the point it grows from hidden among them.
			auto addTopWhorl = [&](const glm::vec3& pos, float radius)
			{
				const float az0 = rng.next01() * TWO_PI;
				for (int k = 0; k < sp.crownTopSlots; ++k)
				{
					const float az = az0 + TWO_PI * (float)k / (float)sp.crownTopSlots + rng.signed1() * 0.3f;
					const glm::vec3 horiz(std::cos(az), 0.0f, std::sin(az));
					const float pitch = glm::radians(sp.crownTopAngle + rng.signed1() * sp.slotAngleVar);
					TreeSlot slot;
					slot.pos = pos;
					slot.dir = glm::normalize(std::cos(pitch) * WORLD_UP + std::sin(pitch) * horiz);
					slot.length = sp.crownTopReach * sp.crownRadius * rng.range(0.8f, 1.1f);
					slot.radius = radius;
					out.slots.push_back(slot);
				}
			};
			if (split)
			{
				// SPLITS (Trunk Split): from the stem's (open) end, `count` branches fanning out `angle` off their parent at
				// even azimuths (jittered), pulled toward up (they follow the upward direction), splitting again `depth`
				// times, every round ROUND_LENGTH x as long; the first round's length makes all rounds together about the
				// span. The FIRST child CONTINUES its parent: from the parent's last ring exactly, as thick, with its sides,
				// turned only half the angle - the parent ends open and the bark carries on, no point beside the split (a
				// closing point next to the children read as a branch ending early). The others branch off from half a
				// parent radius inside it, thinner (parent / sqrt(count), at most 0.75 of it). Each tapers to 0.45 of its
				// start; only the final tips close into a point. Bark, the trunk's wind (bone 0). The Crown Top whorl at
				// every final tip.
				TreeBranchShape splitShape = sp.trunkShape;
				splitShape.rings = 6;
				splitShape.elbows = 0.0f;
				splitShape.stubs = 0.0f;
				const int splitSides = glm::max(sp.trunkShape.sides / 2, 6);
				const int count = sp.trunkSplitCount;
				constexpr float ROUND_LENGTH = 0.6f; // each round x this as long as the one before (0.8 left long bare final rounds)
				float rounds = 0.0f;
				for (int d = 0; d < sp.trunkSplitDepth; ++d)
					rounds += std::pow(ROUND_LENGTH, (float)d);
				// coneDir / coneHalf: the directions this branch's subtree fills (fill mode).
				struct SplitAt
				{
					glm::vec3 pos, dir; float length, radius; int depth, sides;
					glm::vec3 coneDir = WORLD_UP; float coneHalf = 0.0f;
				};
				// FILL (the 6th Split value): the stem's end is the centre of a half sphere of Crown Radius; the root cone is
				// 80 degrees about up. Each split divides its cone among its children: child half-angle x 0.62 (2) / 0.55
				// (3+), its centre offset to the parent cone's edge (parent half - child half) at even azimuths (a golden
				// angle per node, +-0.15 rad); the child grows toward the point at its cone centre x Crown Radius x
				// (rounds done / depth)^0.8 - the final tips land on the dome, every round fills its own share of it.
				// Every child starts along its parent and bends to that target (`bentPath`, the shape's wobble: snaking).
				// Cone centres stay at least 8 degrees above level.
				const bool fill = sp.trunkSplitFill > 0.0f;
				const glm::vec3 fillCentre = path.points.back();
				uint32 fillNode = 0;
				// The FINAL round curves UP: it leaves in its fan direction and bends toward up (its direction + 1 x up)
				// over its length, so its tip - and the level whorl on it - sits on top: horizontal sheets.
				auto splitFrom = [&](const SplitAt& first)
				{
					oc::small_vector<SplitAt, 32> open;
					open.push_back(first);
					while (!open.empty())
					{
						const SplitAt at = open.back();
						open.pop_back();
						const glm::vec3 side0 = anyPerpendicular(at.dir);
						const float az0 = rng.next01() * TWO_PI;
						if (fill)
						{
							const glm::vec3 coneSide0 = anyPerpendicular(at.coneDir);
							const float fillAz0 = (float)(fillNode++) * GOLDEN_ANGLE;
							const float childHalf = at.coneHalf * (count == 2 ? 0.62f : 0.55f);
							const int roundsDone = sp.trunkSplitDepth - at.depth + 1;
							const float reach = sp.crownRadius * std::pow((float)roundsDone / (float)sp.trunkSplitDepth, 0.8f);
							const bool last = at.depth <= 1;
							for (int c = 0; c < count; ++c)
							{
								// 3+ children: the first keeps the CENTRE direction (the dome's middle stays filled - all on the
								// ring left a bowl), the others evenly on the ring; 2: both on the ring.
								const bool centre = count >= 3 && c == 0;
								const int ring = count >= 3 ? count - 1 : count;
								const int slot = count >= 3 ? c - 1 : c;
								const float az = fillAz0 + TWO_PI * (float)slot / (float)ring + rng.signed1() * 0.15f;
								const glm::vec3 coneSide = glm::angleAxis(az, at.coneDir) * coneSide0;
								const float offset = centre ? 0.0f : at.coneHalf - childHalf;
								glm::vec3 cc = glm::normalize(std::cos(offset) * at.coneDir + std::sin(offset) * coneSide);
								const float minY = std::sin(glm::radians(8.0f));
								if (cc.y < minY)
									cc = glm::normalize(glm::vec3(cc.x, 0.0f, cc.z) * std::sqrt(1.0f - minY * minY) + WORLD_UP * minY);
								// The stretch weighted by how UP the cone points (cc.y^2): the middle rises, the sides stay put (a
								// uniform stretch lifted the whole canopy).
								const float stretch = 1.0f + (sp.trunkSplitFillHeight - 1.0f) * cc.y * cc.y;
								const glm::vec3 target = fillCentre + glm::vec3(cc.x, cc.y * stretch, cc.z) * reach;
								const bool continues = c == 0;
								const glm::vec3 start = continues ? at.pos : at.pos - at.dir * (at.radius * 0.5f);
								const glm::vec3 toTarget = target - start;
								const float length = glm::max(glm::length(toTarget), 0.2f);
								glm::vec3 childDir = glm::normalize(toTarget + WORLD_UP * 1e-3f);
								if (last)
								{
									const glm::vec3 curved = childDir + WORLD_UP * (0.5f * sp.trunkSplitTipCurve);
									if (glm::dot(curved, curved) > 1e-4f)
										childDir = glm::normalize(curved);
								}
								// Fill: each branch tapers to 0.7 only (the children's split already thins them, 1 / sqrt(count)): with
								// 0.45 three rounds left ~5 mm tips under ~5 cm leafy limbs - a step in thickness.
								const float cr0 = continues ? at.radius : glm::min(at.radius * 0.75f, at.radius / std::sqrt((float)count));
								const float cr1 = cr0 * 0.7f;
								TubePlan& child = plan.tubes.emplace_back();
								bentPath(child.path, start, continues ? at.dir : glm::normalize(at.dir + childDir), childDir, length, cr0, cr1,
									9, 0.5f, glm::radians(splitShape.wobble), &rng);
								child.sides = continues ? at.sides : splitSides;
								child.tip = last ? glm::clamp(6.0f * cr1 / length, 0.03f, 0.3f) : 0.0f;
								const BranchPath childPath = child.path; // the next emplace_back may move `child`
								const BranchPath::Sample end = childPath.sample(1.0f - child.tip);
								// SHOOTS (the 8th Split value): short side branches along an inner branch (35..85 % of it, spread
								// evenly, alternate sides), out and up, 0.35 x its length, half its thickness there, each ending in a
								// leafy whorl - leaves inside the crown, not only on the dome.
								// Not on the first round (the innermost branches: too many leaves deep inside).
								if (!last && sp.trunkSplitShoots > 0 && at.depth < sp.trunkSplitDepth)
									for (int k = 0; k < sp.trunkSplitShoots; ++k)
									{
										const float u = glm::mix(0.35f, 0.85f, ((float)k + 0.5f) / (float)sp.trunkSplitShoots);
										const BranchPath::Sample s = childPath.sample(u);
										const glm::vec3 side = glm::angleAxis((float)k * GOLDEN_ANGLE + rng.signed1() * 0.4f, s.tangent)
											* anyPerpendicular(s.tangent);
										const glm::vec3 shootDir = glm::normalize(side + s.tangent * 0.6f + WORLD_UP * 0.4f);
										const float shootLength = length * 0.35f * rng.range(0.8f, 1.1f);
										const float sr0 = s.radius * 0.5f;
										TubePlan& shoot = plan.tubes.emplace_back();
										bentPath(shoot.path, s.pos - shootDir * (s.radius * 0.5f), shootDir, shootDir, shootLength, sr0, sr0 * 0.45f,
											5, 0.5f, glm::radians(splitShape.wobble), &rng);
										shoot.sides = splitSides;
										shoot.tip = glm::clamp(6.0f * sr0 * 0.45f / shootLength, 0.03f, 0.3f);
										const BranchPath shootPath = shoot.path;
										if (sp.crownTopSlots > 0)
										{
											const BranchPath::Sample tipAt = shootPath.sample(1.0f - shoot.tip);
											addTopWhorl(tipAt.pos, tipAt.radius);
										}
									}
								if (!last)
								{
									SplitAt next{ end.pos, end.tangent, 0.0f, end.radius, at.depth - 1, continues ? at.sides : splitSides };
									next.coneDir = cc;
									next.coneHalf = childHalf;
									open.push_back(next);
								}
								else if (sp.crownTopSlots > 0)
									addTopWhorl(end.pos, end.radius);
							}
							continue;
						}
						for (int c = 0; c < count; ++c)
						{
							const float az = az0 + TWO_PI * (float)c / (float)count + rng.signed1() * 0.3f;
							const glm::vec3 side = glm::angleAxis(az, at.dir) * side0;
							const bool continues = c == 0;
							const float angle = glm::radians(sp.trunkSplitAngle * rng.range(0.7f, 1.2f)) * (continues ? 0.5f : 1.0f);
							const glm::vec3 childDir = glm::normalize(glm::normalize(std::cos(angle) * at.dir + std::sin(angle) * side) + WORLD_UP * 0.3f);
							const float length = at.length * rng.range(0.8f, 1.0f);
							const float cr0 = continues ? at.radius : glm::min(at.radius * 0.75f, at.radius / std::sqrt((float)count));
							const float cr1 = cr0 * 0.45f;
							const bool last = at.depth <= 1;
							glm::vec3 lastDir = childDir + WORLD_UP * sp.trunkSplitTipCurve;
							lastDir = glm::dot(lastDir, lastDir) > 1e-4f ? glm::normalize(lastDir) : childDir;
							TubePlan& child = plan.tubes.emplace_back();
							elbows.clear();
							if (continues)
								bentPath(child.path, at.pos, at.dir, last ? lastDir : childDir, length, cr0, cr1, 9, last ? 0.8f : 0.35f,
									glm::radians(splitShape.wobble), &rng);
							else if (last)
								bentPath(child.path, at.pos - at.dir * (at.radius * 0.5f), childDir, lastDir, length, cr0, cr1, 9, 0.8f,
									glm::radians(splitShape.wobble), &rng);
							else
								growBranch(child.path, at.pos - at.dir * (at.radius * 0.5f), childDir, length, cr0, cr1,
									splitShape, WORLD_UP, rng, &elbows);
							child.sides = continues ? at.sides : splitSides;
							child.tip = last ? glm::clamp(6.0f * cr1 / glm::max(length, 0.1f), 0.03f, 0.3f) : 0.0f;
							const BranchPath childPath = child.path; // the next emplace_back may move `child`
							const BranchPath::Sample end = childPath.sample(1.0f - child.tip);
							if (!last)
								open.push_back({ end.pos, end.tangent, at.length * ROUND_LENGTH, end.radius, at.depth - 1,
									continues ? at.sides : splitSides });
							else if (sp.crownTopSlots > 0)
								addTopWhorl(end.pos, end.radius);
						}
					}
				};
				{
					SplitAt root{ path.points.back(), path.tangents.back(), stemHeight * sp.trunkSplitSpan / rounds, path.r1,
						sp.trunkSplitDepth, sp.trunkShape.sides };
					root.coneHalf = glm::radians(80.0f);
					splitFrom(root);
				}

				// SPLIT LIMBS (Crown SplitLimbs): side limbs that split the same way - from the crown's start to near the
				// stem's end, evenly up it (jittered), at azimuths in the stem's sector (or a golden spiral), `angle` from
				// up (+-8), `length` x the stem height long, 0.6 x the stem there thick (from half a stem radius inside it,
				// tapering to 0.6), ending OPEN into one round fewer of splits than the top (at least one), the first
				// round half the limb's length.
				const int splitLimbs = sp.crownSplitLimbs;
				for (int k = 0; k < splitLimbs; ++k)
				{
					const float u = glm::clamp(glm::mix(crownStart, 0.9f, ((float)k + 0.5f + rng.signed1() * 0.3f) / (float)splitLimbs), 0.0f, 0.95f);
					const BranchPath::Sample s = path.sample(u);
					float az = (float)k * GOLDEN_ANGLE + rng.signed1() * 0.3f;
					if (stems > 1 && glm::dot(outward, outward) > 0.0f)
					{
						const float f = (float)k * 0.618034f;
						az = std::atan2(outward.z, outward.x) + (f - std::floor(f) - 0.5f) * TWO_PI / (float)stems * 1.2f;
					}
					const glm::vec3 horiz(std::cos(az), 0.0f, std::sin(az));
					const float pitch = glm::radians(sp.crownSplitLimbAngle + rng.signed1() * 8.0f);
					const glm::vec3 limbDir = glm::normalize(std::cos(pitch) * WORLD_UP + std::sin(pitch) * horiz);
					// Their length follows the crown SHAPE by their height in the whole tree (Cone: long low, short high).
					const float shapeH = glm::clamp((s.pos.y / height - treeCrownStart) / glm::max(1.0f - treeCrownStart, 1e-3f), 0.0f, 1.0f);
					const float length = stemHeight * sp.crownSplitLimbLength * crownEnvelope(sp.crownShape, shapeH) * rng.range(0.8f, 1.1f);
					const float lr0 = s.radius * 0.6f, lr1 = lr0 * 0.6f;
					TubePlan& limb = plan.tubes.emplace_back();
					elbows.clear();
					growBranch(limb.path, s.pos - limbDir * (s.radius * 0.5f), limbDir, length, lr0, lr1, splitShape, WORLD_UP, rng, &elbows);
					limb.sides = splitSides;
					const BranchPath limbPath = limb.path;
					splitFrom({ limbPath.points.back(), limbPath.tangents.back(), length * 0.5f, lr1,
						glm::max(sp.trunkSplitDepth - 1, 1), splitSides });
				}
			}
			else if (sp.leader)
			{
				// From where the trunk's closing tip starts: the leader grows out of the narrowing point, the two taper
				// together (from the very end it stood on an open cut).
				const float tipT = 1.0f - plan.tubes[(size_t)firstTube].tip;
				TreeSlot slot;
				slot.pos = path.sample(tipT).pos;
				slot.dir = path.sample(tipT).tangent;
				slot.length = glm::max(0.3f * sp.crownRadius, 0.15f * stemHeight);
				slot.radius = path.r1;
				out.slots.push_back(slot);
			}
			if (!split && sp.crownTopSlots > 0)
			{
				// Crown Top: the stem ENDS in a whorl from where its tip closes.
				const BranchPath::Sample s = path.sample(1.0f - plan.tubes[(size_t)firstTube].tip);
				addTopWhorl(s.pos, s.radius);
			}
		}
		meshPiece(sp, plan, out, true);
	}

	// Module-local frame: +Y = the root branch direction, +Z = the side that faces world-up after the
	// composite. `up` is world-up in this frame for a module attached at `pitch` (radians from up) - what the
	// up-attraction, the droop and the leaf orientation bend toward.
	void generateModule(const TreeSpeciesDesc& sp, uint32 seed, float nominalLength, float pitch, TreePiece& out)
	{
		Rng rng(seed);
		out.length = nominalLength;
		out.pitch = pitch;
		const glm::vec3 up = glm::normalize(glm::vec3(0.0f, std::cos(pitch), std::sin(pitch)));

		struct Branch { BranchPath path; uint8 bone; };
		// One entry per tier: [0] = the module root, [i] = sub-branch level i (kept for the leaf pass).
		oc::vector<oc::vector<Branch>> tiers;
		oc::vector<Branch> current;
		oc::vector<Branch> next;

		Branch root;
		root.bone = 0;
		const float r0 = sp.moduleRadius * nominalLength;
		out.baseRadius = r0;
		PiecePlan plan;
		plan.maxLevel = (int)sp.levels.size();
		oc::vector<Elbow> elbows;
		growBranch(root.path, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), nominalLength, r0, r0 * 0.15f,
			sp.moduleShape, up, rng, &elbows);
		plan.tubes.push_back({ .path = root.path, .sides = sp.moduleShape.sides, .bone = 0, .level = 0, .tip = sp.moduleShape.tip,
			.section = sp.moduleShape.section, .channel = sp.moduleShape.channel, .swell = sp.moduleShape.swell });
		planStubs(plan, root.path, elbows, sp.moduleShape, 0, 0, rng);
		out.bones.push_back({ -1, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), nominalLength });
		current.push_back(oc::move(root));

		for (size_t levelIdx = 0; levelIdx < sp.levels.size(); ++levelIdx)
		{
			const TreeBranchLevel& level = sp.levels[levelIdx];
			next.clear();
			for (const Branch& parent : current)
			{
				for (int i = 0; i < level.count; ++i)
				{
					const float start = glm::clamp(level.start, 0.0f, 0.95f);
					const float jitter = rng.signed1() * 0.4f / (float)level.count;
					const float t = glm::clamp(start + (1.0f - start) * (((float)i + 0.5f) / (float)level.count + jitter), 0.0f, 0.98f);
					const BranchPath::Sample s = parent.path.sample(t);
					const float az = (float)i * GOLDEN_ANGLE + rng.signed1() * 0.5f;
					glm::vec3 perp = glm::angleAxis(az, s.tangent) * anyPerpendicular(s.tangent);
					// Flat: the side toward the nearer horizontal side of the parent - the children fan out in a sheet.
					const glm::vec3 side = glm::cross(s.tangent, up);
					if (level.flat > 0.0f && glm::dot(side, side) > 1e-6f)
					{
						const glm::vec3 flatSide = glm::normalize(side) * (glm::dot(perp, side) < 0.0f ? -1.0f : 1.0f);
						perp = glm::normalize(glm::mix(perp, flatSide, level.flat));
					}
					const float angle = glm::radians(level.angle + rng.signed1() * level.angleVar);
					glm::vec3 dir = glm::normalize(std::cos(angle) * s.tangent + std::sin(angle) * perp);
					if (level.rise > 0.0f)
					{
						const glm::vec3 risen = glm::mix(dir, up, level.rise);
						if (glm::dot(risen, risen) > 1e-6f)
							dir = glm::normalize(risen);
					}
					float length = parent.path.length * level.length * (1.0f - level.lengthTaper * t);
					if (level.lengthVar > 0.0f)
						length *= 1.0f - level.lengthVar * rng.next01();
					const float cr0 = glm::min(s.radius * level.radius, s.radius * 0.9f);

					Branch child;
					// Level-1 branches get their own bone (wind sways each sub-branch); deeper ones ride it.
					if (levelIdx == 0 && out.bones.size() < 255)
					{
						child.bone = (uint8)out.bones.size();
						out.bones.push_back({ (int16)parent.bone, s.pos, dir, length });
					}
					else
						child.bone = parent.bone;
					elbows.clear();
					growBranch(child.path, s.pos, dir, length, cr0, cr0 * 0.15f, level.shape, up, rng, &elbows);
					const int levelNumber = (int)levelIdx + 1;
					plan.tubes.push_back({ .path = child.path, .sides = level.shape.sides, .bone = child.bone, .level = levelNumber,
						.tip = level.shape.tip, .section = level.shape.section, .channel = level.shape.channel, .swell = level.shape.swell });
					planStubs(plan, child.path, elbows, level.shape, child.bone, levelNumber, rng);
					next.push_back(oc::move(child));
				}
			}
			tiers.push_back(oc::move(current));
			current = oc::move(next);
			next = {};
		}
		tiers.push_back(oc::move(current));

		// Leaves along the last `leafLevels` tiers. PerBranch counts for an average last-tier branch; other
		// branches scale it by their length, so the density per metre is the same on every carrying tier.
		float refLength = 0.0f;
		for (const Branch& branch : tiers.back())
			refLength += branch.path.length;
		refLength = glm::max(refLength / (float)glm::max<size_t>(tiers.back().size(), 1), 1e-3f);
		const bool pads = sp.padCount > 0 && sp.leafType == ETreeLeafType::Cluster;
		const size_t firstLeafTier = pads ? tiers.size() : tiers.size() - glm::min((size_t)sp.leafLevels, tiers.size());
		const float leafLength = sp.leafSize;
		const float leafWidth = sp.leafSize * sp.leafAspect;
		if (pads)
		{
			// FOLIAGE PADS: big cards through a flat dome over the limb, not on the twigs - centred at a random point of
			// the limb's outer 85 %, out to either side over a disc (area-uniform, narrowing toward the limb's tip), at a
			// height that is highest over the limb and falls off to the edge (1 - r^2); each card level +- a small tilt,
			// turned at random about up, centred on its point.
			const BranchPath& limb = tiers[0][0].path;
			const float halfWidth = sp.padWidth * nominalLength, domeHeight = sp.padHeight * nominalLength;
			for (int k = 0; k < sp.padCount; ++k)
			{
				// From padStart to the limb's end, denser toward the end (sqrt): the inner part lies in the tiers' shade.
				const float u = glm::mix(sp.padStart, 1.0f, std::sqrt(rng.next01()));
				const BranchPath::Sample s = limb.sample(u);
				glm::vec3 side = glm::cross(s.tangent, up);
				side = glm::dot(side, side) > 1e-6f ? glm::normalize(side) : anyPerpendicular(s.tangent);
				const glm::vec3 forward = glm::normalize(glm::cross(up, side));
				const float r = std::sqrt(rng.next01()), a = rng.next01() * TWO_PI;
				const float reach = halfWidth * glm::mix(1.0f, 0.75f, u);
				const glm::vec3 centre = s.pos + side * (std::cos(a) * r * reach) + forward * (std::sin(a) * r * reach * 0.4f)
					+ up * (s.radius + domeHeight * (1.0f - r * r) * rng.range(0.6f, 1.0f));
				// The card's base edge (the texture's twig, v = 0) toward the limb: pointing out from it, +- 40 degrees.
				glm::vec3 away = centre - s.pos;
				away -= up * glm::dot(away, up);
				const float turn = glm::radians(rng.signed1() * 40.0f);
				const glm::vec3 along = glm::angleAxis(turn, up) * (glm::dot(away, away) > 1e-4f ? glm::normalize(away) : forward);
				const glm::vec3 tilt(rng.signed1(), rng.signed1(), rng.signed1());
				glm::vec3 normal = up + tilt * sp.padTilt;
				normal -= along * glm::dot(normal, along);
				normal = glm::normalize(normal);
				const float size = rng.range(0.8f, 1.2f);
				const uint32 cell = rng.next() & 3u;
				const glm::vec3 stem = centre - along * (0.5f * leafLength * size);
				plan.leaves.push_back({ stem, along, normal, leafLength * size, leafWidth * size, cell, 0 });
				// Its TWIG: a thin arched tube from the limb to the card's base edge, where the texture's twig starts (as thick
				// as it: 0.011 of the card), so every card hangs on wood. Level 1: the coarse LODs drop it with the twigs.
				const float twigRadius = 0.011f * leafLength * size;
				const glm::vec3 from = s.pos + up * (s.radius * 0.5f);
				if (glm::distance(from, stem) < 0.05f)
					continue;
				TubePlan& twig = plan.tubes.emplace_back();
				twig.path.points = { from, glm::mix(from, stem, 0.5f) + up * (0.12f * glm::distance(from, stem)), stem };
				twig.path.length = glm::distance(from, stem);
				twig.path.r0 = glm::max(twigRadius * 1.6f, 1e-3f);
				twig.path.r1 = glm::max(twigRadius, 5e-4f);
				twig.path.finish();
				twig.sides = 3;
				twig.bone = 0;
				twig.level = 1;
			}
		}
		for (size_t tier = firstLeafTier; tier < tiers.size(); ++tier)
		for (const Branch& branch : tiers[tier])
		{
			const int numLeaves = glm::min((int)std::round((float)sp.leavesPerBranch * branch.path.length / refLength), 1024);
			for (int k = 0; k < numLeaves; ++k)
			{
				const float t = glm::clamp(0.15f + 0.85f * ((float)k + 0.5f) / (float)numLeaves + rng.signed1() * 0.05f, 0.0f, 1.0f);
				const BranchPath::Sample s = branch.path.sample(t);
				glm::vec3 perp = glm::angleAxis(rng.next01() * TWO_PI, s.tangent) * anyPerpendicular(s.tangent);
				// Top: the card's side direction blended into the branch's HORIZONTAL side (the nearer one), so it lies flat
				// in the branch's sheet facing up (the normal below leans to `up`), its stem lifted onto the branch's top.
				glm::vec3 stemLift(0.0f);
				const glm::vec3 flatSide = glm::cross(s.tangent, up);
				if (sp.leafTop > 0.0f && glm::dot(flatSide, flatSide) > 1e-6f)
				{
					const glm::vec3 side = glm::normalize(flatSide) * (glm::dot(perp, flatSide) < 0.0f ? -1.0f : 1.0f);
					perp = glm::normalize(glm::mix(perp, side, sp.leafTop));
					const glm::vec3 top = up - s.tangent * glm::dot(up, s.tangent);
					if (glm::dot(top, top) > 1e-6f)
						stemLift = glm::normalize(top) * ((s.radius + 0.1f * leafLength) * sp.leafTop);
				}
				// Align 1: the card lies along the branch, facing out from it (a weeping strand's curtain).
				glm::vec3 along = glm::normalize(glm::mix(glm::normalize(perp * 0.8f + s.tangent * 0.6f), s.tangent, sp.leafAlign));
				// Top: the card held LEVEL (its slope along a drooping twig taken out), so it never dips under the branch.
				if (sp.leafTop > 0.0f)
				{
					const glm::vec3 level = along - up * (glm::dot(along, up) * sp.leafTop);
					if (glm::dot(level, level) > 1e-6f)
						along = glm::normalize(level);
				}
				const glm::vec3 randomDir = glm::normalize(glm::vec3(rng.signed1(), rng.signed1(), rng.signed1()) + glm::vec3(0.0f, 0.0f, 1e-3f));
				glm::vec3 normal = glm::mix(up * 0.8f + perp * 0.4f + randomDir * 0.3f, perp + randomDir * 0.2f, sp.leafAlign);
				if (sp.leafFaceUp > 0.0f)
				{
					normal = glm::mix(normal, up + randomDir * 0.15f, sp.leafFaceUp);
					// Lying on the branch: the card raised onto its TOP (its radius + 5 % of a card), not through the wood.
					const glm::vec3 top = up - s.tangent * glm::dot(up, s.tangent);
					if (glm::dot(top, top) > 1e-6f)
						stemLift += glm::normalize(top) * ((s.radius * 1.15f + 0.05f * leafLength) * sp.leafFaceUp);
				}
				normal -= along * glm::dot(normal, along);
				normal = glm::dot(normal, normal) > 1e-6f ? glm::normalize(normal) : anyPerpendicular(along);
				const float size = rng.range(0.8f, 1.2f);
				if (sp.leafType == ETreeLeafType::Single)
				{
					plan.leaves.push_back({ s.pos + perp * s.radius, along, normal, leafLength * size, leafWidth * size, 0u, branch.bone });
					continue;
				}
				// Cards start at the branch centre: the twig in the texture grows out of the wood.
				const uint32 cell = rng.next() & 3u;
				// Lift: above the branches along the module's up, a dome on top - the module frame's +Y is the limb, +X the
				// sheet's side (perpendicular to the limb and to `up`), so |x| is how far out to the side.
				if (sp.leafLift.x > 0.0f || sp.leafLift.y > 0.0f)
				{
					const float u = glm::clamp(s.pos.y / nominalLength, 0.0f, 1.0f);
					const float side = glm::abs(s.pos.x) / (0.45f * nominalLength);
					const float dome = glm::max(1.0f - side * side, 0.0f) * (1.0f - glm::smoothstep(0.55f, 1.0f, u));
					stemLift += up * (sp.leafLift.x * leafLength + sp.leafLift.y * nominalLength * dome);
				}
				// Top: never under the LIMB (the module root - it curves up past its own sub-branches): the card at least
				// the limb's height there + its radius + 0.1 card above, the limb taken at the card's distance along it.
				if (sp.leafTop > 0.0f)
				{
					const BranchPath& limb = tiers[0][0].path;
					const glm::vec3 p = s.pos + stemLift;
					const BranchPath::Sample at = limb.sample(glm::clamp(p.y / nominalLength, 0.0f, 1.0f));
					const float need = glm::dot(at.pos - p, up) + at.radius + 0.1f * leafLength;
					if (need > 0.0f)
						stemLift += up * need;
				}
				plan.leaves.push_back({ s.pos + stemLift, along, normal, leafLength * size, leafWidth * size, cell, branch.bone });
			}
		}
		meshPiece(sp, plan, out, false);
	}
}

namespace Procedural
{
	void generateTreeLibrary(const TreeSpeciesDesc& species, TreeLibrary& out)
	{
		out.trunks.clear();
		out.modules.clear();
		out.trunks.resize((size_t)species.trunkCount);
		for (int i = 0; i < species.trunkCount; ++i)
			generateTrunk(species, treeHash(species.seed, 1000u + (uint32)i), out.trunks[i]);
		const float nominalLength = species.moduleLength > 0.0f ? species.moduleLength : species.crownRadius;
		// A drooping species grows each module for its own pitch, spread over the slot angles (compositeTree places
		// it at that pitch exactly, so its down is the world's); the others all for the mean pitch.
		const float lo = glm::radians(glm::clamp(glm::min(species.slotAngle.x, species.slotAngle.y) - species.slotAngleVar, 2.0f, 175.0f));
		const float hi = glm::radians(glm::clamp(glm::max(species.slotAngle.x, species.slotAngle.y) + species.slotAngleVar, 2.0f, 175.0f));
		const float meanPitch = glm::radians((species.slotAngle.x + species.slotAngle.y) * 0.5f);
		const bool hangs = speciesHangs(species);
		out.modules.resize((size_t)species.moduleCount);
		for (int i = 0; i < species.moduleCount; ++i)
		{
			const float pitch = hangs ? glm::mix(lo, hi, ((float)i + 0.5f) / (float)species.moduleCount) : meanPitch;
			generateModule(species, treeHash(species.seed, 2000u + (uint32)i), nominalLength, pitch, out.modules[i]);
		}
	}

	void bakeTreeVariant(const TreeSpeciesDesc& species, const TreeLibrary& library, uint32 seed, TreePiece& out)
	{
		out = TreePiece{};
		oc::vector<TreePiecePlacement> placements;
		float treeScale = 1.0f;
		compositeTree(species, library, seed, placements, treeScale);

		// `windPhase`: the module placement's wind phase, added to every module vertex's payload (two placements of one
		// module sway apart).
		auto append = [](TreeMesh& dst, const TreeMesh& src, const Transform& t, uint32 windPhase)
		{
			const uint32 base = dst.numVertices();
			for (uint32 v = 0; v < src.numVertices(); ++v)
			{
				dst.positions.push_back(t.transformPoint(src.positions[v]));
				dst.normals.push_back(t.quat * src.normals[v]); // uniform scale: directions only rotate
				dst.tangents.push_back(t.quat * src.tangents[v]);
				dst.bitangents.push_back(t.quat * src.bitangents[v]);
				glm::vec3 uv = src.texCoords[v];
				if (windPhase != 0u && uv.z > 1.0f && uv.z < 2.0f)
					uv.z = encodeWind((uint32)std::round((uv.z - 1.0f) * TREE_WIND_PAYLOAD_SCALE) | windPhase);
				dst.texCoords.push_back(uv);
				dst.bones.push_back(0);
			}
			for (uint32 index : src.indices)
				dst.indices.push_back(base + index);
		};
		for (uint32 p = 0; p < (uint32)placements.size(); ++p)
		{
			TreePiecePlacement& placement = placements[p];
			const TreePiece& piece = placement.trunk ? library.trunks[placement.pieceIdx] : library.modules[placement.pieceIdx];
			placement.windPhase = placement.trunk ? (uint8)0 : (uint8)(treeHash(seed, 7000u + p) & 7u);
			const uint32 windPhase = (uint32)placement.windPhase << TREE_WIND_MODULE_PHASE_SHIFT;
			for (uint32 lod = 0; lod < TREE_PIECE_LODS; ++lod)
			{
				append(out.bark[lod], piece.bark[lod], placement.local, windPhase);
				append(placement.trunk ? out.trunkBark[lod] : out.branchBark[lod], piece.bark[lod], placement.local, windPhase);
				append(out.leaves[lod], piece.leaves[lod], placement.local, windPhase);
				out.lodError[lod] = glm::max(out.lodError[lod], piece.lodError[lod] * placement.local.scale);
			}
			if (placement.trunk)
			{
				out.length = piece.length;
				out.baseRadius = piece.baseRadius;
			}
		}
		out.bones.push_back({ -1, glm::vec3(0.0f), WORLD_UP, out.length });
		out.placements = oc::move(placements);
	}

	void compositeTree(const TreeSpeciesDesc& species, const TreeLibrary& library, uint32 seed,
		oc::vector<TreePiecePlacement>& out, float& outTreeScale)
	{
		out.clear();
		outTreeScale = 1.0f;
		if (library.trunks.empty())
			return;

		const uint32 trunkIdx = treeHash(seed, 0u) % (uint32)library.trunks.size();
		outTreeScale = glm::mix(species.scale.x, species.scale.y, treeHash01(treeHash(seed, 1u)));
		const glm::quat trunkRot = glm::angleAxis(treeHash01(treeHash(seed, 2u)) * TWO_PI, WORLD_UP);
		out.push_back({ (uint16)trunkIdx, true, Transform(glm::vec3(0.0f), 1.0f, trunkRot) });
		if (library.modules.empty())
			return;

		const TreePiece& trunk = library.trunks[trunkIdx];
		const float maxRoll = glm::radians(25.0f);
		const bool hangs = speciesHangs(species);
		for (uint32 s = 0; s < (uint32)trunk.slots.size(); ++s)
		{
			const TreeSlot& slot = trunk.slots[s];
			const uint32 hs = treeHash(seed, 16u + s);
			if (treeHash01(hs) > species.slotFill)
				continue;
			uint32 moduleIdx = treeHash(hs, 1u) % (uint32)library.modules.size();
			const float roll = (treeHash01(treeHash(hs, 2u)) * 2.0f - 1.0f) * maxRoll;

			// Module +Y -> slot direction, module +Z -> the up-facing side, then the roll jitter about +Y.
			glm::vec3 dirY = trunkRot * slot.dir;
			glm::vec3 dirZ = WORLD_UP - dirY * glm::dot(WORLD_UP, dirY);
			dirZ = glm::dot(dirZ, dirZ) > 1e-6f ? glm::normalize(dirZ) : anyPerpendicular(dirY);
			if (hangs)
			{
				// Drooping: the module grown for the nearest pitch, placed AT that pitch with no roll - its up is then
				// exactly world-up, and its strands hang plumb.
				const float slotPitch = std::acos(glm::clamp(dirY.y, -1.0f, 1.0f));
				for (uint32 m = 0; m < (uint32)library.modules.size(); ++m)
					if (glm::abs(library.modules[m].pitch - slotPitch) < glm::abs(library.modules[moduleIdx].pitch - slotPitch))
						moduleIdx = m;
				const float pitch = library.modules[moduleIdx].pitch;
				glm::vec3 horiz(dirY.x, 0.0f, dirY.z);
				horiz = glm::dot(horiz, horiz) > 1e-8f ? glm::normalize(horiz) : glm::vec3(1.0f, 0.0f, 0.0f);
				dirY = std::cos(pitch) * WORLD_UP + std::sin(pitch) * horiz;
				dirZ = std::sin(pitch) * WORLD_UP - std::cos(pitch) * horiz;
			}
			else
				dirZ = glm::angleAxis(roll, dirY) * dirZ;
			const glm::vec3 dirX = glm::cross(dirY, dirZ);
			const glm::quat rot = glm::quat_cast(glm::mat3(dirX, dirY, dirZ));

			const TreePiece& module = library.modules[moduleIdx];
			// Pipe model: a branch is never thicker than BranchRadius x the trunk where it attaches. The scale
			// is uniform, so the cap shortens the module too - thin trunk tops carry short branches.
			const float maxScale = species.branchRadius * slot.radius / glm::max(module.baseRadius, 1e-4f);
			const float scale = glm::min(slot.length / module.length * glm::mix(0.9f, 1.1f, treeHash01(treeHash(hs, 3u))), maxScale);
			out.push_back({ (uint16)moduleIdx, false, Transform(trunkRot * slot.pos, scale, rot) });
		}
	}
}
