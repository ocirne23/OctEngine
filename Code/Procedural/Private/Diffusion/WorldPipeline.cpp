module Procedural;

import Core;
import Core.Log;
import :Diffusion.InfiniteTensor;
import :Diffusion.Laplacian;
import :Diffusion.ModelAssets;
import :Diffusion.Onnx;
import :Diffusion.Pipeline;
import :Diffusion.Rng;
import :Diffusion.Scheduler;
import :Diffusion.SyntheticMap;
import :Diffusion.Tensor;

namespace Procedural::Diffusion
{
	namespace
	{
		constexpr int32 COARSE_TILE_SIZE = 64;
		constexpr int32 COARSE_TILE_STRIDE = 48;
		constexpr int32 COARSE_STEPS = 20;
		constexpr int32 LATENT_TILE_SIZE = 64;
		// MUST be exactly LATENT_TILE_SIZE / 2. This is NOT a tunable overlap/cost dial, however much it
		// looks like one — the latent stage silently produces a garbage world at any other value. Measured,
		// on a region whose correct elevation is 3017..5708 m (100% land):
		//     stride 16 -> -128..1661 m,  93% land   (wrong)
		//     stride 32 ->  3017..5708 m, 100% land  (correct)
		//     stride 48 -> -4494..-4263 m,  0% land  (mountain range comes out as deep ocean)
		//     stride 64 -> -4483..-3892 m,  0% land
		// Only SIZE/2 is right, and 64 being just as broken as 48 rules out any coarse-lattice alignment
		// story: at 50% overlap the tent windows sum to exactly 1 (a partition of unity), and this stage
		// depends on that. Blending LATENTS is not blending outputs — the weight-channel normalisation that
		// makes the other stages stride-agnostic does not rescue this one.
		// The failure is silent and does not look like a bug: overlapping windows still AGREE with each
		// other to 7 cm, so any tile-agreement check reports "consistent" while the terrain is inverted.
		// Catching it needs an ABSOLUTE check — the native surface against the coarse stage's own
		// prediction for the same region — not tiles compared against each other.
		constexpr int32 LATENT_TILE_STRIDE = LATENT_TILE_SIZE / 2;

		// Batch sizes, chosen from measured DirectML scaling on these models rather than the reference's
		// values (it uses 4 for latent and never batches coarse). Per-item cost at batch 1 -> 4 -> 8:
		//   base    43.7 -> 32.9 -> 21.9 ms
		//   coarse   6.1 ->  4.9 ->  3.3 ms
		//   decoder 48.8 -> 59.7 -> 51.9 ms  (gets WORSE: the decoder is compute-bound, so batching only
		//                                     adds latency. It deliberately stays unbatched.)
		// 8 is where it stops paying: a later sweep at 16 and 32 moved base by 0% (675 -> 674 -> 691 ms)
		// while cutting dispatches 4x, which is the evidence that this pipeline is NOT dispatch-bound and
		// that neither bigger batches nor fp16 nor IO binding has anything left to reclaim. The only wins
		// left are algorithmic — do less work (see the stride above), not dispatch it better.
		// Both models take a dynamic batch dim, and a partial final batch is fine.
		constexpr int32 LATENT_BATCH = 8;
		constexpr int32 COARSE_BATCH = 8;
		constexpr int32 DECODER_TILE_SIZE = 256;
		// Unlike the latent stride above, this one IS a real overlap/cost dial: every value tried (192, 224,
		// 240, 256) reproduces the reference elevation to within ~0.1% (3017..5708 m -> 3024..5706 at 224),
		// because the decoder's weight-channel normalisation genuinely is stride-agnostic.
		// 192 -> 224 measured: decoder 401 -> 347 ms, total inference ~1300 -> ~1156 ms (-11%). Less than
		// the tile count suggests — wider tiles need more latent tiles around the edges (74 -> 86 items), so
		// part of the decoder saving is handed back to the latent stage. 240/256 gave nothing further.
		// Stopping at 224 (1.14x redundancy): the decoder draws the visible fine detail, so it is the stage
		// where a too-thin blend window would actually be seen, and a range check cannot detect a seam.
		constexpr int32 DECODER_TILE_STRIDE = 224;
		constexpr size_t CACHE_LIMIT_BYTES = 100ull * 1024 * 1024; // per stage, as in the reference

		// The low-frequency elevation band's normalisation. Hardcoded in the reference too (not config).
		constexpr float LOWFREQ_MEAN = -31.4f;
		constexpr float LOWFREQ_STD = 38.6f;

		// Latent-conditioning normalisation: 6 coarse channels + a mask channel.
		constexpr float COND_MEANS[7] = { 14.99f, 11.65f, 15.87f, 619.26f, 833.12f, 69.40f, 0.66f };
		constexpr float COND_STDS[7] = { 21.72f, 21.78f, 10.40f, 452.29f, 738.09f, 34.59f, 0.47f };
		// mp_concat component sizes: 16 + 16 + 4 + 16 + 5 + 1 = 58
		constexpr int32 COND_DIMS[6] = { 16, 16, 4, 16, 5, 1 };
		constexpr int32 COND_TOTAL = 58;

		// Separable tent: 1.0 at the tile centre falling linearly to eps at each edge. Every stage
		// pre-multiplies its data channels by this and ships the raw window as the last channel, so summing
		// overlapping tiles and dividing by the summed weight yields a smooth partition-of-unity blend.
		oc::vector<float> linearWeightWindow(int32 size)
		{
			oc::vector<float> w((size_t)size * size);
			const float mid = (float)(size - 1) / 2.0f;
			const float eps = 1e-3f;
			for (int32 r = 0; r < size; r++)
			{
				const float wy = 1.0f - (1.0f - eps) * oc::min(1.0f, std::abs((float)r - mid) / mid);
				for (int32 c = 0; c < size; c++)
				{
					const float wx = 1.0f - (1.0f - eps) * oc::min(1.0f, std::abs((float)c - mid) / mid);
					w[(size_t)r * size + c] = wy * wx;
				}
			}
			return w;
		}

