module Procedural;

import Core;
import :Diffusion.Tensor;
import :Diffusion.InfiniteTensor;

namespace Procedural::Diffusion
{
	namespace
	{
		WindowKey toKey(oc::span<const int32> windowIndex)
		{
			assert(windowIndex.size() <= 4);
			WindowKey k;
			for (size_t i = 0; i < windowIndex.size(); i++)
				k.v[i] = windowIndex[i];
			return k;
		}
	}

	// =====================================================================================================
	// InfiniteTensor
	// =====================================================================================================

	void InfiniteTensor::validateOutputShape(const FloatTensor& t) const
	{
		const oc::vector<int32>& want = m_outputWindow.size();
		assert(t.ndim() == (int32)want.size());
		for (size_t d = 0; d < want.size(); d++)
			assert(t.shape()[d] == want[d]);
		(void)t;
	}

	void InfiniteTensor::ensureComputedRanges(const oc::vector<oc::vector<Range>>& pixelRanges)
	{
		// --- Phase 1: collect the windows we still need, deduped but INSERTION-ORDERED.
		// Order is not cosmetic: it decides how computeBatched groups windows into ONNX batches. A hash set
		// here would silently reshuffle every batch.
		oc::vector<WindowKey> pending;
		oc::unordered_set<WindowKey, WindowKeyHash> seen;
		oc::vector<int32> lo((size_t)m_ndim), hi((size_t)m_ndim);

		for (const oc::vector<Range>& range : pixelRanges)
		{
			m_outputWindow.getLowestIntersection(range, lo);
			m_outputWindow.getHighestIntersection(range, hi);
			iterateWindows(lo, hi, [&](oc::span<const int32> wi)
			{
				const WindowKey key = toKey(wi);
				if (m_store->isWindowCached(m_id, key)) // probe only: must not promote
					return;
				if (seen.insert(key).second)
					pending.push_back(key);
			});
		}
		if (pending.empty())
			return;

		// --- Phase 2: recurse into dependencies. Each dep is handed the exact per-window range list.
		oc::vector<Range> depBounds((size_t)m_ndim);
		for (size_t i = 0; i < m_deps.size(); i++)
		{
			oc::vector<oc::vector<Range>> depRanges;
			depRanges.reserve(pending.size());
			for (const WindowKey& wi : pending)
			{
				m_depWindows[i].getBounds(oc::span<const int32>(wi.v, (size_t)m_ndim), depBounds);
				depRanges.push_back(depBounds);
			}
			m_deps[i]->ensureComputedRanges(depRanges);
		}

		// --- Phase 3: evaluate. Every pending window of THIS stage runs before any caller descends to the
		// next stage. That stage-major order is what makes model offloading affordable — interleaving
		// stages per tile would rebuild an ONNX session per tile.
		if (m_batchSize > 0 && m_batchFn)
			computeBatched(pending);
		else
			for (const WindowKey& wi : pending)
				computeSingle(wi);
	}

	void InfiniteTensor::computeSingle(const WindowKey& windowIndex)
	{
		const oc::span<const int32> wi(windowIndex.v, (size_t)m_ndim);

		oc::vector<FloatTensor> args;
		args.reserve(m_deps.size());
		oc::vector<Range> b((size_t)m_ndim);
		oc::vector<int32> depStart((size_t)m_ndim), depEnd((size_t)m_ndim);
		for (size_t i = 0; i < m_deps.size(); i++)
		{
			m_depWindows[i].getBounds(wi, b);
			for (int32 d = 0; d < m_ndim; d++)
			{
				depStart[d] = b[d].start;
				depEnd[d] = b[d].stop;
			}
			args.push_back(m_deps[i]->getSlice(depStart, depEnd)); // hits cache: phase 2 materialised it
		}

		FloatTensor out = m_fn(wi, args);
		validateOutputShape(out);
		m_store->cacheWindow(m_id, windowIndex, oc::move(out));
	}

