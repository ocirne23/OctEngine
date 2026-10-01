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
			const float edge = glm::mix(ra, rb, t) - glm::distance(p, a + ab * t);
			const float alpha = glm::clamp(edge / px + 0.5f, 0.0f, 1.0f);
			if (alpha > 0.0f)
				cell.canvas.blend(x, y, color, alpha);
		});
	}

	// One leaf: origin at the petiole, `dir` along the blade. Pointed ellipse-like profile, lighter midrib,
	// darker rim, one half slightly shaded (the fold).
	void drawLeaf(const Cell& cell, glm::vec2 origin, glm::vec2 dir, float length, float halfWidth, const glm::vec3& color)
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
		const glm::vec2 tip(0.5f + rng.signed1() * 0.12f, 0.88f);
		const glm::vec2 ctrl(0.5f + rng.signed1() * 0.15f, 0.45f);
		constexpr int TWIG_SEGMENTS = 12;
		for (int i = 0; i < TWIG_SEGMENTS; ++i)
		{
			const float t0 = (float)i / TWIG_SEGMENTS, t1 = (float)(i + 1) / TWIG_SEGMENTS;
			drawCapsule(cell, bezier(base, ctrl, tip, t0), bezier(base, ctrl, tip, t1),
				glm::mix(0.012f, 0.004f, t0), glm::mix(0.012f, 0.004f, t1), species.barkColor);
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
			const float length = species.clusterLeafSize * rng.range(0.75f, 1.15f) * (1.0f - 0.25f * t);
			const glm::vec3 color = glm::mix(species.leafColor, autumn, rng.next01() * 0.35f) * rng.range(0.85f, 1.12f);
			drawLeaf(cell, at + dir * 0.01f, dir, length, length * 0.24f, glm::clamp(color, 0.0f, 1.0f));
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
	void generateLeafClusterImage(const TreeSpeciesDesc& species, uint32 size, oc::vector<uint8>& outRgba)
	{
		Canvas canvas;
		canvas.size = size;
		// Transparent texels carry the leaf colour, so filtering at the silhouette never darkens it.
		canvas.px.assign((size_t)size * size, glm::vec4(species.leafColor, 0.0f));

		Rng rng(treeHash(species.seed, 3000u));
		const float cellPx = (float)size / (float)TREE_LEAF_ATLAS_CELLS;
		for (uint32 cy = 0; cy < TREE_LEAF_ATLAS_CELLS; ++cy)
			for (uint32 cx = 0; cx < TREE_LEAF_ATLAS_CELLS; ++cx)
				drawCluster(Cell{ canvas, glm::vec2((float)cx, (float)cy) * cellPx, cellPx }, species, rng);
		outRgba = toRgba8(canvas, 1.0f);
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
