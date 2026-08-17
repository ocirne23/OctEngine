module;

#include <zstd/zstd.h> // the disk tile cache's compressor; see the tile-cache block below

module Procedural;

import Core;
import File; // disk access goes through FileSystem
import Core.Log;
import Threading;
import :GeneratorV3;
import :Noise;
import :Diffusion.Laplacian;
import :Diffusion.ModelAssets;
import :Diffusion.Onnx;
import :Diffusion.Pipeline;
import :Diffusion.Rng;
import :Diffusion.Tensor;
import :TerrainSampler;

namespace Procedural
{
	namespace
	{
		using namespace Procedural::Diffusion;

		// Full-detail tile: native model pixels (30 m each by default), so one tile is 7.68 km.
		constexpr int32 TILE = 256;
		constexpr int32 HALO = 1; // one pixel each side, so bilinear/gradient never crosses a tile edge
		constexpr int32 TILE_W = TILE + 2 * HALO;

		// Coarse tile: the COARSE STAGE's own lattice, where one pixel is 256 native pixels. A 64-pixel
		// coarse tile therefore spans 16,384 native pixels — ~491 km at 30 m/px. That is the whole point of
		// the coarse path: a wide-area query resolves to one or two of these instead of the thousands of
		// full-detail tiles the same area would otherwise demand.
		constexpr int32 CTILE = 64;
		constexpr int32 CHALO = 1;
		constexpr int32 CTILE_W = CTILE + 2 * CHALO;

		// One cached rectangle of the field. Both detail levels share this shape; `width` says which.
		struct FieldTile
		{
			oc::vector<float> elev;    // metres in the MODEL's frame, SIGNED (seabed stays negative)
			oc::vector<float> macro;   // the COARSE stage's surface (7.68 km/px), resampled to this tile: the
			                            // reference the shader measures crag relief against. Identical in
			                            // meaning on both detail levels, which is the point.
			// Degrees C AT SEA LEVEL — the same baseline everything else in the engine carries (the baked
			// terrain-data map, TerrainPoint::temperatureSeaLevel). Temperature at height is baseline +
			// the one global lapse (cfg.lapseRate) x elevation, applied at sample time. The model's own
			// temperature-at-elevation and its per-region regressed rate are DERIVATION inputs, undone at
			// tile build (the model constructed temp as tSea + beta*elev, so tSea is recovered exactly);
			// carrying them here just made every sample redo that subtraction and cost a whole extra plane.
			oc::vector<float> tempSea;
			oc::vector<float> precip;  // mm/yr
			int32 width = 0;            // TILE_W or CTILE_W
		};
		using FieldTilePtr = oc::shared_ptr<const FieldTile>;

		uint64 tileKey(int32 ti, int32 tj)
		{
			return ((uint64)(uint32)ti << 32) | (uint64)(uint32)tj;
		}

		// -------------------------------------------------------------------------------------------------
		// Disk tile cache: Local/Diffusion/<seed>/{full|coarse}_x<j>_z<i>.tile (cwd is Assets/). Inference
		// is deterministic per (seed, precision, tile), so a tile generated once never needs the GPU again —
		// a cached fetch is a ~0.7 MB read instead of ~1.5 s of diffusion. Per-seed folders make a reseed a
		// different directory rather than an invalidation, and fp16 gets its own suffix because precision
		// CHANGES the terrain for a given seed (it is part of the world, not a perf knob — see
		// TerrainConfigV3::useFp16).
		// -------------------------------------------------------------------------------------------------
		constexpr uint32 TILE_CACHE_MAGIC = 0x43445456; // 'VTDC'
		// Bump when FieldTile's layout or ANYTHING in the pipeline that shapes its content changes
		// (stages, strides, the coarse beta regression, ...): stale files fail the check and regenerate
		// in place, exactly like the cooked scene cache.
		// v2: planes are uint16, quantized over a per-plane [min, max] carried in the file — half the
		// bytes of raw floats, and the worst-case error (range/65535) is centimetres on the steepest
		// tile, far below the pipeline's own ~6 cm overlap tolerance. Saving ROUNDTRIPS the tile through
		// the quantizer, so RAM and disk always hold bit-identical values: a tile can never change
		// across an eviction/reload cycle, which is what keeps chunk borders crack-free.
		// v3: the quantized plane is then delta-coded (wraparound, row-major linear), byte-SPLIT for the
		// 16-bit planes (all low bytes, then all high bytes) and zstd-compressed. The transform is what
		// earns the ratio: deltas of a smooth field are small, so after the split the high-byte half is
		// almost all zeros and the entropy concentrates in the low bytes. Lossless by construction — the
		// float values are untouched, only the integer container is recoded — and per plane the STORED
		// form wins if compression can't beat it, so a file is never larger than its uncompressed self.
		// v4: one fewer plane and two thinner ones. The beta plane is GONE (the baseline is recovered at
		// tile build now — see FieldTile::tempSea), and tempSea + precip store 8-bit: the rest of the
		// engine already encodes them at 8 bits (the baked map's [-25, 50] C channel, the 8-bit humidity),
		// so 16 bits here was precision no consumer could ever see. Per-tile [min, max] makes the 8-bit
		// step FINER than the map's fixed-range one on any tile narrower than the full climate range.
		// 10 B/px -> 6 B/px before zstd even starts.
		constexpr uint32 TILE_CACHE_VERSION = 4;
		constexpr int32 TILE_CACHE_ZSTD_LEVEL = 9; // write-once cache: spend a little more than default

		enum class EPlaneCodec : uint8
		{
			Stored = 0, // raw quantized values, n * (bytes per value)
			Zstd = 1,   // zstd(delta [+ byte-split when 16-bit])
		};

		// Quantized plane -> delta bytes (16-bit additionally byte-split: lo half, then hi half), and
		// back. Wraparound arithmetic makes the roundtrip exact for any input.
		void deltaSplit16(const uint16* q, size_t n, uint8* out)
		{
			uint16 prev = 0;
			for (size_t i = 0; i < n; i++)
			{
				const uint16 d = (uint16)(q[i] - prev);
				prev = q[i];
				out[i] = (uint8)(d & 0xFF);
				out[n + i] = (uint8)(d >> 8);
			}
		}
		void unsplitUndelta16(const uint8* in, size_t n, uint16* q)
		{
			uint16 prev = 0;
			for (size_t i = 0; i < n; i++)
			{
				const uint16 d = (uint16)(in[i] | ((uint16)in[n + i] << 8));
				prev = (uint16)(prev + d);
				q[i] = prev;
			}
		}
		void delta8(const uint8* q, size_t n, uint8* out)
		{
			uint8 prev = 0;
			for (size_t i = 0; i < n; i++)
			{
				out[i] = (uint8)(q[i] - prev);
				prev = q[i];
			}
		}
		void undelta8(const uint8* in, size_t n, uint8* q)
		{
			uint8 prev = 0;
			for (size_t i = 0; i < n; i++)
			{
				prev = (uint8)(prev + in[i]);
				q[i] = prev;
			}
		}

		struct TileCacheHeader
		{
			uint32 magic = 0;
			uint32 version = 0;
			uint64 seed = 0;
			int32 ti = 0;
			int32 tj = 0;
			int32 width = 0;
			uint32 pad = 0;
		};

		oc::string tileCachePath(uint64 seed, bool fp16, bool coarse, int32 ti, int32 tj)
		{
			// j <-> x, i <-> z: the same axis convention sampleFromBlock documents.
			return FileSystem::join(FileSystem::join("Local/Diffusion",
					fp16 ? oc::format("{}_fp16", seed) : oc::format("{}", seed)),
				oc::format("{}_x{}_z{}.tile", coarse ? "coarse" : "full", tj, ti));
		}

