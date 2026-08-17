export module Procedural:GeneratorV3;

import Core;
import :Noise;
import :TerrainSampler;

// The Terrain Diffusion generator (Private/Diffusion), exposed through the ITerrainSampler the rest of the
// terrain stack consumes.
//
// WHY THE SHAPE OF THIS CLASS IS WHAT IT IS: an ITerrainSampler looks like a pure function of (x, z), but
// this one is a neural diffusion pipeline that generates 256x256-pixel TILES (7.68 km at the default
// 30 m/px) through three ONNX models. So:
//   * Sampling a point means bilinearly reading a resident tile. A miss BLOCKS the caller while the tile is
//     generated (~1.5 s cold; ~0 once resident, and one tile covers ~225 chunks at chunkSize 512).
//   * All inference is serialised onto one thread. That is required, not a convenience: the tile store is
//     not thread-safe, and sampleHeight is called from the terrain streamer worker, the scatter worker AND
//     the height-map baker's std::async.
//   * The models (2.28 GB) load once per process and are RESEEDED, never reloaded. Constructing a
//     TerrainGenV3 is therefore cheap; a tweak change that rebuilds the sampler costs nothing unless the
//     seed actually moved.
//   * The model resolves 30 m/px. Sub-pixel relief is procedural: a slope-masked noise layer rides on top,
//     which is what `sampleAltitude` (macro) vs `sampleHeight` (macro + detail) already models.
//
// NOT READY YET is a real state: until the models are downloaded and loaded, isReady() is false and the
// generator must not be used to build geometry (it would bake a flat world into the chunk cache).
// TerrainStreamer polls isReady() and rebuilds once it flips.
export namespace Procedural
{
	struct TerrainConfigV3
	{
		uint32 seed = 1337;
		float seaLevel = 0.0f;

		// --- World scale. The model natively resolves 30 m per pixel; that is what it was trained for and
		// what makes its continents continent-sized and its peaks ~10 km.
		// metersPerPixel is a UNIFORM scale: it shrinks elevation and detail by the same factor it shrinks
		// the horizontal, so a compressed world keeps the model's proportions — same slopes, same shapes,
		// same climate, just smaller and quicker to fly across. Lowering it is quadratically more expensive
		// (the same view distance then spans more model pixels, so more tiles must be generated).
		// heightScale is then a pure vertical exaggeration ON TOP: 1 = the model's real proportions.
		float metersPerPixel = 3.0f;
		float heightScale = 1.0f;

		// --- Sub-pixel detail. The diffusion field is smooth below 30 m, so without this a close-up
		// hillside is bilinear mush. Masked by SLOPE, so plains and seabed stay smooth and only mountain
		// faces get crags. Wavelengths are in metres and scale with metersPerPixel.
		float detailSlopeGain = 0.75f;   // sf = min(1, slope * gain); slope is m/m
		float detailWavelengthA = 220.0f; // coarse crags
		float detailAmplitudeA = 38.0f;
		uint32 detailOctavesA = 4;
		float detailWavelengthB = 45.0f;  // fine break-up
		float detailAmplitudeB = 11.0f;
		uint32 detailOctavesB = 3;

		// --- Climate mapping. The model emits precipitation in mm/year; ITerrainSampler wants humidity in
		// [0,1], so this is the precipitation that reads as fully humid.
		float precipForFullHumidity = 2200.0f;
		// Shifts the whole planet wetter/drier after that mapping, the humidity counterpart of
		// temperatureOffset. Biomes are picked by (temperature, humidity) proximity, so this slides the
		// world along the arid <-> lush axis: +0.2 turns steppe into forest, -0.2 turns it into desert.
		float humidityOffset = 0.0f; // added to the [0,1] humidity, then re-clamped

		// --- Temperature shaping. The model's temperatures are REAL (WorldClim-calibrated) and the climate
		// boxes are in real units to match, so temperatureOffset defaults to 0: out of the box you get the
		// model's own climate, shifted only if you want a colder/warmer planet.
		float temperatureOffset = 0.0f;  // degrees C

