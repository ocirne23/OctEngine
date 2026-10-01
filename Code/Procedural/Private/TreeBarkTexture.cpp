module Procedural;

import Core;
import Core.glm;

import :TreeSpecies;
import :TreeGenerator;
import :TreeBarkTexture;

namespace
{
	using namespace Procedural;

	int wrap(int i, int period) { return ((i % period) + period) % period; }

	uint32 latticeHash(int x, int y, uint32 seed)
	{
		return treeHash(treeHash((uint32)x * 73856093u ^ seed, (uint32)y * 19349663u));
	}

	// Gradient (Perlin) noise on a lattice that repeats every (px, py) cells - tileable when x, y span whole
	// periods. Remapped to ~[0,1]. NOT value noise: smoothstep-interpolated value noise has a zero gradient on
	// every lattice line, which the normal map turned into evenly spaced bands across the bark.
	float gradientNoise(float x, float y, int px, int py, uint32 seed)
	{
		const float fx = std::floor(x), fy = std::floor(y);
		const int ix = (int)fx, iy = (int)fy;
		const float tx = x - fx, ty = y - fy;
		auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
		auto g = [&](int dx, int dy)
		{
			const float angle = treeHash01(latticeHash(wrap(ix + dx, px), wrap(iy + dy, py), seed)) * 6.28318531f;
			return std::cos(angle) * (tx - (float)dx) + std::sin(angle) * (ty - (float)dy);
		};
		const float sx = fade(tx), sy = fade(ty);
		const float p = glm::mix(glm::mix(g(0, 0), g(1, 0), sx), glm::mix(g(0, 1), g(1, 1), sx), sy);
		return glm::clamp(0.5f + 0.85f * p, 0.0f, 1.0f);
	}

	// Tileable fbm over [0,1)^2: octave o has (fu, fv) * 2^o cells per tile.
	float fbm(float u, float v, int fu, int fv, int octaves, uint32 seed)
	{
		float sum = 0.0f, amp = 0.5f, norm = 0.0f;
		for (int o = 0; o < octaves; ++o)
		{
			const int pu = fu << o, pv = fv << o;
			sum += amp * gradientNoise(u * (float)pu, v * (float)pv, pu, pv, seed + (uint32)o * 1013u);
			norm += amp;
			amp *= 0.5f;
		}
		return sum / norm;
	}

	// Furrowed bark: two families of `nu` fissure lines running along the branch (v), each meandering
	// sideways by its own tileable noise, so the families cross and merge into long braided ridges. Rare
	// thin horizontal breaks (`nv` rows per tile) split a ridge now and then. Distances are in ridge-width
	// units: 0 on a fissure, ~0.5 in the middle of a ridge.
	struct FurrowSample
	{
		float dist = 0.5f;     // to the nearest fissure (either family or a break)
		float width = 1.0f;    // local fissure width multiplier (varies along a line)
		float fineDist = 0.5f; // to the nearest fine crack (short, shallow, between the fissures)
		float striaDist = 0.5f; // to the nearest striation (many long thin lines along the branch)
		uint32 ridgeId = 0;    // which ridge segment (tint / height variation)
	};

	// Lines fade out where their own noise is high: per line column (one noise cell per ridge width across)
	// and short cells along, so every line turns into segments with gaps. 0 = continuous lines.
	float lineGap(float u, float v, int columns, int rows, float breakup, uint32 seed)
	{
		const float n = gradientNoise(u * (float)columns, v * (float)rows, columns, rows, seed);
		return 0.5f * glm::smoothstep(1.0f - breakup * 0.6f, 1.05f - breakup * 0.6f + 0.15f, n);
	}

