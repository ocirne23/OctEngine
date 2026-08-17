module Procedural;

import Core;
import :Diffusion.Laplacian;

// Precision: the double/float mixing below mirrors the reference exactly. Java evaluates `-0.5 * x * x /
// (sigma*sigma)` in double (the literal promotes) but computes `sigma*sigma` in float first; the windowed
// regression accumulates FLOAT products into DOUBLE sums. Both are reproduced literally. This TU is
// /fp:precise so none of it gets reassociated.
namespace Procedural::Diffusion
{
	namespace
	{
		int32 clampi(int32 v, int32 lo, int32 hi)
		{
			return v < lo ? lo : (v > hi ? hi : v);
		}

		// Java's Math.round(double) is floor(x + 0.5) — NOT std::lround, which rounds half away from zero
		// and so disagrees for negatives. Everything here is positive, but be explicit rather than lucky.
		int32 javaRound(double x)
		{
			return (int32)std::floor(x + 0.5);
		}
	}

	Grid bilinearResize(const Grid& src, int32 dstH, int32 dstW)
	{
		assert(src.h > 0 && src.w > 0 && dstH > 0 && dstW > 0);
		Grid dst(dstH, dstW);
		for (int32 r = 0; r < dstH; r++)
		{
			const float srcR = (((float)r + 0.5f) * (float)src.h / (float)dstH) - 0.5f;
			const int32 r0raw = (int32)std::floor(srcR);
			// The weight comes from the UNCLAMPED coordinate, then the indices clamp. At a border the two
			// taps coincide and (1-w)*v + w*v == v, so it degenerates to replicate padding regardless of the
			// odd-looking weight. Do not reorder.
			const float wr = srcR - (float)r0raw;
			const int32 r0 = clampi(r0raw, 0, src.h - 1);
			const int32 r1 = clampi(r0raw + 1, 0, src.h - 1);

			for (int32 c = 0; c < dstW; c++)
			{
				const float srcC = (((float)c + 0.5f) * (float)src.w / (float)dstW) - 0.5f;
				const int32 c0raw = (int32)std::floor(srcC);
				const float wc = srcC - (float)c0raw;
				const int32 c0 = clampi(c0raw, 0, src.w - 1);
				const int32 c1 = clampi(c0raw + 1, 0, src.w - 1);

				dst.at(r, c) = (1 - wr) * (1 - wc) * src.at(r0, c0)
				             + (1 - wr) * wc * src.at(r0, c1)
				             + wr * (1 - wc) * src.at(r1, c0)
				             + wr * wc * src.at(r1, c1);
			}
		}
		return dst;
	}

	Grid bilinearResizeExtrapolated(const Grid& src, int32 dstH, int32 dstW)
	{
		const int32 sH = src.h, sW = src.w;
		// The reference silently reads out of bounds when the ratio isn't integral (newH - 2*padH < dstH).
		// It's latent there only because lc=8 divides everything in the shipped config. Fail loudly instead.
		assert(dstH % sH == 0 && dstW % sW == 0);

		Grid padded(sH + 2, sW + 2);
		for (int32 r = 0; r < sH; r++)
			for (int32 c = 0; c < sW; c++)
				padded.at(r + 1, c + 1) = src.at(r, c);

		// Rows first from src, then columns from the already-row-padded array — so the corners end up
		// doubly extrapolated. That's what the reference does.
		for (int32 c = 1; c <= sW; c++)
		{
			padded.at(0, c) = (sH > 1) ? 2 * src.at(0, c - 1) - src.at(1, c - 1) : src.at(0, c - 1);
			padded.at(sH + 1, c) = (sH > 1) ? 2 * src.at(sH - 1, c - 1) - src.at(sH - 2, c - 1)
			                                : src.at(sH - 1, c - 1);
		}
		for (int32 r = 0; r < sH + 2; r++)
		{
			padded.at(r, 0) = (sW > 1) ? 2 * padded.at(r, 1) - padded.at(r, 2) : padded.at(r, 1);
			padded.at(r, sW + 1) = (sW > 1) ? 2 * padded.at(r, sW) - padded.at(r, sW - 1) : padded.at(r, sW);
		}

		const int32 newH = javaRound((double)dstH + 2.0 * (double)dstH / (double)sH);
		const int32 newW = javaRound((double)dstW + 2.0 * (double)dstW / (double)sW);
		const Grid resized = bilinearResize(padded, newH, newW);

		const int32 padH = javaRound((double)dstH / (double)sH);
		const int32 padW = javaRound((double)dstW / (double)sW);
		assert(newH - 2 * padH >= dstH && newW - 2 * padW >= dstW);

		Grid cropped(dstH, dstW);
		for (int32 r = 0; r < dstH; r++)
			for (int32 c = 0; c < dstW; c++)
				cropped.at(r, c) = resized.at(r + padH, c + padW);
		return cropped;
	}

