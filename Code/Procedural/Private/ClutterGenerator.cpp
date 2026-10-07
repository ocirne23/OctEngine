module Procedural;

import Core;
import Core.glm;
import RendererVK;

import :ClutterGenerator;
import :ClutterType;
import :RockType;
import :RockGenerator;
import :TreeGenerator; // treeHash

namespace
{
	using namespace Procedural;
	using RendererVKLayout::ClutterVertexGpu;
	using RendererVKLayout::CLUTTER_LODS;

	constexpr float PI = 3.14159265f;

	// A seeded stream of [0, 1) values.
	struct Random
	{
		uint32 seed;
		uint32 next = 0;
		float operator()() { return treeHash01(treeHash(seed, next++)); }
		float range(float lo, float hi) { return lo + (hi - lo) * (*this)(); }
	};

	struct Builder
	{
		oc::vector<ClutterVertexGpu>& vertices;
		oc::vector<uint32>& indices;

		uint32 vertex(const glm::vec3& p, const glm::vec3& n, float ao, float part, const glm::vec4& uv = glm::vec4(0.0f))
		{
			vertices.push_back(ClutterVertexGpu{ glm::vec4(p, ao), glm::vec4(glm::normalize(n), part), uv });
			return (uint32)vertices.size() - 1;
		}
		void quad(uint32 a, uint32 b, uint32 c, uint32 d) { indices.insert(indices.end(), { a, b, c, a, c, d }); }
	};

	// ---- PEBBLE: the rock SDF generator's chain ----

	void pebbleMeshes(const ClutterTypeDesc& type, uint32 gridResolution, oc::vector<Renderer::ClutterMesh>& out)
	{
		for (int v = 0; v < type.variantCount; ++v)
		{
			RockVariant rock;
			// A type with thin features asks for more cells (`Resolution`, as a .rock's).
			const uint32 resolution = (uint32)glm::clamp((float)gridResolution * type.rock.resolution, 8.0f, 128.0f);
			generateRockVariant(type.rock, treeHash(type.seed + 7000u, (uint32)v), resolution, rock);
			Renderer::ClutterMesh& mesh = out.emplace_back();
			for (uint32 lod = 0; lod < glm::min(rock.lodCount, CLUTTER_LODS); ++lod)
			{
				const RockMesh& src = rock.lods[lod];
				for (uint32 i = 0; i < src.numVertices(); ++i)
					mesh.vertices[lod].push_back(ClutterVertexGpu{ glm::vec4(src.positions[i], glm::clamp(src.texCoords[i].x, 0.0f, 1.0f)),
						glm::vec4(src.normals[i], 0.0f), glm::vec4(0.0f) });
				mesh.indices[lod] = src.indices;
			}
		}
	}

	// ---- BRANCH: a bent, tapering tube along X, lying on the ground ----

	struct Tube
	{
		glm::vec3 start;
		glm::vec3 dir;      // unit
		glm::vec3 side;     // unit, horizontal: the bend's direction
		float length;
		float radius0;      // at the start
		float radius1;      // at the end
		float bend;         // the end's offset along `side` (m)
		float rise;         // the end's lift (m)
	};

	glm::vec3 tubePoint(const Tube& t, float s)
	{
		return t.start + t.dir * (t.length * s) + t.side * (t.bend * std::sin(PI * s) * 0.5f + t.bend * s * 0.5f) + glm::vec3(0.0f, t.rise * s, 0.0f);
	}

