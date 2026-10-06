export module Settings.Terrain;

import Core;
import Core.glm;

// "Terrain*" tweaks: Procedural's TerrainStreamer ("Terrain", "Terrain/Water", "Terrain/Textures", "Terrain/Tessellation",
// "Terrain/V3") and TerrainCollider ("Terrain/Collision"). The two bake rules below stay in namespace Procedural: the
// streamer bakes them and hands them out (activeWaterReach / activeFlowField), HeightMapBaker applies them.
export namespace Procedural
{
	// Sea level everywhere is a lie the ocean believes: a generator that models no lakes (V3) reports the ocean's level
	// at every point, so every scrap of terrain within a metre of it reads as shoreline. The bake drops the water level
	// far below any ground the ocean cannot reach (a chamfer distance to real ocean); the swash's landlocked-water gate
	// and the beach overlay do the rest. Lakes (a level off sea level) are never touched. See applyWaterReach.
	struct WaterReach
	{
		float radius = 4.0f;   // world metres of ocean proximity that still counts as shore
		float feather = 70.0f;  // world metres over which the drop eases in (no hard line on a beach)
		float drop = 1.0f;      // how far below sea level to sink unreachable water. Must clear the swash
		                        // gate's 1 m fade with room to spare; also pushes the beach overlay off.
		// How deep water must be to count as OCEAN and seed reach for the ground around it. Without it any
		// texel a hair under sea level qualifies, so one shallow inland dip vouches for every hollow within
		// the radius of it. 0 = any water below sea level.
		float swashDepth = 2.0f;

		// Compared to decide whether a baked map went stale (HeightMapBaker::update). Defaulted so a field added
		// above is covered without anyone remembering to extend it.
		bool operator==(const WaterReach&) const = default;
	};

	// Direction assignment for the baked 8-bit flow channel - where the local water MOVES. Ocean texels within
	// `oceanRange` of land point AT that land (waves travel inland at the coast); everything else points downhill.
	// See applyFlowField.
	struct FlowField
	{
		float oceanRange = 250.0f;   // world m: how far offshore the toward-land direction still applies
		// World m at the END of that range over which the direction eases back to the WIND heading, so the
		// encoded field meets the unencoded (= wind-driven) open ocean without a visible turn.
		float oceanFade = 120.0f;
		// World m of box averaging over the shore directions (the raw nearest-land field is Voronoi
		// piecewise-constant). Averaged as VECTORS, so opposing shores cancel to "none".
		float smoothRadius = 40.0f;
		float minSlope = 0.02f;      // land: slopes flatter than this (m per m) carry no direction
		// NOT a tweak: the swell heading the fade band returns to (the ocean's), radians in XZ. The app pushes it
		// every frame through TerrainStreamer::setFlowWindAngle.
		float windAngle = 0.0f;

		// Staleness comparison (HeightMapBaker::update), same reasoning as WaterReach's.
		bool operator==(const FlowField&) const = default;
	};
}

export struct TerrainSettings
{
	// --- "Terrain": the generator config (a change marks the streamer config-dirty) and the streaming ring ---
	bool  enabled = false;
	bool  v3LoadModels = false; // off = the terrain runs on the existing .tile cache only (no weights loaded)
	int   seed = 516121;
	// Where in the seed's world the engine origin sits (world metres; TerrainConfigV3::originX/Z).
	float originX = 0.0f;
	float originZ = 0.0f;
	int   chunkSize = 256;
	int   lod0Res = 128;
	// IN CHUNKS (256 m since 2026-10-03). The ring is 24 km, not the old 32: 128 chunks would be ~51 k chunk meshes
	// against the 16-bit mesh index (Renderer addMeshInfos).
	int   ringRadius = 96;     // max generation range from the camera chunk, in chunks
	float lodStep = 1.2f;      // LOD0 band width in chunks (fractional ok); each next LOD band is twice as wide (geometric)
	float fullResDist = 1.2f;  // chunks whose nearest edge is within this many chunks are always LOD0; bands start beyond it
	int   maxLod = 4;
	int   maxUploadsPerFrame = 16;
	// Byte cap on one frame's upload batch, below the 100 MB staging ring. The first chunk always goes.
	float maxUploadMBPerFrame = 48.0f;
	int   maxGenJobs = 12;     // concurrent chunk generations
	float seaLevel = 0.0f;     // THE sea level datum for the world (the ocean floats on it)
	float skirtDepth = 5.0f;

