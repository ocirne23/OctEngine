export module Procedural:Diffusion.Tensor;

import Core;

// The two value types the infinite-tensor machinery is built from (port of the Java mod's
// infinitetensor/FloatTensor + TensorWindow). Both are plain data: no laziness, no caching.
export namespace Procedural::Diffusion
{
	// Half-open pixel interval [start, stop). Pixel ranges are half-open everywhere in this pipeline;
	// WINDOW INDEX ranges are inclusive on both ends. Keeping those straight matters.
	struct Range
	{
		int32 start = 0;
		int32 stop = 0;

		int32 count() const { return stop - start; }
	};

	// Row-major (C-order) N-dim float buffer. In this pipeline ndim is always 3: (channel, row, col).
	// data is public and written in place by the stage tile functions, matching the reference.
	class FloatTensor
	{
	public:
		FloatTensor() = default;
		// Zero-initialised — the accumulate semantics of addFrom depend on it.
		explicit FloatTensor(oc::span<const int32> shape);

		int32 ndim() const { return (int32)m_shape.size(); }
		size_t byteSize() const { return data.size() * sizeof(float); }
		const oc::vector<int32>& shape() const { return m_shape; }
		const oc::vector<int32>& strides() const { return m_strides; }

		oc::vector<float> data;

		// data[dstRegion] += src.data[srcRegion], elementwise. ACCUMULATE, not assign: this is the sole
		// reason overlapping windows blend (see InfiniteTensor). dstRegion drives the extents; srcRegion
		// must have identical counts (asserted).
		void addFrom(const FloatTensor& src, oc::span<const Range> dstRegion, oc::span<const Range> srcRegion);

	private:
		oc::vector<int32> m_shape;
		oc::vector<int32> m_strides;
	};

	// The affine map from a window index to the pixel bounds it covers:
	//     bounds[d] = [i[d]*stride[d] + offset[d], i[d]*stride[d] + offset[d] + size[d])
	// Windows OVERLAP when stride < size, which is the whole point — the overlap is what gets blended.
	class TensorWindow
	{
	public:
		TensorWindow() = default;
		// stride defaults to size (non-overlapping), offset to 0.
		explicit TensorWindow(oc::span<const int32> size);
		TensorWindow(oc::span<const int32> size, oc::span<const int32> stride);
		TensorWindow(oc::span<const int32> size, oc::span<const int32> stride, oc::span<const int32> offset);

		int32 ndim() const { return (int32)m_size.size(); }
		const oc::vector<int32>& size() const { return m_size; }

		// Pixel bounds covered by a window index. out must have ndim entries.
		void getBounds(oc::span<const int32> windowIndex, oc::span<Range> out) const;

		// The inclusive window-index range whose windows intersect pixelRange. Note these are INCLUSIVE
		// on both ends, unlike the half-open pixel ranges they are derived from.
		void getLowestIntersection(oc::span<const Range> pixelRange, oc::span<int32> out) const;
		void getHighestIntersection(oc::span<const Range> pixelRange, oc::span<int32> out) const;

	private:
		oc::vector<int32> m_size;
		oc::vector<int32> m_stride;
		oc::vector<int32> m_offset;
	};

	// Visit every window index in the INCLUSIVE box [lo, hi], last dim varying fastest. Returns without
	// calling fn if the box is empty in any dimension.
	void iterateWindows(oc::span<const int32> lo, oc::span<const int32> hi,
	                    const oc::function<void(oc::span<const int32>)>& fn);
}
