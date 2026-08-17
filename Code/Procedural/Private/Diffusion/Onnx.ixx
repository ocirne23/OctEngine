export module Procedural:Diffusion.Onnx;

import Core;

// ONNX Runtime wrapper for the diffusion models (port of the reference's OnnxModel).
//
// ORT is kept entirely behind a pimpl: no onnxruntime header is included by any .ixx, so the runtime never
// reaches Procedural's public surface — the same discipline as box3d in Physics and Steam Audio in Audio.
// ORT's C++ API throws; every throw is caught at this boundary and turned into a logged failure, because
// the rest of the engine is exception-free by style.
export namespace Procedural::Diffusion
{
	enum class EInferenceDevice : uint8
	{
		Gpu,  // DirectML, or hard-fail (so a silent CPU fallback can't quietly tank the frame rate)
		Cpu,  // CPU only
		Auto  // DirectML if available, else CPU
	};

	// One named model input. The reference passes Object[][] with unchecked casts; this is the same data,
	// typed. `data` is not retained: ORT copies it into native memory during run().
	struct OnnxInput
	{
		const char* name = nullptr;
		oc::span<const float> data;
		oc::span<const int64> shape;
	};

	class OnnxModel
	{
	public:
		OnnxModel();
		~OnnxModel();
		OnnxModel(const OnnxModel&) = delete;
		OnnxModel& operator=(const OnnxModel&) = delete;

		// Creates the session. Returns false (and logs) on failure, leaving the model unusable.
		// `name` is only for diagnostics.
		bool load(const oc::string& modelPath, oc::string_view name, EInferenceDevice device);
		bool isValid() const;

		// Runs the model and copies OUTPUT 0 into `out` (resized to the element count). Any further outputs
		// are ignored, as in the reference — these models have exactly one.
		// Safe to call concurrently on one instance (ORT sessions are), but the pipeline serialises anyway.
		bool run(oc::span<const OnnxInput> inputs, oc::vector<float>& out);

		// Accumulated inference cost, for working out which stage actually dominates. `batchItems` counts
		// the leading dimension of input "x", so calls/batchItems shows how well a stage is batching.
		struct RunStats
		{
			uint64 calls = 0;
			uint64 batchItems = 0;
			double totalMs = 0.0;
		};
		RunStats stats() const;
		void resetStats();

		// "DirectML" / "CPU" / "" before any model has loaded. Reported once per process.
		static oc::string_view resolvedProvider();

	private:
		struct Impl;
		oc::unique_ptr<Impl> m_impl;
	};
}
