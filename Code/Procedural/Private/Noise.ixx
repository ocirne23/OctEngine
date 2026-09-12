export module Procedural:Noise;

import Core;
import Core.glm;

// Deterministic, seed-driven coherent noise sampled in continuous world space. All methods are pure and
// thread-safe (state is just the seed), so multiple worker threads can share one NoiseField. Coordinates
// are in the caller's own units - multiply world meters by a frequency before sampling to set feature size.
export namespace Procedural
{
	class NoiseField
	{
	public:
		NoiseField() = default;
		explicit NoiseField(uint32 seed) : m_seed(seed) {}
		void setSeed(uint32 seed) { m_seed = seed; }
		uint32 getSeed() const { return m_seed; }

		// Single-octave gradient (Perlin-style) noise, output ~[-1, 1], continuous with continuous gradient.
		float noise2D(float x, float y) const;

		// Same noise with its analytic derivative: (value, d/dx, d/dy). Basis of fbmEroded.
		glm::vec3 noise2Dd(float x, float y) const;

		// Slope-damped fBm (Musgrave 1989 "The Synthesis and Rendering of Eroded Fractal Terrains";
		// Quilez's derivative formulation, iquilezles.org/articles/morenoise): each octave is divided by
		// the accumulated gradient magnitude, so flats collect detail (smooth valley floors) while steep
		// slopes damp it (eroded hillsides) - the classic cheap erosion look. Output ~[-1, 1].
		float fbmEroded(float x, float y, uint32 octaves, float lacunarity = 2.0f, float persistence = 0.5f) const;

		// Fractal Brownian motion: sum of octaves, normalized to ~[-1, 1].
		float fbm(float x, float y, uint32 octaves, float lacunarity = 2.0f, float persistence = 0.5f) const;

		// Ridged multifractal (sharp mountain ridges), output ~[0, 1].
		float ridged(float x, float y, uint32 octaves, float lacunarity = 2.0f, float persistence = 0.5f) const;

		// Billow noise (rounded, cloud/hill-like), output ~[0, 1].
		float billow(float x, float y, uint32 octaves, float lacunarity = 2.0f, float persistence = 0.5f) const;

	private:
		glm::vec2 gradient(int32 ix, int32 iy) const;

		uint32 m_seed = 0;
	};
}
