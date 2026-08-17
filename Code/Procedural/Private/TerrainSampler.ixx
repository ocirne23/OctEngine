export module Procedural:TerrainSampler;

import Core;

export namespace Procedural
{
	// Encoding range of the baked 8-bit temperature channel: TEMPERATURE_MIN_C maps to 0, MAX_C to 255
	// (~0.29 C steps). Shared by the baker and the terrain shaders (terrain_height.inc.glsl mirrors it).
	inline constexpr float TEMPERATURE_MIN_C = -25.0f;
	inline constexpr float TEMPERATURE_MAX_C = 50.0f;

	// Humidity encoding: sampleHumidity is [0,1], where 1 means HUMIDITY_FULL_PRECIP_MM of annual
	// precipitation or more. The generator models real precipitation (its diffusion model emits mm/yr)
	// and maps through this.
	inline constexpr float HUMIDITY_FULL_PRECIP_MM = 2200.0f;

	// Physical climate -> the normalized (t01, h01) space that the scatter rules' climate attractors and
	// the terrain shader's texture splatting BOTH share. The shader mirrors temperatureTo01 as
	// clamp((temp + 25) / 75) — keep them in step.
	constexpr float temperatureTo01(float celsius)
	{
		constexpr float invRange = 1.0f / (TEMPERATURE_MAX_C - TEMPERATURE_MIN_C);
		const float t = (celsius - TEMPERATURE_MIN_C) * invRange;
		return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	}
	constexpr float precipTo01(float mmPerYear)
	{
		constexpr float invFull = 1.0f / HUMIDITY_FULL_PRECIP_MM;
		const float h = mmPerYear * invFull;
		return h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
	}
	// Encoding range of the baked 8-bit fog height-falloff MULTIPLIER: 0..FOG_FALLOFF_MUL_MAX maps to
	// 0..255 (1.0 = the global Fog/Height Falloff unchanged).
	// NOT baked any more — it is a pure function of temperature, so the shaders recompute it (see
	// fogFalloffFromTemperature) and its 8 bits in the packed channel now carry the LAPSE RATE, which
	// nothing else can reconstruct. Kept because both sides still need the range to agree.
	inline constexpr float FOG_FALLOFF_MUL_MAX = 4.0f;

	// Fog height-falloff from temperature: cold air hugs the ground, warm air lets fog tower. Mirrored by
	// terrain_height.inc.glsl — keep them in step.
	constexpr float fogFalloffFromTemperature(float celsius)
	{
		const float t01 = (celsius + 10.0f) * (1.0f / 40.0f);
		const float c = t01 < 0.0f ? 0.0f : (t01 > 1.0f ? 1.0f : t01);
		const float f = 1.8f - 1.2f * c;
		return f < 0.0f ? 0.0f : (f > FOG_FALLOFF_MUL_MAX ? FOG_FALLOFF_MUL_MAX : f);
	}

	// Every field the terrain-data bake needs, from ONE evaluation. Sampling these one at a time is fine
	// for a noise field but not for a generator with a per-point cost (V3 does a tile lookup + bilinear per
	// call, so the six separate calls were six times the work).
	struct TerrainPoint
	{
		float height = 0.0f;         // surface Y (m)
		float waterLevel = 0.0f;     // water surface Y (m)
		float altitude = 0.0f;       // macro elevation above sea level (m)
		float temperature = 15.0f;   // degrees CELSIUS
		float humidity = 0.5f;       // [0,1]
		float fogThickness = 0.0f;   // [0,1]
		float fogFalloffMul = 1.0f;  // [0, FOG_FALLOFF_MUL_MAX], 1 = neutral. NOT baked (derived from
		                             // temperature); still reported for CPU consumers.
		// The SEA-LEVEL baseline `temperature` is derived from: temperature = temperatureSeaLevel +
		// lapse * max(0, height above sea level), for a lapse rate the GENERATOR holds fixed and publishes
		// once (ITerrainSampler::lapseRatePerMetre) rather than varying per point.
		//
		// This — not `temperature` — is what the terrain-data map bakes, and the reason is the map's two
		// cascades: they bake DIFFERENT heights for the same spot (the near one the full-detail surface, the
		// far one its 7.68 km average, which cannot know a peak exists). A baked temperature is only valid at
		// the height it was baked from, so the far one reported the temperature of the plateau a peak stands
		// on — 10.6 C too warm, wider than a climate box, and its texture flipped at the crossfade. A
		// baseline plus a shared constant has no such anchor: every consumer evaluates at the height it
		// shades, and the cascades agree by construction.
		//
		// A generator that models no lapse (lapseRatePerMetre() == 0) reports this equal to `temperature`:
		// evaluating at any height then returns it unchanged, so the two agree by construction.
		float temperatureSeaLevel = 15.0f;
		float flowAngle01 = -1.0f;   // angle/2pi in [0,1), or < 0 for no direction
	};

	// How much fidelity a query needs.
	//   Full   — the real field, whatever it costs. Geometry and near-field bakes.
	//   Coarse — a cheap, low-resolution approximation, for bakes that span tens of km at low texel
	//            density. A generator whose field is built hierarchically may answer these from a much
	//            cheaper level; one that is a plain function of (x, z) just ignores the hint.
	// This exists because V3 (diffusion) generates 7.68 km TILES at real cost: a far cascade covering the
	// whole view distance would otherwise force thousands of full-detail tiles — for terrain largely beyond
	// the mesh ring — and evict the ones the geometry needs. V3 answers Coarse from its coarse stage alone,
	// where one tile covers several hundred km. The shader crossfades near->far, so the drop in fidelity
	// blends in rather than seaming.
	enum class ESampleDetail : uint8
	{
		Full,
		Coarse
	};