	// --- Shared terrain-data map (fog terrain-following + regional thickness, ocean shore fallback, terrain coloring) ---
	bool  terrainMapEnabled = true;
	bool  terrainMapDebugLog = false;     // log the decoded climate range of every baked cascade
	bool  waterReachEnabled = true;
	Procedural::WaterReach waterReach;
	bool  flowFieldEnabled = true;
	Procedural::FlowField flowField;
	float terrainMapRange = 4096.0f;      // near cascade world size (m), centered on the camera
	float terrainMapFarRange = 8192.0f;   // far cascade world size (m; same texel count, coarser texels)

	// --- "Terrain/Water": the surface water, read by the renderer (TerrainResources' wetness tick, the UBO) ---
	// ONE wetness field feeds ONE water surface: the FIELD is a TERRAIN_WET_RES^2 toroidal clipmap around the scene
	// focus, integrated on a FIXED TICK (wetUpdateRate: a per-frame change at high fps is below the R16F image's step
	// and rounds away), rain everywhere, full wetness under the ocean, drying back with time. The SURFACE fills the
	// splat relief to a LEVEL (rain puddles in the crevices); where the live ocean stands higher the film follows it
	// across the waterline. The GROUND under it darkens and glosses with the same wetness, drying in islands.
	bool  wetEnabled = true;
	float wetTexelSize = 2.0f;        // m per texel (1024 texels = 2048 m around the scene focus)
	float wetUpdateRate = 20.0f;      // Hz: the pass's fixed tick
	float wetDiffusionRate = 5.0f;    // 1/s sideways spread (framerate independent)
	float wetRain = 0.0f;             // wetness per second added everywhere
	float wetDryTime = 10.0f;         // s to decay to 1/e on cool ground
	float wetDryTempSens = 0.04f;     // extra decay rate per C above 15 C
	float wetInTime = 5.0f;           // s for ground under water to reach full wetness
	float wetSlopeDrain = 20.0f;      // steep ground dries faster / soaks slower; 0 = off
	float wetFillStart = 0.5f;        // wetness at which water begins to stand in the relief's low points
	float wetFillFull = 1.0f;         // wetness that submerges the relief (level 1)
	float wetFillCurve = 1.0f;        // exponent between them: 1 = linear, > 1 = fills late, < 1 = early
	float wetEdgeFade = 0.5f;         // m of water depth the film fades out over at the terrain intersection
	float wetOceanBlend = 5.0f;       // m of live ocean water over the ground the film fades into the ocean over
	float wetOceanEdgeFade = 0.5f;    // m of column the ocean blends out over at its edge (0 = hard edge)
	float wetFilmMaxSlope = 25.0f;    // degrees: no standing water on steeper ground (90 = off)
	float wetFilmSlopeFade = 5.0f;    // degrees below the max over which the pool level sinks to nothing
	float wetFilmFlowSpeed = 1.0f;    // m/s the film's ripples run downhill at 45 degrees (x sqrt(tan slope))
	float wetFilmFlowMinSlope = 8.0f; // degrees: flow full from here, fading in from half of it
	float wetFilmFlowCycle = 1.0f;    // s: the flow map's phase cycle
	float wetWaviness = 1.0f;         // film normal: 0 = level plane, 1 = the live FFT wave normal
	float wetNormalScale = 2.0f;      // film wave normal strength, x the ocean's "Normal strength"
	float wetRippleStrength = 0.1f;   // inland wind ripples (0 = off)
	float wetRoughness = 0.08f;       // the water film's perceptual roughness
	float wetGroundRoughness = 0.3f;  // ground roughness at full wetness
	float wetDryingPattern = 1.0f;    // 0..1: uniform drying (0) to dry islands (1)
	float wetDarkeningEdge = 0.5f;    // soft band around the darkening's drying level: wide = smoother fade
	float wetRoughnessEdge = 0.5f;    // soft band around the gloss's drying level: small = crisp islands
	float wetDryingPatternSize = 0.4f;     // m: size of the drying blotches
	float wetDryingPatternRelief = 0.6f;   // 0..1: share of the splat relief in the pattern
	float wetDryingPatternContrast = 2.5f; // stretch of the noise toward fully dry / wet: higher = stronger islands
	float wetUnderwaterRoughness = 0.9f;   // ground roughness under the live ocean and the film
	float wetDarkening = 0.55f;            // ground albedo multiplier at full wetness
	float wetDarkeningThreshold = 0.3f;    // wetness at which the ground's darkening is full
	float wetRoughnessThreshold = 1.0f;    // wetness at which the ground's wet gloss is full
	float wetGroundNormalScale = 1.75f;    // normal map tilt at full gloss: 1 = unchanged, < 1 flatter, > 1 stronger
	float wetGlintSize = 0.04f;            // m: glint patch cell size
	float wetGlintCoverage = 0.15f;        // ~ share of the wet ground that glints (0 = off)
	float wetGlintRoughness = 0.15f;       // GGX alpha inside a glint patch

