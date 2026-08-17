module;

// dml_provider_factory.h drags in d3d12.h/DirectML.h and therefore Windows.h, so the usual guards apply:
// NOMINMAX in particular, or the min/max macros collide with the standard library.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <onnxruntime/onnxruntime_cxx_api.h>
#include <onnxruntime/dml_provider_factory.h>
#include <comdef.h>

module Procedural;

import Core;
import File; // disk access goes through FileSystem
import Core.Log;
import :Diffusion.Onnx;

namespace Procedural::Diffusion
{
	namespace
	{
		// The ORT environment is a process-wide singleton and is never destroyed (matching the reference,
		// where OrtEnvironment is likewise never closed). Constructed on first use.
		//
		// ERROR, not WARNING: ORT logs straight to stdout, bypassing Core.Log, and at WARNING every session
		// emits VerifyEachNodeIsAssignedToAnEp ("some nodes were not assigned to the preferred execution
		// providers"). That one is unactionable by construction — ORT deliberately keeps shape/reshape ops
		// on the CPU because they compute tensor metadata, and a GPU round-trip per op to produce a few
		// integers would be slower. Nothing here reads ORT's warning stream: a DirectML EP that genuinely
		// fails to attach THROWS, and is reported below as an explicit error plus "inference provider: CPU".
		Ort::Env& ortEnv()
		{
			static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "OctEngine.Diffusion");
			return env;
		}

		std::mutex g_providerMutex;
		oc::string g_resolvedProvider;

		void setResolvedProviderOnce(oc::string_view p)
		{
			std::lock_guard<std::mutex> lk(g_providerMutex);
			if (!g_resolvedProvider.empty())
				return;
			g_resolvedProvider.assign(p.data(), p.size());
			Log::info(oc::format("[Diffusion] inference provider: {}", g_resolvedProvider));
		}

