module Procedural;

import Core;
import File; // disk access goes through FileSystem
import Core.Log;
import :Diffusion.Json;
import :Diffusion.ModelAssets;

namespace Procedural::Diffusion
{
	namespace
	{
		// Expected sizes for the assets shipped in Assets/TerrainDiffusion (upstream:
		// huggingface.co/xandergos/terrain-diffusion-30m-onnx @ ad2df557eca5645f588766101cf3bc3682455c3e).
		// These are a sanity check, not a security one — their real job is catching the two failure modes
		// that would otherwise surface as an inscrutable protobuf error deep inside ONNX Runtime:
		// an unfetched git-lfs pointer, and a truncated copy.
		struct RequiredAsset
		{
			std::string_view name;
			uint64 sizeBytes;
			bool coarseSetOnly;
		};

		// sizeBytes = 0 means "do not size-check". The JSONs are TEXT, and git rewrites their line endings
		// on checkout unless told otherwise (.gitattributes marks *.onnx binary; nothing covers these), so
		// their byte count is not a property of the file's content — world_pipeline_config.json is 774 bytes
		// with LF and 817 with CRLF, and pinning either one makes a normal git operation look exactly like a
		// corrupt download. Neither failure this table exists to catch can reach them anyway: they are not
		// lfs-tracked, and they are parsed immediately below, so damage surfaces as a JSON error naming the
		// file rather than as an inscrutable protobuf failure inside ONNX Runtime. The .onnx blobs keep
		// their sizes — they are binary, lfs-tracked, and fed straight to ORT.
		constexpr RequiredAsset REQUIRED[] = {
			{ "world_pipeline_config.json",          0ull, true },
			{ "pipeline_data.json",                  0ull, true },
			{ "coarse_model.onnx",           22497125ull, true },
			{ "decoder_model.onnx",         223854143ull, false },
			{ "base_model.onnx",           2029994361ull, false },
		};

		constexpr std::string_view LFS_POINTER_MAGIC = "version https://git-lfs.github.com/spec/v1";

		bool assetInSet(const RequiredAsset& a, EAssetSet set)
		{
			return set == EAssetSet::Full || a.coarseSetOnly;
		}

		// A git-lfs pointer is a small text file. Detect it explicitly so the error can say what to do.
		bool looksLikeLfsPointer(const std::string& p)
		{
			// Model asset checks run at generator init (a worker job) — no main-thread exemption.
			const std::string head = FileSystem::readFileStr(p);
			return std::string_view(head).starts_with(LFS_POINTER_MAGIC);
		}

		// A converted model counts as usable only if it is real weights. No size check, unlike REQUIRED,
		// which pins exact upstream byte counts — these are converted locally, so there is no known-good
		// size to compare against, and a bad conversion surfaces as an ORT load error naming the file. But
		// they sit beside the originals under the repo's "*.onnx filter=lfs" rule, so a clone without
		// `git lfs pull` leaves a text pointer exactly where the weights should be; feeding that to ORT is
		// the inscrutable-protobuf-error failure the fp32 check already exists to prevent.
		bool fp16Usable(const std::string& p)
		{
			return FileSystem::exists(p) && !looksLikeLfsPointer(p);
		}

		bool readAll(const std::string& p, std::string& out)
		{
			if (!FileSystem::exists(p))
				return false;
			out = FileSystem::readFileStr(p);
			return true;
		}
	}

	std::string ModelAssets::modelDir()
	{
		// CWD is Assets/ (FileSystem::initialize).
		return "TerrainDiffusion";
	}

	std::string ModelAssets::assetPath(std::string_view fileName)
	{
		return FileSystem::join(modelDir(), fileName);
	}

	std::string ModelAssets::fp16Path(std::string_view stem)
	{
		// Beside the fp32 originals, NOT in Local/: these are shipped assets like the models they came from,
		// converted once and committed, not a cache the engine may regenerate. Nothing here ever writes
		// them — Tools/convert_models_fp16.py does, by hand, when the model set changes.
		return assetPath(std::string(stem) + "_fp16.onnx");
	}

	std::string ModelAssets::modelPath(std::string_view stem, EPrecision precision)
	{
		if (precision == EPrecision::Fp16 && fp16Usable(fp16Path(stem)))
			return fp16Path(stem);
		return assetPath(std::string(stem) + ".onnx");
	}

	bool ModelAssets::hasFp16Models(EAssetSet set)
	{
		for (const RequiredAsset& a : REQUIRED)
		{
			if (!a.name.ends_with(".onnx") || !assetInSet(a, set))
				continue;
			if (!fp16Usable(fp16Path(a.name.substr(0, a.name.size() - 5))))
				return false;
		}
		return true;
	}

