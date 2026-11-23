module Procedural;

import Core;
import Core.glm;
import Core.Log;
import File;
import Threading;

import :SnowRelief;

namespace
{
	using namespace Procedural;

	constexpr uint32 N = SnowRelief::SIZE;
	constexpr float PI = 3.14159265f;
	static_assert((N & (N - 1)) == 0, "the FFT is radix 2");

	struct Cpx { float re, im; };

	uint32 hashU(uint32 x)
	{
		x ^= x >> 16; x *= 0x7feb352du;
		x ^= x >> 15; x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}
	float rand01(uint32 seed, uint32 a, uint32 b)
	{
		return (float)(hashU(seed * 0x9E3779B9u ^ hashU(a * 0x85EBCA6Bu ^ hashU(b + 0x632BE5ABu))) >> 8) * (1.0f / 16777216.0f);
	}

	// In place, N points, inverse (tw = exp(+2 pi i k / N), k < N / 2). Unscaled: every field is normalized after.
	void fft(Cpx* a, const Cpx* tw)
	{
		for (uint32 i = 1, j = 0; i < N; ++i)
		{
			uint32 bit = N >> 1;
			for (; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;
			if (i < j)
			{
				const Cpx t = a[i];
				a[i] = a[j];
				a[j] = t;
			}
		}
		for (uint32 len = 2; len <= N; len <<= 1)
		{
			const uint32 half = len >> 1, step = N / len;
			for (uint32 i = 0; i < N; i += len)
				for (uint32 k = 0; k < half; ++k)
				{
					const Cpx w = tw[k * step];
					Cpx& u = a[i + k];
					Cpx& v = a[i + k + half];
					const Cpx t = { v.re * w.re - v.im * w.im, v.re * w.im + v.im * w.re };
					v = { u.re - t.re, u.im - t.im };
					u = { u.re + t.re, u.im + t.im };
				}
		}
	}

	// The rows, then the columns (each chunk gathers its columns into its own buffer).
	void ifft2d(oc::vector<Cpx>& s, const oc::vector<Cpx>& tw)
	{
		Globals::jobSystem.parallelFor(0, N, 32, { "Snow relief FFT rows", EProfileCategory::Procedural }, [&](uint32 begin, uint32 end)
		{
			for (uint32 y = begin; y < end; ++y)
				fft(&s[(size_t)y * N], tw.data());
		}, EJobPriority::Low);
		Globals::jobSystem.parallelFor(0, N, 32, { "Snow relief FFT columns", EProfileCategory::Procedural }, [&](uint32 begin, uint32 end)
		{
			oc::vector<Cpx> col(N);
			for (uint32 x = begin; x < end; ++x)
			{
				for (uint32 y = 0; y < N; ++y)
					col[y] = s[(size_t)y * N + x];
				fft(col.data(), tw.data());
				for (uint32 y = 0; y < N; ++y)
					s[(size_t)y * N + x] = col[y];
			}
		}, EJobPriority::Low);
	}

	// One band-limited field on the torus: amplitude amp(kx, ky) per integer frequency (cycles per tile, in [-N/2, N/2)),
	// random phases, the real part - zero mean, unit RMS.
	template<class Amp>
	oc::vector<float> bandField(uint32 seed, const oc::vector<Cpx>& tw, Amp amp)
	{
		oc::vector<Cpx> s((size_t)N * N);
		Globals::jobSystem.parallelFor(0, N, 64, { "Snow relief spectrum", EProfileCategory::Procedural }, [&](uint32 begin, uint32 end)
		{
			for (uint32 y = begin; y < end; ++y)
			{
				const int ky = y < N / 2 ? (int)y : (int)y - (int)N;
				for (uint32 x = 0; x < N; ++x)
				{
					const int kx = x < N / 2 ? (int)x : (int)x - (int)N;
					const float a = (kx == 0 && ky == 0) ? 0.0f : amp((float)kx, (float)ky);
					const float phase = 2.0f * PI * rand01(seed, x, y);
					s[(size_t)y * N + x] = { a * std::cos(phase), a * std::sin(phase) };
				}
			}
		}, EJobPriority::Low);
		ifft2d(s, tw);
		oc::vector<float> f((size_t)N * N);
		double sum = 0.0, sum2 = 0.0;
		for (size_t i = 0; i < f.size(); ++i)
		{
			f[i] = s[i].re;
			sum += f[i];
			sum2 += (double)f[i] * f[i];
		}
		const double mean = sum / (double)f.size();
		const float invRms = (float)(1.0 / glm::max(std::sqrt(glm::max(sum2 / (double)f.size() - mean * mean, 0.0)), 1e-12));
		for (float& v : f)
			v = (v - (float)mean) * invRms;
		return f;
	}

	float band(float k, float lo, float hi) { return k >= lo && k <= hi ? 1.0f : 0.0f; }
	float uniformOf(float x) { return 0.5f * (1.0f + std::erf(x * 0.70710678f)); } // a unit-RMS field's value -> its quantile
	float smooth(float a, float b, float x) { return glm::smoothstep(a, b, x); }
	size_t at(int x, int y) { return (size_t)(y & (int)(N - 1)) * N + (size_t)(x & (int)(N - 1)); }

	bool writeRgba(const char* path, uint32 w, uint32 h, const oc::vector<uint8>& rgba)
	{
		if (!ImageIO::writePngRgba8(path, w, h, oc::span<const uint8>(rgba.data(), rgba.size())))
		{
			Log::warning(oc::format("Snow relief: failed to write {}", path));
			return false;
		}
		return true;
	}
}

namespace Procedural::SnowRelief
{
	bool ensureSources(const oc::atomic<bool>& stopRequested)
	{
		if (FileSystem::exists(DIFF_PATH) && FileSystem::exists(NOR_PATH) && FileSystem::exists(DISP_PATH))
			return true;
		const auto start = std::chrono::steady_clock::now();
		FileSystem::createDirectories("Local/TerrainTex/Gen/");

		oc::vector<Cpx> tw(N / 2);
		for (uint32 k = 0; k < N / 2; ++k)
			tw[k] = { std::cos(2.0f * PI * (float)k / (float)N), std::sin(2.0f * PI * (float)k / (float)N) };

		// The wind: ripples cross it, sastrugi run along it.
		const glm::vec2 wind = glm::vec2(std::cos(glm::radians(30.0f)), std::sin(glm::radians(30.0f)));
		// THE LUMPS (2.5 .. 0.8 m, a falling spectrum): the base.
		oc::vector<float> h = bandField(21u, tw, [](float kx, float ky)
		{
			const float k = std::sqrt(kx * kx + ky * ky);
			return band(k, 4.0f, 12.5f) * std::pow(k, -1.5f);
		});
		if (stopRequested.load(oc::memory_order_relaxed))
			return true;
		// The patches: wind-packed CRUST (smooth, sastrugi, no ripples) and the RIPPLE fields between (their mask fields
		// freed at once: the peak memory is a few fields).
		oc::vector<float> crust((size_t)N * N), rippleMask((size_t)N * N);
		{
			const oc::vector<float> m2 = bandField(12u, tw, [](float kx, float ky) { return band(std::sqrt(kx * kx + ky * ky), 3.0f, 8.0f); });
			for (size_t i = 0; i < h.size(); ++i)
				crust[i] = smooth(0.55f, 0.75f, uniformOf(m2[i]));
		}
		{
			const oc::vector<float> m1 = bandField(11u, tw, [](float kx, float ky) { return band(std::sqrt(kx * kx + ky * ky), 2.0f, 5.0f); });
			for (size_t i = 0; i < h.size(); ++i)
				rippleMask[i] = smooth(0.35f, 0.65f, uniformOf(m1[i])) * (1.0f - crust[i]);
		}
		// THE RIPPLES (14 .. 10 cm) across the wind: sharp crests, broad troughs.
		{
			const oc::vector<float> r = bandField(22u, tw, [wind](float kx, float ky)
			{
				const float k = std::sqrt(kx * kx + ky * ky);
				const float c = k > 0.0f ? std::abs((kx * wind.x + ky * wind.y) / k) : 0.0f;
				return band(k, 70.0f, 100.0f) * std::pow(c, 8.0f);
			});
			for (size_t i = 0; i < h.size(); ++i)
				h[i] += 0.15f * rippleMask[i] * (r[i] + 0.35f * r[i] * std::abs(r[i]));
			rippleMask = oc::vector<float>();
		}
		if (stopRequested.load(oc::memory_order_relaxed))
			return true;
		// THE SASTRUGI: long ridges along the wind (ridged noise of an anisotropic band), softened over 3 texels.
		{
			const oc::vector<float> sa = bandField(23u, tw, [wind](float kx, float ky)
			{
				const float along = kx * wind.x + ky * wind.y, across = -kx * wind.y + ky * wind.x;
				return band(std::sqrt(16.0f * along * along + across * across), 8.0f, 33.0f);
			});
			float maxAbs = 1e-6f;
			for (float v : sa)
				maxAbs = glm::max(maxAbs, std::abs(v));
			oc::vector<float> ridge((size_t)N * N);
			for (size_t i = 0; i < sa.size(); ++i)
			{
				const float t = 1.0f - std::abs(sa[i]) / maxAbs;
				ridge[i] = t * t;
			}
			for (int y = 0; y < (int)N; ++y)
				for (int x = 0; x < (int)N; ++x)
				{
					float b = 0.0f;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
							b += ridge[at(x + dx, y + dy)];
					h[at(x, y)] += 0.35f * crust[at(x, y)] * (b * (1.0f / 9.0f));
				}
			crust = oc::vector<float>();
		}
		// THE GRAIN (3 .. 1 cm), flat.
		{
			const oc::vector<float> g = bandField(24u, tw, [](float kx, float ky) { return band(std::sqrt(kx * kx + ky * ky), 330.0f, 1000.0f); });
			for (size_t i = 0; i < h.size(); ++i)
				h[i] += 0.04f * g[i];
		}
		// THE PITS: 600 small wrapped dents (0.6 .. 1.2 cm), a smooth profile.
		{
			const float texelM = SnowRelief::TILE_METRES / (float)N;
			for (uint32 p = 0; p < 600; ++p)
			{
				const float cx = rand01(31u, p, 0u) * (float)N, cy = rand01(31u, p, 1u) * (float)N;
				const float r = glm::mix(0.006f, 0.012f, rand01(31u, p, 2u)) / texelM;
				const int ri = (int)std::ceil(r);
				for (int dy = -ri; dy <= ri; ++dy)
					for (int dx = -ri; dx <= ri; ++dx)
					{
						const int x = (int)std::floor(cx) + dx, y = (int)std::floor(cy) + dy;
						const float d = glm::length(glm::vec2((float)x + 0.5f - cx, (float)y + 0.5f - cy)) / r;
						if (d < 1.0f)
							h[at(x, y)] -= 0.1f * (1.0f - d * d) * (1.0f - d * d);
					}
			}
		}
		if (stopRequested.load(oc::memory_order_relaxed))
			return true;

		// The height in [0, 1] (6 sigma over the range), and the normals from the FLOAT height (not the 8-bit one) at the
		// slope the bake's displacement gives it: (h01 - 0.5) x heightScale x the ground depth. R = world +X, G = world +Z
		// (the image's rows follow v = z x "Snow uv scale"): snowNormalOn's decode.
		double sum = 0.0, sum2 = 0.0;
		for (float v : h)
		{
			sum += v;
			sum2 += (double)v * v;
		}
		const float mean = (float)(sum / (double)h.size());
		const float sigma = (float)std::sqrt(glm::max(sum2 / (double)h.size() - (double)mean * mean, 1e-12));
		for (float& v : h)
			v = glm::clamp(0.5f + (v - mean) / (6.0f * sigma), 0.0f, 1.0f);

		oc::vector<uint8> disp((size_t)N * N * 4), nor((size_t)N * N * 4);
		const float slopeScale = SnowRelief::REF_DEPTH * SnowRelief::HEIGHT_SCALE / (2.0f * SnowRelief::TILE_METRES / (float)N);
		Globals::jobSystem.parallelFor(0, N, 64, { "Snow relief normals", EProfileCategory::Procedural }, [&](uint32 begin, uint32 end)
		{
			for (uint32 y = begin; y < end; ++y)
				for (uint32 x = 0; x < N; ++x)
				{
					const size_t i = (size_t)y * N + x;
					const uint8 hv = (uint8)glm::clamp(h[i] * 255.0f + 0.5f, 0.0f, 255.0f);
					disp[i * 4 + 0] = disp[i * 4 + 1] = disp[i * 4 + 2] = hv;
					disp[i * 4 + 3] = 255;
					const float sx = (h[at((int)x + 1, (int)y)] - h[at((int)x - 1, (int)y)]) * slopeScale;
					const float sz = (h[at((int)x, (int)y + 1)] - h[at((int)x, (int)y - 1)]) * slopeScale;
					const glm::vec3 n = glm::normalize(glm::vec3(-sx, -sz, 1.0f));
					nor[i * 4 + 0] = (uint8)glm::clamp((0.5f + 0.5f * n.x) * 255.0f + 0.5f, 0.0f, 255.0f);
					nor[i * 4 + 1] = (uint8)glm::clamp((0.5f + 0.5f * n.y) * 255.0f + 0.5f, 0.0f, 255.0f);
					nor[i * 4 + 2] = (uint8)glm::clamp((0.5f + 0.5f * n.z) * 255.0f + 0.5f, 0.0f, 255.0f);
					nor[i * 4 + 3] = 255;
				}
		}, EJobPriority::Low);

		// The albedo source: flat white (the shaders take the state's albedo; only the slot's machinery reads it).
		constexpr uint32 DIFF_SIZE = 16;
		oc::vector<uint8> diff((size_t)DIFF_SIZE * DIFF_SIZE * 4, (uint8)250);
		for (size_t i = 3; i < diff.size(); i += 4)
			diff[i] = 255;

		if (stopRequested.load(oc::memory_order_relaxed))
			return true; // nothing written: the next run generates all three
		const bool ok = writeRgba(DISP_PATH, N, N, disp) && writeRgba(NOR_PATH, N, N, nor) && writeRgba(DIFF_PATH, DIFF_SIZE, DIFF_SIZE, diff);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		Log::info(oc::format("Snow relief: generated the {} sources in {} ms", SnowRelief::STEM, ms));
		return ok;
	}
}