		// The tile cache moved off std::fstream when Core stopped exporting it: the file is read/written
		// WHOLE through FileSystem (one syscall instead of a dozen small ones) and parsed from memory.
		struct ByteReader
		{
			const oc::vector<uint8>& data;
			size_t cursor = 0;
			bool ok = true;
			bool read(void* dst, size_t bytes)
			{
				if (!ok || cursor + bytes > data.size())
					return ok = false;
				std::memcpy(dst, data.data() + cursor, bytes);
				cursor += bytes;
				return true;
			}
			bool atEnd() const { return cursor == data.size(); }
		};
		struct ByteWriter
		{
			oc::vector<uint8> data;
			void write(const void* src, size_t bytes)
			{
				const uint8* p = (const uint8*)src;
				data.insert(data.end(), p, p + bytes);
			}
		};

		struct PlaneHeader
		{
			float minV = 0.0f;
			float maxV = 0.0f;
		};

		// The file's fixed plane schema — order and bit depth. Loader and writer MUST agree; a change here
		// is a TILE_CACHE_VERSION bump.
		struct PlaneSchema
		{
			oc::vector<float> FieldTile::* plane;
			bool eightBit;
		};
		constexpr PlaneSchema TILE_PLANES[] = {
			{ &FieldTile::elev,    false }, // geometry: 8 bits would terrace metres-high steps
			{ &FieldTile::macro,   false }, // crag relief reference: same reason
			{ &FieldTile::tempSea, true  }, // consumers are 8-bit anyway (baked map channel)
			{ &FieldTile::precip,  true  }, // humidity is 8-bit anyway
		};

		FieldTilePtr loadTileFromDisk(const oc::string& path, uint64 seed, int32 ti, int32 tj,
		                              int32 expectWidth)
		{
			// Tile IO runs on the diffusion job (never the main thread) — the FileSystem assert covers it.
			oc::vector<uint8> bytes;
			if (!FileSystem::readFileBytes(path, bytes) || bytes.empty())
				return nullptr;
			ByteReader f{ bytes };

			TileCacheHeader h;
			f.read(&h, sizeof(h));
			// A mismatch is not an error, it is a miss: truncated writes, old versions and hand-copied
			// files from another seed folder all just regenerate and overwrite.
			if (!f.ok || h.magic != TILE_CACHE_MAGIC || h.version != TILE_CACHE_VERSION
				|| h.seed != seed || h.ti != ti || h.tj != tj || h.width != expectWidth)
				return nullptr;

			const size_t plane = (size_t)h.width * h.width;
			auto t = oc::make_shared<FieldTile>();
			t->width = h.width;
			oc::vector<uint16> q16(plane);
			oc::vector<uint8> q8(plane), raw(plane * 2), comp;
			for (const PlaneSchema& ps : TILE_PLANES)
			{
				const size_t rawBytes = ps.eightBit ? plane : plane * 2;
				PlaneHeader ph;
				uint8 codec = 0;
				uint32 payloadSize = 0;
				f.read(&ph, sizeof(ph));
				f.read(&codec, sizeof(codec));
				f.read(&payloadSize, sizeof(payloadSize));
				if (!f.ok || payloadSize == 0 || payloadSize > rawBytes)
					return nullptr;

				if ((EPlaneCodec)codec == EPlaneCodec::Stored)
				{
					if (payloadSize != rawBytes)
						return nullptr;
					f.read(ps.eightBit ? (void*)q8.data() : (void*)q16.data(), payloadSize);
					if (!f.ok)
						return nullptr;
				}
				else if ((EPlaneCodec)codec == EPlaneCodec::Zstd)
				{
					comp.resize(payloadSize);
					f.read(comp.data(), payloadSize);
					if (!f.ok)
						return nullptr;
					const size_t n = ZSTD_decompress(raw.data(), rawBytes, comp.data(), comp.size());
					if (ZSTD_isError(n) || n != rawBytes)
						return nullptr;
					if (ps.eightBit)
						undelta8(raw.data(), plane, q8.data());
					else
						unsplitUndelta16(raw.data(), plane, q16.data());
				}
				else
					return nullptr;

				// MUST match the writer's roundtrip expression exactly (same step, same order of
				// operations) — bit-identical dequantization is what the crack-free guarantee rests on.
				const float maxQ = ps.eightBit ? 255.0f : 65535.0f;
				const float step = (ph.maxV - ph.minV) / maxQ;
				oc::vector<float>& v = (*t).*ps.plane;
				v.resize(plane);
				for (size_t i = 0; i < plane; i++)
					v[i] = ph.minV + (float)(ps.eightBit ? q8[i] : q16[i]) * step;
			}
			// Exactly at the end means the file is whole; anything left over is truncation or garbage.
			if (!f.ok || !f.atEnd())
				return nullptr;
			return t;
		}

		// NOT const: every plane is roundtripped through its quantized form in place, so the tile the RAM
		// cache ends up holding is exactly what a later load returns.
		void saveTileToDisk(const oc::string& path, FieldTile& t, uint64 seed,
		                    int32 ti, int32 tj)
		{
			FileSystem::createDirectories(FileSystem::parentPath(path));

			ByteWriter f; // built in memory, written in one go below
			TileCacheHeader h;
			h.magic = TILE_CACHE_MAGIC;
			h.version = TILE_CACHE_VERSION;
			h.seed = seed;
			h.ti = ti;
			h.tj = tj;
			h.width = t.width;
			f.write(&h, sizeof(h));

			const size_t plane = (size_t)t.width * t.width;
			oc::vector<uint16> q16(plane);
			oc::vector<uint8> q8(plane), raw(plane * 2);
			oc::vector<uint8> comp(ZSTD_compressBound(plane * 2));
			for (const PlaneSchema& ps : TILE_PLANES)
			{
				oc::vector<float>& v = t.*ps.plane;
				float lo = v[0], hi = v[0];
				for (float x : v)
				{
					lo = oc::min(lo, x);
					hi = oc::max(hi, x);
				}
				const PlaneHeader ph{ lo, hi };
				f.write(&ph, sizeof(ph));
				const float maxQ = ps.eightBit ? 255.0f : 65535.0f;
				const float range = hi - lo;
				const float scale = range > 0.0f ? maxQ / range : 0.0f;
				const float step = range / maxQ; // the loader's expression, reused for the roundtrip
				for (size_t i = 0; i < plane; i++)
				{
					const uint32 qi = (uint32)oc::min(maxQ, std::round((v[i] - lo) * scale));
					if (ps.eightBit)
						q8[i] = (uint8)qi;
					else
						q16[i] = (uint16)qi;
					v[i] = lo + (float)qi * step;
				}

				const size_t rawBytes = ps.eightBit ? plane : plane * 2;
				if (ps.eightBit)
					delta8(q8.data(), plane, raw.data());
				else
					deltaSplit16(q16.data(), plane, raw.data());
				const size_t n = ZSTD_compress(comp.data(), comp.size(), raw.data(), rawBytes,
				                               TILE_CACHE_ZSTD_LEVEL);
				// Per plane, smaller form wins: a plane the transform can't help (it can only expand on
				// data that is already noise-dense) falls back to stored, so a file never loses to its
				// uncompressed self.
				if (!ZSTD_isError(n) && n < rawBytes)
				{
					const uint8 codec = (uint8)EPlaneCodec::Zstd;
					const uint32 payloadSize = (uint32)n;
					f.write(&codec, sizeof(codec));
					f.write(&payloadSize, sizeof(payloadSize));
					f.write(comp.data(), n);
				}
				else
				{
					const uint8 codec = (uint8)EPlaneCodec::Stored;
					const uint32 payloadSize = (uint32)rawBytes;
					f.write(&codec, sizeof(codec));
					f.write(&payloadSize, sizeof(payloadSize));
					f.write(ps.eightBit ? (const void*)q8.data() : (const void*)q16.data(), rawBytes);
				}
			}
			// A write cut short (crash, full disk) leaves a file the loader's size probe rejects, so the
			// worst case is a regenerate — no tmp+rename dance needed, same contract as the .vsc cache.
			if (!FileSystem::writeFileBytes(path, f.data))
				Log::warning(oc::format("[Diffusion] tile cache write failed: {}", path));
		}

