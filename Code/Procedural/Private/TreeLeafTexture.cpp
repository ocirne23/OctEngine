module Procedural;

import Core;
import Core.glm;

import :TreeSpecies;
import :TreeGenerator;
import :TreeLeafTexture;

namespace
{
	using namespace Procedural;

	constexpr float PI = 3.14159265f;

	struct Rng
	{
		uint32 state;
		explicit Rng(uint32 seed) : state(treeHash(seed)) {}
		uint32 next() { state = treeHash(state); return state; }
		float next01() { return treeHash01(next()); }
		float range(float lo, float hi) { return lo + (hi - lo) * next01(); }
		float signed1() { return next01() * 2.0f - 1.0f; }
	};

	// Float RGBA canvas: rgb holds the 8-bit sRGB-encoded colour as 0..1 (the species colours are authored
	// that way, like the solid-colour materials), a is coverage.
	struct Canvas
	{
		uint32 size = 0;
		oc::vector<glm::vec4> px;

		void blend(int x, int y, const glm::vec3& color, float a)
		{
			glm::vec4& d = px[(size_t)y * size + (size_t)x];
			d = glm::vec4(glm::mix(glm::vec3(d), color, a), a + d.w * (1.0f - a));
		}
	};

	// One cell of the atlas, in cell-local [0,1]^2 (y = v, the growth direction).
	struct Cell
	{
		Canvas& canvas;
		glm::vec2 origin; // pixels
		float cellPx;     // pixels per cell unit
		// The LEAF NORMAL MAP (tangent space: x along u, y along v, encoded * 0.5 + 0.5; nullptr = none), blended with
		// the same coverage as the colour.
		Canvas* normals = nullptr;

		void blendNormal(int x, int y, glm::vec3 n, float a) const
		{
			if (normals)
				normals->blend(x, y, glm::normalize(n) * 0.5f + 0.5f, a);
		}

		// Visits every pixel of the cell-local box [lo, hi] (clipped to the cell) with its cell-local centre.
		template<typename F>
		void forBox(glm::vec2 lo, glm::vec2 hi, F&& f) const
		{
			const int x0 = glm::max((int)std::floor(origin.x + lo.x * cellPx), (int)origin.x);
			const int y0 = glm::max((int)std::floor(origin.y + lo.y * cellPx), (int)origin.y);
			const int x1 = glm::min((int)std::ceil(origin.x + hi.x * cellPx), (int)(origin.x + cellPx) - 1);
			const int y1 = glm::min((int)std::ceil(origin.y + hi.y * cellPx), (int)(origin.y + cellPx) - 1);
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
					f(x, y, (glm::vec2((float)x, (float)y) + 0.5f - origin) / cellPx);
		}
	};

	// A tapered capsule (twig segment), anti-aliased over one pixel.
	void drawCapsule(const Cell& cell, glm::vec2 a, glm::vec2 b, float ra, float rb, const glm::vec3& color)
	{
		const float r = glm::max(ra, rb);
		const glm::vec2 ab = b - a;
		const float len2 = glm::max(glm::dot(ab, ab), 1e-8f);
		const float px = 1.0f / cell.cellPx;
		cell.forBox(glm::min(a, b) - r - px, glm::max(a, b) + r + px, [&](int x, int y, glm::vec2 p)
		{
			const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
			const float radius = glm::mix(ra, rb, t);
			const glm::vec2 off = p - (a + ab * t);
			const float edge = radius - glm::length(off);
			const float alpha = glm::clamp(edge / px + 0.5f, 0.0f, 1.0f);
			if (alpha > 0.0f)
			{
				cell.canvas.blend(x, y, color, alpha);
				// Round across: the normal leans out toward the side the pixel sits on.
				const glm::vec2 lean = off / glm::max(radius, 1e-5f) * 0.85f;
				cell.blendNormal(x, y, glm::vec3(lean, std::sqrt(glm::max(1.0f - glm::dot(lean, lean), 0.05f))), alpha);
			}
		});
	}