	oc::vector<float> gaussianKernel1D(float sigma)
	{
		const int32 ks = ((int32)(sigma * 2.0f) / 2) * 2 + 1; // matches PyTorch gaussian_blur
		assert(ks > 0);
		oc::vector<float> k((size_t)ks);
		const int32 half = ks / 2;
		float sum = 0.0f;
		for (int32 i = 0; i < ks; i++)
		{
			const float x = (float)(i - half);
			// Java: -0.5 (a double literal) promotes the product to double, but sigma*sigma is evaluated in
			// float first and only then widened.
			k[(size_t)i] = (float)std::exp(-0.5 * (double)x * (double)x / (double)(sigma * sigma));
			sum += k[(size_t)i];
		}
		for (int32 i = 0; i < ks; i++)
			k[(size_t)i] /= sum;
		return k;
	}

	Grid separableGaussianBlur(const Grid& src, oc::span<const float> kernel1D)
	{
		const int32 ks = (int32)kernel1D.size();
		const int32 pad = ks / 2;
		const int32 H = src.h, W = src.w;

		Grid tmp(H, W);
		for (int32 r = 0; r < H; r++)
			for (int32 c = 0; c < W; c++)
			{
				float sum = 0.0f;
				for (int32 ki = 0; ki < ks; ki++)
					sum += src.at(r, clampi(c + ki - pad, 0, W - 1)) * kernel1D[(size_t)ki]; // replicate, not reflect
				tmp.at(r, c) = sum;
			}

		Grid result(H, W);
		for (int32 r = 0; r < H; r++)
			for (int32 c = 0; c < W; c++)
			{
				float sum = 0.0f;
				for (int32 ki = 0; ki < ks; ki++)
					sum += tmp.at(clampi(r + ki - pad, 0, H - 1), c) * kernel1D[(size_t)ki];
				result.at(r, c) = sum;
			}
		return result;
	}

	Grid laplacianDecode(const Grid& residual, const Grid& lowres)
	{
		const Grid up = bilinearResize(lowres, residual.h, residual.w);
		Grid result(residual.h, residual.w);
		for (size_t i = 0; i < result.data.size(); i++)
			result.data[i] = residual.data[i] + up.data[i];
		return result;
	}

	Grid laplacianDenoise(const Grid& residual, const Grid& lowres, float sigma)
	{
		const Grid upEx = bilinearResizeExtrapolated(lowres, residual.h, residual.w);
		Grid decoded(residual.h, residual.w);
		for (size_t i = 0; i < decoded.data.size(); i++)
			decoded.data[i] = residual.data[i] + upEx.data[i];

		const Grid downsampled = bilinearResize(decoded, lowres.h, lowres.w);
		return separableGaussianBlur(downsampled, gaussianKernel1D(sigma));
	}

	void localBaselineTemperature(const Grid& T, const Grid& e, int32 win, float fallbackThreshold,
	                              Grid& outTSea, Grid& outBeta)
	{
		const int32 H = T.h, W = T.w;
		const int32 outH = H - win + 1, outW = W - win + 1;
		assert(outH > 0 && outW > 0);
		assert(e.h == H && e.w == W);

		constexpr float fallbackBeta = -0.0065f; // standard atmospheric lapse rate, C/m
		constexpr float betaMin = -0.012f, betaMax = 0.0f;
		constexpr float eps = 1e-6f;
		const int32 n = win * win;
		const int32 pad = (win - 1) / 2;

		outTSea = Grid(outH, outW);
		outBeta = Grid(outH, outW);

		// O(outH * outW * win^2) brute force, as in the reference. It stays cheap because this runs on the
		// COARSE grid (outH/outW are single digits), not at native resolution — don't be tempted by an
		// integral-image rewrite, which would change the summation order for no real gain.
		for (int32 r = 0; r < outH; r++)
		{
			for (int32 c = 0; c < outW; c++)
			{
				// Double accumulators are load-bearing: muE2 - muE*muE catastrophically cancels in float
				// once elevations reach the thousands of metres.
				double muT = 0, muE = 0, muE2 = 0, muET = 0, sumW = 0;
				for (int32 dr = 0; dr < win; dr++)
				{
					for (int32 dc = 0; dc < win; dc++)
					{
						const float ev = e.at(r + dr, c + dc);
						const float tv = T.at(r + dr, c + dc);
						const float land = (ev > 0.0f) ? 1.0f : 0.0f;
						muT += tv * land;
						muE += ev * land;
						muE2 += ev * ev * land;
						muET += ev * tv * land;
						sumW += land;
					}
				}
				const double den = sumW + eps;
				muT /= den;
				muE /= den;
				muE2 /= den;
				muET /= den;

				const double varE = muE2 - muE * muE;
				const double covET = muET - muE * muT;
				// varE < 1.0 m^2 means there's no elevation signal to regress against.
				double beta = (varE < 1.0 || sumW < (double)(fallbackThreshold * (float)n))
					? (double)fallbackBeta
					: (covET / (varE + eps));
				beta = oc::max((double)betaMin, oc::min((double)betaMax, beta));

				const float Tc = T.at(r + pad, c + pad);
				const float ec = e.at(r + pad, c + pad);
				outTSea.at(r, c) = (float)((double)Tc - beta * (double)ec);
				outBeta.at(r, c) = (float)beta;
			}
		}
	}
}
