export module Procedural:Diffusion.InfiniteTensor;

import Core;
import :Diffusion.Tensor;

// The lazy infinite-tensor graph (port of the Java mod's infinitetensor/InfiniteTensor + MemoryTileStore,
// itself a port of the Python _apply_f_range). This is the machinery that makes the diffusion world
// unbounded and randomly accessible: a tensor is defined by a tile function over an infinite window grid,
// evaluated on demand and memoised.
//
// THE ONE IDEA TO INTERNALISE: overlapping windows are SUMMED, and nothing here knows about blending.
// The blend is a convention the pipeline layers on top — every stage emits C+1 channels (the C data
// channels PRE-MULTIPLIED by a tent weight window, plus the raw weight as the last channel). Summing then
// yields (sum of w*v, sum of w) per pixel, and every consumer divides to recover the weighted average.
// Hence the `w > 1e-6f` guards all over WorldPipeline.
export namespace Procedural::Diffusion
{
	// Window index key. ndim is 3 throughout this pipeline (channel, row, col); the 4th slot keeps the key
	// trivially copyable and hashable for any plausible extension. The reference uses List<Integer>, which
	// boxes and allocates on every single lookup — this is the same thing without the garbage.
	struct WindowKey
	{
		int32 v[4] = { 0, 0, 0, 0 };

		bool operator==(const WindowKey& o) const
		{
			return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2] && v[3] == o.v[3];
		}
	};

	struct WindowKeyHash
	{
		size_t operator()(const WindowKey& k) const
		{
			uint64 h = 1469598103934665603ull;
			for (int32 i = 0; i < 4; i++)
			{
				h ^= (uint64)(uint32)k.v[i];
				h *= 1099511628211ull;
			}
			return (size_t)h;
		}
	};

	class MemoryTileStore;

	// windowIndex -> the tile. args[depIdx] is this window's slice of dependency depIdx.
	using TensorFn = oc::function<FloatTensor(oc::span<const int32> windowIndex, oc::span<const FloatTensor> args)>;
	// Batched variant. NOTE args is DEP-MAJOR: args[depIdx][batchIdx], matching the reference.
	using BatchTensorFn = oc::function<oc::vector<FloatTensor>(
		oc::span<const WindowKey> windowIndices, oc::span<const oc::vector<FloatTensor>> args)>;

	class InfiniteTensor
	{
	public:
		// The one entry point. Returns a tensor of shape (end - start), materialising whatever tiles are
		// needed (recursively, through this tensor's dependencies) and summing every window that overlaps.
		// Coordinates are in THIS tensor's own pixel space and may be negative.
		FloatTensor getSlice(oc::span<const int32> start, oc::span<const int32> end);

	private:
		friend class MemoryTileStore;
		InfiniteTensor() = default;

		// Materialise every window of every given pixel range, depth-first through dependencies.
		// Deps are handed the exact per-window range list, NOT a bounding-box union: a union would compute
		// windows that intersect nothing. Parity with Python's _apply_f_range.
		void ensureComputedRanges(const oc::vector<oc::vector<Range>>& pixelRanges);
		void computeSingle(const WindowKey& windowIndex);
		void computeBatched(const oc::vector<WindowKey>& pending);
		void validateOutputShape(const FloatTensor& t) const;

		oc::string m_id;
		int32 m_ndim = 0;
		TensorFn m_fn;
		BatchTensorFn m_batchFn;
		int32 m_batchSize = 0;
		TensorWindow m_outputWindow;
		oc::vector<InfiniteTensor*> m_deps;      // non-owning; the store owns every tensor
		oc::vector<TensorWindow> m_depWindows;
		size_t m_cacheLimitBytes = (size_t)-1;
		MemoryTileStore* m_store = nullptr;
	};

	// Owns every InfiniteTensor and a per-tensor access-ordered LRU of computed windows.
	//
	// NOT thread-safe, exactly like the reference — callers must serialise. TerrainGenV3 does this with a
	// single inference thread, which is required rather than merely convenient (sampleHeight is called from
	// the terrain streamer worker, the scatter worker and the height-map baker's std::async).
	class MemoryTileStore
	{
	public:
		MemoryTileStore() = default;
		MemoryTileStore(const MemoryTileStore&) = delete;
		MemoryTileStore& operator=(const MemoryTileStore&) = delete;

		InfiniteTensor* getOrCreate(oc::string_view id, int32 ndim, TensorFn fn, const TensorWindow& outputWindow,
		                            oc::span<InfiniteTensor* const> deps, oc::span<const TensorWindow> depWindows,
		                            size_t cacheLimitBytes);
		InfiniteTensor* getOrCreateBatched(oc::string_view id, int32 ndim, BatchTensorFn fn,
		                                   const TensorWindow& outputWindow,
		                                   oc::span<InfiniteTensor* const> deps, oc::span<const TensorWindow> depWindows,
		                                   size_t cacheLimitBytes, int32 batchSize);

		// Monotonic lifetime stat: counts cache INSERTIONS, not tile-function invocations. Survives clearAllCaches.
		uint64 getTotalComputedWindowCount() const { return m_totalComputedWindowCount; }
		void clearAllCaches();
		size_t getResidentBytes() const;

	private:
		friend class InfiniteTensor;

		struct Entry
		{
			WindowKey key;
			FloatTensor tensor;
		};
		struct Cache
		{
			// front = least recently used, back = most recent. Splicing on access never invalidates the
			// iterators held in `map`, which is what makes this safe.
			oc::list<Entry> lru;
			oc::unordered_map<WindowKey, oc::list<Entry>::iterator, WindowKeyHash> map;
			size_t bytes = 0;
		};

		// PROMOTES to most-recently-used (the reference's LinkedHashMap.get in access-order mode).
		const FloatTensor* getCachedWindow(const oc::string& id, const WindowKey& key);
		// Does NOT promote (the reference's containsKey). The asymmetry is deliberate: phase 1 probes with
		// this, phase 2 reads with getCachedWindow, so a window is promoted exactly once per slice.
		bool isWindowCached(const oc::string& id, const WindowKey& key) const;
		void cacheWindow(const oc::string& id, const WindowKey& key, FloatTensor&& t);
		void evictIfNeeded(const oc::string& id, size_t limitBytes);

		oc::unordered_map<oc::string, Cache> m_caches;
		oc::vector<oc::unique_ptr<InfiniteTensor>> m_tensors;
		oc::unordered_map<oc::string, InfiniteTensor*> m_byId;
		uint64 m_totalComputedWindowCount = 0;
	};
}
