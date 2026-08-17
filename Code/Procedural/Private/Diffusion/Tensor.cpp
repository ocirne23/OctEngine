module Procedural;

import Core;
import :Diffusion.Rng;
import :Diffusion.Tensor;

namespace Procedural::Diffusion
{
	namespace
	{
		oc::vector<int32> computeStrides(const oc::vector<int32>& shape)
		{
			oc::vector<int32> s(shape.size());
			int32 stride = 1;
			for (int32 i = (int32)shape.size() - 1; i >= 0; i--)
			{
				s[i] = stride;
				stride *= shape[i];
			}
			return s;
		}
	}

	FloatTensor::FloatTensor(oc::span<const int32> shape)
		: m_shape(shape.begin(), shape.end())
	{
		m_strides = computeStrides(m_shape);
		size_t total = 1;
		for (int32 d : m_shape)
		{
			assert(d >= 0);
			total *= (size_t)d;
		}
		data.assign(total, 0.0f); // zero-init is load-bearing: addFrom accumulates into it
	}

	void FloatTensor::addFrom(const FloatTensor& src, oc::span<const Range> dstRegion, oc::span<const Range> srcRegion)
	{
		const int32 n = ndim();
		assert((int32)dstRegion.size() == n && (int32)srcRegion.size() == n);
		assert(src.ndim() == n);

		for (int32 d = 0; d < n; d++)
		{
			// The reference never checks this and silently reads the wrong memory when it disagrees.
			assert(dstRegion[d].count() == srcRegion[d].count());
			if (dstRegion[d].count() <= 0)
				return;
		}

		// Base offsets at the region origin.
		size_t dstBase0 = 0, srcBase0 = 0;
		for (int32 d = 0; d < n; d++)
		{
			dstBase0 += (size_t)dstRegion[d].start * (size_t)m_strides[d];
			srcBase0 += (size_t)srcRegion[d].start * (size_t)src.strides()[d];
		}

		// Walk the outer dims with an odometer and run the (contiguous, stride-1) innermost dim as a
		// tight loop. Elementwise += cannot be reassociated, so this stays exact.
		const int32 last = n - 1;
		const int32 innerCount = dstRegion[last].count();
		const int32 dstInnerStride = m_strides[last];
		const int32 srcInnerStride = src.strides()[last];

		oc::vector<int32> idx((size_t)last, 0);
		for (;;)
		{
			size_t dstOff = dstBase0, srcOff = srcBase0;
			for (int32 d = 0; d < last; d++)
			{
				dstOff += (size_t)idx[d] * (size_t)m_strides[d];
				srcOff += (size_t)idx[d] * (size_t)src.strides()[d];
			}

			float* pDst = data.data() + dstOff;
			const float* pSrc = src.data.data() + srcOff;
			if (dstInnerStride == 1 && srcInnerStride == 1)
			{
				for (int32 k = 0; k < innerCount; k++)
					pDst[k] += pSrc[k];
			}
			else
			{
				for (int32 k = 0; k < innerCount; k++)
					pDst[(size_t)k * dstInnerStride] += pSrc[(size_t)k * srcInnerStride];
			}

			if (last == 0)
				break;
			int32 d = last - 1;
			for (;; d--)
			{
				if (++idx[d] < dstRegion[d].count())
					break;
				idx[d] = 0;
				if (d == 0)
					return;
			}
		}
	}

	TensorWindow::TensorWindow(oc::span<const int32> size)
		: m_size(size.begin(), size.end()), m_stride(size.begin(), size.end()), m_offset(size.size(), 0)
	{
	}

	TensorWindow::TensorWindow(oc::span<const int32> size, oc::span<const int32> stride)
		: m_size(size.begin(), size.end()), m_stride(stride.begin(), stride.end()), m_offset(size.size(), 0)
	{
		assert(m_size.size() == m_stride.size());
	}

	TensorWindow::TensorWindow(oc::span<const int32> size, oc::span<const int32> stride, oc::span<const int32> offset)
		: m_size(size.begin(), size.end()), m_stride(stride.begin(), stride.end()), m_offset(offset.begin(), offset.end())
	{
		assert(m_size.size() == m_stride.size() && m_size.size() == m_offset.size());
		for (int32 s : m_stride)
			assert(s > 0); // the intersection math assumes it; the reference never checks
	}

	void TensorWindow::getBounds(oc::span<const int32> windowIndex, oc::span<Range> out) const
	{
		const int32 n = ndim();
		assert((int32)windowIndex.size() == n && (int32)out.size() == n);
		for (int32 d = 0; d < n; d++)
		{
			const int32 start = windowIndex[d] * m_stride[d] + m_offset[d];
			out[d] = { start, start + m_size[d] };
		}
	}

	void TensorWindow::getHighestIntersection(oc::span<const Range> pixelRange, oc::span<int32> out) const
	{
		const int32 n = ndim();
		for (int32 d = 0; d < n; d++)
		{
			// Highest w with w*stride + offset <= stop-1. floorDiv, NOT '/': these coordinates go negative.
			const int32 p = pixelRange[d].stop - 1;
			out[d] = floorDiv(p - m_offset[d], m_stride[d]);
		}
	}

	void TensorWindow::getLowestIntersection(oc::span<const Range> pixelRange, oc::span<int32> out) const
	{
		const int32 n = ndim();
		for (int32 d = 0; d < n; d++)
		{
			// Lowest w with start < w*stride + offset + size, i.e. w >= ceil((start - offset - size + 1)/stride).
			// Kept as the reference's explicit sign branch: both arms divide non-negative operands, where
			// Java's and C++'s '/' agree. Do not "simplify" — the signs are the whole point.
			const int32 numerator = pixelRange[d].start - m_offset[d] - m_size[d] + 1;
			if (numerator >= 0)
				out[d] = (numerator + m_stride[d] - 1) / m_stride[d];
			else
				out[d] = -((-numerator) / m_stride[d]);
		}
	}

	void iterateWindows(oc::span<const int32> lo, oc::span<const int32> hi,
	                    const oc::function<void(oc::span<const int32>)>& fn)
	{
		const int32 n = (int32)lo.size();
		assert((int32)hi.size() == n);
		for (int32 d = 0; d < n; d++)
			if (lo[d] > hi[d])
				return;

		oc::vector<int32> cur(lo.begin(), lo.end());
		for (;;)
		{
			fn(cur);
			int32 d = n - 1;
			for (;; d--)
			{
				if (++cur[d] <= hi[d])
					break;
				cur[d] = lo[d];
				if (d == 0)
					return;
			}
		}
	}
}