	bool ModelAssets::load(EAssetSet set)
	{
		m_loaded = false;
		m_error.clear();

		auto fail = [&](std::string s)
		{
			m_error = std::move(s);
			Log::error(std::format("[Diffusion] {}", m_error));
			return false;
		};

		// --- Presence / integrity ------------------------------------------------------------------
		for (const RequiredAsset& a : REQUIRED)
		{
			if (!assetInSet(a, set))
				continue;

			const std::string p = assetPath(a.name);
			if (!FileSystem::exists(p))
				return fail(std::format("missing model asset Assets/{} - the diffusion models ship with the "
				                        "repo; if this is a fresh clone run 'git lfs pull'", p));

			const uint64 actual = FileSystem::fileSize(p);
			if (actual == 0)
				return fail(std::format("cannot stat Assets/{}", p));

			if (a.sizeBytes != 0 && actual != a.sizeBytes)
			{
				if (looksLikeLfsPointer(p))
					return fail(std::format("Assets/{} is an unfetched git-lfs pointer, not the real file - "
					                        "run 'git lfs pull'", p));
				return fail(std::format("Assets/{} is {} bytes, expected {} - the file looks truncated or is "
				                        "from a different model revision", p, actual, a.sizeBytes));
			}
		}

		// --- world_pipeline_config.json --------------------------------------------------------------
		std::string text;
		JsonValue root;
		std::string jerr;
		if (!readAll(assetPath("world_pipeline_config.json"), text) || !JsonValue::parse(text, root, jerr))
			return fail(std::format("could not read/parse world_pipeline_config.json: {}", jerr));

		{
			ModelConfig c;
			auto num = [&](std::string_view k, float& dst) -> bool
			{
				const JsonValue* v = root.find(k);
				if (!v || v->type != JsonValue::EType::Number)
					return false;
				dst = v->asFloat();
				return true;
			};
			const JsonValue* lc = root.find("latent_compression");
			const JsonValue* cp = root.find("coarse_pooling");
			const JsonValue* cm = root.find("coarse_means");
			const JsonValue* cs = root.find("coarse_stds");
			const JsonValue* sn = root.find("cond_snr");
			const JsonValue* fm = root.find("frequency_mult");
			const JsonValue* hr = root.find("histogram_raw");

			if (!num("native_resolution", c.nativeResolution) || c.nativeResolution <= 0.0f)
				return fail("world_pipeline_config: native_resolution missing or <= 0");
			if (!num("residual_mean", c.residualMean) || !num("residual_std", c.residualStd))
				return fail("world_pipeline_config: residual_mean/residual_std missing");
			if (!lc || lc->asInt() <= 0)
				return fail("world_pipeline_config: latent_compression missing or <= 0");
			c.latentCompression = lc->asInt();
			if (!cp || cp->asInt() <= 0)
				return fail("world_pipeline_config: coarse_pooling missing or <= 0");
			c.coarsePooling = cp->asInt();
			// The reference throws on anything else, and so do we: coarse_pooling != 1 changes the
			// conditioning path in ways nothing here implements.
			if (c.coarsePooling != 1)
				return fail(std::format("world_pipeline_config: coarse_pooling={} is not supported", c.coarsePooling));

			if (!cm || !cm->asFloatArray(c.coarseMeans, 6))
				return fail("world_pipeline_config: coarse_means must be 6 numbers");
			if (!cs || !cs->asFloatArray(c.coarseStds, 6))
				return fail("world_pipeline_config: coarse_stds must be 6 numbers");
			if (!sn || !sn->asFloatArray(c.condSnr, 5))
				return fail("world_pipeline_config: cond_snr must be 5 numbers");
			if (!fm || !fm->asFloatArray(c.frequencyMult, 5))
				return fail("world_pipeline_config: frequency_mult must be 5 numbers");

			// histogram_raw is null in the shipped config; the reference substitutes five zeros.
			if (!hr || hr->isNull())
				c.histogramRaw.assign(5, 0.0f);
			else if (!hr->asFloatArray(c.histogramRaw, 5))
				return fail("world_pipeline_config: histogram_raw must be 5 numbers or null");

			m_config = std::move(c);
		}

		// --- pipeline_data.json ----------------------------------------------------------------------
		if (!readAll(assetPath("pipeline_data.json"), text) || !JsonValue::parse(text, root, jerr))
			return fail(std::format("could not read/parse pipeline_data.json: {}", jerr));

		{
			PipelineData d;
			const JsonValue* nq = root.find("n_quantiles");
			const JsonValue* dq = root.find("data_quantile_tables");
			const JsonValue* a = root.find("a_temp_std");
			const JsonValue* b = root.find("b_temp_std");
			const JsonValue* p1 = root.find("temp_std_p1");
			const JsonValue* p99 = root.find("temp_std_p99");
			if (!nq || nq->asInt() <= 0)
				return fail("pipeline_data: n_quantiles missing or <= 0");
			d.nQuantiles = nq->asInt();
			if (!dq || !dq->asFloatArray2D(d.dataQuantiles, 5, d.nQuantiles))
				return fail(std::format("pipeline_data: data_quantile_tables must be [5][{}]", d.nQuantiles));
			if (!a || !b || !p1 || !p99)
				return fail("pipeline_data: a_temp_std/b_temp_std/temp_std_p1/temp_std_p99 missing");
			d.aTempStd = a->asFloat();
			d.bTempStd = b->asFloat();
			d.tempStdP1 = p1->asFloat();
			d.tempStdP99 = p99->asFloat();
			// noise_quantile_tables is also present in the file but deliberately unused: the noise side is
			// seed-dependent, so SyntheticMapFactory rebuilds it per world (as the reference does).
			m_data = std::move(d);
		}

		Log::verbose(std::format("[Diffusion] model assets found ({} m/px, latent compression {})",
		                         m_config.nativeResolution, m_config.latentCompression));
		m_loaded = true;
		return true;
	}
}
