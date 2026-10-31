module Procedural;

import Core;
import Core.glm;
import :Noise;

namespace Procedural
{
	namespace
	{
		// Integer hash: map a lattice coordinate + seed to a well-mixed 32-bit value.
		uint32 hashCoord(int32 ix, int32 iy, uint32 seed)
		{
			uint32 h = seed + 0x9e3779b9u;
			h ^= (uint32)ix * 0x8da6b343u;
			h ^= (uint32)iy * 0xd8163841u;
			h *= 0x27d4eb2fu;
			h ^= h >> 15;
			h *= 0x85ebca6bu;
			h ^= h >> 13;
			return h;
		}

		// THE GRADIENT TABLE: the 65536 directions gradient() picks from (the hash's low 16 bits as an angle), computed
		// ONCE - a cos and a sin per lattice corner were about a third of every fbm (the terrain detail, the climate
		// wander, the tree placement's patches). Namespace scope, built in static init, before any job samples (512 KB).
		struct GradientTable
		{
			glm::vec2 dirs[65536];
			GradientTable()
			{
				for (uint32 i = 0; i < 65536; ++i)
				{
					const float angle = (float)i / 65535.0f * 6.28318530718f;
					dirs[i] = glm::vec2(std::cos(angle), std::sin(angle));
				}
			}
		};
		const GradientTable g_gradients;
	}

	glm::vec2 NoiseField::gradient(int32 ix, int32 iy) const
	{
		return g_gradients.dirs[hashCoord(ix, iy, m_seed) & 0xffffu];
	}

	float NoiseField::noise2D(float x, float y) const
	{
		const int32 ix = (int32)std::floor(x);
		const int32 iy = (int32)std::floor(y);
		const float fx = x - (float)ix;
		const float fy = y - (float)iy;

		// Quintic smoothstep fade
		const float u = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
		const float v = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

		auto corner = [&](int32 cx, int32 cy) -> float
		{
			const glm::vec2 g = gradient(cx, cy);
			return g.x * (x - (float)cx) + g.y * (y - (float)cy);
		};

		const float n00 = corner(ix,     iy);
		const float n10 = corner(ix + 1, iy);
		const float n01 = corner(ix,     iy + 1);
		const float n11 = corner(ix + 1, iy + 1);

		const float nx0 = glm::mix(n00, n10, u);
		const float nx1 = glm::mix(n01, n11, u);
		// Gradient noise output is ~[-0.707, 0.707]; scale to ~[-1, 1].
		return glm::mix(nx0, nx1, v) * 1.41421356f;
	}

	glm::vec3 NoiseField::noise2Dd(float x, float y) const
	{
		const int32 ix = (int32)std::floor(x);
		const int32 iy = (int32)std::floor(y);
		const float fx = x - (float)ix;
		const float fy = y - (float)iy;

		// Quintic fade + its derivative.
		const float u = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
		const float v = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
		const float du = 30.0f * fx * fx * (fx - 1.0f) * (fx - 1.0f);
		const float dv = 30.0f * fy * fy * (fy - 1.0f) * (fy - 1.0f);

		const glm::vec2 ga = gradient(ix, iy);
		const glm::vec2 gb = gradient(ix + 1, iy);
		const glm::vec2 gc = gradient(ix, iy + 1);
		const glm::vec2 gd = gradient(ix + 1, iy + 1);

		const float va = ga.x * fx + ga.y * fy;
		const float vb = gb.x * (fx - 1.0f) + gb.y * fy;
		const float vc = gc.x * fx + gc.y * (fy - 1.0f);
		const float vd = gd.x * (fx - 1.0f) + gd.y * (fy - 1.0f);
		const float k = va - vb - vc + vd;

		const float value = va + u * (vb - va) + v * (vc - va) + u * v * k;
		const float ddx = ga.x + u * (gb.x - ga.x) + v * (gc.x - ga.x) + u * v * (ga.x - gb.x - gc.x + gd.x)
			+ du * ((vb - va) + v * k);
		const float ddy = ga.y + u * (gb.y - ga.y) + v * (gc.y - ga.y) + u * v * (ga.y - gb.y - gc.y + gd.y)
			+ dv * ((vc - va) + u * k);
		return glm::vec3(value, ddx, ddy) * 1.41421356f;
	}

	float NoiseField::fbmEroded(float x, float y, uint32 octaves, float lacunarity, float persistence) const
	{
		float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
		glm::vec2 dsum(0.0f);
		for (uint32 i = 0; i < octaves; ++i)
		{
			const glm::vec3 n = noise2Dd(x * frequency, y * frequency);
			dsum += glm::vec2(n.y, n.z); // accumulated slope (Quilez: unscaled across octaves)
			sum += amplitude * n.x / (1.0f + glm::dot(dsum, dsum));
			norm += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		return norm > 0.0f ? sum / norm : 0.0f;
	}

	float NoiseField::fbm(float x, float y, uint32 octaves, float lacunarity, float persistence) const
	{
		float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
		for (uint32 i = 0; i < octaves; ++i)
		{
			sum += noise2D(x * frequency, y * frequency) * amplitude;
			norm += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		return norm > 0.0f ? sum / norm : 0.0f;
	}

	float NoiseField::ridged(float x, float y, uint32 octaves, float lacunarity, float persistence) const
	{
		float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
		for (uint32 i = 0; i < octaves; ++i)
		{
			float n = 1.0f - std::fabs(noise2D(x * frequency, y * frequency));
			n *= n; // sharpen the ridges
			sum += n * amplitude;
			norm += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		return norm > 0.0f ? sum / norm : 0.0f;
	}

	float NoiseField::billow(float x, float y, uint32 octaves, float lacunarity, float persistence) const
	{
		float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
		for (uint32 i = 0; i < octaves; ++i)
		{
			sum += std::fabs(noise2D(x * frequency, y * frequency)) * amplitude;
			norm += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		return norm > 0.0f ? sum / norm : 0.0f;
	}
}