	// --- "Terrain/Textures" + "Terrain/Tessellation": splat shaping, read by the renderer (the UBO) ---
	// The crag thresholds and the crag wander are metres at the MODEL's true scale: the renderer scales them by V3's
	// world scale (Renderer::setTerrainCragScale - the loaded model's native resolution, not a tweak). Snow is a layer
	// on top of ground and rock that sheds EARLIER than rock appears (a steepening slope loses its snow first). Parallax
	// (TERRAIN_POM) and tessellation are BAKED switches: flipping one reloads the terrain pipelines (Renderer listener).
	// The tessellation displaces around the mesh (height 0.5 = the mesh), so the TLAS, the collider and the shadow map
	// still see the relief's mean surface.
	float texUvScaleGround = 0.15f;  // 1/m: ~7 m texture repeat on flat ground
	float texUvScaleRock = 0.05f;    // 1/m: rock features read larger on cliffs
	float texUvScaleSnow = 0.10f;    // 1/m
	float texClimateBlend = 0.02f;   // Gaussian sigma OUTSIDE a climate box, in (t01, h01) units
	// Slope is 1 - N.y: 0.30 = 45 deg (about where soil stops holding), 0.55 = 63 deg.
	float texSlopeRockStart = 0.25f;
	float texSlopeRockFull = 0.60f;
	// Crag wander: breaks the rock boundary off the elevation contour it otherwise traces. In metres at the model's
	// true scale, like the crag thresholds it modulates.
	float texCragWanderAmp = 66.0f;
	float texCragWanderWavelength = 400.0f;
	float texCragStart = 34.0f;      // crag relief start (m above macro altitude)
	float texCragFull = 400.0f;
	float texBeachBand = 2.5f;       // beach band height (m above water level)
	// Snow COVER. Below freezing is NOT permanent snow (Siberia averages -10 C and is forest), so these sit well below 0.
	float texSnowTempFull = -7.0f;   // C at/below which cover is complete
	float texSnowTempNone = -1.0f;   // C at/above which there is none
	float texSnowSlopeStart = 0.26f;
	float texSnowSlopeFull = 0.60f;
	float texSnowAridity = 0.10f;    // humidity at/below which cold ground stays bare (polar desert)
	// Relief from the splat HEIGHT maps (terrain_splat.inc.glsl): ONE parallax march in world space over the height-blended
	// composite of the visible layers, near the camera only; the height blend shows a layer where it stands HIGHER.
	bool  texParallaxEnabled = false;
	float texParallaxDepthGround = 0.12f; // m
	float texParallaxDepthRock = 0.35f;   // m
	float texParallaxFadeStart = 15.0f;   // m from the camera
	float texParallaxFadeEnd = 30.0f;     // m
	int   texParallaxSteps = 24;
	float texParallaxShadow = 1.0f;       // relief self-shadow strength (0 = off)
	float texHeightBlendContrast = 3.0f;  // 0 = linear layer blend
	bool  texTessEnabled = true;
	int   texTessMaxFactor = 8;
	float texTessTargetPx = 7.0f;
	float texTessFadeStart = 15.0f;       // m
	float texTessFadeEnd = 75.0f;         // m
	float texTessFalloffExponent = 0.5f;  // tess factor: 1 - t^p across the fade band
	float texTessHeightFalloffExponent = 2.0f; // displacement height: 1 - t^p across the fade band
	float texTessFreezeDistance = 15.0f;  // m: closer in, the factor and the height mip hold
	float texTessDepthGround = 0.6f;      // m
	float texTessDepthRock = 0.7f;        // m