		// -------------------------------------------------------------------------------------------------
		// The process-lifetime runtime: models are 2.28 GB, so they load ONCE and are reseeded, never
		// rebuilt. Every TerrainGenV3 shares this; constructing a generator is consequently cheap, which
		// matters because TerrainStreamer::rebuildMaps() makes a fresh one on every tweak change.
		// -------------------------------------------------------------------------------------------------
		class DiffusionRuntime
		{
		public:
			static DiffusionRuntime& get()
			{
				static DiffusionRuntime r;
				return r;
			}

			void beginLoad()
			{
				std::lock_guard<std::mutex> lk(m_loadMutex);
				if (m_loadStarted)
					return;
				m_loadStarted = true;
				// The assets are on disk (shipped with the repo), but getting 2.28 GB of ONNX into DirectML
				// still takes seconds, so it stays off the main thread.
				m_loader = std::thread([this]() { loadWorker(); });
			}

			bool isReady() const { return m_ready.load(oc::memory_order_acquire); }
			bool hasFailed() const { return m_failed.load(oc::memory_order_acquire); }
			// The model's own resolution, in metres per pixel (30 in the shipped config).
			float nativeResolution() const { return m_nativeResolution.load(oc::memory_order_relaxed); }
			// Native pixels per coarse pixel (256 in the shipped config).
			int32 nativePerCoarsePixel() const { return m_nativePerCoarse.load(oc::memory_order_relaxed); }

			oc::string statusText() const
			{
				if (m_ready.load(oc::memory_order_acquire))
					return "Ready";
				std::lock_guard<std::mutex> lk(m_statusMutex);
				if (m_failed.load(oc::memory_order_acquire))
					return m_assets.error().empty() ? "Failed - see log" : m_assets.error();
				return m_status;
			}

			// Blocking on a miss. Serialised through m_pipelineMutex: the tile store is not thread-safe.
			FieldTilePtr fetchTile(int32 ti, int32 tj);
			// The coarse-stage counterpart. Also blocking, but a tiny fraction of the cost per unit area.
			FieldTilePtr fetchCoarseTile(int32 ti, int32 tj);

			void setSeed(uint64 seed)
			{
				// Mirror first, unconditionally (this runs even before isReady): the disk cache builds its
				// per-seed paths from this without taking the pipeline lock.
				m_activeSeed.store(seed, oc::memory_order_relaxed);
				if (!isReady())
					return;
				JobMutex::Scope lk(m_pipelineMutex);
				if (seed == m_pipeline->seed())
					return;
				m_pipeline->setSeed(seed);
				{
					std::lock_guard<std::mutex> ck(m_cacheMutex);
					m_cache.clear();
					m_coarseCache.clear();
				}
				Log::info(oc::format("[Diffusion] reseeded to {}", seed));
			}

			void setMaxResidentTiles(int32 n)
			{
				std::lock_guard<std::mutex> ck(m_cacheMutex);
				m_maxTiles = oc::max(4, n);
				evictLocked();
			}

			// Unlike setSeed, this CANNOT be done in place: precision is a property of the model file, so the
			// sessions have to be torn down and up to 2.28 GB reloaded. Callers see isReady() drop and come
			// back, which is the same handover TerrainStreamer already polls for at startup.
			// Called before beginLoad() on the first construction, so the usual path picks the precision up
			// without ever paying for a reload.
			void setPrecision(EPrecision p)
			{
				std::lock_guard<std::mutex> lk(m_loadMutex);
				m_activeFp16.store(p == EPrecision::Fp16, oc::memory_order_relaxed); // disk-cache path mirror
				if (p == m_precision)
					return;
				if (!m_loadStarted)
				{
					m_precision = p; // nothing to reload; loadWorker will read it
					return;
				}

				// Join FIRST: the old worker may still be building a pipeline, and m_precision must not
				// change under it. Blocks the caller for the rest of an in-flight load (seconds, once, only
				// when the toggle is flipped mid-load); afterwards the thread has already exited and this
				// returns at once.
				if (m_loader.joinable())
					m_loader.join();
				m_precision = p;

				m_ready.store(false, oc::memory_order_release);
				m_failed.store(false, oc::memory_order_release);
				setStatus("Reloading models...");
				{
					// Waits out any inference already running. New fetches then see a null pipeline and
					// return no tile, which the sample path already treats as "not generated yet".
					JobMutex::Scope pk(m_pipelineMutex);
					m_pipeline.reset();
				}
				{
					// The weights changed, so every cached tile is from the wrong model.
					std::lock_guard<std::mutex> ck(m_cacheMutex);
					m_cache.clear();
					m_coarseCache.clear();
				}
				Log::info(oc::format("[Diffusion] switching to {} - reloading models",
				                      p == EPrecision::Fp16 ? "fp16" : "fp32"));
				m_loader = std::thread([this]() { loadWorker(); });
			}

		private:
			DiffusionRuntime() = default;
			~DiffusionRuntime()
			{
				if (m_loader.joinable())
					m_loader.join();
			}

			void setStatus(oc::string_view s)
			{
				std::lock_guard<std::mutex> lk(m_statusMutex);
				m_status.assign(s.data(), s.size());
			}

			void loadWorker()
			{
				Globals::profiler.registerThread("V3 Loader", Profiler::SORT_KEY_BACKGROUND + 2);
				ProfileScope scope("DiffusionRuntime::loadWorker", EProfileCategory::Procedural);

				setStatus("Locating models...");
				if (!m_assets.load(EAssetSet::Full))
				{
					m_failed.store(true, oc::memory_order_release);
					return;
				}
				m_nativeResolution.store(m_assets.config().nativeResolution, oc::memory_order_relaxed);
				m_nativePerCoarse.store(32 * m_assets.config().latentCompression, oc::memory_order_relaxed);

				// Read once, without m_loadMutex: setPrecision only ever writes it with no loader running
				// (it joins first), and starting this thread is the happens-before edge that publishes it.
				const EPrecision precision = m_precision;
				if (precision == EPrecision::Fp16 && !ModelAssets::hasFp16Models(EAssetSet::Full))
					Log::warning("[Diffusion] fp16 requested but some *_fp16.onnx models are missing or are "
					             "unfetched git-lfs pointers - those stages stay fp32. Try 'git lfs pull', or "
					             "regenerate them with Tools/convert_models_fp16.py.");

				setStatus("Loading models onto the GPU...");
				auto p = oc::make_unique<WorldPipeline>();
				if (!p->initialize(m_seedAtLoad, m_assets.config(), m_assets.data(),
				                   EInferenceDevice::Gpu, /*coarseOnly*/ false, precision))
				{
					m_failed.store(true, oc::memory_order_release);
					return;
				}
				{
					JobMutex::Scope lk(m_pipelineMutex);
					// What the live pipeline was actually built with — the disk cache labels SAVES with this
					// (readable wherever m_pipeline is non-null under the lock), never with the request-side
					// mirror, so a precision flip racing a generation can't file a tile under the wrong folder.
					m_pipelinePrecision = precision;
					m_pipeline = oc::move(p);
				}
				m_ready.store(true, oc::memory_order_release);
				Log::info("[Diffusion] V3 terrain ready");
			}