		// Recover a value from the weighted-sum/weight pair. The guard is the reference's; see InfiniteTensor
		// for why the sums are weighted in the first place.
		float unweight(float weightedValue, float weight)
		{
			return (weight > 1e-6f) ? weightedValue / weight : 0.0f;
		}

		void nearestUpsample(const float* src, int32 C, int32 sH, int32 sW, int32 dH, int32 dW, float* dst)
		{
			for (int32 c = 0; c < C; c++)
				for (int32 r = 0; r < dH; r++)
				{
					const int32 sr = r * sH / dH; // integer division, as in the reference
					const float* srow = src + (size_t)c * sH * sW + (size_t)sr * sW;
					float* drow = dst + (size_t)c * dH * dW + (size_t)r * dW;
					for (int32 col = 0; col < dW; col++)
						drow[col] = srow[col * sW / dW];
				}
		}

		// Bilinear sample of a grid at a fractional index, clamped to the grid.
		float bilinearSample(const Grid& g, float gy, float gx)
		{
			const float y = oc::clamp(gy, 0.0f, (float)(g.h - 1));
			const float x = oc::clamp(gx, 0.0f, (float)(g.w - 1));
			const int32 y0 = (int32)y, x0 = (int32)x;
			const int32 y1 = oc::min(g.h - 1, y0 + 1), x1 = oc::min(g.w - 1, x0 + 1);
			const float wy = y - (float)y0, wx = x - (float)x0;
			return (1 - wy) * (1 - wx) * g.at(y0, x0) + (1 - wy) * wx * g.at(y0, x1)
			     + wy * (1 - wx) * g.at(y1, x0) + wy * wx * g.at(y1, x1);
		}

		float javaSignum(float x)
		{
			if (x > 0.0f)
				return 1.0f;
			if (x < 0.0f)
				return -1.0f;
			return x;
		}
	}

	WorldPipeline::WorldPipeline() = default;
	WorldPipeline::~WorldPipeline() = default;

	bool WorldPipeline::isValid() const { return m_valid; }

	uint64 WorldPipeline::totalComputedWindowCount() const
	{
		return m_tileStore ? m_tileStore->getTotalComputedWindowCount() : 0;
	}

	size_t WorldPipeline::residentCacheBytes() const
	{
		return m_tileStore ? m_tileStore->getResidentBytes() : 0;
	}

	oc::string WorldPipeline::inferenceStatsText() const
	{
		auto fmt = [](oc::string_view name, const OnnxModel::RunStats& s)
		{
			if (s.calls == 0)
				return oc::format("{} idle", name);
			return oc::format("{} {} calls / {} items, {:.0f} ms ({:.2f} ms/call)",
			                   name, s.calls, s.batchItems, s.totalMs, s.totalMs / (double)s.calls);
		};
		return oc::format("{} | {} | {}", fmt("coarse", m_coarseModel.stats()),
		                   fmt("base", m_baseModel.stats()), fmt("decoder", m_decoderModel.stats()));
	}

	void WorldPipeline::resetInferenceStats()
	{
		m_coarseModel.resetStats();
		m_baseModel.resetStats();
		m_decoderModel.resetStats();
	}

	bool WorldPipeline::initialize(uint64 seed, const ModelConfig& cfg, const PipelineData& data,
	                               EInferenceDevice device, bool coarseOnly, EPrecision precision)
	{
		m_seed = seed;
		m_config = cfg;
		m_data = data;
		m_coarseOnly = coarseOnly;

		// The decoder stage divides S and ST by latentCompression; nothing in the reference checks that it
		// divides evenly, which would silently misalign the latent patch it reads.
		if (!coarseOnly && (DECODER_TILE_SIZE % cfg.latentCompression != 0 ||
		                    DECODER_TILE_STRIDE % cfg.latentCompression != 0))
		{
			Log::error(oc::format("[Diffusion] latent_compression={} does not divide the decoder tile "
			                       "size/stride ({}/{})", cfg.latentCompression, DECODER_TILE_SIZE,
			                       DECODER_TILE_STRIDE));
			return false;
		}

		// Precision resolves PER MODEL: an fp16 file that was never converted falls back to its fp32
		// original rather than failing the load, so a partial conversion still runs.
		if (!m_coarseModel.load(ModelAssets::modelPath("coarse_model", precision), "coarse", device))
			return false;
		if (!coarseOnly)
		{
			if (!m_baseModel.load(ModelAssets::modelPath("base_model", precision), "base", device))
				return false;
			if (!m_decoderModel.load(ModelAssets::modelPath("decoder_model", precision), "decoder", device))
				return false;
		}

		m_condVals.resize(m_config.condSnr.size());
		for (size_t i = 0; i < m_config.condSnr.size(); i++)
			m_condVals[i] = (float)std::log((double)m_config.condSnr[i] / 8.0);

		// mp_concat scales: C / sqrt(dim_i) / k, where C = sqrt(sum(dims) * k).
		{
			int32 sumN = 0;
			for (int32 n : COND_DIMS)
				sumN += n;
			const int32 k = (int32)ARRAY_SIZE(COND_DIMS);
			const float C = (float)std::sqrt((double)sumN * (double)k);
			m_mpConcatScales.resize((size_t)k);
			for (int32 i = 0; i < k; i++)
				m_mpConcatScales[(size_t)i] = C / (float)std::sqrt((double)COND_DIMS[i]) / (float)k;
		}

		m_ww64 = linearWeightWindow(COARSE_TILE_SIZE);
		m_ww256 = linearWeightWindow(DECODER_TILE_SIZE);

		const auto t0 = Clock::now();
		m_syntheticMap = oc::make_unique<SyntheticMapFactory>(m_seed, m_config, m_data);
		const auto t1 = Clock::now();
		Log::verbose(oc::format("[Diffusion] synthetic map built in {} ms",
		                         std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));

		m_tileStore = oc::make_unique<MemoryTileStore>();
		buildCoarseStage();
		if (!coarseOnly)
		{
			buildLatentStage();
			buildDecoderStage();
		}

		m_valid = true;
		return true;
	}

