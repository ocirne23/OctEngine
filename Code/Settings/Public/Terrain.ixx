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
	float originX = -21064.0f;
	float originZ = 63673.0f;
	int   chunkSize = 256;
	int   lod0Res = 128;
	int   ringRadius = 128;    // max generation range from the camera chunk, in chunks
	float lodStep = 1.1f;      // LOD0 band width in chunks (fractional ok); each next LOD band is twice as wide (geometric)
	float fullResDist = 0.7f;  // chunks whose nearest edge is within this many chunks are always LOD0; bands start beyond it
	int   maxLod = 6;
	int   maxUploadsPerFrame = 16;
	// Byte cap on one frame's upload batch, below the 100 MB staging ring. The first chunk always goes.
	float maxUploadMBPerFrame = 48.0f;
	int   maxGenJobs = 12;     // concurrent chunk generations
	float seaLevel = 0.0f;     // THE sea level datum for the world (the ocean floats on it)
	bool  edgeStitch = true;   // the terrain VS snaps chunk edges onto the coarser neighbour (no skirts); off = debug, shows the cracks

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
	float wetGroundRoughness = 0.48f;  // ground roughness at full wetness
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
	// Macro variation: the splat textures repeat every few metres, and at a distance the eye finds that grid in their
	// low-frequency content. A world noise at two unrelated scales varies the brightness, a warm/cool hue and the
	// roughness of the ground, beach and rock layers (not the snow).
	float texMacroStrength = 0.18f;  // albedo brightness +- (0 = off)
	float texMacroHue = 0.06f;       // warm/cool shift +-
	float texMacroRoughness = 0.15f; // roughness +-
	float texMacroSize = 40.0f;      // m: the noise's largest feature (the second tap's: 3.71x)
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
	float v3MetersPerPixel = 5.0f;   // 30 = the model's true training scale; lower compresses the world
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

	// --- "Terrain/Rivers": the drainage network (Docs/RiverPlan.md). MODEL-frame units: model metres, real m3/s.
	bool  riverEnabled = true;       // the terrain sampler carves the rivers and reports their water (RiverTerrain)
	bool  riverPreview = true;       // draw the coarse network on the lobby's world preview
	int   riverCoarseDomain = 2;     // coarse tiles of margin each coarse tile's network is routed with
	float riverSeaDepth = 20.0f;     // model m below sea level that seeds the sea (V3's sea-level film is not sea)
	float riverPetPerC = 64.0f;      // potential evaporation, mm/yr per C above -5 C
	float riverBudykoW = 5.0f;       // the Budyko (Fu) curve's shape: higher = more of the rain evaporates
	float riverLakeEvap = 2.5f;      // open-water evaporation, x the potential evaporation
	float riverLoss = 0.001f;        // channel loss in dry land, m3/s per km per sqrt(m3/s) per unit of aridity past 1
	float riverBreachDepth = 110.0f; // model m: a shallower depression is cut through, not a lake
	int   riverLakeMinCells = 30;    // coarse pixels: a smaller depression is cut through, not a lake
	float riverMapMinQ = 30.0f;      // m3/s: a coarse link the preview draws as a river
	float riverMapDryCells = 40.0f;  // catchment in coarse pixels: a link this big under the min Q draws as a dry bed
	// The river UNITS (N x N full tiles, routed at native resolution, joined to their neighbours by crossings).
	int   riverUnitTiles = 6;          // full tiles per unit side
	int   riverCrossWindow = 64;       // native px either side of a tile edge's middle that a crossing may move to
	float riverUnitBreachDepth = 75.0f; // model m: a shallower depression in a unit is cut through, not a lake
	int   riverUnitLakeMinCells = 8334; // native px
	float riverChannelMinQ = 1.0f;     // m3/s: a channel starts here
	float riverChannelFadeQ = 2.0f;    // m3/s past the minimum over which a channel grows from nothing to its full size
	float riverPerennialQ = 0.0f;      // m3/s: a channel whose water never reaches this is ephemeral (a dry bed)
	float riverWidthA = 25.0f;         // hydraulic geometry: width = a * Q^0.5 (model m)
	float riverDepthC = 1.5f;          //                     depth = c * Q^0.4 (model m)
	float riverRapidsSlope = 0.05f;    // water surface slope that marks rapids
	float riverFallSlope = 0.3f;       // ... and a fall
	float riverEdgeWall = 12.3f;       // model m: water leaves a unit through a non-crossing edge when every outlet climbs more
	float riverPathSmoothing = 10.0f;  // native px: the Gaussian sigma a river's D8 path is smoothed with (0 = the raw staircase)
	float riverMeanderAmplitude = 0.33f;   // the meanders' sideways swing, x the channel width (0 = off)
	float riverMeanderWavelength = 15.0f; // their wavelength along the river, x the channel width
	float riverMeanderSlope = 0.02f;      // water-surface slope (m/m) by which a reach swings at the 30 % floor
	float riverMeanderSmallAmplitude = 5.0f;  // the swing on the smallest stream, x the amplitude (1 from the full Q up)
	float riverMeanderSmallWavelength = 1.0f; // the wavelength on the smallest stream, x the wavelength (1 from the full Q up)
	float riverMeanderFullQ = 20.0f;          // m3/s: from here up a river meanders as set (log Q blend from the min Q)
	// The CARVE (RiverTerrain): channel -> a curve over the bank and the floodplain -> valley wall, only ever lowering the
	// ground. Depths in MODEL m (x metersPerPixel / 30 in the world: / 6 at mpp 5).
	float riverChannelDepthScale = 2.5f;  // the channel's depth below the water, x the hydraulic depth (c * Q^0.4)
	float riverChannelMinDepth = 0.2f;    // ... but at least this
	float riverChannelWallSlope = 0.1f;   // the channel's sides (rise / run) down to a flat bed, whatever its depth
	float riverBankHeight = 2.16f;        // the floodplain's outer edge above the water, x the channel depth
	float riverFloodplainCurve = 1.0f;    // the rise over the bank + floodplain: 1 = straight, 2 = a bowl, higher = flatter near the channel
	float riverValleyDepth = 0.0f;        // the river and its floodplain sunk this far below the original ground
	float riverValleyDepthPerQ = 0.5f;    // ... plus this x Q^0.4 (bigger rivers cut deeper valleys)
	float riverBankFactor = 0.5f;       // bank width, x the channel half-width
	float riverFloodplainFactor = 0.15f; // floodplain width, x the channel half-width
	float riverValleySlope = 0.28f;     // the valley wall's slope past the floodplain (m/m)
	float riverCarveReach = 1500.0f;    // model m: how far the valley wall may run past the floodplain's edge (fades out over it)
	float riverCarveReachQ = 10.0f;     // m3/s: a river this big gets the whole reach; a smaller one sqrt(Q / this) of it
	float riverVegetationClear = 0.97f;  // trees and rocks keep out where the river influence (1 channel .. 0 floodplain edge) is above this
	bool  riverDebugLines = false;     // draw the units around the camera as debug lines
	float riverDebugRadius = 3000.0f;  // engine m
	// "Terrain/Rivers/Surface": the water the RiverSurface pipeline draws (RendererVK EPipelineIndex::River,
	// River/river.fs.glsl) - river ribbons and lake surfaces. World units.
	bool  riverSurface = true;                                     // draw river and lake water
	float riverSurfaceRadius = 8000.0f;                            // engine m: units this close get water meshes
	glm::vec3 riverAbsorption = glm::vec3(0.30f, 0.10f, 0.08f);    // 1/m: Beer-Lambert through the water (red goes first)
	glm::vec3 riverScatterColor = glm::vec3(0.05f, 0.11f, 0.10f);  // the body's colour where the bed is out of reach
	float riverRoughness = 0.05f;
	float riverRippleSize = 4.0f;       // m: the flow ripples' pattern size
	float riverRippleStrength = 0.12f;  // the noise ripples' slope (fine detail over the FFT waves)
	float riverFlowSpeed = 1.0f;        // x the river's hydraulic speed: how fast the ripples drift downstream
	float riverLakeRipple = 0.3f;       // the wind ripples on still lakes, x the ripple strength
	float riverFoamStrength = 1.0f;     // whitewater on rapids and falls
	glm::vec3 riverFoamColor = glm::vec3(0.85f, 0.88f, 0.88f);
	float riverEdgeSoftness = 0.25f;    // a river ribbon fades out over this share of its half-width at each side
	float riverLakeEdgeFade = 1.0f;     // m of water column a lake fades in over at its shore
	// The WAVES: the ocean's FFT field (the ocean always runs with the rivers), at "Wave tiling" x its frequency, dragged
	// downstream. Geometry near the camera (dense cells within "Near radius"), shading everywhere.
	float riverWaveHeight = 0.15f;      // x the ocean's wave height
	float riverWaveTiling = 3.0f;       // x the ocean's wave frequency (shorter river waves)
	float riverWaveRapids = 4.0f;
	float riverFullSizeDepth = 2.0f;    // engine m of channel depth from which a river has its full waves and flow
	float riverSmallFlow = 0.3f;        // the flow's speed on the smallest river, x its own (rises to 1 at the full-size depth)       // the waves x (1 + this x the whitewater amount) on rapids and falls
	float riverNearRadius = 250.0f;     // engine m: the dense wave geometry around the camera (fades out toward it)
	float riverNearSpacing = 1.0f;      // engine m between the dense mesh's rows
	int   riverNearAcross = 12;         // vertices across a dense ribbon
	float riverNearDrop = 0.0f;        // engine m the light ribbon sinks inside the near radius, under the dense waves
	float riverWetness = 0.6f;         // the terrain wetness under and beside rivers / lakes (0 = none, 1 = soaked)
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