			// Access-ordered LRU. Splicing on touch is O(1) and never invalidates the iterators the map
			// holds. (A deque + oc::find was O(residentTiles) per LOOKUP, and lookups happen per sample.)
			struct Lru
			{
				struct Entry
				{
					uint64 key;
					FieldTilePtr value;
				};
				oc::list<Entry> order; // front = least recently used
				oc::unordered_map<uint64, oc::list<Entry>::iterator> map;

				FieldTilePtr get(uint64 key)
				{
					auto it = map.find(key);
					if (it == map.end())
						return nullptr;
					order.splice(order.end(), order, it->second);
					return it->second->value;
				}
				void put(uint64 key, FieldTilePtr v)
				{
					auto it = map.find(key);
					if (it != map.end())
					{
						it->second->value = oc::move(v);
						order.splice(order.end(), order, it->second);
						return;
					}
					order.push_back(Entry{ key, oc::move(v) });
					map[key] = oc::prev(order.end());
				}
				void trim(size_t maxSize)
				{
					while (order.size() > maxSize)
					{
						map.erase(order.front().key);
						order.pop_front();
					}
				}
				void clear()
				{
					order.clear();
					map.clear();
				}
			};

			void evictLocked()
			{
				m_cache.trim((size_t)oc::max(4, m_maxTiles));
				// Coarse tiles are ~52 KB and each covers hundreds of km, so a small fixed budget is plenty —
				// and they must never be evicted by full-detail pressure, since they ARE the cheap fallback.
				m_coarseCache.trim(64);
			}

			ModelAssets m_assets;
			std::thread m_loader;
			std::mutex m_loadMutex;
			bool m_loadStarted = false;
			uint64 m_seedAtLoad = 1337;
			EPrecision m_precision = EPrecision::Fp32; // guarded by m_loadMutex; see setPrecision
			oc::atomic<bool> m_ready{ false };
			oc::atomic<bool> m_failed{ false };
			oc::atomic<float> m_nativeResolution{ 30.0f };
			oc::atomic<int32> m_nativePerCoarse{ 256 };
			mutable std::mutex m_statusMutex;
			oc::string m_status;

			// The pipeline is single-threaded by construction; this lock IS the inference thread.
			// A JobMutex so a ~1.5s cold-tile wait from a job parks its FIBER instead of a pooled
			// worker thread (unregistered threads still just block).
			JobMutex m_pipelineMutex;
			oc::unique_ptr<WorldPipeline> m_pipeline;
			EPrecision m_pipelinePrecision = EPrecision::Fp32; // guarded by m_pipelineMutex; see loadWorker

			// Request-side mirrors for building disk-cache paths WITHOUT the pipeline lock (a disk hit must
			// not queue behind another tile's inference). Guesses, not authority: a load that raced a
			// reseed/precision flip just misses (the header validates) and falls through to generation,
			// which labels its save from the pipeline itself.
			oc::atomic<uint64> m_activeSeed{ 1337 };
			oc::atomic<bool> m_activeFp16{ false };

			std::mutex m_cacheMutex;
			Lru m_cache;
			Lru m_coarseCache;
			int32 m_maxTiles = 64;
			// Dedups concurrent requests for the same tile so N callers cause 1 generation, not N;
			// joiners park on the tile's JobEvent instead of blocking a worker on a shared_future.
			struct PendingTile
			{
				JobEvent done;
				FieldTilePtr result;
			};
			oc::unordered_map<uint64, oc::shared_ptr<PendingTile>> m_pending;
			std::mutex m_pendingMutex;
		};

		FieldTilePtr DiffusionRuntime::fetchTile(int32 ti, int32 tj)
		{
			const uint64 key = tileKey(ti, tj);
			{
				std::lock_guard<std::mutex> ck(m_cacheMutex);
				if (FieldTilePtr hit = m_cache.get(key))
					return hit;
			}

			// Not resident. Claim the generation, or join whoever already claimed it, so N concurrent
			// callers cause one generation rather than N.
			oc::shared_ptr<PendingTile> pending;
			oc::shared_ptr<oc::function<FieldTilePtr()>> ownerTask;
			bool owner = false;
			{
				std::lock_guard<std::mutex> pk(m_pendingMutex);
				auto it = m_pending.find(key);
				if (it != m_pending.end())
				{
					pending = it->second;
				}
				else
				{
					auto task = oc::make_shared<oc::function<FieldTilePtr()>>([this, ti, tj]() -> FieldTilePtr
					{
						// Disk first, WITHOUT the pipeline lock: a cached tile must not queue behind another
						// tile's ~1.5 s inference. The post-load mirror re-check pins the race where a reseed
						// lands between path building and here — a changed seed drops the loaded tile and
						// falls through to generation against the live pipeline.
						{
							const uint64 seed = m_activeSeed.load(oc::memory_order_relaxed);
							const bool fp16 = m_activeFp16.load(oc::memory_order_relaxed);
							FieldTilePtr t = loadTileFromDisk(tileCachePath(seed, fp16, false, ti, tj),
							                                  seed, ti, tj, TILE_W);
							if (t && m_activeSeed.load(oc::memory_order_relaxed) == seed)
								return t;
						}

						// One inference at a time: the tile store is not thread-safe.
						JobMutex::Scope lk(m_pipelineMutex);
						if (!m_pipeline)
							return nullptr;

						const int32 i1 = ti * TILE - HALO, i2 = (ti + 1) * TILE + HALO;
						const int32 j1 = tj * TILE - HALO, j2 = (tj + 1) * TILE + HALO;

						oc::vector<float> elev, climate, macro;
						if (!m_pipeline->get(i1, j1, i2, j2, /*withClimate*/ true, elev, climate, &macro))
							return nullptr;

						const size_t plane = (size_t)TILE_W * TILE_W;
						auto t = oc::make_shared<FieldTile>();
						t->width = TILE_W;
						t->elev = oc::move(elev);
						t->macro = oc::move(macro);
						t->precip.assign(climate.begin() + (ptrdiff_t)(2 * plane),
						                 climate.begin() + (ptrdiff_t)(3 * plane));
						// Recover the SEA-LEVEL baseline here, once, instead of storing (temp, beta) and
						// redoing the subtraction per sample: computeClimate constructed channel 0 as
						// tSea + beta*max(0, elev), so undoing it with the same elevation is exact.
						t->tempSea.resize(plane);
						const float* temp = climate.data();                     // channel 0
						const float* beta = climate.data() + (size_t)4 * plane; // channel 4 = lapse rate
						for (size_t i = 0; i < plane; i++)
							t->tempSea[i] = temp[i] - beta[i] * oc::max(0.0f, t->elev[i]);

						// Labeled from the pipeline itself — the seed/precision this tile was ACTUALLY built
						// with — never the mirrors. The write is ~1 MB against ~1.5 s of inference, so holding
						// the lock for it costs nothing.
						const uint64 seed = m_pipeline->seed();
						saveTileToDisk(tileCachePath(seed, m_pipelinePrecision == EPrecision::Fp16, false, ti, tj),
						               *t, seed, ti, tj);
						return t;
					});
					pending = oc::make_shared<PendingTile>();
					m_pending[key] = pending;
					ownerTask = task;
					owner = true;
				}
			}

			// The claimant generates on its own thread; everyone else waits on the future. Callers are
			// already worker threads that expect to block (the terrain streamer worker, the scatter worker,
			// the height-map baker's std::async), and m_pipelineMutex is what actually serialises inference —
			// so a dedicated inference thread would only add a hop and a thread per tile.
			if (!owner)
			{
				// Joiners park on the tile's event: a fiber frees its worker to run other jobs, an
				// unregistered thread blocks - either way one generation serves everyone.
				pending->done.wait();
				return pending->result;
			}

			FieldTilePtr tile = pending->result = (*ownerTask)();

			if (tile)
			{
				// cache first so callers arriving after the pending-erase below hit it immediately
				std::lock_guard<std::mutex> ck(m_cacheMutex);
				m_cache.put(key, tile);
				evictLocked();
			}
			{
				std::lock_guard<std::mutex> pk(m_pendingMutex);
				m_pending.erase(key);
			}
			pending->done.signal(); // joiners read pending->result, ordered by the event
			return tile;
		}

