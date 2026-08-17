module;

// FastNoiseLite is included ONLY here, never from an .ixx: this TU is compiled /fp:precise, and the noise
// the coarse model is conditioned on must not be reassociated by the project-wide /fp:fast.
#include <FastNoiseLite/FastNoiseLite.h>

module Procedural;

import Core;
import :Diffusion.ModelAssets;
import :Diffusion.SyntheticMap;

// Parity note: this reproduces the reference's arithmetic, including its double/float mixing. /fp:precise
// pins the widths and forbids reassociation. It does still permit FMA contraction (VS2022 changed that
// default), so results may differ from the Java/Python reference in the last bit. That is fine and
// deliberate: what the world actually requires is DETERMINISM WITHIN A BUILD (same seed -> same world every
// run), not bit-equality with another language's build. Do not "simplify" the casts regardless — dropping
// them changes results by far more than a last bit.
namespace Procedural::Diffusion
{
	namespace
	{
		constexpr int32 N_CHANNELS = SyntheticMapFactory::NUM_CHANNELS;
		constexpr float BASE_FREQUENCY = 0.05f;
		constexpr int32 OCTAVES[N_CHANNELS] = { 4, 2, 4, 4, 4 }; // channel 1 (temp) is uniquely 2
		constexpr float LACUNARITY = 2.0f;
		constexpr float GAIN = 0.5f;
		constexpr int32 NOISE_QUANTILES = 64;
		constexpr float QUANTILE_EPS = 1e-4f;

		// Java's Math.signum: 0 for +-0, NaN for NaN, +-1 otherwise. std::copysign(1.0f, x) is WRONG here
		// (it yields +-1 for zero).
		float javaSignum(float x)
		{
			if (x > 0.0f)
				return 1.0f;
			if (x < 0.0f)
				return -1.0f;
			return x; // preserves -0.0 and NaN, as Java does
		}

		float clampf(float v, float lo, float hi)
		{
			return v < lo ? lo : (v > hi ? hi : v);
		}

		// np.interp clone: clamped at both ends, linear between. xp must be strictly increasing, which is
		// exactly what the monotonicity fix-up in buildNoiseQuantiles guarantees — without it the divide
		// below can be 0/0.
		float interp(float x, const float* xp, const float* fp, int32 n)
		{
			if (x <= xp[0])
				return fp[0];
			if (x >= xp[n - 1])
				return fp[n - 1];
			int32 lo = 0, hi = n - 1;
			while (hi - lo > 1)
			{
				const int32 mid = (int32)((uint32)(lo + hi) >> 1); // Java's >>> ; overflow-safe midpoint
				if (xp[mid] <= x)
					lo = mid;
				else
					hi = mid;
			}
			const float t = (x - xp[lo]) / (xp[hi] - xp[lo]);
			return fp[lo] + t * (fp[hi] - fp[lo]);
		}