	// The point-evaluable terrain field the generator (TerrainGenV3) implements.
	// All methods are pure functions of world position — seeded, thread-safe, world-continuous — which is
	// what keeps chunks, LODs, bakes (shore/fog maps) and buoyancy consistent with each other regardless
	// of which generator produced them. Consumers hold shared_ptr<const ITerrainSampler>.
	//
	// A PURE interface: nothing here has a default. The defaults it used to carry composed the single-field
	// samplers point by point, which is only free if the field is a cheap function of (x, z) — true of the
	// noise generator they were written for, false of a tile-based one, which would silently inherit a
	// cache lock per texel of every bake. An implementer answers each question the way that is actually
	// cheap for it, or says so explicitly (a constant, or a value it does not model).
	class ITerrainSampler
	{
	public:
		virtual ~ITerrainSampler() = default;

		// Everything at once, from one evaluation.
		virtual void samplePoint(double worldX, double worldZ, TerrainPoint& out,
		                         ESampleDetail = ESampleDetail::Full) const = 0;

		// A regular XZ grid: point (i, j) sits at (originX + i*step, originZ + j*step) and is written to
		// out[j*resX + i]. out must hold resX*resZ entries.
		//
		// This is where an implementer hoists its PER-POINT overhead out of the loop, and wide-area consumers
		// should prefer it over samplePoint. For V3 it is the difference between a shared-cache lock per point
		// and one per tile: a terrain-data cascade is 512x512 texels resolving to a couple of dozen tiles, so
		// a point-at-a-time loop means a quarter-million lock/unlock pairs contending with the mesh worker.
		virtual void sampleGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
		                        oc::span<TerrainPoint> out, ESampleDetail detail = ESampleDetail::Full) const = 0;

		// Terrain surface height in world meters (Y).
		virtual float sampleHeight(double worldX, double worldZ) const = 0;
		// Water surface height (world Y): the sea, plus whatever elevated water the generator models.
		// Water is present wherever this exceeds sampleHeight.
		virtual float sampleWaterHeight(double worldX, double worldZ) const = 0;
		// Both fields from one evaluation (bakes sampling both per texel use this).
		virtual float sampleHeightAndWater(double worldX, double worldZ, float& outWaterLevel) const = 0;
		virtual float sampleTemperature(double worldX, double worldZ) const = 0; // degrees CELSIUS (baked over TEMPERATURE_MIN_C..MAX_C)
		virtual float sampleHumidity(double worldX, double worldZ) const = 0;   // normalized [0,1] (baked 1:1 — 0 -> 0.0, 255 -> 1.0)
		virtual float sampleFogThickness(double worldX, double worldZ) const = 0; // [0,1] regional fog multiplier
		// Regional multiplier on the global Fog/Height Falloff, [0, FOG_FALLOFF_MUL_MAX] (1 = neutral):
		// lets a generator make fog hug the ground in one region and tower in another. 1 = no variation.
		virtual float sampleFogHeightFalloff(double worldX, double worldZ) const = 0;
		// Directed water-flow angle at (x, z), as angle / 2pi in [0,1) (0 = +X, counter-clockwise), or
		// < 0 where the water has no direction (open ocean/lakes -> the global wind drives the waves).
		// Baked into the ocean shore map so the water shader orients its wave field along rivers.
		// A generator that models no rivers returns < 0 everywhere.
		virtual float sampleFlowAngle01(double worldX, double worldZ) const = 0;
		// The MACRO elevation (m above sea level) before fine detail — what separates "a mountain" (height
		// far above the local altitude) from "high-altitude flatland" (height ~ altitude). Fed per-vertex
		// to the terrain shader for coloring. A generator with no macro/detail split returns the full
		// height above sea level.
		virtual float sampleAltitude(double worldX, double worldZ) const = 0;
		virtual float seaLevel() const = 0;

		// Temperature change per metre of elevation above sea level, in this generator's VERTICAL FRAME
		// (model metres for V3 — divide by vertScale for world metres). ONE value for the whole world, not a
		// field: `sampleTemperature` and `TerrainPoint::temperatureSeaLevel` are consistent with it by
		// construction, so a consumer can re-derive the temperature at any height from the baseline alone.
		//
		// It was per-point once, regressed by the diffusion model per region and baked into the terrain-data
		// map. Measured, that bought nothing: the two cascades regress the same data through the same
		// windows, so their rates AGREE — near-vs-far disagreement was 0.15 C max either way, and all of it
		// came from the baselines. The per-region rate only bought fidelity to the model's own temperature,
		// which has no consumer (the field is an artistic input, not a measurement), and cost 8 bits of the
		// packed climate channel. What it did buy was regional variety in the lapse; that is now a tweak.
		//
		// 0 = temperature does not vary with height — correct for a generator that folds its own lapse into
		// sampleTemperature and exposes no way to move it: there the baseline IS the temperature, and a
		// consumer lapsing it again would double-count.
		virtual float lapseRatePerMetre() const = 0;
	};
}