	void WorldPipeline::setSeed(uint64 newSeed)
	{
		if (newSeed == m_seed || !m_valid)
			return;
		m_seed = newSeed;
		// The stage lambdas read m_seed through `this`, so they pick this up with no rebuild — matching the
		// reference, where seed is volatile and read through the lambda's enclosing instance. Capturing the
		// seed BY VALUE in the lambda would silently break reseeding.
		m_syntheticMap = oc::make_unique<SyntheticMapFactory>(m_seed, m_config, m_data);
		m_tileStore->clearAllCaches();
	}

	// =====================================================================================================
	// Coarse stage
	// =====================================================================================================

	void WorldPipeline::buildCoarseStage()
	{
		const int32 S = COARSE_TILE_SIZE, ST = COARSE_TILE_STRIDE;
		const int32 outSize[3] = { 7, S, S };
		const int32 outStride[3] = { 7, ST, ST };
		const TensorWindow outWin(outSize, outStride);

		m_coarse = m_tileStore->getOrCreateBatched(
			"base_coarse_map", 3,
			[this](oc::span<const WindowKey> wis, oc::span<const oc::vector<FloatTensor>>)
			{
				return coarseBatch(wis);
			},
			outWin, {}, {}, CACHE_LIMIT_BYTES, COARSE_BATCH);
	}

	oc::vector<FloatTensor> WorldPipeline::coarseBatch(oc::span<const WindowKey> windowIndices)
	{
		const int32 S = COARSE_TILE_SIZE, ST = COARSE_TILE_STRIDE;
		const size_t plane = (size_t)S * S;
		const int32 batch = (int32)windowIndices.size();
		static constexpr int32 MEAN_IDX[5] = { 0, 2, 3, 4, 5 };

		// --- Per-tile conditioning + initial noise ---------------------------------------------------
		oc::vector<float> condMixed((size_t)batch * 5 * plane);
		oc::vector<float> sample((size_t)batch * 6 * plane);
		oc::vector<float> syn, condNoise((size_t)5 * plane);

		EDMScheduler protoSched(COARSE_STEPS);
		const float sigma0 = protoSched.sigmas()[0];

		for (int32 b = 0; b < batch; b++)
		{
			const int32 i1 = windowIndices[(size_t)b].v[1] * ST; // output window offset is 0
			const int32 j1 = windowIndices[(size_t)b].v[2] * ST;

			// Conditioning: the synthetic climate sketch.
			// The i/j swap is deliberate and load-bearing (the reference carries the same note): i is row/y,
			// j is column/x, and sample() takes (x1, y1, x2, y2).
			m_syntheticMap->sample(j1, i1, j1 + S, i1 + S, syn);

			// Temperature channel: compress everything at or below 20 C away from 20.
			{
				float* temp = syn.data() + 1 * plane;
				for (size_t px = 0; px < plane; px++)
					if (temp[px] <= 20.0f)
						temp[px] = (temp[px] - 20.0f) * 1.25f + 20.0f;
			}

			gaussianNoisePatch(m_seed, i1, j1, S, S, 5, S, S, condNoise.data());

			// Normalise, then mix toward noise at the configured SNR. Note the index map: the synthetic map
			// has no p5 channel, so it lines up with model channels [0, 2, 3, 4, 5], skipping 1.
			for (int32 ch = 0; ch < 5; ch++)
			{
				const float mean = m_config.coarseMeans[(size_t)MEAN_IDX[ch]];
				const float stdv = m_config.coarseStds[(size_t)MEAN_IDX[ch]];
				const double t = std::atan((double)m_config.condSnr[(size_t)ch]);
				const float cosT = (float)std::cos(t);
				const float sinT = (float)std::sin(t);

				const float* src = syn.data() + (size_t)ch * plane;
				const float* cn = condNoise.data() + (size_t)ch * plane;
				float* dst = condMixed.data() + ((size_t)b * 5 + ch) * plane;
				for (size_t px = 0; px < plane; px++)
					dst[px] = cosT * ((src[px] - mean) / stdv) + sinT * cn[px];
			}

			// Initial sample: pure noise at sigma_max.
			float* s = sample.data() + (size_t)b * 6 * plane;
			gaussianNoisePatch(m_seed + 1, i1, j1, S, S, 6, S, S, s);
			for (size_t k = 0; k < 6 * plane; k++)
				s[k] *= sigma0;
		}

		// --- 20-step DPM-Solver++, all tiles stepping together ---------------------------------------
		// Every tile shares the sigma schedule, so step k is one dispatch for the whole batch. Each tile
		// still needs its own scheduler: the multistep solver carries per-tile history.
		oc::vector<EDMScheduler> scheds;
		scheds.reserve((size_t)batch);
		for (int32 b = 0; b < batch; b++)
			scheds.emplace_back(COARSE_STEPS);

		oc::vector<float> xIn((size_t)batch * 11 * plane);
		oc::vector<float> scaledIn(6 * plane);
		oc::vector<float> modelOut;
		oc::vector<float> cnoise((size_t)batch);
		oc::vector<float> condScalar((size_t)batch);
		const int64 xShape[4] = { batch, 11, S, S };
		const int64 scalarShape[1] = { batch };
		static const char* const COND_NAMES[5] = { "cond_0", "cond_1", "cond_2", "cond_3", "cond_4" };

		oc::vector<oc::vector<float>> condScalars((size_t)5);
		for (int32 c = 0; c < 5; c++)
			condScalars[(size_t)c].assign((size_t)batch, m_condVals[(size_t)c]);

		auto failed = [&]() -> oc::vector<FloatTensor>
		{
			m_inferenceFailed = true;
			oc::vector<FloatTensor> r;
			for (int32 b = 0; b < batch; b++)
				r.emplace_back(oc::array<int32, 3>{ 7, S, S });
			return r;
		};

		for (int32 step = 0; step < COARSE_STEPS; step++)
		{
			const float sigma = protoSched.sigmas()[(size_t)step];
			oc::fill(cnoise.begin(), cnoise.end(), EDMScheduler::trigflowPreconditionNoise(sigma));

			for (int32 b = 0; b < batch; b++)
			{
				const oc::span<const float> s(sample.data() + (size_t)b * 6 * plane, 6 * plane);
				EDMScheduler::preconditionInputs(s, sigma, scaledIn);
				// x = [6 noisy sample channels | 5 mixed conditioning channels]
				float* dst = xIn.data() + (size_t)b * 11 * plane;
				oc::copy(scaledIn.begin(), scaledIn.end(), dst);
				const float* cm = condMixed.data() + (size_t)b * 5 * plane;
				oc::copy(cm, cm + 5 * plane, dst + 6 * plane);
			}

			OnnxInput inputs[7];
			inputs[0] = { "x", xIn, xShape };
			inputs[1] = { "noise_labels", cnoise, scalarShape };
			for (int32 c = 0; c < 5; c++)
				inputs[(size_t)2 + c] = { COND_NAMES[c], condScalars[(size_t)c], scalarShape };

			if (!m_coarseModel.run(inputs, modelOut))
				return failed();
			assert(modelOut.size() == (size_t)batch * 6 * plane);

			// Each tile advances its own solver state, in place on its slice of the shared batch buffer.
			for (int32 b = 0; b < batch; b++)
			{
				const oc::span<const float> mo(modelOut.data() + (size_t)b * 6 * plane, 6 * plane);
				const oc::span<float> s(sample.data() + (size_t)b * 6 * plane, 6 * plane);
				scheds[(size_t)b].step(mo, s);
			}
		}

		// --- Denormalise + emit ----------------------------------------------------------------------
		oc::vector<FloatTensor> results;
		results.reserve((size_t)batch);
		oc::vector<float> out(6 * plane);
		for (int32 b = 0; b < batch; b++)
		{
			// The coarse output is NOT negated (unlike base/decoder) — it goes through the scheduler's EDM
			// output preconditioning instead.
			for (int32 ch = 0; ch < 6; ch++)
			{
				const float mean = m_config.coarseMeans[(size_t)ch];
				const float stdv = m_config.coarseStds[(size_t)ch];
				const float* src = sample.data() + ((size_t)b * 6 + ch) * plane;
				float* dst = out.data() + (size_t)ch * plane;
				for (size_t px = 0; px < plane; px++)
					dst[px] = (src[px] / EDMScheduler::SIGMA_DATA) * stdv + mean;
			}
			// Channel 1 is emitted as an offset from channel 0; recover the p5 value.
			for (size_t px = 0; px < plane; px++)
				out[plane + px] = out[px] - out[plane + px];

			// Emit 7 channels: the 6 data channels pre-multiplied by the tent window, then the raw window.
			FloatTensor result(oc::array<int32, 3>{ 7, S, S });
			for (int32 ch = 0; ch < 6; ch++)
			{
				const float* src = out.data() + (size_t)ch * plane;
				float* dst = result.data.data() + (size_t)ch * plane;
				for (size_t px = 0; px < plane; px++)
					dst[px] = src[px] * m_ww64[px];
			}
			oc::copy(m_ww64.begin(), m_ww64.end(), result.data.begin() + (ptrdiff_t)(6 * plane));
			results.push_back(oc::move(result));
		}
		return results;
	}