		FieldTilePtr DiffusionRuntime::fetchCoarseTile(int32 ti, int32 tj)
		{
			const uint64 key = tileKey(ti, tj);
			{
				std::lock_guard<std::mutex> ck(m_cacheMutex);
				if (FieldTilePtr hit = m_coarseCache.get(key))
					return hit;
			}

			// Disk before the pipeline lock, same shape as fetchTile's load (including the mirror re-check).
			{
				const uint64 seed = m_activeSeed.load(oc::memory_order_relaxed);
				const bool fp16 = m_activeFp16.load(oc::memory_order_relaxed);
				FieldTilePtr t = loadTileFromDisk(tileCachePath(seed, fp16, true, ti, tj),
				                                  seed, ti, tj, CTILE_W);
				if (t && m_activeSeed.load(oc::memory_order_relaxed) == seed)
				{
					std::lock_guard<std::mutex> ck(m_cacheMutex);
					m_coarseCache.put(key, t);
					evictLocked();
					return t;
				}
			}

			// No pending-dedup here, unlike fetchTile: a coarse tile is a handful of 64x64 model calls, and
			// cheapness is the entire point of this path. Two threads racing the same tile is a rare, small
			// waste; the pipeline lock below already serialises them.
			FieldTilePtr tile;
			{
				JobMutex::Scope lk(m_pipelineMutex);
				if (!m_pipeline)
					return nullptr;
				{
					// Someone may have filled it while we waited on the pipeline lock.
					std::lock_guard<std::mutex> ck(m_cacheMutex);
					if (FieldTilePtr hit = m_coarseCache.get(key))
						return hit;
				}

				// PADDED by the regression window: unlike the full path, the coarse tensor carries no lapse
				// rate (its channels are elev/p5/temp/temp_std/precip/precip_std), so this path has to
				// regress its own the same way computeClimate does — and localBaselineTemperature crops
				// (win - 1) from each axis. Fetching CBETA_PAD extra on every side makes its output land
				// exactly on the tile.
				constexpr int32 CBETA_WIN = 15;             // same window computeClimate regresses over
				constexpr int32 CBETA_PAD = CBETA_WIN / 2;  // 7
				const int32 i0 = ti * CTILE - CHALO, i1 = (ti + 1) * CTILE + CHALO;
				const int32 j0 = tj * CTILE - CHALO, j1 = (tj + 1) * CTILE + CHALO;
				const FloatTensor slice = m_pipeline->getCoarseSlice(i0 - CBETA_PAD, j0 - CBETA_PAD,
				                                                     i1 + CBETA_PAD, j1 + CBETA_PAD);

				const int32 pw = CTILE_W + 2 * CBETA_PAD;
				const size_t pplane = (size_t)pw * pw;
				const size_t plane = (size_t)CTILE_W * CTILE_W;
				assert(slice.data.size() == 7 * pplane);

				// Decode the padded region once: signed elevation for the tile itself, and the
				// clamped-to-sea-level copy the regression wants (computeClimate clamps for the same reason
				// — the lapse is only meaningful over land).
				Grid pTemp(pw, pw), pElevLand(pw, pw);
				oc::vector<float> pElev(pplane), pPrecip(pplane);
				for (size_t i = 0; i < pplane; i++)
				{
					const float w = slice.data[6 * pplane + i];
					const float inv = (w > 1e-6f) ? 1.0f / w : 0.0f;
					// Undo the model's signed-sqrt elevation compression, keeping the SIGN: the far cascade
					// feeds the fog and the ocean, which need the seabed, unlike the pipeline's own climate
					// path which clamps ocean to zero.
					const float e = slice.data[0 * pplane + i] * inv;
					pElev[i] = (e > 0.0f ? 1.0f : (e < 0.0f ? -1.0f : 0.0f)) * e * e;
					pTemp.data[i] = slice.data[2 * pplane + i] * inv;
					pPrecip[i] = slice.data[4 * pplane + i] * inv;
					const float el = oc::max(0.0f, e);
					pElevLand.data[i] = el * el;
				}

				Grid tSea, betaG;
				localBaselineTemperature(pTemp, pElevLand, CBETA_WIN, 0.02f, tSea, betaG);
				assert(tSea.h == CTILE_W && tSea.w == CTILE_W);

				auto t = oc::make_shared<FieldTile>();
				t->width = CTILE_W;
				t->elev.resize(plane);
				t->tempSea.resize(plane);
				t->precip.resize(plane);
				for (int32 r = 0; r < CTILE_W; r++)
					for (int32 c = 0; c < CTILE_W; c++)
					{
						const size_t dst = (size_t)r * CTILE_W + c;
						const size_t src = (size_t)(r + CBETA_PAD) * pw + (c + CBETA_PAD);
						t->elev[dst] = pElev[src];
						t->precip[dst] = pPrecip[src];
						// The FITTED baseline, not one derived from the model's raw coarse temperature. The
						// raw value carries a residual the fit does not — but the full path already threw
						// exactly that away: computeClimate CONSTRUCTS its temperature as tSea + beta*elev
						// and never uses the raw coarse temperature at all (keeping the raw value here left
						// the two levels disagreeing by that residual: measured 1.53 C mean, 8.9 C max).
						// Fitting both the same way is what actually makes them converge — and since the
						// full path's tile build now UNDOES that construction to get its baseline back,
						// this path just stores its fit directly.
						t->tempSea[dst] = tSea.at(r, c);
					}
				// At 7.68 km per pixel the coarse level IS the macro surface, so macro == elev and this
				// path's own (elev - macro) is zero — a coarse tile cannot tell a crag from flat ground.
				// That costs nothing: the shader never subtracts these two. It measures crag as the
				// full-detail MESH height (which it always has) minus this macro, and the FULL path now
				// reports this same coarse surface (WorldPipeline::computeCoarseSurface), so both detail
				// levels hand it the same reference and cannot disagree.
				t->macro = t->elev;
				tile = t;

				// Same contract as fetchTile's save: labeled from the pipeline, inside its lock.
				const uint64 seed = m_pipeline->seed();
				saveTileToDisk(tileCachePath(seed, m_pipelinePrecision == EPrecision::Fp16, true, ti, tj),
				               *t, seed, ti, tj);
			}

			std::lock_guard<std::mutex> ck(m_cacheMutex);
			m_coarseCache.put(key, tile);
			evictLocked();
			return tile;
		}
	}

	// =====================================================================================================
	// TerrainGenV3
	// =====================================================================================================