		// ORT's Windows API takes a wide path; the engine speaks UTF-8 strings everywhere.
		oc::wstring widenPath(const oc::string& p)
		{
			const int len = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), (int)p.size(), nullptr, 0);
			oc::wstring wide((size_t)oc::max(len, 0), L' ');
			if (len > 0)
				MultiByteToWideChar(CP_UTF8, 0, p.c_str(), (int)p.size(), wide.data(), len);
			return wide;
		}
	}

	struct OnnxModel::Impl
	{
		oc::optional<Ort::Session> session;
		oc::string name;
		oc::vector<oc::string> outputNames; // owned: GetOutputNameAllocated's buffer is transient

		oc::atomic<uint64> calls{ 0 };
		oc::atomic<uint64> batchItems{ 0 };
		oc::atomic<uint64> totalNs{ 0 };
	};

	OnnxModel::RunStats OnnxModel::stats() const
	{
		RunStats s;
		s.calls = m_impl->calls.load(oc::memory_order_relaxed);
		s.batchItems = m_impl->batchItems.load(oc::memory_order_relaxed);
		s.totalMs = (double)m_impl->totalNs.load(oc::memory_order_relaxed) / 1e6;
		return s;
	}

	void OnnxModel::resetStats()
	{
		m_impl->calls.store(0, oc::memory_order_relaxed);
		m_impl->batchItems.store(0, oc::memory_order_relaxed);
		m_impl->totalNs.store(0, oc::memory_order_relaxed);
	}

	OnnxModel::OnnxModel() : m_impl(oc::make_unique<Impl>()) {}
	OnnxModel::~OnnxModel() = default;

	bool OnnxModel::isValid() const
	{
		return m_impl && m_impl->session.has_value();
	}

	oc::string_view OnnxModel::resolvedProvider()
	{
		std::lock_guard<std::mutex> lk(g_providerMutex);
		return g_resolvedProvider;
	}

	bool OnnxModel::load(const oc::string& modelPath, oc::string_view name, EInferenceDevice device)
	{
		m_impl->name.assign(name.data(), name.size());

		if (!FileSystem::exists(modelPath))
		{
			Log::error(oc::format("[Diffusion] model '{}' not found at {}", name, modelPath));
			return false;
		}

		try
		{
			Ort::SessionOptions opts;
			opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

			bool dml = false;
			if (device != EInferenceDevice::Cpu)
			{
				// DirectML REQUIRES both of these. The Java addDirectML sets them for you; the C++ API does
				// not, and session creation fails without them.
				opts.DisableMemPattern();
				//opts.SetExecutionMode(ORT_SEQUENTIAL);
				opts.SetExecutionMode(ORT_PARALLEL);
				try
				{
					Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(opts, 0));
					dml = true;
				}
				catch (const Ort::Exception& e)
				{
					if (device == EInferenceDevice::Gpu)
					{
						Log::error(oc::format("[Diffusion] DirectML requested but unavailable: {}", e.what()));
						return false;
					}
					Log::warning(oc::format("[Diffusion] DirectML unavailable ({}), falling back to CPU. "
					                         "Inference will be very slow.", e.what()));
				}
			}
			setResolvedProviderOnce(dml ? "DirectML" : "CPU");

			const auto t0 = Clock::now();
			m_impl->session.emplace(ortEnv(), widenPath(modelPath).c_str(), opts);
			const auto t1 = Clock::now();

			Ort::AllocatorWithDefaultOptions alloc;
			const size_t nOut = m_impl->session->GetOutputCount();
			m_impl->outputNames.clear();
			for (size_t i = 0; i < nOut; i++)
				m_impl->outputNames.push_back(m_impl->session->GetOutputNameAllocated(i, alloc).get());

			Log::info(oc::format("[Diffusion] loaded '{}' in {} ms",
			                      name,
			                      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
			return true;
		}
		catch (const Ort::Exception& e)
		{
			Log::error(oc::format("[Diffusion] failed to load model '{}': {}", name, e.what()));
			m_impl->session.reset();
			return false;
		}
	}

	bool OnnxModel::run(oc::span<const OnnxInput> inputs, oc::vector<float>& out)
	{
		if (!isValid())
			return false;

		const auto t0 = Clock::now();
		try
		{
			const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

			oc::vector<const char*> names;
			oc::vector<Ort::Value> values;
			names.reserve(inputs.size());
			values.reserve(inputs.size());
			for (const OnnxInput& in : inputs)
			{
				// ORT's CreateTensor wants a mutable pointer but does not write through it.
				values.push_back(Ort::Value::CreateTensor<float>(
					mem, const_cast<float*>(in.data.data()), in.data.size(),
					in.shape.data(), in.shape.size()));
				names.push_back(in.name);
			}

			const char* outName = m_impl->outputNames[0].c_str();
			std::vector<Ort::Value> results = m_impl->session->Run( // Ort::Value is move-only; keep the ONNX container
				Ort::RunOptions{ nullptr }, names.data(), values.data(), values.size(), &outName, 1);

			const Ort::Value& r = results[0];
			const size_t count = r.GetTensorTypeAndShapeInfo().GetElementCount();
			const float* src = r.GetTensorData<float>();
			// Copy out before `results` dies: the buffer belongs to the Ort::Value.
			out.assign(src, src + count);

			m_impl->calls.fetch_add(1, oc::memory_order_relaxed);
			// Leading dim of "x" == the batch, so calls vs batchItems shows how well a stage batches.
			if (!inputs.empty() && !inputs[0].shape.empty())
				m_impl->batchItems.fetch_add((uint64)inputs[0].shape[0], oc::memory_order_relaxed);
			m_impl->totalNs.fetch_add(
				(uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count(),
				oc::memory_order_relaxed);
			return true;
		}
		catch (const Ort::Exception& e)
		{
			Log::error(oc::format("[Diffusion] inference failed on '{}': {}", m_impl->name, e.what()));
			return false;
		}
	}
}