		// Samples the channel's noise on a 1024x1024 grid at stride 32 over [0, 32768), sorts, and reads off
		// nQuantiles order statistics. Matches Python's _compute_map_stats (x/y in arange(0, 32*1024, 32)).
		oc::vector<float> buildNoiseQuantiles(const FastNoiseLite& fnl, int32 nQuantiles, float eps)
		{
			oc::vector<float> values((size_t)1024 * 1024);
			size_t k = 0;
			for (int32 r = 0; r < 1024; r++)
				for (int32 c = 0; c < 1024; c++)
					values[k++] = fnl.GetNoise((float)(c * 32), (float)(r * 32));

			// Java's Arrays.sort uses IEEE total order (-0.0 < 0.0, NaN last); oc::sort with < does not.
			// Perlin fBm produces neither NaN nor -0.0 here, so they agree.
			oc::sort(values.begin(), values.end());

			const int32 n = (int32)values.size();
			oc::vector<float> q((size_t)nQuantiles);
			for (int32 i = 0; i < nQuantiles; i++)
			{
				const float pct = eps + (float)i * (1.0f - 2.0f * eps) / (float)(nQuantiles - 1);
				const float idx = pct * (float)(n - 1);
				const int32 lo = (int32)idx; // idx >= 0, so truncation == floor
				const int32 hi = oc::min(lo + 1, n - 1);
				q[(size_t)i] = values[(size_t)lo] + (idx - (float)lo) * (values[(size_t)hi] - values[(size_t)lo]);
			}

			// Force strictly increasing (matches Python build_quantiles). LOAD-BEARING, not cosmetic:
			// interp() divides by xp[hi]-xp[lo].
			float minDiff = FLT_MAX;
			for (int32 i = 1; i < nQuantiles; i++)
				if (q[(size_t)i] > q[(size_t)i - 1])
					minDiff = oc::min(minDiff, q[(size_t)i] - q[(size_t)i - 1]);
			if (minDiff == FLT_MAX)
				minDiff = 1e-10f;
			for (int32 i = 1; i < nQuantiles; i++)
				if (q[(size_t)i] <= q[(size_t)i - 1])
					q[(size_t)i] = q[(size_t)i - 1] + minDiff * 0.1f;

			return q;
		}
	}

	struct SyntheticMapFactory::Impl
	{
		FastNoiseLite noises[N_CHANNELS];
		oc::vector<float> noiseQuantiles[N_CHANNELS]; // per seed
		const float* dataQuantiles[N_CHANNELS] = {};   // into m_dataStorage
		oc::vector<float> dataStorage;                // copy of PipelineData::dataQuantiles
		int32 nDataQuantiles = 0;
		float aTempStd = 0.0f, bTempStd = 0.0f, tempStdP1 = 0.0f, tempStdP99 = 0.0f;
	};

	SyntheticMapFactory::SyntheticMapFactory(uint64 worldSeed, const ModelConfig& cfg, const PipelineData& data)
		: m_impl(oc::make_unique<Impl>())
	{
		Impl& im = *m_impl;
		im.dataStorage = data.dataQuantiles;
		im.nDataQuantiles = data.nQuantiles;
		im.aTempStd = data.aTempStd;
		im.bTempStd = data.bTempStd;
		im.tempStdP1 = data.tempStdP1;
		im.tempStdP99 = data.tempStdP99;
		assert(im.dataStorage.size() == (size_t)N_CHANNELS * data.nQuantiles);
		for (int32 ch = 0; ch < N_CHANNELS; ch++)
			im.dataQuantiles[ch] = im.dataStorage.data() + (size_t)ch * data.nQuantiles;

		assert(cfg.frequencyMult.size() == (size_t)N_CHANNELS);
		for (int32 ch = 0; ch < N_CHANNELS; ch++)
		{
			// 31-bit mask: the per-channel seed is always non-negative, and this is NOT a plain narrowing
			// cast. worldSeed + ch + 1 wraps in Java; the mask makes the wrap irrelevant.
			const int32 seed = (int32)((worldSeed + (uint64)ch + 1ull) & 0x7FFFFFFFull);
			FastNoiseLite& fnl = im.noises[ch];
			fnl.SetSeed(seed);
			fnl.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
			fnl.SetFrequency(BASE_FREQUENCY * cfg.frequencyMult[(size_t)ch]);
			fnl.SetFractalType(FastNoiseLite::FractalType_FBm);
			fnl.SetFractalOctaves(OCTAVES[ch]);
			fnl.SetFractalLacunarity(LACUNARITY);
			// Gain last: SetFractalOctaves and SetFractalGain each recompute mFractalBounding but
			// SetFractalLacunarity does not, so this order leaves it correct. Don't reorder.
			fnl.SetFractalGain(GAIN);
		}

		// Embarrassingly parallel across channels, and the sort makes order irrelevant. Worth it: the
		// serial cost is ~0.5s in Release and several seconds in Debug, paid on every seed change.
		oc::vector<std::thread> workers;
		workers.reserve(N_CHANNELS);
		for (int32 ch = 0; ch < N_CHANNELS; ch++)
			workers.emplace_back([&im, ch]()
			{
				im.noiseQuantiles[ch] = buildNoiseQuantiles(im.noises[ch], NOISE_QUANTILES, QUANTILE_EPS);
			});
		for (std::thread& t : workers)
			t.join();
	}