		// THE lapse rate: how fast it cools with altitude, C per MODEL metre (it rides metersPerPixel like
		// every other vertical quantity here). This is the snow line dial — it multiplies altitude, so it
		// costs lowlands and ocean nothing.
		//
		// Fixed, not per-region. The model regresses its own rate per coarse pixel and that used to be baked
		// into the terrain-data map, 8 bits of it. It bought nothing measurable: both cascades regress the
		// same data through the same windows, so their rates agree, and near-vs-far disagreement was 0.15 C
		// max with or without it — all of it from the baselines. It only bought fidelity to the model's
		// absolute temperature, which nothing consumes. Using one number instead frees those bits, makes the
		// snow line a dial rather than a model output, and keeps every consumer able to re-derive the
		// temperature at any height from the baseline alone.
		// The regressed rate is still read internally — at tile build — to recover that baseline from the
		// model's temperature; the tiles then carry only the baseline (FieldTile::tempSea).
		// Default -0.008 = the measured mean of what the model regresses (its own values run -0.012..-0.004
		// above 1000 m, so this sits mid-range; Earth's environmental lapse is ~-0.0065).
		float lapseRate = -0.008f;

		// --- Microclimate. The model's temperature is essentially a FUNCTION OF ELEVATION (it regresses a
		// lapse rate against the coarse map), so every climate boundary it produces is an isotherm — and an
		// isotherm follows a contour line. Left alone, grass gives way to rock at the same height right
		// across a mountain range, like a tide mark, and the snow line is a perfect ring.
		// This is a slow wander added to the temperature to break that: real treelines and snow lines move
		// hundreds of metres up and down a range with soil, drainage, wind scour and shelter. Adding it in
		// DEGREES lets it act on the same axis the lapse rate does, so one field breaks up the ground
		// transitions AND the snow line together, and it lands in the climate the scatter system and fog
		// read too — so trees keep marking the treeline the textures draw.
		// The wavelength is in model metres (it rides metersPerPixel like the detail layer). Keep it LONG:
		// it is baked into the terrain-data map, whose far cascade is ~16 m per texel, and a wavelength near
		// that aliases into near/far disagreement. The default is ~30x the far texel at mpp=3.
		float climateNoiseAmplitudeC = 1.5f;   // 0 = off (the model's own banded climate)
		float climateNoiseWavelength = 2000.0f; // model metres
		uint32 climateNoiseOctaves = 3;


		// --- Regional fog character, derived from the model's real climate.
		float humidFogAmount = 0.6f;  // broad fog in wet regions
		float valleyFogAmount = 0.8f; // fog pooling below ridgelines

		// --- Residency. One tile is 256x256 native px + halo, ~800 KB across elevation/temperature/precip.
		int32 maxResidentTiles = 256;

		// --- Inference precision. Requires the optional fp16 models produced by
		// Tools/convert_models_fp16.py; without them this silently stays fp32 (see ModelAssets::modelPath).
		// Halves model VRAM and load time but does NOT speed generation up (this pipeline is dispatch-bound;
		// measurements in TerrainStreamer's tweak registration). Changing it RELOADS the models (precision
		// lives in the file) and changes the terrain for a given seed, so it belongs here in the world
		// config rather than being a pure perf knob.
		bool useFp16 = false;
	};

	class TerrainGenV3 final : public ITerrainSampler
	{
	public:
		explicit TerrainGenV3(const TerrainConfigV3& cfg);
		~TerrainGenV3() override = default;