	// One evaluation of the diffusion field at a world position.
	struct TerrainGenV3::Sample
	{
		float elev = 0.0f;     // metres in the MODEL's frame (before worldScale/heightScale)
		float macro = 0.0f;    // the low-frequency band of elev, same frame
		float tempSea = 15.0f; // C, the SEA-LEVEL baseline (see FieldTile::tempSea)
		float precip = 800.0f; // mm/yr
		float slope = 0.0f;    // m/m in WORLD space (already through worldScale * heightScale)
		bool valid = false;
	};

	// A block of tiles covering a query region, fetched UP FRONT — one shared-cache lock per tile — and
	// then sampled with no locking at all. This is what sampleGrid buys over per-point fetching: a
	// 512x512 bake resolves to a couple of dozen tiles, not a quarter-million lock/unlock pairs.
	struct TerrainGenV3::TileBlock
	{
		oc::vector<FieldTilePtr> tiles; // row-major over [ti0, ti0+th) x [tj0, tj0+tw)
		int32 ti0 = 0, tj0 = 0, tw = 0, th = 0;
		int32 span = 0;                  // lattice pixels per tile (TILE or CTILE)
		int32 halo = 0;
		// lattice coordinate = world * scale + offset.
		// The offset is NOT decoration: the coarse lattice is PIXEL-CENTRED with respect to the native
		// one. WorldPipeline::computeClimate maps native -> coarse as (native + 0.5)/S - 0.5, i.e. coarse
		// pixel c is centred at native (c + 0.5)*S. Sampling it as native/S instead shifts the entire
		// coarse field by HALF A COARSE PIXEL — 128 native pixels, which is 3.8 km at 30 m/px — so the
		// far cascade's climate lands somewhere else than the near cascade's and the textures disagree
		// for the same position.
		double latticeScale = 0.0;
		double latticeOffset = 0.0;
		bool coarse = false;

		double latticeX(double worldX) const { return worldX * latticeScale + latticeOffset; }
		double latticeZ(double worldZ) const { return worldZ * latticeScale + latticeOffset; }
		double worldPerLattice() const { return latticeScale > 0.0 ? 1.0 / latticeScale : 0.0; }

		const FieldTile* tileAt(int32 ti, int32 tj) const
		{
			const int32 ri = ti - ti0, rj = tj - tj0;
			if (ri < 0 || ri >= th || rj < 0 || rj >= tw)
				return nullptr;
			return tiles[(size_t)ri * tw + rj].get();
		}
	};

	// metersPerPixel is a true world SCALE, not just a horizontal one: it shrinks elevation by the same
	// factor, so a compressed world keeps the proportions the model drew (same slopes, same shape, just
	// smaller). heightScale is then a pure vertical exaggeration on top.
	// Resolves to the static worldScale(float) overload -- one argument, so the const overload is not a
	// candidate. Same number, computed in one place.
	float TerrainGenV3::worldScale() const { return worldScale(m_cfg.metersPerPixel); }
	// Model metres -> world metres.
	float TerrainGenV3::vertScale() const { return worldScale() * m_cfg.heightScale; }

	// Resolve every tile a world-space rect touches. Returns false if the models aren't up yet.
	bool TerrainGenV3::resolveBlock(double x0, double z0, double x1, double z1, bool coarse, TileBlock& out) const
	{
		DiffusionRuntime& rt = DiffusionRuntime::get();
		if (!rt.isReady())
			return false;

		out.coarse = coarse;
		out.span = coarse ? CTILE : TILE;
		out.halo = coarse ? CHALO : HALO;

		if (!coarse)
		{
			out.latticeScale = (double)m_invMpp; // world -> native pixels, 1:1 with the tile lattice
			out.latticeOffset = 0.0;
		}
		else
		{
			const int32 npc = rt.nativePerCoarsePixel();
			if (npc <= 0)
				return false;
			// coarse = (native + 0.5)/npc - 0.5, matching WorldPipeline::computeClimate exactly.
			out.latticeScale = (double)m_invMpp / (double)npc;
			out.latticeOffset = 0.5 / (double)npc - 0.5;
		}
		if (out.latticeScale <= 0.0)
			return false;

		// +-1 lattice pixel of slack so the bilinear/gradient taps at the rect's edge stay in range.
		const int32 j0 = (int32)std::floor(out.latticeX(x0)) - 1;
		const int32 j1 = (int32)std::floor(out.latticeX(x1)) + 1;
		const int32 i0 = (int32)std::floor(out.latticeZ(z0)) - 1;
		const int32 i1 = (int32)std::floor(out.latticeZ(z1)) + 1;

		out.ti0 = floorDiv(i0, out.span);
		out.tj0 = floorDiv(j0, out.span);
		out.th = floorDiv(i1, out.span) - out.ti0 + 1;
		out.tw = floorDiv(j1, out.span) - out.tj0 + 1;
		if (out.th <= 0 || out.tw <= 0)
			return false;

		out.tiles.assign((size_t)out.th * out.tw, nullptr);
		for (int32 ti = 0; ti < out.th; ti++)
			for (int32 tj = 0; tj < out.tw; tj++)
				out.tiles[(size_t)ti * out.tw + tj] = coarse
					? rt.fetchCoarseTile(out.ti0 + ti, out.tj0 + tj)
					: rt.fetchTile(out.ti0 + ti, out.tj0 + tj);
		return true;
	}

	// Sample a point from an already-resolved block. No locks, no fetches.
	TerrainGenV3::Sample TerrainGenV3::sampleFromBlock(const TileBlock& b, double worldX, double worldZ) const
	{
		Sample s;
		// j <-> x, i <-> z (the pipeline's i is the row axis).
		const double nj = b.latticeX(worldX);
		const double ni = b.latticeZ(worldZ);
		const int32 j0 = (int32)std::floor(nj);
		const int32 i0 = (int32)std::floor(ni);
		const float fj = (float)(nj - (double)j0);
		const float fi = (float)(ni - (double)i0);

		const int32 ti = floorDiv(i0, b.span), tj = floorDiv(j0, b.span);
		const FieldTile* tile = b.tileAt(ti, tj);
		if (!tile)
			return s;

		const int32 w = tile->width;
		const int32 li = i0 - (ti * b.span - b.halo);
		const int32 lj = j0 - (tj * b.span - b.halo);
		assert(li >= 0 && li + 1 < w && lj >= 0 && lj + 1 < w);

		auto bilerp = [&](const oc::vector<float>& v) -> float
		{
			const float a = v[(size_t)li * w + lj];
			const float b2 = v[(size_t)li * w + lj + 1];
			const float c = v[(size_t)(li + 1) * w + lj];
			const float d = v[(size_t)(li + 1) * w + lj + 1];
			return (1 - fi) * ((1 - fj) * a + fj * b2) + fi * ((1 - fj) * c + fj * d);
		};

		s.elev = bilerp(tile->elev);
		s.macro = bilerp(tile->macro);
		s.tempSea = bilerp(tile->tempSea);
		s.precip = bilerp(tile->precip);

		// Central differences over the lattice -> WORLD slope. The halo is what lets this work at a tile
		// edge without a seam. The vertical scaling matters: without it, compressing the world would
		// silently steepen every slope, which then skews the detail mask and the fog. With it, worldScale
		// cancels and the slope depends only on heightScale — i.e. proportions hold.
		const float eL = tile->elev[(size_t)li * w + (lj > 0 ? lj - 1 : lj)];
		const float eR = tile->elev[(size_t)li * w + lj + 1];
		const float eU = tile->elev[(size_t)(li > 0 ? li - 1 : li) * w + lj];
		const float eD = tile->elev[(size_t)(li + 1) * w + lj];
		const float step = 2.0f * (float)b.worldPerLattice(); // world metres between the two taps
		const float vs = vertScale();
		const float dx = (eR - eL) * vs / step;
		const float dz = (eD - eU) * vs / step;
		s.slope = std::sqrt(dx * dx + dz * dz);
		s.valid = true;
		return s;
	}