	SyntheticMapFactory::~SyntheticMapFactory() = default;

	void SyntheticMapFactory::sample(int32 x1, int32 y1, int32 x2, int32 y2, oc::vector<float>& out) const
	{
		const Impl& im = *m_impl;
		const int32 H = y2 - y1;
		const int32 W = x2 - x1;
		assert(H > 0 && W > 0);
		const size_t plane = (size_t)H * W;
		out.assign((size_t)N_CHANNELS * plane, 0.0f);

		// Pass 1: noise -> quantile transform. result[r][c] = noise(x1+c, y1+r).
		for (int32 ch = 0; ch < N_CHANNELS; ch++)
		{
			const FastNoiseLite& fnl = im.noises[ch];
			const float* nq = im.noiseQuantiles[ch].data();
			const float* dq = im.dataQuantiles[ch];
			float* dst = out.data() + (size_t)ch * plane;
			size_t k = 0;
			for (int32 r = 0; r < H; r++)
				for (int32 c = 0; c < W; c++)
				{
					// int arithmetic then widen, exactly as Java does. Beyond |coord| ~ 2^24 this
					// quantises; the reference has the same latent limit.
					const float nv = fnl.GetNoise((float)(x1 + c), (float)(y1 + r));
					dst[k++] = interp(nv, nq, dq, NOISE_QUANTILES);
				}
		}
		// (The reference copies rawChannels into an identically-shaped array here. It is a pure no-op —
		// five full-buffer copies for nothing — and is deliberately not reproduced.)

		// Pass 2: sample_full_synthetic_map post-processing, in place.
		float* elevCh = out.data() + 0 * plane;
		float* tempCh = out.data() + 1 * plane;
		float* tStdCh = out.data() + 2 * plane;
		float* precCh = out.data() + 3 * plane;
		float* pStdCh = out.data() + 4 * plane;

		for (size_t i = 0; i < plane; i++)
		{
			const float elev = elevCh[i];
			float temp = tempCh[i];
			float tempStd = tStdCh[i];
			const float precip = precCh[i];
			float precipStd = pStdCh[i];

			// Temperature: lapse-rate correction against elevation. Wetter air -> shallower lapse rate.
			float lapseRate = -6.5f + 0.0015f * precip;
			lapseRate = clampf(lapseRate, -9.8f, -4.0f) / 1000.0f; // C/m
			temp = temp + lapseRate * (elev > 0.0f ? elev : 0.0f);
			temp = clampf(temp, -10.0f, 40.0f);

			// Temperature seasonality, re-baselined against the corrected temperature.
			const float baseline = im.aTempStd * temp + im.bTempStd;
			const float t01 = (tempStd - im.tempStdP1) / (im.tempStdP99 - im.tempStdP1);
			const float baselineClipped = oc::max(im.tempStdP1, -baseline);
			tempStd = t01 * (im.tempStdP99 - baselineClipped) + baselineClipped + baseline;
			tempStd = oc::max(tempStd, 20.0f);

			// Precipitation seasonality falls off as precipitation rises.
			precipStd = precipStd * oc::max(0.0f, (185.0f - 0.04111f * precip) / 185.0f);

			// Elevation is fed to the model signed-sqrt compressed. WorldPipeline inverts this at the end.
			// float * double -> double -> float, as in Java (Math.sqrt is double).
			const float elevSqrt = (float)((double)javaSignum(elev) * std::sqrt((double)std::abs(elev)));

			elevCh[i] = elevSqrt;
			tempCh[i] = temp;
			tStdCh[i] = tempStd;
			pStdCh[i] = precipStd;
			// precip (channel 3) passes through unchanged.
		}
	}
}