	// One leaf: origin at the petiole, `dir` along the blade. Pointed ellipse-like profile, lighter midrib,
	// darker rim, one half slightly shaded (the fold). Its NORMAL (with a normal canvas): `cup` folds the sides up about
	// the midrib (the normal leans in toward it, more toward the rim) and curls the tip down (leaning toward the tip),
	// plus `tilt` - the whole leaf's own random lean, so neighbouring leaves catch the sun differently.
	void drawLeaf(const Cell& cell, glm::vec2 origin, glm::vec2 dir, float length, float halfWidth, const glm::vec3& color,
		float cup = 0.0f, glm::vec2 tilt = glm::vec2(0.0f))
	{
		const glm::vec2 side(-dir.y, dir.x);
		const float px = 1.0f / cell.cellPx;
		const glm::vec2 tip = origin + dir * length;
		const glm::vec2 lo = glm::min(origin, tip) - halfWidth - px;
		const glm::vec2 hi = glm::max(origin, tip) + halfWidth + px;
		cell.forBox(lo, hi, [&](int x, int y, glm::vec2 p)
		{
			const glm::vec2 q = p - origin;
			const float s = glm::dot(q, dir) / length;
			if (s <= 0.0f || s >= 1.0f)
				return;
			const float r = glm::dot(q, side);
			const float h = halfWidth * std::sin(PI * std::pow(s, 0.75f));
			const float edge = h - glm::abs(r);
			const float alpha = glm::clamp(edge / px + 0.5f, 0.0f, 1.0f);
			if (alpha <= 0.0f)
				return;
			float shade = 0.9f + 0.2f * s;
			if (glm::abs(r) < glm::max(0.07f * h, px * 0.7f))
				shade *= 1.18f;               // midrib
			else if (edge < 2.0f * px)
				shade *= 0.82f;               // rim
			if (r > 0.0f)
				shade *= 0.92f;               // fold
			cell.canvas.blend(x, y, glm::clamp(color * shade, 0.0f, 1.0f), alpha);
			if (cell.normals)
			{
				const float across = glm::clamp(r / glm::max(h, 1e-5f), -1.0f, 1.0f);
				const float fold = cup * (0.6f * across + 0.25f * (across >= 0.0f ? 1.0f : -1.0f));
				const glm::vec2 lean = side * -fold + dir * (cup * 0.5f * (s - 0.35f)) + tilt;
				cell.blendNormal(x, y, glm::vec3(lean, 1.0f), alpha);
			}
		});
	}

	glm::vec2 bezier(glm::vec2 a, glm::vec2 c, glm::vec2 b, float t)
	{
		const float u = 1.0f - t;
		return a * (u * u) + c * (2.0f * u * t) + b * (t * t);
	}

	void drawCluster(const Cell& cell, const TreeSpeciesDesc& species, Rng& rng)
	{
		const glm::vec2 base(0.5f, 0.02f);
		// The twig ends where its tip leaf (the longest it can be: 1.15 x 0.75 x the size, below) still fits the cell.
		const float tipY = glm::clamp(0.97f - species.clusterLeafSize * 0.75f * 1.15f - 0.01f, 0.5f, 0.88f);
		const glm::vec2 tip(0.5f + rng.signed1() * 0.12f, tipY);
		const glm::vec2 ctrl(0.5f + rng.signed1() * 0.15f, 0.45f);
		constexpr int TWIG_SEGMENTS = 12;
		for (int i = 0; i < TWIG_SEGMENTS; ++i)
		{
			const float t0 = (float)i / TWIG_SEGMENTS, t1 = (float)(i + 1) / TWIG_SEGMENTS;
			drawCapsule(cell, bezier(base, ctrl, tip, t0), bezier(base, ctrl, tip, t1),
				glm::mix(0.012f, 0.004f, t0), glm::mix(0.012f, 0.004f, t1), species.leafTwigColor());
		}

		const int numLeaves = glm::max(species.clusterLeaves, 1);
		const glm::vec3 autumn = species.leafColor * glm::vec3(1.25f, 1.1f, 0.6f);
		for (int i = 0; i < numLeaves; ++i)
		{
			const bool last = i == numLeaves - 1;
			const float t = last ? 1.0f : 0.15f + 0.8f * ((float)i + 0.5f) / (float)(numLeaves - 1);
			const glm::vec2 at = bezier(base, ctrl, tip, glm::min(t, 1.0f));
			const glm::vec2 tangent = glm::normalize(bezier(base, ctrl, tip, glm::min(t + 0.01f, 1.0f)) - bezier(base, ctrl, tip, glm::max(t - 0.01f, 0.0f)));
			const float sideSign = (i & 1) ? 1.0f : -1.0f;
			const float angle = last ? rng.signed1() * 0.15f : sideSign * glm::radians(rng.range(35.0f, 65.0f));
			const glm::vec2 dir(tangent.x * std::cos(angle) - tangent.y * std::sin(angle), tangent.x * std::sin(angle) + tangent.y * std::cos(angle));
			float length = species.clusterLeafSize * rng.range(0.75f, 1.15f) * (1.0f - 0.25f * t);
			// `Color LeafVariation` widens the autumn mix and the brightness; `Color Accent` replaces AccentShare of them.
			const float variation = species.leafVariation;
			glm::vec3 color = glm::mix(species.leafColor, autumn, glm::min(rng.next01() * 0.35f * variation, 1.0f))
				* rng.range(glm::max(1.0f - 0.15f * variation, 0.3f), 1.0f + 0.12f * variation);
			if (rng.next01() < species.accentShare)
				color = species.accentColor * rng.range(0.85f, 1.1f);
			const glm::vec2 tilt(rng.signed1() * 0.35f, rng.signed1() * 0.35f);
			// Inside the cell: the cell clips the drawing, and a large leaf (the tip leaf above all, which grows on from
			// v = 0.88) lost its tip. Shrunk until its tip and its widest points (at about 0.6 of its length,
			// drawLeaf's profile) fit, with a margin for the mip chain.
			const glm::vec2 origin = at + dir * 0.01f;
			const glm::vec2 side(-dir.y, dir.x);
			const float widthRatio = species.clusterLeafWidth;
			auto fits = [&](float len)
			{
				constexpr float MARGIN = 0.02f;
				const glm::vec2 mid = origin + dir * (0.6f * len);
				for (const glm::vec2 p : { origin + dir * len, mid + side * (widthRatio * len), mid - side * (widthRatio * len) })
					if (p.x < MARGIN || p.x > 1.0f - MARGIN || p.y < MARGIN || p.y > 1.0f - MARGIN)
						return false;
				return true;
			};
			for (int k = 0; k < 16 && !fits(length); ++k)
				length *= 0.92f;
			drawLeaf(cell, origin, dir, length, length * widthRatio, glm::clamp(color, 0.0f, 1.0f), species.leafCup, tilt);
		}
	}

