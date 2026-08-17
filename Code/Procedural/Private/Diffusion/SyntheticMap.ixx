export module Procedural:Diffusion.SyntheticMap;

import Core;
import :Diffusion.ModelAssets;

// The coarse stage's conditioning input (port of the reference's SyntheticMapFactory, itself a port of
// world_pipeline.py make_synthetic_map_factory).
//
// What it does: sample 5 channels of FastNoiseLite Perlin fBm, then QUANTILE-MATCH each channel onto a real
// -world (WorldClim/ETOPO) distribution. That is what turns generic [-1,1] noise into plausible elevation /
// temperature / precipitation values for the model to refine — it is the "coarse sketch" the diffusion
// stack is conditioned on, and it is where the world seed actually enters the pipeline.
export namespace Procedural::Diffusion
{
	class SyntheticMapFactory
	{
	public:
		static constexpr int32 NUM_CHANNELS = 5; // [elev_sqrt, temp, temp_std, precip, precip_std]

		// Building the per-channel noise quantile tables costs 5 x 1M fBm evaluations plus 5 sorts, so this
		// constructor is expensive (hundreds of ms; far worse in Debug). It runs once per SEED, not per tile.
		// `data`'s quantile tables are seed-independent and come straight from pipeline_data.json.
		SyntheticMapFactory(uint64 worldSeed, const ModelConfig& cfg, const PipelineData& data);
		~SyntheticMapFactory();
		SyntheticMapFactory(const SyntheticMapFactory&) = delete;
		SyntheticMapFactory& operator=(const SyntheticMapFactory&) = delete;

		// Samples the half-open rect [x1,x2) x [y1,y2). x is the COLUMN axis and y the ROW axis.
		// `out` is resized to 5*H*W and indexed out[ch*H*W + r*W + c], H = y2-y1, W = x2-x1.
		// Callers beware: WorldPipeline's coarse tile deliberately calls this with its i/j swapped.
		void sample(int32 x1, int32 y1, int32 x2, int32 y2, oc::vector<float>& out) const;

	private:
		struct Impl; // hides FastNoiseLite, which must only ever be compiled in the /fp:precise .cpp
		oc::unique_ptr<Impl> m_impl;
	};
}