	// The point path: resolve a one-tile block and sample it.
	TerrainGenV3::Sample TerrainGenV3::sampleField(double worldX, double worldZ, bool coarse) const
	{
		TileBlock b;
		if (!resolveBlock(worldX, worldZ, worldX, worldZ, coarse, b))
			return Sample{};
		return sampleFromBlock(b, worldX, worldZ);
	}

	// The slope mask: sf^2 * sqrt(sf), so detail ramps in sharply and plains stay perfectly smooth.
	float TerrainGenV3::slopeMask(float slope) const
	{
		const float sf = oc::min(1.0f, slope * m_cfg.detailSlopeGain);
		return sf * sf * std::sqrt(sf);
	}

	// Sub-model-pixel relief, in WORLD metres. Both the wavelengths and the amplitudes ride worldScale,
	// so a compressed world gets proportionally finer, shallower crags rather than the same absolute
	// ones. Not affected by heightScale: the detail amplitudes are already direct tweaks in metres.
	float TerrainGenV3::detail(double worldX, double worldZ, float slope) const
	{
		const float mask = slopeMask(slope);
		if (mask <= 0.0f)
			return 0.0f;
		const float scale = worldScale();
		const float fA = 1.0f / (m_cfg.detailWavelengthA * scale);
		const float fB = 1.0f / (m_cfg.detailWavelengthB * scale);
		const float a = m_detailA.fbm((float)(worldX * fA), (float)(worldZ * fA), m_cfg.detailOctavesA);
		const float b = m_detailB.fbm((float)(worldX * fB), (float)(worldZ * fB), m_cfg.detailOctavesB);
		return (a * m_cfg.detailAmplitudeA + b * m_cfg.detailAmplitudeB) * mask * scale;
	}

	// Climate lives in the MODEL's elevation frame and stays there. metersPerPixel and heightScale are
	// presentation choices — we draw the same planet at a different size, not relocate it — so the
	// climate rides along unscaled and a shrunk world keeps the snow caps and biome bands of the
	// full-size one. Only the procedural detail adds relief the model never saw, so it alone earns a
	// lapse correction (converted back into model metres to stay in that frame).
	// A slow wander in degrees C. This is what stops every climate boundary from being a contour line:
	// see TerrainConfigV3::climateNoiseAmplitudeC. A pure function of world position, so the near (Full)
	// and far (Coarse) terrain-data cascades get identical values and cannot disagree about it.
	float TerrainGenV3::climateWander(double worldX, double worldZ) const
	{
		if (m_cfg.climateNoiseAmplitudeC <= 0.0f)
			return 0.0f;
		const float f = 1.0f / oc::max(1.0f, m_cfg.climateNoiseWavelength * worldScale());
		return m_climateNoise.fbm((float)(worldX * f), (float)(worldZ * f), m_cfg.climateNoiseOctaves)
		     * m_cfg.climateNoiseAmplitudeC;
	}

	// THE lapse rate, C per MODEL metre. One number for the world (see TerrainConfigV3::lapseRate), so
	// the temperature is an evaluable function of height rather than a field of samples.
	float TerrainGenV3::lapseOf() const { return oc::min(m_cfg.lapseRate, 0.0f); }

	// The temperature this point would have AT SEA LEVEL: the baseline the lapse rate is applied to.
	// This — not a temperature — is what the terrain-data map bakes, so consumers can evaluate at their
	// own height (see TerrainPoint::temperatureSeaLevel).
	//
	// Everything that does not depend on height belongs here, and everything that does belongs in the
	// lapse. The wander is a property of WHERE you are, not how high, so it rides the baseline; the
	// snow-line tweak scales with altitude, so it is folded into lapseOf instead.
	float TerrainGenV3::temperatureSeaLevelFrom(double worldX, double worldZ, const Sample& s) const
	{
		// The baseline is recovered at TILE BUILD now (the model's temp = tSea + beta*elev round-trip
		// is undone there, once per pixel instead of once per sample), so this is just the shaping layer:
		// the microclimate wander and the planet-wide offset.
		return s.tempSea + climateWander(worldX, worldZ) + m_cfg.temperatureOffset;
	}

	// Temperature at the point's FULL surface height (model elevation + the procedural detail).
	//
	// Literally the baseline evaluated at this height, so it CANNOT disagree with what the map bakes —
	// which was worth restructuring for. The previous form built the temperature by bolting corrections
	// onto the model's value, and the detail layer's correction used a hardcoded -0.0065 while the
	// model's own relief used the regressed beta; the result was not an affine function of the reported
	// height at all, and the consumer trying to move it off that height was left ~1.5 C short.
	float TerrainGenV3::temperatureFrom(double worldX, double worldZ, const Sample& s, float detailWorldM) const
	{
		const float elevWithDetail = s.elev + detailWorldM / oc::max(0.01f, vertScale());
		return temperatureSeaLevelFrom(worldX, worldZ, s) + lapseOf() * oc::max(0.0f, elevWithDetail);
	}

	float TerrainGenV3::humidityOf(const Sample& s) const
	{
		const float h = s.precip / oc::max(1.0f, m_cfg.precipForFullHumidity) + m_cfg.humidityOffset;
		return oc::clamp(h, 0.0f, 1.0f);
	}

	float TerrainGenV3::fogThicknessOf(const Sample& s) const
	{
		// Two contributions, both keyed off the model's real climate: wet regions carry broad haze, and
		// flat land collects valley fog.
		const float humidity = humidityOf(s);
		const float humid = m_cfg.humidFogAmount * humidity * humidity;
		const float flat = 1.0f - oc::min(1.0f, s.slope * 2.0f);
		const float land = s.elev > 0.0f ? 1.0f : 0.0f;
		const float valley = m_cfg.valleyFogAmount * flat * land * humidity;
		return oc::clamp(oc::max(humid, valley), 0.0f, 1.0f);
	}

	float TerrainGenV3::fogFalloffOf(float temperature) const
	{
		// Cold air hugs the ground; warm humid air lets fog tower.
		const float t01 = oc::clamp((temperature + 10.0f) / 40.0f, 0.0f, 1.0f);
		return oc::clamp(1.8f - 1.2f * t01, 0.0f, FOG_FALLOFF_MUL_MAX);
	}

	// Everything from one field evaluation. `withDetail` is false on the coarse path: sub-model-pixel
	// noise is meaningless at a far cascade's texel density and would only cost noise lookups.
	void TerrainGenV3::fill(double worldX, double worldZ, const Sample& s, bool withDetail, TerrainPoint& out) const
	{
		if (!s.valid)
		{
			out = TerrainPoint{};
			out.height = m_cfg.seaLevel;
			out.waterLevel = m_cfg.seaLevel;
			return;
		}
		const float d = withDetail ? detail(worldX, worldZ, s.slope) : 0.0f;
		const float vs = vertScale();
		// altitude is the MACRO band, NOT the full surface. The terrain shader's rock layer keys on the
		// crag relief (height - altitude) to tell a mountain from flat ground at altitude, so reporting
		// the full elevation here made that difference the detail layer alone (a few metres) and no
		// slope ever grew rock — mountain tops came out textured as whatever biome the lowland was.
		// The macro surface, unperturbed. The crag WANDER that stops the rock boundary being a contour
		// line lives in the terrain shader, not here: it has to be scaled by the local relief (so it can
		// only modulate relief that exists, never invent rock on a plain), and relief is exactly what
		// this path cannot supply on the coarse level — macro == elev there, so relief is 0 by
		// construction and the far cascade would silently get no wander at all while the near one did.
		// The shader has the full-detail MESH height whichever cascade it reads, so it can always
		// compute the real relief. See terrainSplat / u_terrainTexParams5.
		out.altitude = s.macro * vs;
		out.height = m_cfg.seaLevel + s.elev * vs + d;
		out.waterLevel = m_cfg.seaLevel;
		out.temperature = temperatureFrom(worldX, worldZ, s, d);
		// What the map bakes. The rate that pairs with it is published once (lapseRatePerMetre), not
		// per point.
		out.temperatureSeaLevel = temperatureSeaLevelFrom(worldX, worldZ, s);
		out.humidity = humidityOf(s);
		out.fogThickness = fogThicknessOf(s);
		out.fogFalloffMul = fogFalloffOf(out.temperature);
		out.flowAngle01 = -1.0f;
	}