	FurrowSample furrows(float u, float v, int nu, int nv, float breakup, uint32 seed)
	{
		// Sideways meander, in ridge widths. Few cells across (lines in a neighbourhood bend together), a
		// few along (the bends are long).
		const float warpA = 0.9f * (fbm(u, v, glm::max(nu / 4, 1), glm::max(nv / 2, 1), 3, seed + 1u) - 0.5f) * 2.0f;
		const float warpB = 0.9f * (fbm(u, v, glm::max(nu / 4, 1), glm::max(nv / 2, 1), 3, seed + 2u) - 0.5f) * 2.0f;
		const float xa = u * (float)nu + warpA;
		const float xb = u * (float)nu + 0.5f + warpB;
		const float fa = xa - std::floor(xa), fb = xb - std::floor(xb);
		const float da = glm::min(fa, 1.0f - fa) + lineGap(u, v, nu, nv * 3, breakup, seed + 6u);
		const float db = glm::min(fb, 1.0f - fb) + lineGap(u, v, nu, nv * 3, breakup, seed + 7u);

		FurrowSample s;
		s.dist = glm::min(da, db);
		s.width = 0.5f + gradientNoise(u * (float)nu, v * (float)(nv * 4), nu, nv * 4, seed + 8u);

		// Fine cracks: a denser, less warped family, mostly gaps - short shallow splits on the ridges.
		const float warpC = 0.5f * (fbm(u, v, glm::max(nu / 2, 1), nv, 2, seed + 9u) - 0.5f) * 2.0f;
		const float xc = u * (float)(nu * 3) + warpC;
		const float fc = xc - std::floor(xc);
		s.fineDist = glm::min(fc, 1.0f - fc) + lineGap(u, v, nu * 3, nv * 8, glm::min(breakup + 0.35f, 1.0f), seed + 10u);

		// Striations: 8x denser than the fissures, gently warped, short-to-medium segments.
		const float warpS = 0.6f * (fbm(u, v, nu, nv * 2, 2, seed + 11u) - 0.5f) * 2.0f;
		const float xs = u * (float)(nu * 8) + warpS;
		const float fs = xs - std::floor(xs);
		s.striaDist = glm::min(fs, 1.0f - fs) + lineGap(u, v, nu * 8, nv * 8, glm::min(breakup * 0.5f + 0.25f, 1.0f), seed + 12u);
		const int colA = wrap((int)std::floor(xa), nu), colB = wrap((int)std::floor(xb), nu);

		// Horizontal breaks: per (column, row) a break at a jittered height, present with 20% probability.
		// Vertical distance converted to ridge widths (x nu / nv), then x2.5 so a break is thinner than a fissure.
		const float y = v * (float)nv;
		const int row = (int)std::floor(y);
		for (int dr = -1; dr <= 1; ++dr)
		{
			const uint32 h = latticeHash(colA, wrap(row + dr, nv), seed + 3u);
			if (treeHash01(h) > 0.2f)
				continue;
			const float breakY = (float)(row + dr) + treeHash01(treeHash(h));
			s.dist = glm::min(s.dist, glm::abs(y - breakY) / (float)nv * (float)nu * 2.5f);
		}
		s.ridgeId = latticeHash(colA, colB, seed + 4u) ^ latticeHash(row, 0, seed + 5u);
		return s;
	}