	void InfiniteTensor::computeBatched(const oc::vector<WindowKey>& pending)
	{
		oc::vector<Range> b((size_t)m_ndim);
		oc::vector<int32> depStart((size_t)m_ndim), depEnd((size_t)m_ndim);

		for (size_t from = 0; from < pending.size(); from += (size_t)m_batchSize)
		{
			const size_t to = oc::min(from + (size_t)m_batchSize, pending.size());
			const oc::span<const WindowKey> batch(pending.data() + from, to - from);

			// Dep-major: args[depIdx][batchIdx]. The final batch may be smaller than m_batchSize, which is
			// why the models need a dynamic batch dimension.
			oc::vector<oc::vector<FloatTensor>> args(m_deps.size());
			for (size_t i = 0; i < m_deps.size(); i++)
			{
				args[i].reserve(batch.size());
				for (const WindowKey& wi : batch)
				{
					m_depWindows[i].getBounds(oc::span<const int32>(wi.v, (size_t)m_ndim), b);
					for (int32 d = 0; d < m_ndim; d++)
					{
						depStart[d] = b[d].start;
						depEnd[d] = b[d].stop;
					}
					args[i].push_back(m_deps[i]->getSlice(depStart, depEnd));
				}
			}

			oc::vector<FloatTensor> outs = m_batchFn(batch, args);
			assert(outs.size() == batch.size());
			for (size_t k = 0; k < batch.size(); k++)
			{
				validateOutputShape(outs[k]);
				m_store->cacheWindow(m_id, batch[k], oc::move(outs[k]));
			}
		}
	}

	FloatTensor InfiniteTensor::getSlice(oc::span<const int32> start, oc::span<const int32> end)
	{
		assert((int32)start.size() == m_ndim && (int32)end.size() == m_ndim);

		oc::vector<Range> pixelRange((size_t)m_ndim);
		oc::vector<int32> outShape((size_t)m_ndim);
		for (int32 d = 0; d < m_ndim; d++)
		{
			pixelRange[d] = { start[d], end[d] };
			outShape[d] = end[d] - start[d];
			assert(outShape[d] >= 0);
		}

		ensureComputedRanges({ pixelRange });

		FloatTensor output(outShape);
		oc::vector<int32> lo((size_t)m_ndim), hi((size_t)m_ndim);
		m_outputWindow.getLowestIntersection(pixelRange, lo);
		m_outputWindow.getHighestIntersection(pixelRange, hi);

		oc::vector<Range> wBounds((size_t)m_ndim), srcRegion((size_t)m_ndim), dstRegion((size_t)m_ndim);
		iterateWindows(lo, hi, [&](oc::span<const int32> wi)
		{
			m_outputWindow.getBounds(wi, wBounds);

			for (int32 d = 0; d < m_ndim; d++)
			{
				const int32 s = oc::max(pixelRange[d].start, wBounds[d].start);
				const int32 e = oc::min(pixelRange[d].stop, wBounds[d].stop);
				if (s >= e)
					return; // this window doesn't actually touch the request
				srcRegion[d] = { s - wBounds[d].start, e - wBounds[d].start };
				dstRegion[d] = { s - pixelRange[d].start, e - pixelRange[d].start };
			}

			const FloatTensor* cached = m_store->getCachedWindow(m_id, toKey(wi)); // promotes
			// The reference silently contributes nothing here. That would be invisible: a missing window
			// drops out of BOTH the data and weight sums, and the /w normalisation hides it as an average
			// over the surviving tiles. Phase 1 guarantees it's present, so assert rather than paper over it.
			assert(cached != nullptr);
			output.addFrom(*cached, dstRegion, srcRegion); // ACCUMULATE: this is the blend
		});

		m_store->evictIfNeeded(m_id, m_cacheLimitBytes);
		return output;
	}

	// =====================================================================================================
	// MemoryTileStore
	// =====================================================================================================

	InfiniteTensor* MemoryTileStore::getOrCreate(oc::string_view id, int32 ndim, TensorFn fn,
	                                             const TensorWindow& outputWindow,
	                                             oc::span<InfiniteTensor* const> deps,
	                                             oc::span<const TensorWindow> depWindows,
	                                             size_t cacheLimitBytes)
	{
		auto it = m_byId.find(oc::string(id));
		if (it != m_byId.end())
			return it->second;

		// Same construction path as the batched form, then swap in the single-window function.
		InfiniteTensor* t = getOrCreateBatched(id, ndim, BatchTensorFn{}, outputWindow, deps, depWindows,
		                                       cacheLimitBytes, 0);
		t->m_fn = oc::move(fn);
		return t;
	}

