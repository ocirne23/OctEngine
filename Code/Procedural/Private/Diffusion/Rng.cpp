module Procedural;

import Core;
import :Diffusion.Rng;

namespace Procedural::Diffusion
{
	namespace
	{
		constexpr uint64 PCG64_MULT = 6364136223846793005ull;
		constexpr uint64 PCG64_INC = 1442695040888963407ull;
		constexpr double INV_2P32 = 1.0 / 4294967296.0;

		struct PcgStep
		{
			uint64 state;
			uint32 out32;
		};

		// One PCG64 step: state = state*MULT + INC (wrapping), output the XSH-RR 64/32 permutation.
		// Java's >>> on a long is a logical shift, which is plain >> on uint64 here.
		PcgStep pcg64Next(uint64 state)
		{
			state = state * PCG64_MULT + PCG64_INC;
			const uint64 x = (((state >> 18) ^ state) >> 27) & 0xFFFFFFFFull;
			const uint32 rot = (uint32)(state >> 59);
			const uint64 out32 = ((x >> rot) | (x << ((32u - rot) & 31u))) & 0xFFFFFFFFull;
			return { state, (uint32)out32 };
		}
	}

	int32 floorDiv(int32 a, int32 b)
	{
		int32 q = a / b;
		if ((a % b != 0) && ((a < 0) != (b < 0)))
			--q;
		return q;
	}

	int32 floorMod(int32 a, int32 b)
	{
		return a - floorDiv(a, b) * b;
	}

	uint64 tileSeed(uint64 baseSeed, int32 ty, int32 tx)
	{
		// Java: (ty & 0xFFFFFFFFL) sign-extends ty to long then keeps the low 32 bits == (uint64)(uint32)ty.
		uint64 h = baseSeed * 0x9E3779B9ull;
		h = h + (uint64)(uint32)ty;
		h = h * 0x9E3779B9ull + (uint64)(uint32)tx;
		return h;
	}

	void fillStandardNormal(uint64 seed, float* out, size_t count)
	{
		uint64 state = seed;
		size_t i = 0;
		while (i < count)
		{
			const PcgStep r1 = pcg64Next(state);
			state = r1.state;
			const PcgStep r2 = pcg64Next(state);
			state = r2.state;

			const double v1 = 2.0 * ((double)r1.out32 + 1.0) * INV_2P32 - 1.0;
			const double v2 = 2.0 * ((double)r2.out32 + 1.0) * INV_2P32 - 1.0;
			const double s = v1 * v1 + v2 * v2;
			// Rejection is what keeps the stream aligned with the reference: a rejected pair consumes two
			// PCG steps and emits nothing.
			if (s > 0.0 && s < 1.0)
			{
				const double f = std::sqrt(-2.0 * std::log(s) / s);
				out[i++] = (float)(v1 * f);
				if (i < count)
					out[i++] = (float)(v2 * f);
			}
		}
	}

	void gaussianNoisePatch(uint64 baseSeed, int32 y0, int32 x0, int32 h, int32 w,
	                        int32 channels, int32 tileH, int32 tileW, float* out)
	{
		const int32 ty0 = floorDiv(y0, tileH);
		const int32 ty1 = floorDiv(y0 + h - 1, tileH);
		const int32 tx0 = floorDiv(x0, tileW);
		const int32 tx1 = floorDiv(x0 + w - 1, tileW);

		const size_t tileLen = (size_t)channels * tileH * tileW;
		oc::vector<float> tile(tileLen);

		for (int32 ty = ty0; ty <= ty1; ty++)
		{
			const int32 tileY0 = ty * tileH;
			for (int32 tx = tx0; tx <= tx1; tx++)
			{
				const int32 tileX0 = tx * tileW;

				const int32 oy0 = y0 > tileY0 ? y0 : tileY0;
				const int32 oy1 = (y0 + h) < (tileY0 + tileH) ? (y0 + h) : (tileY0 + tileH);
				const int32 ox0 = x0 > tileX0 ? x0 : tileX0;
				const int32 ox1 = (x0 + w) < (tileX0 + tileW) ? (x0 + w) : (tileX0 + tileW);
				if (oy0 >= oy1 || ox0 >= ox1)
					continue;

				// The whole tile is always generated even when only a sliver is used: the stream is
				// sequential, so a partial fill would desynchronise every later value in the tile.
				fillStandardNormal(tileSeed(baseSeed, ty, tx), tile.data(), tileLen);

				for (int32 c = 0; c < channels; c++)
				{
					const size_t tileCh = (size_t)c * tileH * tileW;
					const size_t outCh = (size_t)c * h * w;
					for (int32 py = oy0; py < oy1; py++)
					{
						const float* src = &tile[tileCh + (size_t)(py - tileY0) * tileW + (ox0 - tileX0)];
						float* dst = &out[outCh + (size_t)(py - y0) * w + (ox0 - x0)];
						memcpy(dst, src, (size_t)(ox1 - ox0) * sizeof(float));
					}
				}
			}
		}
	}
}