	uint8 toByte(float v) { return (uint8)(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

	oc::vector<uint8> downsampleAlbedo(const oc::vector<uint8>& src, uint32 srcSize)
	{
		const uint32 size = glm::max(srcSize / 2, 1u);
		oc::vector<uint8> dst((size_t)size * size * 4);
		for (uint32 y = 0; y < size; ++y)
			for (uint32 x = 0; x < size; ++x)
				for (uint32 c = 0; c < 4; ++c)
				{
					uint32 sum = 0;
					for (uint32 k = 0; k < 4; ++k)
					{
						const uint32 sx = glm::min(x * 2 + (k & 1), srcSize - 1), sy = glm::min(y * 2 + (k >> 1), srcSize - 1);
						sum += src[((size_t)sy * srcSize + sx) * 4 + c];
					}
					dst[((size_t)y * size + x) * 4 + c] = (uint8)((sum + 2) / 4);
				}
		return dst;
	}

	oc::vector<uint8> downsampleNormals(const oc::vector<uint8>& src, uint32 srcSize)
	{
		const uint32 size = glm::max(srcSize / 2, 1u);
		oc::vector<uint8> dst((size_t)size * size * 4);
		for (uint32 y = 0; y < size; ++y)
		{
			for (uint32 x = 0; x < size; ++x)
			{
				glm::vec3 n(0.0f);
				for (uint32 k = 0; k < 4; ++k)
				{
					const uint32 sx = glm::min(x * 2 + (k & 1), srcSize - 1), sy = glm::min(y * 2 + (k >> 1), srcSize - 1);
					const uint8* p = &src[((size_t)sy * srcSize + sx) * 4];
					n += glm::vec3(p[0], p[1], p[2]) / 127.5f - 1.0f;
				}
				n = glm::dot(n, n) > 1e-8f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
				uint8* d = &dst[((size_t)y * size + x) * 4];
				d[0] = toByte(n.x * 0.5f + 0.5f);
				d[1] = toByte(n.y * 0.5f + 0.5f);
				d[2] = toByte(n.z * 0.5f + 0.5f);
				d[3] = 255;
			}
		}
		return dst;
	}
}

namespace Procedural
{
	void generateBarkImages(const TreeSpeciesDesc& species, uint32 size, oc::vector<uint8>& outAlbedo, oc::vector<uint8>& outNormal)
	{
		const uint32 seed = treeHash(species.seed, 4000u);
		const int nu = (int)species.barkPlates.x, nv = (int)species.barkPlates.y;
		const glm::vec3 lichenColor(0.42f, 0.46f, 0.36f);

		oc::vector<float> height((size_t)size * size);
		oc::vector<glm::vec3> albedo((size_t)size * size);
		for (uint32 y = 0; y < size; ++y)
		{
			for (uint32 x = 0; x < size; ++x)
			{
				const float u = ((float)x + 0.5f) / (float)size;
				const float v = ((float)y + 0.5f) / (float)size;
				const FurrowSample f = furrows(u, v, nu, nv, species.barkBreakup, seed);
				// 0 in a fissure; a soft edge, the width varying along the line.
				const float plate = glm::smoothstep(0.0f, species.barkCrack * f.width, f.dist);
				// Fine cracks are a quarter as wide (in their own, 3x denser units) and shallow.
				const float fine = 1.0f - glm::smoothstep(0.0f, species.barkCrack * 0.25f * 3.0f, f.fineDist);
				// Striations: ~1/6 of their own spacing wide, a soft V.
				const float stria = 1.0f - glm::smoothstep(0.0f, 0.16f, f.striaDist);
				// Rounded ridge crown (V-shaped fissures, not flat plates), per-segment height jitter.
				const float crown = std::sqrt(glm::clamp(f.dist * 2.0f, 0.0f, 1.0f));
				const float ridgeHeight = treeHash01(treeHash(f.ridgeId, 7u));
				const float grain = fbm(u, v, 8, 8, 4, seed + 11u);
				// Fibres: noise stretched along the branch (v), the grain of the wood under the bark.
				const float fibre = gradientNoise(u * (float)(nu * 6), v * (float)nv, nu * 6, nv, seed + 23u);

				const size_t i = (size_t)y * size + x;
				// Fissure floor at 35% of the ridge height: shallow enough that the shading does not ink them.
				height[i] = glm::mix(0.35f, 1.0f, plate) * (0.55f + 0.45f * crown) * (0.85f + 0.15f * ridgeHeight)
					- 0.3f * fine - 0.12f * stria * plate + 0.1f * (grain - 0.5f) + 0.08f * (fibre - 0.5f);

				glm::vec3 c = species.barkColor * (0.9f + 0.2f * treeHash01(treeHash(f.ridgeId, 13u)));
				c *= 0.85f + 0.3f * crown; // ridge tops weathered lighter
				c *= 0.8f + 0.4f * grain;
				c *= 0.9f + 0.2f * fibre; // streaks along the branch: tone variation that is not a line
				// Low albedo contrast in the cracks: the normal map's shading carries most of their darkness,
				// dark outlines read as cartoon ink.
				c = glm::mix(c, c * 0.85f, fine);
				c = glm::mix(c, c * 0.9f, stria);
				c = glm::mix(species.barkColor * 0.85f, c, plate);
				const float lichen = glm::smoothstep(0.58f, 0.72f, fbm(u, v, 3, 2, 3, seed + 37u)) * species.barkLichen;
				c = glm::mix(c, lichenColor * (0.85f + 0.3f * grain), lichen * plate);
				albedo[i] = c;
			}
		}

		// Normals from the wrapped central-difference gradient (x along u, y along v, z out of the bark).
		const float strength = species.barkRelief * (float)size * 0.012f;
		oc::vector<uint8> albedo0((size_t)size * size * 4);
		oc::vector<uint8> normal0((size_t)size * size * 4);
		auto h = [&](int x, int y) { return height[(size_t)wrap(y, (int)size) * size + (size_t)wrap(x, (int)size)]; };
		for (int y = 0; y < (int)size; ++y)
		{
			for (int x = 0; x < (int)size; ++x)
			{
				const size_t i = (size_t)y * size + (size_t)x;
				const float dhdu = (h(x + 1, y) - h(x - 1, y)) * 0.5f;
				const float dhdv = (h(x, y + 1) - h(x, y - 1)) * 0.5f;
				const glm::vec3 n = glm::normalize(glm::vec3(-dhdu * strength, -dhdv * strength, 1.0f));
				albedo0[i * 4 + 0] = toByte(albedo[i].x);
				albedo0[i * 4 + 1] = toByte(albedo[i].y);
				albedo0[i * 4 + 2] = toByte(albedo[i].z);
				albedo0[i * 4 + 3] = 255;
				normal0[i * 4 + 0] = toByte(n.x * 0.5f + 0.5f);
				normal0[i * 4 + 1] = toByte(n.y * 0.5f + 0.5f);
				normal0[i * 4 + 2] = toByte(n.z * 0.5f + 0.5f);
				normal0[i * 4 + 3] = 255;
			}
		}

		outAlbedo = oc::move(albedo0);
		outNormal = oc::move(normal0);
	}

	void buildBarkMips(oc::span<const uint8> albedo0, oc::span<const uint8> normal0, uint32 size, TreeBarkTexture& out)
	{
		out.size = size;
		out.albedoMips.clear();
		out.normalMips.clear();
		out.albedoMips.push_back(oc::vector<uint8>(albedo0.begin(), albedo0.end()));
		out.normalMips.push_back(oc::vector<uint8>(normal0.begin(), normal0.end()));
		for (uint32 levelSize = size; levelSize > 1; levelSize /= 2)
		{
			out.albedoMips.push_back(downsampleAlbedo(out.albedoMips.back(), levelSize));
			out.normalMips.push_back(downsampleNormals(out.normalMips.back(), levelSize));
		}
	}
}