	FloatTensor WorldPipeline::getCoarseSlice(int32 ci0, int32 cj0, int32 ci1, int32 cj1)
	{
		assert(m_valid);
		const int32 start[3] = { 0, ci0, cj0 };
		const int32 end[3] = { 7, ci1, cj1 };
		return m_coarse->getSlice(start, end);
	}

	// =====================================================================================================
	// Latent stage — 2 flow-matching steps, batched
	// =====================================================================================================

	void WorldPipeline::buildLatentStage()
	{
		const int32 S = LATENT_TILE_SIZE, ST = LATENT_TILE_STRIDE;
		const int32 outSize[3] = { 6, S, S };
		const int32 outStride[3] = { 6, ST, ST };
		const TensorWindow outWin(outSize, outStride);

		// The hinge of the whole coordinate system: a latent tile INDEX steps 1 coarse pixel, and reads a
		// 4x4 coarse patch with a 1-pixel halo (hence the -1 offset). Since a latent tile advances 32 latent
		// pixels per index step, 1 coarse pixel == 32 * latentCompression native pixels.
		const int32 coarseSize[3] = { 7, 4, 4 };
		const int32 coarseStride[3] = { 7, 1, 1 };
		const int32 coarseOffset[3] = { 0, -1, -1 };
		const TensorWindow coarseWin(coarseSize, coarseStride, coarseOffset);

		const float tInit = (float)std::atan((double)(EDMScheduler::SIGMA_MAX / EDMScheduler::SIGMA_DATA));
		InfiniteTensor* coarseDep[1] = { m_coarse };
		const TensorWindow coarseDepWin[1] = { coarseWin };

		m_initLatent = m_tileStore->getOrCreateBatched(
			"init_latent_map", 3,
			[this, tInit](oc::span<const WindowKey> wis, oc::span<const oc::vector<FloatTensor>> args)
			{
				return latentBatch(wis, nullptr, args[0], tInit, 5819);
			},
			outWin, coarseDep, coarseDepWin, CACHE_LIMIT_BYTES, LATENT_BATCH);

		const float interT = (float)std::atan((double)(0.35f / EDMScheduler::SIGMA_DATA));
		InfiniteTensor* stepDeps[2] = { m_initLatent, m_coarse };
		const TensorWindow stepDepWins[2] = { outWin, coarseWin };

		m_latents = m_tileStore->getOrCreateBatched(
			"step_latent_map_0", 3,
			[this, interT](oc::span<const WindowKey> wis, oc::span<const oc::vector<FloatTensor>> args)
			{
				return latentBatch(wis, &args[0], args[1], interT, 5820);
			},
			outWin, stepDeps, stepDepWins, CACHE_LIMIT_BYTES, LATENT_BATCH);
	}