		// Starts loading the models (which ship in Assets/TerrainDiffusion) onto the GPU on a background
		// thread. Cheap and idempotent; the TerrainGenV3 constructor calls it for you.
		static void beginLoad();
		// Applies the inference precision process-wide (see TerrainConfigV3::useFp16). Cheap to call on
		// every rebuild — only a CHANGE does anything — but a change RELOADS the models, so isReady() must
		// be re-read after this, never cached across it.
		static void setPrecision(bool useFp16);
		// False while the models are loading, and permanently if that failed. Sampling before this is true
		// yields a flat sea-level world, so callers must gate geometry generation on it.
		static bool isReady();
		static bool hasFailed();
		// metersPerPixel / the model's native resolution: the uniform factor every world-space length the
		// generator produces is scaled by (elevation, crag relief, detail). Anything OUTSIDE the generator
		// that compares against those lengths in metres — the terrain shader's crag thresholds — has to
		// scale by this too, or it is tuned for exactly one metersPerPixel. Valid before the models load
		// (the native resolution has a default), so callers need not gate on isReady().
		static float worldScale(float metersPerPixel);
		// Short human-readable status, or the reason it failed.
		static oc::string statusText();

		float sampleHeight(double worldX, double worldZ) const override;
		float sampleWaterHeight(double worldX, double worldZ) const override;
		float sampleHeightAndWater(double worldX, double worldZ, float& outWaterLevel) const override;
		float sampleTemperature(double worldX, double worldZ) const override;
		float sampleHumidity(double worldX, double worldZ) const override;
		float sampleFogThickness(double worldX, double worldZ) const override;
		float sampleFogHeightFalloff(double worldX, double worldZ) const override;
		// Always < 0: the diffusion model emits no river network, so water here is only the sea and has no
		// direction of its own. samplePoint/fill report the same.
		float sampleFlowAngle01(double worldX, double worldZ) const override;
		// One evaluation for every field, and the only way to ask for the cheap coarse level (ESampleDetail).
		void samplePoint(double worldX, double worldZ, TerrainPoint& out,
		                 ESampleDetail detail = ESampleDetail::Full) const override;
		// Resolves the grid's whole tile set up front, then fills it without touching the shared cache.
		// Wide-area consumers should use this rather than looping samplePoint — on V3 the difference is one
		// lock per tile versus one per point.
		void sampleGrid(double originX, double originZ, double step, uint32 resX, uint32 resZ,
		                oc::span<TerrainPoint> out, ESampleDetail detail = ESampleDetail::Full) const override;
		// The smooth 30 m/px diffusion surface WITHOUT the procedural detail layer — the terrain shader uses
		// this to tell "a mountain" from "high flatland".
		float sampleAltitude(double worldX, double worldZ) const override;
		float seaLevel() const override;
		float lapseRatePerMetre() const override;

		const TerrainConfigV3& config() const;

	private:
		// Defined in the .cpp. They are the tile-lattice mechanics — vectors of cached tiles, lattice
		// offsets — which no consumer can name and none needs to see; declaring them here only lets the
		// helpers below traffic in them.
		struct Sample;    // one evaluation of the diffusion field at a world position
		struct TileBlock; // the tiles covering a query region, resolved up front

		// The uniform world scale (the static worldScale(float) applied to this config's metersPerPixel),
		// and it times heightScale: model metres -> world metres.
		float worldScale() const;
		float vertScale() const;

		bool resolveBlock(double x0, double z0, double x1, double z1, bool coarse, TileBlock& out) const;
		Sample sampleFromBlock(const TileBlock& b, double worldX, double worldZ) const;
		Sample sampleField(double worldX, double worldZ, bool coarse = false) const;

		// The shaping layer: the model's raw field -> what the terrain stack actually consumes.
		float slopeMask(float slope) const;
		float detail(double worldX, double worldZ, float slope) const;
		float climateWander(double worldX, double worldZ) const;
		float lapseOf() const;
		float temperatureSeaLevelFrom(double worldX, double worldZ, const Sample& s) const;
		float temperatureFrom(double worldX, double worldZ, const Sample& s, float detailWorldM) const;
		float humidityOf(const Sample& s) const;
		float fogThicknessOf(const Sample& s) const;
		float fogFalloffOf(float temperature) const;
		void fill(double worldX, double worldZ, const Sample& s, bool withDetail, TerrainPoint& out) const;

		TerrainConfigV3 m_cfg;
		NoiseField m_detailA;
		NoiseField m_detailB;
		NoiseField m_climateNoise; // the microclimate wander; see temperatureFrom
		float m_invMpp = 1.0f / 30.0f;
	};
}
