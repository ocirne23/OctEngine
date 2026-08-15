export module Procedural:Diffusion.ModelAssets;

import Core;

// Locates the diffusion model assets and parses the per-model constants that drive the pipeline.
// (Port of the reference's ModelAssetManager + WorldPipelineModelConfig, minus the downloader.)
//
// The models ship WITH the repo under Assets/TerrainDiffusion/ (git-lfs, like the .dll/.lib deps) rather
// than being fetched at runtime as the reference mod does — they are always present, so there is no network
// path, no manifest and no hash check here. The upstream provenance is recorded in
// Assets/THIRD_PARTY_ASSETS.md.
export namespace Procedural::Diffusion
{
	// Which assets a given stage needs. The coarse model is 22 MB; base + decoder are another 2.25 GB, so
	// the coarse-only bring-up path avoids loading them.
	enum class EAssetSet : uint8
	{
		CoarseOnly, // coarse_model.onnx + both configs
		Full        // + base_model.onnx + decoder_model.onnx
	};

	// Which weights to run. Precision is baked into the model FILE (ONNX Runtime cannot run an fp32 graph
	// in fp16), so Fp16 means "load <stem>_fp16.onnx instead" — an OPTIONAL, separate set produced offline
	// by Tools/convert_models_fp16.py. The fp32 originals always ship and are never replaced; a missing
	// converted file just falls back to them.
	//
	// Their graph inputs/outputs are still fp32 (the conversion casts just inside the boundary), so nothing
	// downstream of the file path changes. Output is NOT bit-identical to fp32 — the same seed grows
	// slightly different terrain — so the choice is part of the world config, not a free speed knob.
	enum class EPrecision : uint8
	{
		Fp32,
		Fp16
	};

	// world_pipeline_config.json. Only these fields are load-bearing: the reference also parses
	// drop_water_pct, elev_coarse_pool_mode and p5_coarse_pool_mode, which have ZERO call sites there —
	// dead metadata carried over from the Python config. Deliberately not represented.
	struct ModelConfig
	{
		float nativeResolution = 30.0f;   // metres per native pixel
		int32 latentCompression = 8;      // native pixels per latent pixel
		int32 coarsePooling = 1;          // only 1 is supported (as in the reference)
		float residualMean = 0.0f;
		float residualStd = 0.7f;
		std::vector<float> coarseMeans;   // 6
		std::vector<float> coarseStds;    // 6
		std::vector<float> condSnr;       // 5
		std::vector<float> frequencyMult; // 5
		std::vector<float> histogramRaw;  // 5; all zeros when the file says null (which it does)
	};

	// pipeline_data.json — seed-INDEPENDENT real-world (WorldClim/ETOPO) distributions. The matching
	// noise-side quantiles are seed-dependent and are built at runtime by SyntheticMapFactory instead.
	struct PipelineData
	{
		int32 nQuantiles = 64;
		std::vector<float> dataQuantiles; // [5][nQuantiles], row-major
		float aTempStd = 0.0f;
		float bTempStd = 0.0f;
		float tempStdP1 = 0.0f;
		float tempStdP99 = 0.0f;
	};

	class ModelAssets
	{
	public:
		// Synchronous and cheap (a few file stats + two small JSON parses; the .onnx files are NOT read
		// here — OnnxModel does that). Returns false and logs on any problem; error() has the detail.
		bool load(EAssetSet set);
		bool isLoaded() const { return m_loaded; }

		const ModelConfig& config() const { return m_config; }
		const PipelineData& data() const { return m_data; }
		const std::string& error() const { return m_error; }

		// Assets/TerrainDiffusion, relative to the CWD (FileSystem::initialize sets it to Assets/).
		static std::string modelDir();
		static std::string assetPath(std::string_view fileName);
		// The model file to load for `stem` ("coarse_model", "base_model", "decoder_model") at the requested
		// precision, falling back to the fp32 original when the converted file is absent — which is the
		// normal state, since converting is an optional offline step.
		static std::string modelPath(std::string_view stem, EPrecision precision);
		// The converted model beside its fp32 original: Assets/TerrainDiffusion/<stem>_fp16.onnx. Written by
		// Tools/convert_models_fp16.py, by hand; the engine only ever reads it.
		static std::string fp16Path(std::string_view stem);
		// True when EVERY model of the set has an fp16 file present. Mixing precisions across the three
		// stages is allowed and works (each session is independent), but it makes a perf number impossible
		// to interpret, so the caller logs which stages actually got converted.
		static bool hasFp16Models(EAssetSet set);

	private:
		ModelConfig m_config;
		PipelineData m_data;
		std::string m_error;
		bool m_loaded = false;
	};
}