	TerrainGenV3::TerrainGenV3(const TerrainConfigV3& cfg)
		: m_cfg(cfg)
		, m_detailA(cfg.seed * 0x9E3779B9u + 0x51ED2701u)
		, m_detailB(cfg.seed * 0x85EBCA6Bu + 0xC2B2AE35u)
		, m_climateNoise(cfg.seed * 0x27220A95u + 0x165667B1u)
	{
		m_cfg.metersPerPixel = oc::max(0.5f, cfg.metersPerPixel);
		m_invMpp = 1.0f / m_cfg.metersPerPixel;

		// Before beginLoad: on the first construction this just records the choice, so the initial load
		// reads the right weights instead of loading fp32 and immediately reloading.
		DiffusionRuntime::get().setPrecision(cfg.useFp16 ? EPrecision::Fp16 : EPrecision::Fp32);
		DiffusionRuntime::get().beginLoad();
		DiffusionRuntime::get().setSeed((uint64)cfg.seed);
		DiffusionRuntime::get().setMaxResidentTiles(cfg.maxResidentTiles);
	}

	void TerrainGenV3::beginLoad() { DiffusionRuntime::get().beginLoad(); }
	float TerrainGenV3::worldScale(float metersPerPixel)
	{
		const float nr = DiffusionRuntime::get().nativeResolution();
		return metersPerPixel / (nr > 0.0f ? nr : 30.0f);
	}
	void TerrainGenV3::setPrecision(bool useFp16)
	{
		DiffusionRuntime::get().setPrecision(useFp16 ? EPrecision::Fp16 : EPrecision::Fp32);
	}
	bool TerrainGenV3::isReady() { return DiffusionRuntime::get().isReady(); }
	bool TerrainGenV3::hasFailed() { return DiffusionRuntime::get().hasFailed(); }
	oc::string TerrainGenV3::statusText() { return DiffusionRuntime::get().statusText(); }

	const TerrainConfigV3& TerrainGenV3::config() const { return m_cfg; }
	float TerrainGenV3::seaLevel() const { return m_cfg.seaLevel; }
	float TerrainGenV3::lapseRatePerMetre() const { return lapseOf(); }

	float TerrainGenV3::sampleAltitude(double worldX, double worldZ) const
	{
		// The MACRO band (see fill): the smooth surface the high-frequency residual rides on, which is
		// what makes height - altitude a meaningful "how craggy is this" measure.
		const Sample s = sampleField(worldX, worldZ);
		return s.macro * vertScale();
	}

	float TerrainGenV3::sampleHeight(double worldX, double worldZ) const
	{
		const Sample s = sampleField(worldX, worldZ);
		if (!s.valid)
			return m_cfg.seaLevel;
		return m_cfg.seaLevel + s.elev * vertScale()
			+ detail(worldX, worldZ, s.slope);
	}

	float TerrainGenV3::sampleWaterHeight(double, double) const
	{
		// The model's elevation is already relative to sea level, so the sea is exactly seaLevel.
		// (Elevated water — lakes, rivers — is not modelled yet.)
		return m_cfg.seaLevel;
	}

	float TerrainGenV3::sampleHeightAndWater(double worldX, double worldZ, float& outWaterLevel) const
	{
		outWaterLevel = m_cfg.seaLevel;
		return sampleHeight(worldX, worldZ);
	}

	float TerrainGenV3::sampleTemperature(double worldX, double worldZ) const
	{
		const Sample s = sampleField(worldX, worldZ);
		if (!s.valid)
			return 15.0f;
		return temperatureFrom(worldX, worldZ, s, detail(worldX, worldZ, s.slope));
	}

	float TerrainGenV3::sampleHumidity(double worldX, double worldZ) const
	{
		const Sample s = sampleField(worldX, worldZ);
		if (!s.valid)
			return 0.5f;
		return humidityOf(s);
	}

	float TerrainGenV3::sampleFogThickness(double worldX, double worldZ) const
	{
		const Sample s = sampleField(worldX, worldZ);
		if (!s.valid)
			return 0.0f;
		return fogThicknessOf(s);
	}

	float TerrainGenV3::sampleFogHeightFalloff(double worldX, double worldZ) const
	{
		const Sample s = sampleField(worldX, worldZ);
		if (!s.valid)
			return 1.0f;
		// Uses the SHAPED temperature so the fog agrees with the biome splatting rather than the raw model.
		return fogFalloffOf(temperatureFrom(worldX, worldZ, s, detail(worldX, worldZ, s.slope)));
	}

	float TerrainGenV3::sampleFlowAngle01(double, double) const
	{
		// No rivers: the model emits elevation and climate, not a flow network, so the only water is the sea
		// and it takes its direction from the global wind. Matches what fill writes.
		return -1.0f;
	}

	void TerrainGenV3::samplePoint(double worldX, double worldZ, TerrainPoint& out, ESampleDetail detail) const
	{
		const bool coarse = (detail == ESampleDetail::Coarse);
		const Sample s = sampleField(worldX, worldZ, coarse);
		fill(worldX, worldZ, s, /*withDetail*/ !coarse, out);
	}

	void TerrainGenV3::sampleGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
	                              oc::span<TerrainPoint> out, ESampleDetail detail) const
	{
		assert(out.size() >= (size_t)resX * resZ);
		if (resX == 0 || resZ == 0)
			return;

		// THE reason this override exists: resolve every tile the grid touches ONCE (one shared-cache lock
		// each), then fill without taking a lock at all. The base class's per-point loop meant a fetch —
		// mutex + LRU touch — for every one of the ~262k texels in a cascade, all of it contending with the
		// mesh worker for a grid that covers only a couple of dozen tiles.
		const bool coarse = (detail == ESampleDetail::Coarse);
		const double x1 = originX + step * (double)(resX - 1);
		const double z1 = originZ + step * (double)(resZ - 1);

		TileBlock block;
		if (!resolveBlock(originX, originZ, x1, z1, coarse, block))
		{
			// Models not up yet: a flat sea-level world. Callers gate geometry on isReady(), so this only
			// shows up in bakes that raced the load.
			for (uint32 j = 0; j < resZ; j++)
				for (uint32 i = 0; i < resX; i++)
					fill(0.0, 0.0, Sample{}, false, out[(size_t)j * resX + i]);
			return;
		}

		for (uint32 j = 0; j < resZ; j++)
		{
			const double wz = originZ + step * (double)j;
			for (uint32 i = 0; i < resX; i++)
			{
				const double wx = originX + step * (double)i;
				const Sample s = sampleFromBlock(block, wx, wz);
				fill(wx, wz, s, /*withDetail*/ !coarse, out[(size_t)j * resX + i]);
			}
		}
	}
}