	// `Style Needles`: one conifer SHOOT along the quadratic Bezier base -> ctrl -> tip - a thin twig densely set with
	// needles on both sides (alternating, slightly curved toward the shoot tip, shorter and steeper toward it) and a
	// tuft at the tip. `needleLength` in cell units; every needle is shortened until it fits inside the cell.
	void drawNeedleShoot(const Cell& cell, glm::vec2 base, glm::vec2 ctrl, glm::vec2 tip, float twigRadius, float needleLength,
		const TreeSpeciesDesc& species, Rng& rng)
	{
		constexpr int TWIG_SEGMENTS = 10;
		float shootLength = 0.0f;
		for (int i = 0; i < TWIG_SEGMENTS; ++i)
		{
			const float t0 = (float)i / TWIG_SEGMENTS, t1 = (float)(i + 1) / TWIG_SEGMENTS;
			const glm::vec2 a = bezier(base, ctrl, tip, t0), b = bezier(base, ctrl, tip, t1);
			drawCapsule(cell, a, b, glm::mix(twigRadius, twigRadius * 0.4f, t0), glm::mix(twigRadius, twigRadius * 0.4f, t1), species.leafTwigColor());
			shootLength += glm::distance(a, b);
		}

		const float px = 1.0f / cell.cellPx;
		const float width = glm::max(needleLength * 0.035f, px * 1.2f); // half width at the needle's base
		const float spacing = width * 2.0f;
		const int rows = glm::max((int)(shootLength / spacing), 4);
		auto needle = [&](glm::vec2 at, glm::vec2 tangent, float angle, float length, float shade)
		{
			auto rotate = [](glm::vec2 v, float a) { return glm::vec2(v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)); };
			const glm::vec2 dir = rotate(tangent, angle);
			// The outer half bends back toward the shoot direction (needles curve forward).
			const glm::vec2 dirOuter = rotate(tangent, angle * 0.75f);
			auto inside = [](glm::vec2 p) { return p.x >= 0.015f && p.x <= 0.985f && p.y >= 0.015f && p.y <= 0.985f; };
			for (int k = 0; k < 12 && !inside(at + dir * (length * 0.55f) + dirOuter * (length * 0.45f)); ++k)
				length *= 0.85f;
			const glm::vec2 mid = at + dir * (length * 0.55f);
			const glm::vec2 end = mid + dirOuter * (length * 0.45f);
			const glm::vec3 color = glm::clamp(species.leafColor * shade, 0.0f, 1.0f);
			drawCapsule(cell, at, mid, width, width * 0.75f, color);
			drawCapsule(cell, mid, end, width * 0.75f, width * 0.25f, color * 1.08f);
		};
		for (int i = 0; i < rows; ++i)
		{
			const float t = glm::clamp(((float)i + 0.5f + rng.signed1() * 0.3f) / (float)rows, 0.0f, 1.0f);
			const glm::vec2 at = bezier(base, ctrl, tip, t);
			const glm::vec2 tangent = glm::normalize(bezier(base, ctrl, tip, glm::min(t + 0.01f, 1.0f)) - bezier(base, ctrl, tip, glm::max(t - 0.01f, 0.0f)));
			// Toward the tip the needles get shorter and point more forward.
			const float lengthScale = glm::mix(0.9f, 0.6f, t * t);
			const float steep = glm::mix(1.0f, 0.55f, t * t * t);
			for (const float side : { -1.0f, 1.0f })
			{
				const float angle = side * glm::radians(rng.range(40.0f, 72.0f)) * steep;
				// The far side reads darker (it sits behind the twig), the rest varies per needle.
				const float shade = rng.range(0.78f, 1.12f) * (side > 0.0f ? 0.88f : 1.0f);
				needle(at, tangent, angle, needleLength * lengthScale * rng.range(0.85f, 1.1f), shade);
			}
		}
		// The tip tuft: a few short needles fanning forward.
		const glm::vec2 tipDir = glm::normalize(tip - bezier(base, ctrl, tip, 0.97f));
		for (int k = 0; k < 5; ++k)
			needle(tip, tipDir, glm::radians(-28.0f + 14.0f * (float)k + rng.signed1() * 5.0f), needleLength * 0.55f * rng.range(0.85f, 1.1f),
				rng.range(0.9f, 1.15f));
	}

	// The cell: `Shoots` needle BRANCHES fanned out from the stem at v = 0 (the middle one straight along v, the others
	// spread over up to +-18 degrees and a little shorter), each a main shoot with `ClusterLeaves` side shoots (short,
	// 24..38 degrees off it - wider read as a flat fan, not a spray). One card
	// then reads as a whole spray, so the tree needs fewer branches and cards for the same fullness.
	void drawNeedleCluster(const Cell& cell, const TreeSpeciesDesc& species, Rng& rng)
	{
		const float needleLength = species.clusterLeafSize;
		const glm::vec2 base(0.5f, 0.02f);
		const float reach = needleLength * 0.6f + 0.02f; // the needles around a shoot tip, inside the cell
		auto fits = [&](glm::vec2 p) { return p.x >= reach && p.x <= 1.0f - reach && p.y >= reach && p.y <= 1.0f - reach; };
		auto rotate = [](glm::vec2 v, float a) { return glm::vec2(v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)); };

		const int numBranches = glm::max(species.clusterShoots, 1);
		// Outer branches first, the middle one last (on top).
		oc::small_vector<int, 8> drawOrder;
		for (int b = 0; b < numBranches; ++b)
			drawOrder.push_back(b);
		oc::sort(drawOrder.begin(), drawOrder.end(), [&](int a, int b)
			{ return glm::abs((float)a - 0.5f * (float)(numBranches - 1)) > glm::abs((float)b - 0.5f * (float)(numBranches - 1)); });
		for (const int b : drawOrder)
		{
			const float spread = numBranches > 1 ? ((float)b / (float)(numBranches - 1)) * 2.0f - 1.0f : 0.0f; // -1..1
			const glm::vec2 dir = rotate(glm::vec2(0.0f, 1.0f), glm::radians(spread * 18.0f + rng.signed1() * 4.0f));
			float length = (0.95f - 0.2f * glm::abs(spread)) * rng.range(0.9f, 1.0f);
			for (int k = 0; k < 16 && !fits(base + dir * length); ++k)
				length *= 0.9f;
			const glm::vec2 tip = base + dir * length;
			const glm::vec2 ctrl = base + dir * (length * 0.5f) + glm::vec2(-dir.y, dir.x) * (length * rng.signed1() * 0.06f);

			// The side shoots first: the branch's main shoot is drawn over their bases.
			const int numShoots = glm::max(species.clusterLeaves, 0);
			for (int i = 0; i < numShoots; ++i)
			{
				const float t = 0.18f + 0.6f * ((float)i + 0.5f) / (float)numShoots + rng.signed1() * 0.04f;
				const glm::vec2 at = bezier(base, ctrl, tip, t);
				const glm::vec2 tangent = glm::normalize(bezier(base, ctrl, tip, t + 0.01f) - bezier(base, ctrl, tip, t - 0.01f));
				const glm::vec2 sideDir = rotate(tangent, ((i & 1) ? 1.0f : -1.0f) * glm::radians(rng.range(24.0f, 38.0f)));
				// Shorter higher up and on the outer branches; the shoot and its needles inside the cell.
				float sideLength = rng.range(0.22f, 0.34f) * (1.0f - 0.45f * t) * length;
				for (int k = 0; k < 12 && !fits(at + sideDir * sideLength); ++k)
					sideLength *= 0.85f;
				const glm::vec2 end = at + sideDir * sideLength;
				// Bent a little toward the branch's direction (side shoots sweep forward).
				const glm::vec2 sideCtrl = at + sideDir * (sideLength * 0.5f) + tangent * (sideLength * 0.12f);
				drawNeedleShoot(cell, at, sideCtrl, end, 0.005f, needleLength * 0.85f, species, rng);
			}
			drawNeedleShoot(cell, base, ctrl, tip, numBranches > 1 ? 0.008f : 0.011f, needleLength, species, rng);
		}
	}

	// `Style Mat`: a lobed PATCH of foliage on a twig SKELETON - a main twig in from the cell's base edge (v = 0) to
	// beyond its middle, 4-6 side twigs off it (alternating, 35-80 degrees, curving), and `ClusterLeaves` short needle
	// shoots along all of them (the twig picked by its length, 30-75 degrees off it, both sides), the twigs drawn over the
	// shoots and a third of the shoots again over the twigs (half hidden in the foliage). Its outline follows the twigs:
	// uneven lobes, not a disc. No single spray outline either: a big card reads as a mass of foliage (`Leaves Pad`), where
	// one spray per card read as one big leaf.
	void drawNeedleMat(const Cell& cell, const TreeSpeciesDesc& species, Rng& rng)
	{
		const float needleLength = species.clusterLeafSize;
		const float reach = needleLength * 0.6f + 0.02f;
		auto fits = [&](glm::vec2 p) { return p.x >= reach && p.x <= 1.0f - reach && p.y >= reach && p.y <= 1.0f - reach; };
		auto rotate = [](glm::vec2 v, float a) { return glm::vec2(v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)); };

		struct Twig { glm::vec2 base, ctrl, tip; float radius, length; };
		oc::small_vector<Twig, 8> twigs;
		const glm::vec2 mainBase(0.5f + rng.signed1() * 0.08f, 0.03f);
		const glm::vec2 mainTip(0.5f + rng.signed1() * 0.15f, rng.range(0.62f, 0.8f));
		const glm::vec2 mainCtrl = (mainBase + mainTip) * 0.5f + glm::vec2(rng.signed1() * 0.1f, 0.0f);
		twigs.push_back({ mainBase, mainCtrl, mainTip, 0.011f, glm::distance(mainBase, mainTip) });
		const int sideTwigs = 4 + (int)(rng.next() % 3u);
		for (int i = 0; i < sideTwigs; ++i)
		{
			const float t = 0.3f + 0.65f * ((float)i + 0.5f) / (float)sideTwigs + rng.signed1() * 0.04f;
			const glm::vec2 at = bezier(mainBase, mainCtrl, mainTip, t);
			const glm::vec2 tangent = glm::normalize(bezier(mainBase, mainCtrl, mainTip, t + 0.01f) - bezier(mainBase, mainCtrl, mainTip, t - 0.01f));
			const glm::vec2 dir = rotate(tangent, ((i & 1) ? 1.0f : -1.0f) * glm::radians(rng.range(35.0f, 80.0f)));
			float length = rng.range(0.18f, 0.36f) * (1.1f - 0.4f * t);
			for (int k = 0; k < 12 && !fits(at + dir * length); ++k)
				length *= 0.85f;
			const glm::vec2 tip = at + dir * length;
			const glm::vec2 ctrl = at + dir * (length * 0.5f) + glm::vec2(-dir.y, dir.x) * (length * rng.signed1() * 0.2f);
			twigs.push_back({ at, ctrl, tip, 0.006f, length });
		}

		float total = 0.0f;
		for (const Twig& twig : twigs)
			total += twig.length;
		const int shoots = glm::max(species.clusterLeaves, 1);
		auto drawShoot = [&]()
		{
			float pick = rng.next01() * total;
			const Twig* twig = &twigs.back();
			for (const Twig& candidate : twigs)
				if ((pick -= candidate.length) < 0.0f) { twig = &candidate; break; }
			const float t = rng.range(0.15f, 1.0f);
			const glm::vec2 at = bezier(twig->base, twig->ctrl, twig->tip, t);
			const glm::vec2 tangent = glm::normalize(bezier(twig->base, twig->ctrl, twig->tip, glm::min(t + 0.01f, 1.0f))
				- bezier(twig->base, twig->ctrl, twig->tip, glm::max(t - 0.01f, 0.0f)));
			const glm::vec2 dir = rotate(tangent, (rng.next01() < 0.5f ? -1.0f : 1.0f) * glm::radians(rng.range(30.0f, 75.0f)));
			float length = rng.range(0.1f, 0.2f);
			for (int k = 0; k < 12 && !fits(at + dir * length); ++k)
				length *= 0.85f;
			const glm::vec2 tip = at + dir * length;
			const glm::vec2 ctrl = at + dir * (length * 0.5f) + tangent * (length * 0.1f);
			drawNeedleShoot(cell, at, ctrl, tip, 0.004f, needleLength, species, rng);
		};
		for (int i = 0; i < shoots; ++i)
			drawShoot();
		constexpr int TWIG_SEGMENTS = 10;
		for (const Twig& twig : twigs)
			for (int s = 0; s < TWIG_SEGMENTS; ++s)
			{
				const float t0 = (float)s / TWIG_SEGMENTS, t1 = (float)(s + 1) / TWIG_SEGMENTS;
				drawCapsule(cell, bezier(twig.base, twig.ctrl, twig.tip, t0), bezier(twig.base, twig.ctrl, twig.tip, t1),
					glm::mix(twig.radius, twig.radius * 0.4f, t0), glm::mix(twig.radius, twig.radius * 0.4f, t1), species.leafTwigColor());
			}
		for (int i = 0; i < shoots / 3; ++i)
			drawShoot();
	}

	// `Style Pinnate`: one BIPINNATE compound leaf (acacia) along base -> ctrl -> tip - a thin greenish rachis with
	// `pairs` pairs of long pinnae (40-58 degrees forward, shorter toward the tip), each a dense comb of narrow
	// leaflets of `leafletLength` (cell units) on both sides. Every pinna fits inside the cell.
	// A cell's CHARACTER (drawPinnateCluster draws one per cell, so the 4 atlas variants differ in more than jitter).
	struct PinnateVariant
	{
		float pinnaAngle = 40.0f;   // degrees forward off the rachis, the lower end (+ up to 18)
		float pinnaScale = 1.0f;    // x the pinna length
		float stagger = 0.0f;       // the right-hand pinnae sit this much (rachis t) further out: alternate, not opposite
		float tint = 1.0f;          // x the leaf colour (younger lighter, older darker)
	};

	void drawPinnateLeaf(const Cell& cell, glm::vec2 base, glm::vec2 ctrl, glm::vec2 tip, int pairs, float leafletLength,
		const PinnateVariant& variant, const TreeSpeciesDesc& species, Rng& rng)
	{
		auto rotate = [](glm::vec2 v, float a) { return glm::vec2(v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)); };
		const float px = 1.0f / cell.cellPx;
		const glm::vec3 stemColor = glm::mix(species.leafTwigColor(), species.leafColor, 0.45f);
		constexpr int RACHIS_SEGMENTS = 10;
		float rachisLength = 0.0f;
		for (int i = 0; i < RACHIS_SEGMENTS; ++i)
		{
			const float t0 = (float)i / RACHIS_SEGMENTS, t1 = (float)(i + 1) / RACHIS_SEGMENTS;
			const glm::vec2 a = bezier(base, ctrl, tip, t0), b = bezier(base, ctrl, tip, t1);
			drawCapsule(cell, a, b, glm::max(glm::mix(0.0045f, 0.0018f, t0), px * 0.6f), glm::max(glm::mix(0.0045f, 0.0018f, t1), px * 0.6f), stemColor);
			rachisLength += glm::distance(a, b);
		}

		// The pinnae are COMBS (the reference photo): long, angled forward off the rachis, each densely packed on both
		// sides with narrow oblong leaflets standing nearly at right angles to it, side by side with no gap.
		const float leafletWidth = leafletLength * 0.2f; // half width
		const float leafletSpacing = glm::max(leafletWidth * 2.1f, px * 2.0f);
		const float reach = leafletLength + 0.015f;
		auto inside = [&](glm::vec2 p) { return p.x >= reach && p.x <= 1.0f - reach && p.y >= reach && p.y <= 1.0f - reach; };
		const float pinnaShade = rng.range(0.92f, 1.06f) * variant.tint;
		for (int k = 0; k < pairs; ++k)
		{
			for (const float side : { -1.0f, 1.0f })
			{
				const float t = glm::min(0.05f + 0.9f * ((float)k + 0.3f) / (float)glm::max(pairs, 1) + (side > 0.0f ? variant.stagger : 0.0f), 0.97f);
				const glm::vec2 at = bezier(base, ctrl, tip, t);
				const glm::vec2 tangent = glm::normalize(bezier(base, ctrl, tip, glm::min(t + 0.01f, 1.0f)) - bezier(base, ctrl, tip, glm::max(t - 0.01f, 0.0f)));
				const glm::vec2 dir = rotate(tangent, side * glm::radians(variant.pinnaAngle + rng.range(0.0f, 18.0f)));
				float pinnaLength = rachisLength * 0.55f * variant.pinnaScale * (1.0f - 0.35f * t) * rng.range(0.9f, 1.05f);
				for (int n = 0; n < 12 && !inside(at + dir * pinnaLength); ++n)
					pinnaLength *= 0.88f;
				if (pinnaLength < leafletSpacing * 3.0f)
					continue;
				// A slight curve: the outer half turns a little back toward the rachis direction.
				const glm::vec2 end = at + rotate(dir, -side * glm::radians(8.0f)) * pinnaLength;
				const glm::vec2 pinnaCtrl = at + dir * (pinnaLength * 0.55f);
				const int leaflets = glm::max((int)(pinnaLength / leafletSpacing), 3);
				for (int j = 0; j < leaflets; ++j)
				{
					const float s = ((float)j + 0.7f) / (float)leaflets;
					const glm::vec2 p = bezier(at, pinnaCtrl, end, s);
					const glm::vec2 pt = glm::normalize(bezier(at, pinnaCtrl, end, glm::min(s + 0.02f, 1.0f)) - bezier(at, pinnaCtrl, end, glm::max(s - 0.02f, 0.0f)));
					// Full length along most of the pinna, shorter in the last quarter (the pinna's rounded tip).
					const float len = leafletLength * rng.range(0.92f, 1.06f) * (s > 0.75f ? glm::mix(1.0f, 0.55f, (s - 0.75f) * 4.0f) : 1.0f);
					for (const float leafSide : { -1.0f, 1.0f })
					{
						const glm::vec2 ld = rotate(pt, leafSide * glm::radians(rng.range(72.0f, 84.0f)));
						// One half of the comb a little darker (the fold of the pinna); small per-leaflet variation.
						const float shade = pinnaShade * rng.range(0.92f, 1.06f) * (leafSide * side > 0.0f ? 0.9f : 1.0f);
						drawLeaf(cell, p, ld, len, len * 0.2f, glm::clamp(species.leafColor * shade, 0.0f, 1.0f));
					}
				}
				// The pinna's midrib over its leaflets' bases.
				drawCapsule(cell, at, end, glm::max(0.0018f, px * 0.6f), glm::max(0.0009f, px * 0.45f), stemColor);
			}
		}
	}

	// The cell: `Shoots` compound leaves fanned from the stem at v = 0 over up to +-28 degrees (the middle one along v,
	// the outer ones a little shorter), each with `ClusterLeaves` pinna pairs.
	void drawPinnateCluster(const Cell& cell, const TreeSpeciesDesc& species, Rng& rng)
	{
		auto rotate = [](glm::vec2 v, float a) { return glm::vec2(v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)); };
		const glm::vec2 base(0.5f, 0.02f);
		const float reach = species.clusterLeafSize + 0.02f;
		auto fits = [&](glm::vec2 p) { return p.x >= reach && p.x <= 1.0f - reach && p.y >= reach && p.y <= 1.0f - reach; };
		const int numLeaves = glm::max(species.clusterShoots, 1);
		// The cell's character: every compound leaf in it shares one.
		const PinnateVariant variant{
			.pinnaAngle = rng.range(30.0f, 50.0f),
			.pinnaScale = rng.range(0.8f, 1.12f),
			.stagger = rng.next01() < 0.5f ? rng.range(0.04f, 0.09f) : 0.0f,
			.tint = rng.range(0.88f, 1.12f),
		};
		const int pairs = glm::max(species.clusterLeaves + (int)(rng.next() % 3u) - 1, 2); // one more or fewer
		const float leafletLength = species.clusterLeafSize * rng.range(0.85f, 1.15f);
		const float lean = rng.signed1() * 12.0f;  // degrees: the whole leaf tilts left or right
		const float bend = rng.signed1() * 0.16f;  // its rachis curves (x its length)
		const float lengthScale = rng.range(0.78f, 1.0f);
		// A short woody twig the leaves grow from.
		drawCapsule(cell, base, base + glm::vec2(0.0f, 0.03f), 0.008f, 0.006f, species.leafTwigColor());
		oc::small_vector<int, 8> drawOrder;
		for (int b = 0; b < numLeaves; ++b)
			drawOrder.push_back(b);
		oc::sort(drawOrder.begin(), drawOrder.end(), [&](int a, int b)
			{ return glm::abs((float)a - 0.5f * (float)(numLeaves - 1)) > glm::abs((float)b - 0.5f * (float)(numLeaves - 1)); });
		for (const int b : drawOrder)
		{
			const float spread = numLeaves > 1 ? ((float)b / (float)(numLeaves - 1)) * 2.0f - 1.0f : 0.0f;
			const glm::vec2 start = base + glm::vec2(0.0f, 0.025f);
			const glm::vec2 dir = rotate(glm::vec2(0.0f, 1.0f), glm::radians(spread * 28.0f + lean + rng.signed1() * 5.0f));
			float length = (0.9f - 0.2f * glm::abs(spread)) * lengthScale * rng.range(0.92f, 1.0f);
			for (int k = 0; k < 16 && !fits(start + dir * length); ++k)
				length *= 0.9f;
			const glm::vec2 tip = start + dir * length;
			const glm::vec2 ctrl = start + dir * (length * 0.5f) + glm::vec2(-dir.y, dir.x) * (length * (bend + rng.signed1() * 0.04f));
			drawPinnateLeaf(cell, start, ctrl, tip, pairs, leafletLength, variant, species, rng);
		}
	}

	// Box downsample; colour weighted by alpha so transparent texels do not bleed into the silhouette.
	Canvas downsample(const Canvas& src)
	{
		Canvas dst;
		dst.size = glm::max(src.size / 2, 1u);
		dst.px.resize((size_t)dst.size * dst.size);
		for (uint32 y = 0; y < dst.size; ++y)
		{
			for (uint32 x = 0; x < dst.size; ++x)
			{
				glm::vec3 rgb(0.0f), rgbPlain(0.0f);
				float a = 0.0f;
				for (uint32 k = 0; k < 4; ++k)
				{
					const uint32 sx = glm::min(x * 2 + (k & 1), src.size - 1), sy = glm::min(y * 2 + (k >> 1), src.size - 1);
					const glm::vec4& s = src.px[(size_t)sy * src.size + sx];
					rgb += glm::vec3(s) * s.w;
					rgbPlain += glm::vec3(s);
					a += s.w;
				}
				dst.px[(size_t)y * dst.size + x] = glm::vec4(a > 1e-5f ? rgb / a : rgbPlain * 0.25f, a * 0.25f);
			}
		}
		return dst;
	}

	float coverage(const Canvas& c, float scale)
	{
		size_t pass = 0;
		for (const glm::vec4& p : c.px)
			pass += glm::min(p.w * scale, 1.0f) >= TREE_LEAF_ALPHA_CUTOFF ? 1 : 0;
		return (float)pass / (float)c.px.size();
	}

	oc::vector<uint8> toRgba8(const Canvas& c, float alphaScale)
	{
		oc::vector<uint8> out(c.px.size() * 4);
		for (size_t i = 0; i < c.px.size(); ++i)
		{
			const glm::vec4& p = c.px[i];
			out[i * 4 + 0] = (uint8)(glm::clamp(p.x, 0.0f, 1.0f) * 255.0f + 0.5f);
			out[i * 4 + 1] = (uint8)(glm::clamp(p.y, 0.0f, 1.0f) * 255.0f + 0.5f);
			out[i * 4 + 2] = (uint8)(glm::clamp(p.z, 0.0f, 1.0f) * 255.0f + 0.5f);
			out[i * 4 + 3] = (uint8)(glm::clamp(p.w * alphaScale, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
		return out;
	}
}

namespace Procedural
{
	void generateLeafClusterImage(const TreeSpeciesDesc& species, uint32 size, oc::vector<uint8>& outRgba, oc::vector<uint8>* outNormal)
	{
		Canvas canvas;
		canvas.size = size;
		// Transparent texels carry the leaf colour, so filtering at the silhouette never darkens it.
		canvas.px.assign((size_t)size * size, glm::vec4(species.leafColor, 0.0f));
		// The normal map: flat (+z) where nothing is drawn.
		Canvas normals;
		normals.size = size;
		if (outNormal)
			normals.px.assign((size_t)size * size, glm::vec4(0.5f, 0.5f, 1.0f, 0.0f));

		Rng rng(treeHash(species.seed, 3000u));
		const float cellPx = (float)size / (float)TREE_LEAF_ATLAS_CELLS;
		for (uint32 cy = 0; cy < TREE_LEAF_ATLAS_CELLS; ++cy)
			for (uint32 cx = 0; cx < TREE_LEAF_ATLAS_CELLS; ++cx)
			{
				const Cell cell{ canvas, glm::vec2((float)cx, (float)cy) * cellPx, cellPx, outNormal ? &normals : nullptr };
				if (species.clusterStyle == ETreeClusterStyle::Needles)
					drawNeedleCluster(cell, species, rng);
				else if (species.clusterStyle == ETreeClusterStyle::Mat)
					drawNeedleMat(cell, species, rng);
				else if (species.clusterStyle == ETreeClusterStyle::Pinnate)
					drawPinnateCluster(cell, species, rng);
				else
					drawCluster(cell, species, rng);
			}
		outRgba = toRgba8(canvas, 1.0f);
		if (outNormal)
		{
			// A = 255: the normal chain's mips average it (buildNormalMips), and the leaf FS does not read it.
			*outNormal = toRgba8(normals, 0.0f);
			for (size_t i = 3; i < outNormal->size(); i += 4)
				(*outNormal)[i] = 255;
		}
	}

	void buildLeafClusterMips(oc::span<const uint8> level0, uint32 size, TreeLeafTexture& out)
	{
		Canvas canvas;
		canvas.size = size;
		canvas.px.resize((size_t)size * size);
		for (size_t i = 0; i < canvas.px.size(); ++i)
			canvas.px[i] = glm::vec4(level0[i * 4], level0[i * 4 + 1], level0[i * 4 + 2], level0[i * 4 + 3]) * (1.0f / 255.0f);

		// Level 0 as is; every further level keeps level 0's alpha-test coverage (binary search on an alpha
		// scale). The unscaled alpha feeds the next downsample.
		out.size = size;
		out.mips.clear();
		const float targetCoverage = coverage(canvas, 1.0f);
		out.mips.push_back(oc::vector<uint8>(level0.begin(), level0.end()));
		Canvas level = oc::move(canvas);
		while (level.size > 1)
		{
			level = downsample(level);
			float lo = 0.0f, hi = 8.0f;
			for (int i = 0; i < 16; ++i)
			{
				const float mid = (lo + hi) * 0.5f;
				(coverage(level, mid) < targetCoverage ? lo : hi) = mid;
			}
			out.mips.push_back(toRgba8(level, hi));
		}
	}
}