	// Rings of `sides` + 1 vertices along the tube (bark, part 0: the last column repeats the first with the full
	// circumference, so the bark's "around" coordinate never wraps inside a segment), the ends capped with end grain
	// (part 1) when asked. The bark coordinates: x along the tube (m from its start, + `along0`), y around it (m).
	void addTube(Builder& b, const Tube& t, int rings, int sides, bool capStart, bool capEnd, float along0 = 0.0f)
	{
		const uint32 first = (uint32)b.vertices.size();
		const uint32 columns = (uint32)sides + 1;
		const float meanRadius = 0.5f * (t.radius0 + t.radius1);
		for (int r = 0; r <= rings; ++r)
		{
			const float s = (float)r / (float)rings;
			const glm::vec3 p = tubePoint(t, s);
			const glm::vec3 tangent = glm::normalize(tubePoint(t, glm::min(s + 0.01f, 1.0f)) - tubePoint(t, glm::max(s - 0.01f, 0.0f)));
			glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f) - tangent * tangent.y;
			up = glm::dot(up, up) > 1e-6f ? glm::normalize(up) : glm::vec3(0.0f, 0.0f, 1.0f);
			const glm::vec3 across = glm::cross(tangent, up);
			const float radius = glm::mix(t.radius0, t.radius1, s);
			for (uint32 k = 0; k < columns; ++k)
			{
				const float a = 2.0f * PI * (float)k / (float)sides;
				const glm::vec3 n = up * std::cos(a) + across * std::sin(a);
				// The underside, against the ground, is darker.
				b.vertex(p + n * radius, n, glm::mix(0.45f, 1.0f, n.y * 0.5f + 0.5f), 0.0f,
					glm::vec4(along0 + s * t.length, a * meanRadius, 0.0f, 0.0f));
			}
		}
		for (int r = 0; r < rings; ++r)
			for (uint32 k = 0; k < (uint32)sides; ++k)
			{
				const uint32 a0 = first + (uint32)r * columns + k, a1 = a0 + 1;
				b.quad(a0, a0 + columns, a1 + columns, a1);
			}
		// The end grain: uv.zw = the point across the cut (m from the axis), for its growth rings.
		const auto cap = [&](float s, bool atStart) {
			const glm::vec3 p = tubePoint(t, s);
			const glm::vec3 tangent = glm::normalize(tubePoint(t, glm::min(s + 0.01f, 1.0f)) - tubePoint(t, glm::max(s - 0.01f, 0.0f)));
			const glm::vec3 n = atStart ? -tangent : tangent;
			const glm::vec3 side = glm::normalize(glm::cross(n, glm::abs(n.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)));
			const glm::vec3 side2 = glm::cross(n, side);
			const uint32 ring = first + (uint32)(atStart ? 0 : rings) * columns;
			const uint32 centre = b.vertex(p, n, 0.9f, 1.0f);
			const uint32 rim = (uint32)b.vertices.size();
			for (int k = 0; k < sides; ++k)
			{
				const glm::vec3 v = glm::vec3(b.vertices[ring + (uint32)k].posAo);
				b.vertex(v, n, 0.8f, 1.0f, glm::vec4(0.0f, 0.0f, glm::dot(v - p, side), glm::dot(v - p, side2)));
			}
			for (int k = 0; k < sides; ++k)
				b.indices.insert(b.indices.end(), { centre, rim + (uint32)k, rim + (uint32)((k + 1) % sides) });
		};
		if (capStart)
			cap(0.0f, true);
		if (capEnd)
			cap(1.0f, false);
	}

	void branchMeshes(const ClutterTypeDesc& type, oc::vector<Renderer::ClutterMesh>& out)
	{
		static constexpr int SIDES[CLUTTER_LODS] = { 7, 5, 3 };
		static constexpr int RINGS[CLUTTER_LODS] = { 10, 5, 3 };
		for (int v = 0; v < type.variantCount; ++v)
		{
			Random rng{ treeHash(type.seed + 8000u, (uint32)v) };
			// The main stick: along X, thick at its start, bent sideways (in the ground's plane) and a little up.
			Tube main;
			main.length = 1.0f;
			main.radius0 = type.thickness * rng.range(0.8f, 1.2f);
			main.radius1 = main.radius0 * rng.range(0.35f, 0.6f);
			main.dir = glm::vec3(1.0f, 0.0f, 0.0f);
			main.side = glm::vec3(0.0f, 0.0f, rng() < 0.5f ? -1.0f : 1.0f);
			main.bend = std::tan(glm::radians(type.bend)) * rng.range(0.3f, 1.0f) * 0.5f;
			main.rise = rng.range(0.0f, 0.04f);
			main.start = glm::vec3(-0.5f, main.radius0, 0.0f);
			oc::vector<Tube> twigs;
			for (int k = 0; k < type.twigs; ++k)
			{
				const float s = rng.range(0.2f, 0.85f);
				const float sideSign = (k & 1) != 0 ? 1.0f : -1.0f;
				const float angle = glm::radians(rng.range(25.0f, 60.0f));
				Tube twig;
				const float parentRadius = glm::mix(main.radius0, main.radius1, s);
				twig.start = tubePoint(main, s) - glm::vec3(0.0f, 0.2f * parentRadius, 0.0f);
				twig.dir = glm::normalize(glm::vec3(std::cos(angle), rng.range(0.0f, 0.25f), sideSign * std::sin(angle)));
				twig.side = glm::normalize(glm::cross(twig.dir, glm::vec3(0.0f, 1.0f, 0.0f)));
				twig.length = rng.range(0.15f, 0.4f) * (1.0f - 0.5f * s);
				twig.radius0 = parentRadius * rng.range(0.45f, 0.65f);
				twig.radius1 = twig.radius0 * 0.3f;
				twig.bend = rng.range(-0.05f, 0.05f);
				twig.rise = -twig.start.y * 0.5f; // it droops to the ground
				twigs.push_back(twig);
			}
			Renderer::ClutterMesh& mesh = out.emplace_back();
			for (uint32 lod = 0; lod < CLUTTER_LODS; ++lod)
			{
				Builder b{ mesh.vertices[lod], mesh.indices[lod] };
				addTube(b, main, RINGS[lod], SIDES[lod], true, lod < 2);
				if (lod < 2)
					for (size_t k = 0; k < twigs.size(); ++k) // each twig its own stretch of the bark pattern
						addTube(b, twigs[k], glm::max(RINGS[lod] / 3, 1), glm::max(SIDES[lod] - 2, 3), false, false, 3.0f + 2.0f * (float)k);
			}
		}
	}

	// ---- MUSHROOM: a lathed stem + cap ----

	// One profile point: radius, height, and the outward normal in the (r, y) plane.
	struct ProfilePoint { float r; float y; glm::vec2 n; };

	// The normals of a profile walked in order: perpendicular to the walk, (dy, -dr) - outward and up for a walk from the
	// rim to the centre over the top, outward for the stem walked up, down for the gills walked from the rim inward.
	void profileNormals(oc::vector<ProfilePoint>& points)
	{
		for (size_t i = 0; i < points.size(); ++i)
		{
			const ProfilePoint& a = points[i > 0 ? i - 1 : i];
			const ProfilePoint& c = points[i + 1 < points.size() ? i + 1 : i];
			const glm::vec2 d(c.r - a.r, c.y - a.y);
			const glm::vec2 n(d.y, -d.x);
			points[i].n = glm::dot(n, n) > 1e-12f ? glm::normalize(n) : glm::vec2(0.0f, 1.0f);
		}
	}

	// The lathe has a wrap-around index (k + 1) % segments: its uv is (cos, sin), so the wrap needs no seam column.
	void addLathe(Builder& b, const oc::vector<ProfilePoint>& profile, int segments, const glm::vec3& origin, const glm::mat3& turn,
		float aoBottom, float aoTop, float part)
	{
		const uint32 first = (uint32)b.vertices.size();
		for (size_t i = 0; i < profile.size(); ++i)
		{
			const ProfilePoint& p = profile[i];
			const float ao = glm::mix(aoBottom, aoTop, profile.size() > 1 ? (float)i / (float)(profile.size() - 1) : 1.0f);
			for (int k = 0; k < segments; ++k)
			{
				const float a = 2.0f * PI * (float)k / (float)segments;
				const glm::vec3 dir(std::cos(a), 0.0f, std::sin(a));
				// uv: (cos, sin) of the angle around (the FS takes its atan: no seam), up the profile.
				b.vertex(origin + turn * (dir * p.r + glm::vec3(0.0f, p.y, 0.0f)), turn * (dir * p.n.x + glm::vec3(0.0f, p.n.y, 0.0f)), ao, part,
					glm::vec4(dir.x, dir.z, profile.size() > 1 ? (float)i / (float)(profile.size() - 1) : 0.0f, 0.0f));
			}
		}
		for (size_t i = 0; i + 1 < profile.size(); ++i)
			for (int k = 0; k < segments; ++k)
			{
				const uint32 a0 = first + (uint32)(i * segments + k), a1 = first + (uint32)(i * segments + (k + 1) % segments);
				b.quad(a0, a1, a1 + (uint32)segments, a0 + (uint32)segments);
			}
	}

	void addMushroom(Builder& b, const ClutterTypeDesc& type, float height, const glm::vec3& origin, const glm::mat3& turn, int segments, int capPoints)
	{
		const float R = type.capSize.x * height;
		const float H = type.capSize.y * height;
		const float stemTop = height - (type.cap == EMushroomCap::Funnel ? 0.6f * H : 0.75f * H);
		const float rs = type.stemRadius * height;

		// The stem: a little wider at its foot.
		oc::vector<ProfilePoint> stem = { { rs * 1.2f, -0.02f * height, {} }, { rs * 1.05f, 0.5f * stemTop, {} }, { rs * 0.9f, stemTop, {} } };
		profileNormals(stem);
		addLathe(b, stem, segments, origin, turn, 0.55f, 0.85f, 0.0f);

		// The cap's top, from the rim to the centre.
		oc::vector<ProfilePoint> top;
		const float y0 = stemTop - 0.1f * H; // the rim's height
		for (int i = 0; i <= capPoints; ++i)
		{
			const float s = (float)i / (float)capPoints; // 0 at the rim, 1 at the centre
			float r = 0.0f, y = 0.0f;
			switch (type.cap)
			{
			case EMushroomCap::Dome:
				r = R * std::cos(s * 0.5f * PI);
				y = y0 + H * std::sin(s * 0.5f * PI);
				break;
			case EMushroomCap::Flat:
				r = R * (1.0f - s);
				y = y0 + H * (0.35f * std::sin(glm::min(s * 3.0f, 1.0f) * 0.5f * PI) + 0.65f * s * s);
				break;
			case EMushroomCap::Cone:
				r = R * (1.0f - s);
				y = y0 + H * std::pow(s, 0.7f);
				break;
			case EMushroomCap::Funnel:
				r = R * (1.0f - s);
				y = y0 + H * (1.0f - 0.7f * s * s); // the rim high, the centre sunk into a funnel
				break;
			}
			top.push_back({ r, y, {} });
		}
		// The rim curls down a little.
		top.insert(top.begin(), ProfilePoint{ R * 0.97f, y0 - 0.12f * H, {} });
		profileNormals(top);
		addLathe(b, top, segments, origin, turn, 0.8f, 1.0f, 1.0f);

		// The gills: from the rim in to the stem, facing down (a funnel's run down the stem).
		const float gillY = type.cap == EMushroomCap::Funnel ? stemTop - 0.5f * H : stemTop;
		oc::vector<ProfilePoint> gills = { { R * 0.97f, y0 - 0.12f * H, {} }, { glm::mix(R, rs, 0.5f), glm::mix(y0 - 0.1f * H, gillY, 0.6f), {} }, { rs * 0.95f, gillY, {} } };
		profileNormals(gills);
		addLathe(b, gills, segments, origin, turn, 0.6f, 0.4f, 2.0f);
	}

	void mushroomMeshes(const ClutterTypeDesc& type, oc::vector<Renderer::ClutterMesh>& out)
	{
		static constexpr int SEGMENTS[CLUTTER_LODS] = { 12, 7, 5 };
		static constexpr int CAP_POINTS[CLUTTER_LODS] = { 5, 3, 2 };
		for (int v = 0; v < type.variantCount; ++v)
		{
			Random rng{ treeHash(type.seed + 9000u, (uint32)v) };
			struct Member { float height; glm::vec3 origin; glm::mat3 turn; };
			oc::vector<Member> members;
			const int count = 1 + (int)(rng() * (float)type.group);
			for (int i = 0; i < glm::min(count, type.group); ++i)
			{
				Member m;
				m.height = i == 0 ? 1.0f : rng.range(0.4f, 0.85f);
				const float a = rng.range(0.0f, 2.0f * PI);
				const float d = i == 0 ? 0.0f : rng.range(0.9f, 1.6f) * type.capSize.x;
				m.origin = glm::vec3(std::cos(a) * d, 0.0f, std::sin(a) * d);
				// A little lean, away from the group's centre.
				const float lean = glm::radians(i == 0 ? rng.range(0.0f, 6.0f) : rng.range(5.0f, 18.0f));
				const glm::vec3 axis = glm::normalize(glm::vec3(std::sin(a), 0.0f, -std::cos(a)));
				m.turn = glm::mat3(glm::angleAxis(lean, axis));
				members.push_back(m);
			}
			Renderer::ClutterMesh& mesh = out.emplace_back();
			for (uint32 lod = 0; lod < CLUTTER_LODS; ++lod)
			{
				Builder b{ mesh.vertices[lod], mesh.indices[lod] };
				for (const Member& m : members)
					addMushroom(b, type, m.height, m.origin, m.turn, SEGMENTS[lod], CAP_POINTS[lod]);
			}
		}
	}
}

namespace Procedural
{
	void generateClutterMeshes(const ClutterTypeDesc& type, uint32 gridResolution, oc::vector<Renderer::ClutterMesh>& out)
	{
		switch (type.kind)
		{
		case EClutterKind::Pebble: pebbleMeshes(type, gridResolution, out); break;
		case EClutterKind::Branch: branchMeshes(type, out); break;
		case EClutterKind::Mushroom: mushroomMeshes(type, out); break;
		case EClutterKind::Flower: break;
		}
	}
}