	void WorldPipeline::buildLatentConditioning(const FloatTensor& coarseSlice, float* out58) const
	{
		constexpr int32 N = 16; // 4x4 coarse patch
		assert(coarseSlice.data.size() == 7 * N);

		// Recover the 6 coarse channels, then normalise. NaN -> 0 (the reference guards this; a coarse
		// pixel with zero weight would otherwise poison the whole conditioning vector).
		float condImg7[7 * N];
		for (int32 ch = 0; ch < 6; ch++)
			for (int32 px = 0; px < N; px++)
			{
				const float raw = unweight(coarseSlice.data[(size_t)ch * N + px], coarseSlice.data[(size_t)6 * N + px]);
				const float v = (raw - COND_MEANS[ch]) / COND_STDS[ch];
				condImg7[ch * N + px] = std::isnan(v) ? 0.0f : v;
			}
		// Channel 6 is an all-ones mask (this region is always "present"), normalised the same way.
		{
			const float maskNorm = (1.0f - COND_MEANS[6]) / COND_STDS[6];
			for (int32 px = 0; px < N; px++)
				condImg7[6 * N + px] = maskNorm;
		}

		// Climate components average only the INNER 2x2 of the patch — the halo is context for the model,
		// not part of the summary.
		float climateMeans[4];
		for (int32 ch = 0; ch < 4; ch++)
		{
			float sum = 0.0f;
			for (int32 r = 1; r < 3; r++)
				for (int32 c = 1; c < 3; c++)
					sum += condImg7[(2 + ch) * N + r * 4 + c];
			climateMeans[ch] = sum / 4.0f;
			if (std::isnan(climateMeans[ch]))
				climateMeans[ch] = 0.0f;
		}

		const float noiseLevelNorm = (0.0f - 0.5f) * (float)std::sqrt(12.0);

		// mp_concat: [means | p5 | climate | mask | histogram | noise level], each scaled to preserve
		// magnitude across the concatenation.
		int32 off = 0;
		auto append = [&](const float* src, int32 count, float scale)
		{
			for (int32 i = 0; i < count; i++)
				out58[off++] = src[i] * scale;
		};
		append(condImg7 + 0 * N, 16, m_mpConcatScales[0]);       // channel 0: elevation means
		append(condImg7 + 1 * N, 16, m_mpConcatScales[1]);       // channel 1: p5
		append(climateMeans, 4, m_mpConcatScales[2]);
		append(condImg7 + 6 * N, 16, m_mpConcatScales[3]);       // mask
		append(m_config.histogramRaw.data(), 5, m_mpConcatScales[4]);
		out58[off++] = noiseLevelNorm * m_mpConcatScales[5];
		assert(off == COND_TOTAL);
	}

	oc::vector<FloatTensor> WorldPipeline::latentBatch(oc::span<const WindowKey> windowIndices,
	                                                    const oc::vector<FloatTensor>* prevSamples,
	                                                    const oc::vector<FloatTensor>& coarseSlices,
	                                                    float t, uint64 seedOffset)
	{
		const int32 S = LATENT_TILE_SIZE, ST = LATENT_TILE_STRIDE;
		const size_t plane = (size_t)S * S;
		const size_t chans = 5;
		const int32 batch = (int32)windowIndices.size();
		const float cosT = (float)std::cos((double)t);
		const float sinT = (float)std::sin((double)t);

		oc::vector<oc::vector<float>> xTArr((size_t)batch);
		oc::vector<float> modelIn((size_t)batch * chans * plane);
		oc::vector<float> cond((size_t)batch * COND_TOTAL);
		oc::vector<float> noise(chans * plane);

		for (int32 b = 0; b < batch; b++)
		{
			const int32 i1 = windowIndices[(size_t)b].v[1] * ST;
			const int32 j1 = windowIndices[(size_t)b].v[2] * ST;

			buildLatentConditioning(coarseSlices[(size_t)b], cond.data() + (size_t)b * COND_TOTAL);

			// The chained step starts from the previous step's (weight-normalised) output; the init step
			// starts from zero.
			oc::vector<float> sample(chans * plane, 0.0f);
			if (prevSamples)
			{
				const FloatTensor& ps = (*prevSamples)[(size_t)b];
				for (size_t ch = 0; ch < chans; ch++)
					for (size_t px = 0; px < plane; px++)
					{
						const float w = ps.data[chans * plane + px];
						sample[ch * plane + px] = unweight(ps.data[ch * plane + px], w) * EDMScheduler::SIGMA_DATA;
					}
			}

			gaussianNoisePatch(m_seed + seedOffset, i1, j1, S, S, (int32)chans, S, S, noise.data());

			oc::vector<float>& xT = xTArr[(size_t)b];
			xT.resize(chans * plane);
			for (size_t k = 0; k < chans * plane; k++)
			{
				const float z = noise[k] * EDMScheduler::SIGMA_DATA;
				xT[k] = cosT * sample[k] + sinT * z;
				modelIn[(size_t)b * chans * plane + k] = xT[k] / EDMScheduler::SIGMA_DATA;
			}
		}

		oc::vector<float> noiseLabels((size_t)batch, t);
		const int64 xShape[4] = { batch, (int64)chans, S, S };
		const int64 nlShape[1] = { batch };
		const int64 condShape[2] = { batch, COND_TOTAL };

		OnnxInput inputs[3];
		inputs[0] = { "x", modelIn, xShape };
		inputs[1] = { "noise_labels", noiseLabels, nlShape };
		inputs[2] = { "cond_0", cond, condShape };

		oc::vector<float> pred;
		oc::vector<FloatTensor> results;
		results.reserve((size_t)batch);

		if (!m_baseModel.run(inputs, pred))
		{
			m_inferenceFailed = true;
			for (int32 b = 0; b < batch; b++)
				results.emplace_back(oc::array<int32, 3>{ 6, S, S });
			return results;
		}
		assert(pred.size() == (size_t)batch * chans * plane);

		for (int32 b = 0; b < batch; b++)
		{
			const oc::vector<float>& xT = xTArr[(size_t)b];
			FloatTensor out(oc::array<int32, 3>{ 6, S, S });
			for (size_t ch = 0; ch < chans; ch++)
				for (size_t px = 0; px < plane; px++)
				{
					const size_t k = ch * plane + px;
					// The base model's output is NEGATED by the caller (as in the reference).
					const float p = -pred[(size_t)b * chans * plane + k];
					const float v = (cosT * xT[k] - sinT * EDMScheduler::SIGMA_DATA * p) / EDMScheduler::SIGMA_DATA;
					out.data[k] = v * m_ww64[px];
				}
			oc::copy(m_ww64.begin(), m_ww64.end(), out.data.begin() + (ptrdiff_t)(chans * plane));
			results.push_back(oc::move(out));
		}
		return results;
	}