	InfiniteTensor* MemoryTileStore::getOrCreateBatched(oc::string_view id, int32 ndim, BatchTensorFn fn,
	                                                    const TensorWindow& outputWindow,
	                                                    oc::span<InfiniteTensor* const> deps,
	                                                    oc::span<const TensorWindow> depWindows,
	                                                    size_t cacheLimitBytes, int32 batchSize)
	{
		const oc::string key(id);
		auto it = m_byId.find(key);
		if (it != m_byId.end())
			return it->second;

		assert(deps.size() == depWindows.size());
		assert(ndim > 0 && ndim <= 4);
		assert((int32)outputWindow.size().size() == ndim);

		m_tensors.push_back(oc::unique_ptr<InfiniteTensor>(new InfiniteTensor()));
		InfiniteTensor* t = m_tensors.back().get();
		t->m_id = key;
		t->m_ndim = ndim;
		t->m_batchFn = oc::move(fn);
		t->m_batchSize = batchSize;
		t->m_outputWindow = outputWindow;
		t->m_deps.assign(deps.begin(), deps.end());
		t->m_depWindows.assign(depWindows.begin(), depWindows.end());
		t->m_cacheLimitBytes = cacheLimitBytes;
		t->m_store = this;

		m_byId[key] = t;
		m_caches[key]; // default-construct the cache
		return t;
	}

	const FloatTensor* MemoryTileStore::getCachedWindow(const oc::string& id, const WindowKey& key)
	{
		auto ci = m_caches.find(id);
		if (ci == m_caches.end())
			return nullptr;
		Cache& c = ci->second;
		auto mi = c.map.find(key);
		if (mi == c.map.end())
			return nullptr;
		c.lru.splice(c.lru.end(), c.lru, mi->second); // promote to most-recently-used
		return &mi->second->tensor;
	}

	bool MemoryTileStore::isWindowCached(const oc::string& id, const WindowKey& key) const
	{
		auto ci = m_caches.find(id);
		if (ci == m_caches.end())
			return false;
		return ci->second.map.find(key) != ci->second.map.end(); // deliberately does not promote
	}

	void MemoryTileStore::cacheWindow(const oc::string& id, const WindowKey& key, FloatTensor&& t)
	{
		Cache& c = m_caches[id];
		auto mi = c.map.find(key);
		if (mi != c.map.end())
		{
			// Already present: keep the incumbent and promote it, discarding the fresh result (reference
			// behaviour). Unreachable in the current flow — phase 1 only ever enqueues uncached windows.
			c.lru.splice(c.lru.end(), c.lru, mi->second);
			return;
		}
		const size_t bytes = t.byteSize();
		c.lru.push_back(Entry{ key, oc::move(t) });
		c.map[key] = oc::prev(c.lru.end());
		c.bytes += bytes;
		++m_totalComputedWindowCount;
	}

	void MemoryTileStore::evictIfNeeded(const oc::string& id, size_t limitBytes)
	{
		if (limitBytes == (size_t)-1)
			return; // sentinel: unlimited
		auto ci = m_caches.find(id);
		if (ci == m_caches.end())
			return;
		Cache& c = ci->second;
		// Soft limit, checked only at the end of a getSlice: a single slice can overshoot arbitrarily
		// before anything is evicted. Peak is not the limit. Always keeps at least one entry.
		while (c.bytes > limitBytes && c.lru.size() > 1)
		{
			Entry& e = c.lru.front();
			c.bytes -= e.tensor.byteSize();
			c.map.erase(e.key);
			c.lru.pop_front();
		}
	}

	void MemoryTileStore::clearAllCaches()
	{
		for (auto& [id, c] : m_caches)
		{
			c.lru.clear();
			c.map.clear();
			c.bytes = 0;
		}
		// m_totalComputedWindowCount is a lifetime stat and deliberately survives.
	}

	size_t MemoryTileStore::getResidentBytes() const
	{
		size_t total = 0;
		for (const auto& [id, c] : m_caches)
			total += c.bytes;
		return total;
	}
}