	// --- "Terrain/V3": the Terrain Diffusion generator (every change marks the streamer config-dirty) ---
	float v3MetersPerPixel = 2.0f;   // 30 = the model's true training scale; lower compresses the world
	float v3HeightScale = 1.0f;
	float v3TemperatureOffset = 0.0f; // C, shifts the whole planet
	float v3LapseRate = -0.008f;      // C per MODEL metre: THE snow-line dial
	// Microclimate wander (C): breaks climate boundaries off the contour lines the lapse rate pins them to.
	float v3ClimateNoiseC = 5.0f;
	float v3ClimateNoiseWavelength = 2000.0f; // model metres
	int   v3ClimateNoiseOctaves = 3;
	float v3DetailSlopeGain = 0.75f;  // slope -> detail mask (the model only resolves 30 m/px)
	// Wavelengths/amplitudes are in MODEL metres and ride metersPerPixel, so the OCTAVE counts set how far below the
	// model's 30 m/px the terrain actually has anything in it (see the registration for the arithmetic).
	float v3DetailWavelengthA = 220.0f;
	float v3DetailAmplitudeA = 38.0f;
	int   v3DetailOctavesA = 4;
	float v3DetailWavelengthB = 45.0f;
	float v3DetailAmplitudeB = 11.0f;
	int   v3DetailOctavesB = 3;
	float v3PrecipFullHumidity = 2200.0f; // mm/yr that reads as humidity 1.0
	float v3HumidityOffset = 0.0f;        // slides the planet along arid <-> lush
	float v3HumidFog = 0.25f;
	float v3ValleyFog = 0.25f;
	int   v3MaxTiles = 256;               // resident tile budget (~800 KB each)
	// Half-precision inference: buys VRAM and load time, NOT generation speed. Reloads the models and changes the
	// terrain for a given seed.
	bool  v3Fp16 = false;
};

// "Terrain/Collision": the focus-centered ring of static collider tiles (Procedural TerrainCollider).
export struct TerrainColliderSettings
{
	bool  enabled = true;
	float radius = 96.0f;   // world m around the focus that carries colliders
	float tileSize = 32.0f; // world m per collider tile
	float spacing = 1.0f;   // m between height samples (render LOD0 is chunkSize/lod0Res = 1 m)
	float friction = 0.8f;
};

// The wetness clipmap's texel (m): the wetness tick's window lattice and u_terrainWater_texelSize must agree.
export inline float terrainWetTexelSize(const TerrainSettings& s) { return glm::max(s.wetTexelSize, 0.05f); }

export namespace Settings
{
	void registerTerrain(TerrainSettings& s);
	void registerTerrainCollider(TerrainColliderSettings& s);
}