	// =====================================================================================================
	// Decoder stage — 1 flow-matching step to the high-frequency residual
	// =====================================================================================================

	void WorldPipeline::buildDecoderStage()
	{
		const int32 S = DECODER_TILE_SIZE, ST = DECODER_TILE_STRIDE, lc = m_config.latentCompression;
		const int32 outSize[3] = { 2, S, S };
		const int32 outStride[3] = { 2, ST, ST };
		const TensorWindow outWin(outSize, outStride);

		const int32 inpSize[3] = { 6, S / lc, S / lc };
		const int32 inpStride[3] = { 6, ST / lc, ST / lc };
		const TensorWindow inpWin(inpSize, inpStride);

		const float t = (float)std::atan((double)(EDMScheduler::SIGMA_MAX / EDMScheduler::SIGMA_DATA));
		InfiniteTensor* deps[1] = { m_latents };
		const TensorWindow depWins[1] = { inpWin };

		m_residual = m_tileStore->getOrCreate(
			"init_residual_map", 3,
			[this, t](oc::span<const int32> wi, oc::span<const FloatTensor> args)
			{
				return decoderTile(wi, args[0], t);
			},
			outWin, deps, depWins, CACHE_LIMIT_BYTES);
	}

	FloatTensor WorldPipeline::decoderTile(oc::span<const int32> windowIndex, const FloatTensor& latentSlice, float t)
	{
		const int32 S = DECODER_TILE_SIZE, ST = DECODER_TILE_STRIDE, lc = m_config.latentCompression;
		const int32 Slc = S / lc;
		const size_t plane = (size_t)S * S;
		const size_t lplane = (size_t)Slc * Slc;
		const int32 i1 = windowIndex[1] * ST, j1 = windowIndex[2] * ST;
		const float cosT = (float)std::cos((double)t);
		const float sinT = (float)std::sin((double)t);

		// Recover latent channels 0..3 (channel 4 is the low-frequency elevation band, used by computeElev;
		// channel 5 is the weight).
		oc::vector<float> lat(4 * lplane);
		for (size_t ch = 0; ch < 4; ch++)
			for (size_t px = 0; px < lplane; px++)
				lat[ch * lplane + px] = unweight(latentSlice.data[ch * lplane + px], latentSlice.data[5 * lplane + px]);

		oc::vector<float> upsampled(4 * plane);
		nearestUpsample(lat.data(), 4, Slc, Slc, S, S, upsampled.data());

		// One flow-matching step from a zero sample, so x_t is pure noise scaled by sin(t).
		oc::vector<float> noise(plane);
		gaussianNoisePatch(m_seed + 5819, i1, j1, S, S, 1, S, S, noise.data());
		oc::vector<float> xT(plane);
		for (size_t k = 0; k < plane; k++)
			xT[k] = sinT * noise[k] * EDMScheduler::SIGMA_DATA;

		// model_in = [xT/sigma_data | upsampled latents] -> 5 channels
		oc::vector<float> modelIn(5 * plane);
		for (size_t k = 0; k < plane; k++)
			modelIn[k] = xT[k] / EDMScheduler::SIGMA_DATA;
		oc::copy(upsampled.begin(), upsampled.end(), modelIn.begin() + (ptrdiff_t)plane);

		const int64 xShape[4] = { 1, 5, S, S };
		const int64 nlShape[1] = { 1 };
		OnnxInput inputs[2];
		inputs[0] = { "x", modelIn, xShape };
		inputs[1] = { "noise_labels", oc::span<const float>(&t, 1), nlShape };

		oc::vector<float> rawPred;
		FloatTensor result(oc::array<int32, 3>{ 2, S, S });
		if (!m_decoderModel.run(inputs, rawPred))
		{
			m_inferenceFailed = true;
			return result;
		}
		assert(rawPred.size() == plane);

		for (size_t k = 0; k < plane; k++)
		{
			const float p = -rawPred[k]; // decoder output is negated by the caller too
			const float v = (cosT * xT[k] - sinT * EDMScheduler::SIGMA_DATA * p) / EDMScheduler::SIGMA_DATA;
			result.data[k] = v * m_ww256[k];
		}
		oc::copy(m_ww256.begin(), m_ww256.end(), result.data.begin() + (ptrdiff_t)plane);
		return result;
	}

	// =====================================================================================================
	// Elevation / climate assembly
	// =====================================================================================================

