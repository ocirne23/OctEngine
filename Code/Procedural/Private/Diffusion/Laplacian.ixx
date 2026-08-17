export module Procedural:Diffusion.Laplacian;

import Core;

// Laplacian pyramid + climate-regression helpers (port of the reference's LaplacianUtils, itself a port of
// terrain_diffusion/data/laplacian_encoder.py + postprocessing.local_baseline_temperature_torch).
//
// The elevation the pipeline emits is a Laplacian pair: a low-frequency band (from the latent stage) plus a
// high-frequency residual (from the decoder stage). These functions recombine them.
export namespace Procedural::Diffusion
{
	// Flat row-major 2D float grid. The reference uses jagged float[][]; this is the same data without the
	// per-row indirection.
	struct Grid
	{
		oc::vector<float> data;
		int32 h = 0;
		int32 w = 0;

		Grid() = default;
		Grid(int32 height, int32 width) : data((size_t)height * width, 0.0f), h(height), w(width) {}

		float& at(int32 r, int32 c) { return data[(size_t)r * w + c]; }
		float at(int32 r, int32 c) const { return data[(size_t)r * w + c]; }
		bool empty() const { return data.empty(); }
	};

	// PyTorch bilinear with align_corners=False.
	Grid bilinearResize(const Grid& src, int32 dstH, int32 dstW);

	// As above but linearly extrapolates a 1-pixel border first, so upsampling doesn't flatten against the
	// edges. Requires dstH % src.h == 0 and dstW % src.w == 0 (asserted): with a non-integral ratio the
	// reference's crop can run past the end of the resized array.
	Grid bilinearResizeExtrapolated(const Grid& src, int32 dstH, int32 dstW);

	// ks = 2*floor(floor(2*sigma)/2) + 1, normalised to sum 1. For the pipeline's sigma=5: ks=11.
	oc::vector<float> gaussianKernel1D(float sigma);

	// Separable blur with REPLICATE (clamp) padding. The reference's doc comment claims reflect padding; the
	// code clamps. The code is what matters — do not "fix" this to match the comment.
	Grid separableGaussianBlur(const Grid& src, oc::span<const float> kernel1D);

	// residual + bilinearUpsample(lowres) at the residual's resolution.
	Grid laplacianDecode(const Grid& residual, const Grid& lowres);

	// Re-derives the low band: decode with EXTRAPOLATED upsampling, resample back down with plain resize,
	// then blur. The asymmetry (extrapolated up, plain down) is deliberate in the reference.
	// Returns only the new lowres, despite the reference's doc claiming otherwise.
	Grid laplacianDenoise(const Grid& residual, const Grid& lowres, float sigma);

	// Windowed weighted linear regression of temperature on elevation, weighted by a land mask (e > 0).
	// Yields the sea-level baseline temperature and the lapse rate beta, so temperature can be re-derived at
	// native resolution from native elevation. Outputs are (H-win+1) x (W-win+1).
	// beta is clamped to [-0.012, 0] (never warmer with altitude) and falls back to the standard -0.0065 C/m
	// where there is too little elevation variance or too little land to regress on.
	void localBaselineTemperature(const Grid& temperature, const Grid& elevation, int32 win,
	                              float fallbackThreshold, Grid& outTSea, Grid& outBeta);
}