	void WorldPipeline::computeElev(int32 i1, int32 j1, int32 i2, int32 j2, oc::vector<float>& outElev,
	                                oc::vector<float>* outMacro)
	{
		const int32 lc = m_config.latentCompression;
		constexpr float sigma = 5.0f;
		const int32 ks = ((int32)(sigma * 2.0f) / 2) * 2 + 1; // 11
		const int32 padLr = ks / 2 + 1;                       // 6 (low-res pixels)
		const int32 padHr = padLr * lc;                       // 48 (native pixels)

		// Pad, snapped to the latent lattice so the low-res grid divides evenly.
		const int32 pi1 = floorDiv(i1 - padHr, lc) * lc;
		const int32 pj1 = floorDiv(j1 - padHr, lc) * lc;
		const int32 pi2 = -floorDiv(-(i2 + padHr), lc) * lc; // ceil
		const int32 pj2 = -floorDiv(-(j2 + padHr), lc) * lc;
		const int32 pH = pi2 - pi1, pW = pj2 - pj1;

		// High-frequency residual at native resolution.
		const int32 resStart[3] = { 0, pi1, pj1 };
		const int32 resEnd[3] = { 2, pi2, pj2 };
		const FloatTensor resSlice = m_residual->getSlice(resStart, resEnd);
		Grid residual(pH, pW);
		{
			const size_t plane = (size_t)pH * pW;
			for (size_t i = 0; i < plane; i++)
				residual.data[i] = unweight(resSlice.data[i], resSlice.data[plane + i])
					* m_config.residualStd + m_config.residualMean;
		}

		// Low-frequency band from latent channel 4.
		const int32 lH = pH / lc, lW = pW / lc;
		const int32 latStart[3] = { 0, floorDiv(pi1, lc), floorDiv(pj1, lc) };
		const int32 latEnd[3] = { 6, floorDiv(pi2, lc), floorDiv(pj2, lc) };
		const FloatTensor latSlice = m_latents->getSlice(latStart, latEnd);
		Grid lowfreq(lH, lW);
		{
			const size_t plane = (size_t)lH * lW;
			for (size_t i = 0; i < plane; i++)
				lowfreq.data[i] = unweight(latSlice.data[4 * plane + i], latSlice.data[5 * plane + i])
					* LOWFREQ_STD + LOWFREQ_MEAN;
		}

		// Re-derive the low band from the decoded surface (denoise), then recombine. This IS the pipeline's
		// macro/detail split: the surface is the smooth low band plus the decoder's high-frequency residual.
		// (Inlined rather than calling laplacianDecode, which would redo the same upsample internally, so
		// the macro band can be reported alongside for free.)
		const Grid newLowres = laplacianDenoise(residual, lowfreq, sigma);
		const Grid macroP = bilinearResize(newLowres, residual.h, residual.w);

		const int32 oi = i1 - pi1, oj = j1 - pj1;
		const int32 H = i2 - i1, W = j2 - j1;
		outElev.resize((size_t)H * W);
		for (int32 r = 0; r < H; r++)
			for (int32 c = 0; c < W; c++)
			{
				// Undo the signed-sqrt compression the model works in: sign(e) * e^2.
				const float ms = macroP.at(oi + r, oj + c);
				const float es = residual.at(oi + r, oj + c) + ms; // == laplacianDecode(residual, newLowres)
				outElev[(size_t)r * W + c] = javaSignum(es) * es * es;
			}

		// NOT the low band `macroP` computed just above, even though it is the pipeline's own macro/detail
		// split and is sitting right there for free. See computeCoarseSurface.
		if (outMacro)
			computeCoarseSurface(i1, j1, i2, j2, *outMacro);
	}

	// The COARSE stage's elevation, resampled onto the native window: the surface the terrain shader
	// measures crag relief against, as (mesh height - altitude).
	//
	// Why not the Laplacian low band, which computeElev has already built: the terrain-data map has two
	// cascades, and the far one is served by ESampleDetail::Coarse, which runs the coarse stage ALONE —
	// skipping the latent stage is exactly what makes it ~249x cheaper. So the far cascade HAS no low band;
	// it can only report the coarse surface. If the near cascade answered with the low band instead, the
	// same mountain would measure its relief against two surfaces ~6x apart in smoothing, and it did:
	// near 19.5 m vs far 427.7 m of mean crag at mpp=30, so peaks that rendered rocky in the distance went
	// bare as the camera approached (55% of a mountain region). Both cascades now answer with the only
	// surface BOTH can compute.
	//
	// The price: macro is a 7.68 km surface, so high ground flatter than one coarse pixel reads as craggy
	// rather than as plateau — there is no longer a reference fine enough to tell those apart.
	//
	// Nearly free: the coarse tiles this reads are already resident, since the latent stage is conditioned
	// on them and computeClimate slices the same window with a larger pad.
	void WorldPipeline::computeCoarseSurface(int32 i1, int32 j1, int32 i2, int32 j2, oc::vector<float>& out)
	{
		const int32 S = 32 * m_config.latentCompression; // native pixels per coarse pixel (256)
		const int32 ci1 = floorDiv(i1, S), cj1 = floorDiv(j1, S);
		const int32 ci2 = -floorDiv(-i2, S), cj2 = -floorDiv(-j2, S); // ceil
		constexpr int32 pad = 2;                                      // bilinear needs 1 neighbour; 2 is slack

		const int32 g0i = ci1 - pad, g0j = cj1 - pad; // coarse index of the slice's [0,0]
		const int32 cStart[3] = { 0, g0i, g0j };
		const int32 cEnd[3] = { 7, ci2 + pad, cj2 + pad }; // 6 data channels + the trailing weight
		const FloatTensor slice = m_coarse->getSlice(cStart, cEnd);
		const int32 cH = (ci2 + pad) - g0i, cW = (cj2 + pad) - g0j;
		const size_t cplane = (size_t)cH * cW;

		Grid coarseElev(cH, cW);
		for (size_t i = 0; i < cplane; i++)
		{
			// SIGNED, unlike computeClimate's otherwise identical decode: that one clamps the ocean to 0
			// because its temperature regression only wants land. Here the seabed must stay below sea level,
			// or the whole ocean floor would report a macro of 0 and read as one enormous crag.
			const float e = unweight(slice.data[0 * cplane + i], slice.data[6 * cplane + i]);
			coarseElev.data[i] = javaSignum(e) * e * e;
		}

		const int32 H = i2 - i1, W = j2 - j1;
		out.resize((size_t)H * W);
		for (int32 r = 0; r < H; r++)
		{
			// Pixel-CENTRE mapping: native -> coarse -> this grid's index. Must match computeClimate's
			// convention exactly. Using native/S instead is a half-coarse-pixel error = 128 native px =
			// 3.84 km at true scale, and it is precisely the bug that made near and far disagree on CLIMATE.
			const float gridY = ((float)(i1 + r) + 0.5f) / (float)S - 0.5f - (float)g0i;
			for (int32 c = 0; c < W; c++)
			{
				const float gridX = ((float)(j1 + c) + 0.5f) / (float)S - 0.5f - (float)g0j;
				out[(size_t)r * W + c] = bilinearSample(coarseElev, gridY, gridX);
			}
		}
	}

	void WorldPipeline::computeClimate(int32 i1, int32 j1, int32 i2, int32 j2, oc::span<const float> elev,
	                                   int32 H, int32 W, oc::vector<float>& outClimate)
	{
		const int32 lc = m_config.latentCompression;
		const int32 S = 32 * lc; // native pixels per coarse pixel

		const int32 ci1 = floorDiv(i1, S), cj1 = floorDiv(j1, S);
		const int32 ci2 = -floorDiv(-i2, S), cj2 = -floorDiv(-j2, S);
		constexpr int32 win = 15;
		constexpr int32 pad = (win - 1) / 2 + 1; // 8

		const int32 cStart[3] = { 0, ci1 - pad, cj1 - pad };
		const int32 cEnd[3] = { 7, ci2 + pad, cj2 + pad };
		const FloatTensor coarseSlice = m_coarse->getSlice(cStart, cEnd);
		const int32 cH = (ci2 + pad) - (ci1 - pad);
		const int32 cW = (cj2 + pad) - (cj1 - pad);
		const size_t cplane = (size_t)cH * cW;

		Grid coarseTemp(cH, cW), coarseElev(cH, cW);
		Grid central[3] = { Grid(cH, cW), Grid(cH, cW), Grid(cH, cW) }; // coarse channels 3, 4, 5
		for (size_t i = 0; i < cplane; i++)
		{
			const float w = coarseSlice.data[6 * cplane + i];
			// Coarse elevation: undo the sqrt, clamping ocean to 0 (as the reference does here — note this
			// differs from computeElev, which keeps the sign).
			const float e = oc::max(0.0f, unweight(coarseSlice.data[0 * cplane + i], w));
			coarseElev.data[i] = e * e;
			coarseTemp.data[i] = unweight(coarseSlice.data[2 * cplane + i], w);
			central[0].data[i] = unweight(coarseSlice.data[3 * cplane + i], w);
			central[1].data[i] = unweight(coarseSlice.data[4 * cplane + i], w);
			central[2].data[i] = unweight(coarseSlice.data[5 * cplane + i], w);
		}

		// Regress temperature on elevation over the coarse grid, so the lapse rate can be re-applied to the
		// much sharper native elevation. This is what puts snow on the peaks the coarse map can't resolve.
		Grid tSea, beta;
		localBaselineTemperature(coarseTemp, coarseElev, win, 0.02f, tSea, beta);

		// The regression output and the centre-cropped channels share a grid: both lose (win-1) rows/cols.
		constexpr int32 cenPad = win / 2; // 7
		const int32 cenH = cH - 2 * cenPad, cenW = cW - 2 * cenPad;
		assert(tSea.h == cenH && tSea.w == cenW);
		Grid cropped[3];
		for (int32 k = 0; k < 3; k++)
		{
			cropped[k] = Grid(cenH, cenW);
			for (int32 r = 0; r < cenH; r++)
				for (int32 c = 0; c < cenW; c++)
					cropped[k].at(r, c) = central[k].at(r + cenPad, c + cenPad);
		}

		outClimate.assign((size_t)5 * H * W, 0.0f);
		const size_t plane = (size_t)H * W;
		for (int32 r = 0; r < H; r++)
		{
			const float gridY = ((float)(i1 + r) + 0.5f) / (float)S - (float)ci1 + 0.5f;
			for (int32 c = 0; c < W; c++)
			{
				const float gridX = ((float)(j1 + c) + 0.5f) / (float)S - (float)cj1 + 0.5f;

				const float tBase = bilinearSample(tSea, gridY, gridX);
				const float b = bilinearSample(beta, gridY, gridX);
				const float e = elev[(size_t)r * W + c];

				outClimate[0 * plane + (size_t)r * W + c] = tBase + b * oc::max(0.0f, e);
				outClimate[1 * plane + (size_t)r * W + c] = bilinearSample(cropped[0], gridY, gridX);
				outClimate[2 * plane + (size_t)r * W + c] = bilinearSample(cropped[1], gridY, gridX);
				outClimate[3 * plane + (size_t)r * W + c] = bilinearSample(cropped[2], gridY, gridX);
				outClimate[4 * plane + (size_t)r * W + c] = b;
			}
		}
	}

	bool WorldPipeline::get(int32 i1, int32 j1, int32 i2, int32 j2, bool withClimate,
	                        oc::vector<float>& outElev, oc::vector<float>& outClimate,
	                        oc::vector<float>* outMacro)
	{
		assert(m_valid);
		if (m_coarseOnly)
			return false;
		if (i2 <= i1 || j2 <= j1)
			return false;

		m_inferenceFailed = false;
		computeElev(i1, j1, i2, j2, outElev, outMacro);
		if (withClimate)
			computeClimate(i1, j1, i2, j2, outElev, i2 - i1, j2 - j1, outClimate);
		else
			outClimate.clear();

		// A failed tile is zeros, which would otherwise look like a flat sea-level plain and get cached as
		// if it were real terrain. Report it instead.
		return !m_inferenceFailed;
	}
}
