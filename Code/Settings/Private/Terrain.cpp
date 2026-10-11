module Settings.Terrain;

import Core;
import Core.glm;
import Settings.Tweaks;

// The generator-config tweaks (Enabled, Load models, Seed, Origin, Chunk size, LOD0 resolution, Sea level, Skirt depth
// and every "Terrain/V3" one) mark the streamer config-dirty: TerrainStreamer::initialize attaches that listener.
void Settings::registerTerrain(TerrainSettings& s)
{
	// Config-dirty so rebuildMaps runs on toggle: enabling is what kicks the V3 model load (disabled terrain
	// never loads the 2.28 GB of models onto the GPU).
	Tweak::boolean("Terrain", "Enabled", &s.enabled);
	Tweak::boolean("Terrain", "Load models", &s.v3LoadModels);

	// The ceiling is float-exact (overrides travel as floats): the lobby seeds the world through one.
	Tweak::intVar("Terrain", "Seed", &s.seed, 0, 16000000, 1.0f);
	Tweak::floatVar("Terrain", "Origin X (m)", &s.originX, -1.0e7f, 1.0e7f, 10.0f);
	Tweak::floatVar("Terrain", "Origin Z (m)", &s.originZ, -1.0e7f, 1.0e7f, 10.0f);
	Tweak::intVar("Terrain", "Chunk size (m)", &s.chunkSize, 128, 1024, 1.0f);
	Tweak::intVar("Terrain", "LOD0 resolution", &s.lod0Res, 128, 1024, 1.0f);
	Tweak::intVar("Terrain", "Range (chunks)", &s.ringRadius, 1, 128, 1.0f); // max generation radius from the camera chunk
	Tweak::floatVar("Terrain", "LOD step (chunks)", &s.lodStep, 0.1f, 16.0f, 0.1f); // LOD0 band width; each next band doubles
	Tweak::floatVar("Terrain", "Full-res distance (chunks)", &s.fullResDist, 0.0f, 32.0f, 0.05f); // edge distance forced to LOD0 before bands begin
	Tweak::intVar("Terrain", "Max LOD", &s.maxLod, 0, 6, 1.0f);
	Tweak::intVar("Terrain", "Uploads/frame", &s.maxUploadsPerFrame, 1, 32, 1.0f);
	Tweak::floatVar("Terrain", "Upload MB/frame", &s.maxUploadMBPerFrame, 4.0f, 96.0f, 1.0f);
	// Concurrent generation jobs: >1 lets warm-tile mesh builds overlap a cold V3 tile wait
	// (inference itself still serializes on the pipeline lock).
	Tweak::intVar("Terrain", "Gen jobs", &s.maxGenJobs, 1, 16, 1.0f);
	Tweak::floatVar("Terrain", "Sea level (m)", &s.seaLevel, -200.0f, 200.0f, 0.5f);
	Tweak::boolean("Terrain", "Edge stitching", &s.edgeStitch);
	// ONE shared baked terrain-data map (height, water level, fog|falloff|temp|hum, altitude): the volumetric
	// fog's terrain follow + regional thickness, the ocean's far shore fallback, and the terrain
	// coloring all read these cascades. Disabling it degrades all three.
	Tweak::boolean("Terrain", "Terrain data map", &s.terrainMapEnabled);
	// Diagnostic: prints what each baked cascade actually contains, decoded the way the shader
	// decodes it. The bake is otherwise invisible - a wrong sampler, pack or upload all look
	// the same from the shader side.
	Tweak::boolean("Terrain", "Log baked data map", &s.terrainMapDebugLog);
	// The ocean runs swash onto anything within ~1 m of the water level, and V3 reports sea level
	// EVERYWHERE (it models no lakes), so inland ground that happens to sit at 0..1 m reads as beach and
	// gets waves. These sink the baked water level well below any ground the ocean cannot reach, which
	// the swash's existing landlocked-water gate then takes care of; the beach texture overlay keys on
	// the same field, so inland sand goes with it. Off = the raw sampler water level.
	Tweak::boolean("Terrain", "Ocean reach limit", &s.waterReachEnabled);
	// How deep water must be before it counts as ocean at all. Guards the reach test against shallow
	// inland dips: one texel a hair under sea level would otherwise vouch for every hollow within the
	// reach radius of it. Raise it if puddles still rescue ground they should not.
	Tweak::floatVar("Terrain", "Ocean swash depth", &s.waterReach.swashDepth, 0.0f, 20.0f, 0.1f);
	Tweak::floatVar("Terrain", "Ocean reach (m)", &s.waterReach.radius, 0.0f, 100.0f, 0.1f);
	Tweak::floatVar("Terrain", "Ocean reach feather (m)", &s.waterReach.feather, 1.0f, 100.0f, 0.1f);
	Tweak::floatVar("Terrain", "Ocean reach drop (m)", &s.waterReach.drop, 0.0f, 50.0f, 0.5f);
	// Baked per-texel flow direction (the data map's 8 packed bits + the ocean shore map's B channel):
	// toward the nearest land through the surf zone - the waves' travel direction at the coast - and
	// downhill everywhere else (future rivers/water simulation). See applyFlowField for each knob's
	// role; changing one re-bakes both maps, like the reach settings above.
	Tweak::boolean("Terrain", "Flow direction", &s.flowFieldEnabled);
	Tweak::floatVar("Terrain", "Flow shore range (m)", &s.flowField.oceanRange, 0.0f, 2000.0f, 10.0f);
	Tweak::floatVar("Terrain", "Flow shore fade (m)", &s.flowField.oceanFade, 0.0f, 1000.0f, 10.0f);
	Tweak::floatVar("Terrain", "Flow smoothing (m)", &s.flowField.smoothRadius, 0.0f, 200.0f, 1.0f);
	Tweak::floatVar("Terrain", "Flow min slope", &s.flowField.minSlope, 0.0f, 0.5f, 0.005f);
	Tweak::floatVar("Terrain", "Data map range (m)", &s.terrainMapRange, 256.0f, 8192.0f, 32.0f);
	// Floor for the far cascade world size: the actual range is raised to cover the resident mesh ring
	// (2*(R+1)*chunkSize) so distant terrain never reads clamp-to-edge frozen altitude/temperature.
	Tweak::floatVar("Terrain", "Data map far range (m)", &s.terrainMapFarRange, 1024.0f, 65536.0f, 256.0f);

	// Terrain texture splatting (TERRAIN pipeline variant; the renderer reads these directly). The surface composites bottom-up
	// as ground -> beach -> rock -> snow; these shape where each layer takes over.
	// TERRAIN SURFACE WATER. ONE wetness field - rain, the ocean's swash,
	// submersion - drives ONE water surface: it fills the splat relief to a level (rain puddles) and
	// follows the LIVE OCEAN where that stands higher, so the water continues across the waterline onto
	// the sand. The ground under it darkens and glosses with the same wetness.
	Tweak::boolean("Terrain/Water", "Enabled", &s.wetEnabled);
	// --- The field: a toroidal clipmap around the scene focus, integrated on a fixed tick.
	Tweak::floatVar("Terrain/Water", "Texel size (m)", &s.wetTexelSize, 0.1f, 4.0f, 0.1f);
	// The pass integrates on a fixed tick, not per frame: a per-frame change at high fps is below the
	// R16F image's step and rounds away (framerate-dependent wetness). Keep it well under the fps.
	Tweak::floatVar("Terrain/Water", "Update rate (Hz)", &s.wetUpdateRate, 1.0f, 30.0f, 0.5f);
	// Sideways spread rate; the on/off toggle is the renderer's own "Diffusion" tweak (a baked define
	// on terrain_wetness.cs.glsl, reloaded on change).
	Tweak::floatVar("Terrain/Water", "Diffusion (1/s)", &s.wetDiffusionRate, 0.0f, 60.0f, 0.5f);
	Tweak::floatVar("Terrain/Water", "Rain (1/s)", &s.wetRain, 0.0f, 0.1f, 0.001f);
	Tweak::floatVar("Terrain/Water", "Dry time (s)", &s.wetDryTime, 1.0f, 600.0f, 1.0f);
	Tweak::floatVar("Terrain/Water", "Dry temp sensitivity", &s.wetDryTempSens, 0.0f, 0.2f, 0.005f);
	Tweak::floatVar("Terrain/Water", "Wet-in time (s)", &s.wetInTime, 0.0f, 10.0f, 0.05f);
	// Steep ground sheds water: the terrain shader decays at rate x (1 + slope * drain) per pixel
	// (mesh normal) and the compute pass divides wet-in / rain by the same factor (map gradient).
	Tweak::floatVar("Terrain/Water", "Slope drain", &s.wetSlopeDrain, 0.0f, 20.0f, 0.1f);
	// --- The water surface: the LEVEL it fills the relief to (0 = its low points, 1 = its top), the
	// fade at its intersection with the terrain, and how far it follows the live ocean.
	Tweak::floatVar("Terrain/Water", "Fill start", &s.wetFillStart, 0.0f, 0.99f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Fill full", &s.wetFillFull, 0.01f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Fill curve", &s.wetFillCurve, 0.05f, 16.0f, 0.05f);
	Tweak::floatVar("Terrain/Water", "Edge fade (m)", &s.wetEdgeFade, 0.0f, 0.5f, 0.005f);
	Tweak::floatVar("Terrain/Water", "Ocean blend (m)", &s.wetOceanBlend, 0.01f, 20.0f, 0.05f);
	Tweak::floatVar("Terrain/Water", "Ocean edge fade (m)", &s.wetOceanEdgeFade, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Film max slope (deg)", &s.wetFilmMaxSlope, 0.0f, 90.0f, 0.5f);
	Tweak::floatVar("Terrain/Water", "Film slope fade (deg)", &s.wetFilmSlopeFade, 0.0f, 45.0f, 0.5f);
	Tweak::floatVar("Terrain/Water", "Film flow speed (m/s)", &s.wetFilmFlowSpeed, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Water", "Film flow min slope (deg)", &s.wetFilmFlowMinSlope, 0.0f, 45.0f, 0.5f);
	Tweak::floatVar("Terrain/Water", "Film flow cycle (s)", &s.wetFilmFlowCycle, 0.05f, 10.0f, 0.05f);
	// --- The look: the ocean's own surface terms, so the two meet seamlessly.
	Tweak::floatVar("Terrain/Water", "Waviness", &s.wetWaviness, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Normal scale", &s.wetNormalScale, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Wind ripples", &s.wetRippleStrength, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Water roughness", &s.wetRoughness, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Wet roughness", &s.wetGroundRoughness, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Drying pattern", &s.wetDryingPattern, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Darkening edge", &s.wetDarkeningEdge, 0.001f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Water", "Roughness edge", &s.wetRoughnessEdge, 0.001f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Water", "Drying pattern size (m)", &s.wetDryingPatternSize, 0.1f, 50.0f, 0.1f);
	Tweak::floatVar("Terrain/Water", "Drying pattern relief", &s.wetDryingPatternRelief, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Drying pattern contrast", &s.wetDryingPatternContrast, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Water", "Underwater roughness", &s.wetUnderwaterRoughness, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Wet darkening", &s.wetDarkening, 0.1f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Darkening threshold", &s.wetDarkeningThreshold, 0.0001f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Water", "Roughness threshold", &s.wetRoughnessThreshold, 0.0001f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Water", "Wet normal scale", &s.wetGroundNormalScale, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Glint size (m)", &s.wetGlintSize, 0.005f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Water", "Glint coverage", &s.wetGlintCoverage, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Water", "Glint roughness", &s.wetGlintRoughness, 0.01f, 1.0f, 0.005f);

	Tweak::floatVar("Terrain/Textures", "Ground uv scale (1/m)", &s.texUvScaleGround, 0.005f, 2.0f);
	Tweak::floatVar("Terrain/Textures", "Rock uv scale (1/m)", &s.texUvScaleRock, 0.005f, 2.0f);
	Tweak::floatVar("Terrain/Textures", "Snow uv scale (1/m)", &s.texUvScaleSnow, 0.005f, 2.0f);
	// How far outside its climate box an entry still competes. Low = crisp climate borders.
	Tweak::floatVar("Terrain/Textures", "Climate blend", &s.texClimateBlend, 0.01f, 0.5f, 0.005f);
	// Slope = 1 - N.y, so 0.30 = 45 deg, 0.55 = 63. NOTE for V3: the diffusion field is 30 m/px and
	// barely reaches 45 deg on its own, so most rock arrives via the crag test below, not this one.
	Tweak::floatVar("Terrain/Textures", "Slope rock start", &s.texSlopeRockStart, 0.0f, 1.0f);
	Tweak::floatVar("Terrain/Textures", "Slope rock full", &s.texSlopeRockFull, 0.0f, 1.0f);
	// Relief above the MACRO altitude: what separates a mountain from flat ground that merely sits
	// high. This is the main rock control under V3.
	// V3's macro altitude is the coarse stage's 7.68 km surface - nearly flat across one mountain - so
	// crag relief is essentially (height - constant) and the rock boundary traces an ELEVATION CONTOUR:
	// grass gives way to stone at one height right across a range. This wanders it up and down.
	// Scaled by the local relief in the shader, so it cannot rock a plain however high it is set; safe
	// to push. 0 = off (contour lines). Both are metres at the model's true scale, like the thresholds
	// below, so they hold their look across "Meters per pixel".
	Tweak::floatVar("Terrain/Textures", "Crag wander (m)", &s.texCragWanderAmp, 0.0f, 600.0f, 5.0f);
	Tweak::floatVar("Terrain/Textures", "Crag wander wavelength (m)", &s.texCragWanderWavelength, 200.0f, 20000.0f, 100.0f);
	// Macro variation against the visible texture repeat: brightness, warm/cool hue and roughness from a world noise.
	Tweak::floatVar("Terrain/Textures", "Macro variation", &s.texMacroStrength, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Textures", "Macro hue", &s.texMacroHue, 0.0f, 0.5f, 0.005f);
	Tweak::floatVar("Terrain/Textures", "Macro roughness", &s.texMacroRoughness, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Textures", "Macro size (m)", &s.texMacroSize, 2.0f, 1000.0f, 1.0f);
	Tweak::floatVar("Terrain/Textures", "Crag relief start (m)", &s.texCragStart, 0.0f, 200.0f);
	Tweak::floatVar("Terrain/Textures", "Crag relief full (m)", &s.texCragFull, 0.0f, 400.0f);
	Tweak::floatVar("Terrain/Textures", "Beach band (m)", &s.texBeachBand, 0.0f, 20.0f);
	// Snow cover. Temperature sets the snow LINE (the generator's lapse rate is what makes it follow
	// the mountains); slope decides how much of the mountain's own rock shows through it.
	Tweak::floatVar("Terrain/Textures", "Snow temp full (C)", &s.texSnowTempFull, -30.0f, 20.0f, 0.5f);
	Tweak::floatVar("Terrain/Textures", "Snow temp none (C)", &s.texSnowTempNone, -30.0f, 20.0f, 0.5f);
	Tweak::floatVar("Terrain/Textures", "Snow slope start", &s.texSnowSlopeStart, 0.0f, 1.0f);
	Tweak::floatVar("Terrain/Textures", "Snow slope full", &s.texSnowSlopeFull, 0.0f, 1.0f);
	Tweak::floatVar("Terrain/Textures", "Snow min humidity", &s.texSnowAridity, 0.0f, 0.5f, 0.005f);
	// Relief from the splat height maps: parallax occlusion mapping near the camera (one world-space
	// march over the blended height of the visible layers) and the height blend at layer borders.
	// Baked (TERRAIN_POM): flipping it reloads the terrain shaders.
	Tweak::boolean("Terrain/Textures", "Parallax", &s.texParallaxEnabled);
	Tweak::floatVar("Terrain/Textures", "Parallax depth ground (m)", &s.texParallaxDepthGround, 0.0f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Textures", "Parallax depth rock (m)", &s.texParallaxDepthRock, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Terrain/Textures", "Parallax fade start (m)", &s.texParallaxFadeStart, 0.0f, 200.0f, 0.5f);
	Tweak::floatVar("Terrain/Textures", "Parallax fade end (m)", &s.texParallaxFadeEnd, 1.0f, 300.0f, 0.5f);
	Tweak::intVar("Terrain/Textures", "Parallax steps", &s.texParallaxSteps, 2, 64, 1.0f);
	Tweak::floatVar("Terrain/Textures", "Parallax self-shadow", &s.texParallaxShadow, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Textures", "Height blend contrast", &s.texHeightBlendContrast, 0.0f, 16.0f, 0.1f);
	// Tessellation: the ground + overlay subdivide near the camera and displace by the same height
	// composite, centred on the mesh. Independent of "Parallax" (both on = relief twice near the camera).
	// Baked: flipping it reloads the cull (TERRAIN_TESS_ROUTE) and builds / drops the tess pipeline's draws.
	Tweak::boolean("Terrain/Tessellation", "Enabled", &s.texTessEnabled);
	Tweak::intVar("Terrain/Tessellation", "Max factor", &s.texTessMaxFactor, 1, 64, 1.0f);
	Tweak::floatVar("Terrain/Tessellation", "Target edge (px)", &s.texTessTargetPx, 2.0f, 64.0f, 0.5f);
	Tweak::floatVar("Terrain/Tessellation", "Fade start (m)", &s.texTessFadeStart, 0.0f, 300.0f, 0.5f);
	Tweak::floatVar("Terrain/Tessellation", "Fade end (m)", &s.texTessFadeEnd, 1.0f, 500.0f, 0.5f);
	// Across the fade band, 1 - t^p: 1 linear, 2 quadratic (holds, drops late), 0.5 square root (drops early).
	// SEPARATE for the subdivision (the tess factor) and the displacement height.
	Tweak::floatVar("Terrain/Tessellation", "Factor falloff exponent", &s.texTessFalloffExponent, 0.05f, 16.0f, 0.05f);
	Tweak::floatVar("Terrain/Tessellation", "Height falloff exponent", &s.texTessHeightFalloffExponent, 0.05f, 16.0f, 0.05f);
	// Closer than this nothing moves: the subdivision and the height mip hold at this distance's values.
	Tweak::floatVar("Terrain/Tessellation", "Freeze distance (m)", &s.texTessFreezeDistance, 0.1f, 100.0f, 0.5f);
	Tweak::floatVar("Terrain/Tessellation", "Depth ground (m)", &s.texTessDepthGround, 0.0f, 2.0f, 0.005f);
	Tweak::floatVar("Terrain/Tessellation", "Depth rock (m)", &s.texTessDepthRock, 0.0f, 4.0f, 0.01f);

	// V3 (Terrain Diffusion). The model resolves 30 m/px, which is what makes its continents
	// continent-sized; everything below that is the slope-masked detail layer below.
	// "Meters per pixel" is a UNIFORM world scale: heights and detail shrink with it, so the model's
	// proportions survive and "Height scale" stays a pure exaggeration on top (1 = real proportions).
	// NOTE ON COST: lowering it compresses the world (mountains become reachable sooner) but is
	// quadratically MORE expensive - the same view distance then spans more model pixels, so more tiles
	// must be generated. 30 -> 15 is ~4x the inference work for the same ring radius.
	Tweak::floatVar("Terrain/V3", "Meters per pixel", &s.v3MetersPerPixel, 0.05f, 30.0f, 0.05f); // 30 = true scale
	Tweak::floatVar("Terrain/V3", "Height scale", &s.v3HeightScale, 0.0f, 4.0f, 0.05f);
	// The model's climate is real-world calibrated and the biome attractors are expressed in real units
	// to match, so both of these default to 0 (= the model's own climate). "Extra lapse" pushes the
	// snow line further down the mountains than reality; "Temp offset" makes the whole planet warmer or
	// colder.
	Tweak::floatVar("Terrain/V3", "Temp offset (C)", &s.v3TemperatureOffset, -30.0f, 30.0f, 0.5f);
	// THE snow-line dial: how fast it cools with altitude, C per MODEL metre (so it rides "Meters per
	// pixel" like every other vertical quantity). More negative = snow lower down the mountains.
	// One number for the world, not the model's own per-region rate. That was baked into the terrain-data
	// map and measured as buying nothing: both cascades regress the same data, so their rates agreed and
	// dropping it left near-vs-far disagreement unchanged at 0.15 C max. It only bought fidelity to the
	// model's absolute temperature, which nothing consumes. -0.008 is the mean of what it regressed
	// (Earth's environmental lapse is ~-0.0065).
	Tweak::floatVar("Terrain/V3", "Lapse rate (C/m)", &s.v3LapseRate, -0.03f, 0.0f, 0.0005f);
	// The model's temperature is a function of ELEVATION, so every climate boundary it draws is an
	// isotherm and therefore a contour line: grass gives way to rock at one height right across a range,
	// and the snow line is a perfect ring. This wanders the temperature so those boundaries ride up and
	// down instead. In degrees, so it breaks up the ground transitions and the snow line with one field,
	// and it goes into the climate the scatter system reads too - trees keep marking the treeline the
	// textures draw. 0 = the model's own banded climate.
	// Roughly: at the model's lapse (~0.005 C/m in model metres) 1 C of wander moves a boundary ~200
	// model metres up or down.
	Tweak::floatVar("Terrain/V3", "Climate wander (C)", &s.v3ClimateNoiseC, 0.0f, 8.0f, 0.1f);
	// Model metres, like the detail wavelengths. Floored well above the far terrain-data cascade's texel
	// (~16 m world): this is BAKED, so a short wavelength aliases and near/far stop agreeing.
	Tweak::floatVar("Terrain/V3", "Climate wander wavelength (m)", &s.v3ClimateNoiseWavelength, 500.0f, 20000.0f, 100.0f);
	Tweak::intVar("Terrain/V3", "Climate wander octaves", &s.v3ClimateNoiseOctaves, 1, 6, 1.0f);
	Tweak::floatVar("Terrain/V3", "Detail slope gain", &s.v3DetailSlopeGain, 0.0f, 4.0f, 0.05f); // higher = detail on gentler slopes
	// THE resolution dial. The model resolves 30 m/px and nothing will make it resolve less, so every
	// feature below that is these two fBm layers - and how far down they reach is set by the OCTAVE
	// count, not the wavelength: finest feature = wavelength / 2^(octaves-1), in MODEL metres, then
	// scaled by metersPerPixel/30 like everything else.
	//
	// At the defaults (A 220 m/4, B 45 m/3) the finest thing in the field is 45/4 = 11 model metres.
	// That is why lowering "Meters per pixel" looks like it adds detail: at mpp=3 those 11 model metres
	// become 1.1 world metres, which finally matches the 1 m vertex grid (chunk size / LOD0 res). It is
	// not generating more - it is shrinking the world until the existing detail reaches the vertices.
	// To get the same crispness at FULL scale (mpp=30), raise the octaves instead:
	//     B 3 -> 5   finest 11.25 m -> 2.81 m
	//     A 4 -> 6   finest 27.50 m -> 6.88 m
	// Do not chase the last factor of two: below ~2 m the detail is finer than the 1 m vertex grid can
	// represent and simply aliases into shimmer. fbm() is amplitude-normalised, so extra octaves add
	// roughness without inflating the relief, and each one is a single noise lookup against a diffusion
	// tile resolve - the cost is not measurable next to inference.
	Tweak::floatVar("Terrain/V3", "Detail A wavelength (m)", &s.v3DetailWavelengthA, 20.0f, 1000.0f, 5.0f);
	Tweak::floatVar("Terrain/V3", "Detail A amp (m)", &s.v3DetailAmplitudeA, 0.0f, 200.0f, 1.0f);
	Tweak::intVar("Terrain/V3", "Detail A octaves", &s.v3DetailOctavesA, 1, 8, 1.0f);
	Tweak::floatVar("Terrain/V3", "Detail B wavelength (m)", &s.v3DetailWavelengthB, 5.0f, 300.0f, 1.0f);
	Tweak::floatVar("Terrain/V3", "Detail B amp (m)", &s.v3DetailAmplitudeB, 0.0f, 80.0f, 0.5f);
	Tweak::intVar("Terrain/V3", "Detail B octaves", &s.v3DetailOctavesB, 1, 8, 1.0f);
	Tweak::floatVar("Terrain/V3", "Precip for full humidity", &s.v3PrecipFullHumidity, 200.0f, 6000.0f, 50.0f);
	Tweak::floatVar("Terrain/V3", "Humidity offset", &s.v3HumidityOffset, -1.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/V3", "Humid fog", &s.v3HumidFog, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/V3", "Valley fog", &s.v3ValleyFog, 0.0f, 1.0f, 0.01f);
	Tweak::intVar("Terrain/V3", "Resident tiles", &s.v3MaxTiles, 4, 256, 1.0f); // ~800 KB each
	// Half-precision inference. This does NOT generate terrain faster: measured at mpp=3, the near
	// cascade is unchanged (4.03 s -> 4.02 s) and the coarse/far path is ~15% SLOWER (1.12 s -> 1.30 s).
	// The pipeline is dispatch-bound, not compute-bound, so halving the FLOPs buys nothing and the
	// boundary Casts cost a little. What it does buy: model VRAM drops ~2.28 GB -> ~1.1 GB (the
	// renderer wants that back) and the models load ~1.8x faster (2.14 s -> 1.19 s for base).
	// Needs the optional fp16 models (Tools/convert_models_fp16.py); without them this does nothing and
	// the log says so. Flipping it RELOADS the weights and regenerates the world - fp16 is not
	// bit-identical, so the same seed grows visibly different fine detail.
	Tweak::boolean("Terrain/V3", "FP16 inference", &s.v3Fp16);
	// "Load models" (registered above): off = never load the 2.28 GB of diffusion weights: the terrain runs on the
	// .tile files already in Local/Diffusion/<seed>/ (a missing tile falls back to the coarse stage, else sea level).
	// Turning it off with the models up unloads them.

	// Rivers (Docs/RiverPlan.md): the coarse drainage network. Runoff = rain - evaporation (the Budyko curve over the
	// model's precipitation and temperature), routed downhill over the conditioned coarse field; lakes balance their
	// inflow against open-water evaporation (full lakes spill, terminal ones and salt pans do not). The lobby preview
	// re-applies a change at once, from its stored samples.
	// Enabled: the terrain sampler is wrapped by the rivers (carved channels, river / lake water in the terrain points).
	// Every row here but the preview and debug ones rebuilds the terrain (TerrainStreamer's listeners).
	Tweak::boolean("Terrain/Rivers", "Enabled", &s.riverEnabled);
	Tweak::boolean("Terrain/Rivers", "Show on preview", &s.riverPreview);
	Tweak::intVar("Terrain/Rivers", "Coarse domain (tiles)", &s.riverCoarseDomain, 0, 4, 1.0f); // 2 = 982 km model margin
	Tweak::floatVar("Terrain/Rivers", "Sea depth (m)", &s.riverSeaDepth, 0.0f, 500.0f, 1.0f);
	Tweak::floatVar("Terrain/Rivers", "Evaporation per C (mm/yr)", &s.riverPetPerC, 0.0f, 150.0f, 1.0f);
	Tweak::floatVar("Terrain/Rivers", "Evaporation curve", &s.riverBudykoW, 1.1f, 6.0f, 0.05f); // higher = more of the rain evaporates
	Tweak::floatVar("Terrain/Rivers", "Lake evaporation", &s.riverLakeEvap, 0.0f, 3.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Dry channel loss", &s.riverLoss, 0.0f, 0.05f, 0.0005f);
	Tweak::floatVar("Terrain/Rivers", "Breach depth (m)", &s.riverBreachDepth, 0.0f, 1000.0f, 5.0f);
	Tweak::intVar("Terrain/Rivers", "Lake min cells", &s.riverLakeMinCells, 1, 64, 1.0f);
	Tweak::floatVar("Terrain/Rivers", "Preview min Q (m3/s)", &s.riverMapMinQ, 1.0f, 5000.0f, 1.0f);
	Tweak::floatVar("Terrain/Rivers", "Preview dry bed cells", &s.riverMapDryCells, 1.0f, 1000.0f, 1.0f);
	// The units: N x N full tiles routed at native resolution; the coarse network decides how much water crosses
	// between units and through which tile edge (a crossing, at the edge's low point near its middle). Any change
	// rebuilds the units (their disk cache is keyed by every river setting).
	Tweak::intVar("Terrain/Rivers", "Unit tiles", &s.riverUnitTiles, 1, 8, 1.0f);
	Tweak::intVar("Terrain/Rivers", "Crossing window (px)", &s.riverCrossWindow, 0, 127, 1.0f);
	Tweak::floatVar("Terrain/Rivers", "Unit breach depth (m)", &s.riverUnitBreachDepth, 0.0f, 500.0f, 0.5f);
	Tweak::intVar("Terrain/Rivers", "Unit lake min cells", &s.riverUnitLakeMinCells, 1, 100000, 10.0f);
	// A basin bigger than this holds water in only its lowest this many pixels; the rest is land, the outflow cuts down
	// through the rim to the lower level. 0 = no limit.
	Tweak::intVar("Terrain/Rivers", "Unit lake max cells", &s.riverUnitLakeMaxCells, 0, 1000000, 100.0f);
	Tweak::floatVar("Terrain/Rivers", "Channel min Q (m3/s)", &s.riverChannelMinQ, 0.01f, 100.0f, 0.01f);
	// A channel's width and depth grow from 0 at the min Q to full size at min Q + this: streams fade in at their head
	// and out where their water drains away.
	Tweak::floatVar("Terrain/Rivers", "Fade Q (m3/s)", &s.riverChannelFadeQ, 0.0f, 100.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Perennial Q (m3/s)", &s.riverPerennialQ, 0.0f, 100.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Width a", &s.riverWidthA, 0.5f, 60.0f, 0.1f);
	Tweak::floatVar("Terrain/Rivers", "Depth c", &s.riverDepthC, 0.05f, 10.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers", "Rapids slope", &s.riverRapidsSlope, 0.0f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Rivers", "Fall slope", &s.riverFallSlope, 0.0f, 5.0f, 0.01f);
	// WATERFALLS: a run steeper than "Fall slope", at most "Fall max length" native px long, dropping at least "Fall min
	// height" model m, becomes a straight drop at its top; the carve cuts a plunge gorge below it. 0 = no waterfalls.
	Tweak::floatVar("Terrain/Rivers", "Fall min height (m)", &s.riverFallMinHeight, 0.0f, 500.0f, 0.5f);
	Tweak::floatVar("Terrain/Rivers", "Fall max length (px)", &s.riverFallMaxLength, 0.0f, 64.0f, 0.25f);
	// The gorge scales with the water: a river of this Q or more cuts the whole max length, a smaller one Q / this of it
	// (a small stream cuts no gorge).
	Tweak::floatVar("Terrain/Rivers", "Fall full Q (m3/s)", &s.riverFallFullQ, 0.01f, 1000.0f, 0.5f);
	// THE SEA CHANNEL: a river reaching the sea runs on down the steepest way until the sea floor is "Sea channel depth"
	// model m deep (at most "Sea channel max length" native px), its channel carved out to deep water - on a coastal flat
	// at about 0 it stopped at the waterline. 0 = off.
	Tweak::floatVar("Terrain/Rivers", "Sea channel depth (m)", &s.riverSeaChannelDepth, 0.0f, 500.0f, 1.0f);
	Tweak::floatVar("Terrain/Rivers", "Sea channel max length (px)", &s.riverSeaChannelMaxLength, 0.0f, 1000.0f, 5.0f);
	// THE LAKE CHANNEL: a river reaching a lake runs on into it down the steepest way until the floor is "Lake channel
	// depth" model m under the level (at most "Lake channel max length" native px), cutting through the shore - it
	// stopped at the lake's first pixel, with dry ground and trees left before the open water. 0 = off.
	Tweak::floatVar("Terrain/Rivers", "Lake channel depth (m)", &s.riverLakeChannelDepth, 0.0f, 200.0f, 0.5f);
	Tweak::floatVar("Terrain/Rivers", "Lake channel max length (px)", &s.riverLakeChannelMaxLength, 0.0f, 500.0f, 1.0f);
	// THE SEA RESCUE: a river of at least "Sea rescue min Q" leaving a unit where the coarse network has no crossing (it
	// ended at the unit's edge) is sent toward the sea or lake its coarse route reaches in the unit, on the lowest-rim
	// way through that route's tiles, cutting a rim of at most "Sea rescue max rim" model m over a path of at most "Sea
	// rescue max length" native px. 0 min Q = off.
	Tweak::floatVar("Terrain/Rivers", "Sea rescue min Q (m3/s)", &s.riverSeaRescueMinQ, 0.0f, 1000.0f, 0.5f);
	Tweak::floatVar("Terrain/Rivers", "Sea rescue max rim (m)", &s.riverSeaRescueMaxRim, 0.0f, 1000.0f, 5.0f);
	Tweak::floatVar("Terrain/Rivers", "Sea rescue max length (px)", &s.riverSeaRescueMaxLength, 0.0f, 5000.0f, 10.0f);
	// A unit's edge away from its crossings is a SOFT wall: higher = more water forced to a crossing (and deeper filled
	// basins on the way), lower = more streams that end at the unit edge.
	Tweak::floatVar("Terrain/Rivers", "Edge wall (m)", &s.riverEdgeWall, 0.0f, 500.0f, 1.0f);
	// The D8 flow path is straight runs at 0 / 45 / 90 degrees with sharp kinks: a Gaussian along the path, sigma in
	// native px (5 world m at mpp 5), rounds them into curves. The ends (junctions, crossings) stay put.
	Tweak::floatVar("Terrain/Rivers", "Path smoothing (px)", &s.riverPathSmoothing, 0.0f, 40.0f, 0.5f);
	// MEANDERS: a sideways swing along the smoothed path - two sines, the wavelength and the swing in multiples of the
	// channel width (rivers meander at ~10-14 widths), the phase from the segment's start. Fades out at junctions and
	// crossings, shrinks to 30 % on a steep reach, and is pulled back wherever it would climb out of the valley floor.
	Tweak::floatVar("Terrain/Rivers", "Meander amplitude", &s.riverMeanderAmplitude, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Meander wavelength", &s.riverMeanderWavelength, 2.0f, 60.0f, 0.5f);
	Tweak::floatVar("Terrain/Rivers", "Meander slope", &s.riverMeanderSlope, 0.001f, 0.5f, 0.001f);
	// Small streams wind tighter and wider (in widths): on the smallest the swing is x "small amplitude" and the
	// wavelength x "small wavelength", blending (log Q) to x 1 at "Meander full Q".
	Tweak::floatVar("Terrain/Rivers", "Meander small amplitude", &s.riverMeanderSmallAmplitude, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Meander small wavelength", &s.riverMeanderSmallWavelength, 0.05f, 4.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers", "Meander full Q (m3/s)", &s.riverMeanderFullQ, 0.01f, 1000.0f, 0.5f);
	// END LAKES: a river that ends in a sink or runs dry (not in the sea, a lake, another river or over a unit edge)
	// floods a terminal lake at its end - "size" native px per m3/s arriving (0 = off: it fades out), at most "max
	// depth" model m above the end's ground; one that would reach the unit's edge stops there.
	Tweak::floatVar("Terrain/Rivers", "End lake size (px per m3/s)", &s.riverEndLakeArea, 0.0f, 5000.0f, 10.0f);
	Tweak::floatVar("Terrain/Rivers", "End lake max depth (m)", &s.riverEndLakeMaxDepth, 0.0f, 300.0f, 1.0f);
	// The carve, in MODEL metres (/ 6 in the world at mpp 5) / multiples of the channel half-width (hydraulic geometry:
	// width = a * Q^0.5). The water sits at the original ground minus the valley depth; the channel is cut below it.
	Tweak::floatVar("Terrain/Rivers", "Channel depth scale", &s.riverChannelDepthScale, 0.0f, 20.0f, 0.1f);
	Tweak::floatVar("Terrain/Rivers", "Channel min depth (m)", &s.riverChannelMinDepth, 0.0f, 50.0f, 0.1f);
	// The channel's sides slope this steeply (rise / run) from the water's edge down to a FLAT bed, rounded at the foot:
	// a deeper channel has the same sides, only longer (a narrow, deep one becomes a V of that slope).
	Tweak::floatVar("Terrain/Rivers", "Channel wall slope", &s.riverChannelWallSlope, 0.1f, 10.0f, 0.05f);
	// A waterfall's plunge gorge (RiverPoint_Gorge): past the channel's edge its walls rise at this (rise / run), with no
	// floodplain - a narrow slot, not the wide valley carve; it fades back to the valley at the gorge's end.
	Tweak::floatVar("Terrain/Rivers", "Gorge wall slope", &s.riverGorgeWallSlope, 0.1f, 20.0f, 0.05f);
	// A waterfall's drop face: within this x the river's half-width of the channel the ground breaks sharply at the lip
	// (the falling sheet's room - the rock clipped its sides); past it the face leans at about "Gorge wall slope".
	Tweak::floatVar("Terrain/Rivers", "Fall clearance", &s.riverFallClearance, 1.0f, 6.0f, 0.05f);
	// The ground under a falling sheet within that clearance rises at most this (rise / run): the sheet falls near-
	// vertically, and a face that steep drew as a comb on the terrain mesh, so the face starts before the lip (under the
	// upper water) as far as it needs. Higher = a shorter undercut, a steeper face.
	Tweak::floatVar("Terrain/Rivers", "Fall face slope", &s.riverFallFaceSlope, 0.5f, 20.0f, 0.1f);
	// On rapids and falls (the piece's steepness, half "Rapids slope" to "Fall slope"; a plunge gorge in full) the channel
	// is up to this much deeper / wider: room for the falling sheet and the waves (the water itself is unchanged).
	Tweak::floatVar("Terrain/Rivers", "Whitewater channel depth", &s.riverWhitewaterDepth, 1.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Whitewater channel widen", &s.riverWhitewaterWiden, 1.0f, 3.0f, 0.01f);
	// THE WATER HUMIDITY: near a perennial river (x its size up to "Water humidity full Q") or a lake the humidity is
	// pulled toward 1 by "Water humidity", fading out over "Water humidity spread" (engine m) from the water's edge.
	// Sampled climate only (biomes, textures, trees, rocks, clutter, grass) - the rivers are built from the raw tiles, so
	// no cycle.
	Tweak::floatVar("Terrain/Rivers", "Water humidity", &s.riverWaterHumidity, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers", "Water humidity spread (m)", &s.riverWaterHumiditySpread, 1.0f, 1000.0f, 5.0f);
	Tweak::floatVar("Terrain/Rivers", "Water humidity full Q (m3/s)", &s.riverWaterHumidityFullQ, 0.01f, 1000.0f, 0.5f);
	// A lake's bed lowered by this much (model m) away from its shore, growing in over the reach (native px) as a
	// smoothstep, so the ground at the waterline keeps its own slope.
	Tweak::floatVar("Terrain/Rivers", "Lake bed deepen (m)", &s.riverLakeBedDeepen, 0.0f, 200.0f, 0.5f);
	Tweak::floatVar("Terrain/Rivers", "Lake bed deepen reach (px)", &s.riverLakeBedDeepenReach, 1.0f, 32.0f, 0.25f);
	// How far (native px) past a lake's wet pixels its shore sand reaches, fading out (no grass or clutter there either).
	Tweak::floatVar("Terrain/Rivers", "Lake shore (px)", &s.riverLakeShore, 0.1f, 16.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Bank height", &s.riverBankHeight, 0.0f, 20.0f, 0.05f);
	// The rise from the water to the floodplain's outer edge (bank + floodplain widths): ((x / width) ^ this) x the bank
	// height. 1 = a straight slope, 2 = a bowl (flat by the channel, steepening outward), higher = a flatter floor.
	Tweak::floatVar("Terrain/Rivers", "Floodplain curve", &s.riverFloodplainCurve, 0.5f, 8.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Valley depth (m)", &s.riverValleyDepth, 0.0f, 300.0f, 0.5f);
	Tweak::floatVar("Terrain/Rivers", "Valley depth per Q", &s.riverValleyDepthPerQ, 0.0f, 50.0f, 0.1f);
	Tweak::floatVar("Terrain/Rivers", "Bank factor", &s.riverBankFactor, 0.05f, 5.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers", "Floodplain factor", &s.riverFloodplainFactor, 0.0f, 20.0f, 0.1f);
	Tweak::floatVar("Terrain/Rivers", "Valley slope", &s.riverValleySlope, 0.02f, 2.0f, 0.01f);
	// How far the valley wall may run PAST the floodplain's edge (the curve itself is always carved whole); it fades
	// out over its last 30 %.
	Tweak::floatVar("Terrain/Rivers", "Carve reach (m)", &s.riverCarveReach, 0.0f, 3000.0f, 10.0f);
	// The valley SCALES WITH THE WATER: a river of this Q or more gets the whole carve reach, a smaller one
	// (Q / this)^exponent of it - a small stream cuts a small valley (0 = every channel the whole reach). Exponent 0.5 =
	// the square root; higher narrows the small streams' valleys faster.
	Tweak::floatVar("Terrain/Rivers", "Carve reach Q (m3/s)", &s.riverCarveReachQ, 0.0f, 1000.0f, 0.5f);
	Tweak::floatVar("Terrain/Rivers", "Carve reach Q exponent", &s.riverCarveReachQExponent, 0.0f, 4.0f, 0.05f);
	// The river influence is 1 in the channel and falls to 0 at the floodplain's edge: trees and rocks keep out above
	// this (1 = only the channel's wet core, 0 = the whole carved bed and floodplain). Re-places the records.
	Tweak::floatVar("Terrain/Rivers", "Vegetation clear", &s.riverVegetationClear, 0.0f, 1.0f, 0.01f);
	Tweak::boolean("Terrain/Rivers", "Debug lines", &s.riverDebugLines);
	Tweak::floatVar("Terrain/Rivers", "Debug radius (m)", &s.riverDebugRadius, 100.0f, 50000.0f, 100.0f);

	// The water (RendererVK EPipelineIndex::River): a ribbon per river and a flat surface per lake, built per unit by
	// Procedural's RiverSystem, shaded like the ocean without its waves - Fresnel, the RT mirror and the RT refraction to
	// the bed under the ocean's "RT" tweaks, flow-mapped ripples drifting downstream, whitewater on rapids and falls.
	Tweak::boolean("Terrain/Rivers/Surface", "Enabled", &s.riverSurface);
	Tweak::floatVar("Terrain/Rivers/Surface", "Radius (m)", &s.riverSurfaceRadius, 500.0f, 50000.0f, 100.0f);
	Tweak::color3("Terrain/Rivers/Surface", "Absorption (1/m)", &s.riverAbsorption);
	Tweak::color3("Terrain/Rivers/Surface", "Scatter colour", &s.riverScatterColor);
	Tweak::floatVar("Terrain/Rivers/Surface", "Roughness", &s.riverRoughness, 0.02f, 1.0f, 0.005f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Ripple size (m)", &s.riverRippleSize, 0.1f, 50.0f, 0.1f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Ripple strength", &s.riverRippleStrength, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Flow speed", &s.riverFlowSpeed, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Lake ripple", &s.riverLakeRipple, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Foam strength", &s.riverFoamStrength, 0.0f, 4.0f, 0.05f);
	Tweak::color3("Terrain/Rivers/Surface", "Foam colour", &s.riverFoamColor);
	Tweak::floatVar("Terrain/Rivers/Surface", "Edge softness", &s.riverEdgeSoftness, 0.01f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Lake edge fade (m)", &s.riverLakeEdgeFade, 0.01f, 5.0f, 0.01f);
	// The waves: the OCEAN's FFT field (the ocean always runs with the rivers) at "Wave tiling" x its frequency and
	// "Wave height" x its height, dragged downstream by the flow. Real geometry inside "Near radius" (RiverSystem's dense
	// cells; the light ribbon sinks "Near drop" under them there), the shading normal everywhere.
	Tweak::floatVar("Terrain/Rivers/Surface", "Wave height", &s.riverWaveHeight, 0.0f, 2.0f, 0.005f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Wave tiling", &s.riverWaveTiling, 0.25f, 20.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Wave rapids", &s.riverWaveRapids, 0.0f, 20.0f, 0.1f);
	// SMALL RIVERS: from this channel depth (engine m) up a river behaves as set; below it its waves shrink in proportion
	// (a stream's fading head has none) and its flow slows toward "Small river flow" x its speed. Lakes are full size.
	Tweak::floatVar("Terrain/Rivers/Surface", "Full size depth (m)", &s.riverFullSizeDepth, 0.01f, 20.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Small river flow", &s.riverSmallFlow, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Near radius (m)", &s.riverNearRadius, 0.0f, 2000.0f, 10.0f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Near spacing (m)", &s.riverNearSpacing, 0.1f, 5.0f, 0.05f);
	Tweak::intVar("Terrain/Rivers/Surface", "Near across", &s.riverNearAcross, 2, 64, 1.0f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Near drop (m)", &s.riverNearDrop, 0.0f, 5.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Wetness", &s.riverWetness, 0.0f, 1.0f, 0.01f);
	// THE MIST off rapids and falls: RiverSystem hands the whitewater stretches within "Mist radius" up to the renderer,
	// whose producer (river_mist.cs.glsl) spawns Effects/river_mist.pfx particles over them at "Mist rate" per m2 per s x
	// the whitewater past "Mist threshold" (the whitewater is scaled by the river's size, so a stream's riffle barely
	// mists). "Particles/River mist" switches it.
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist radius (m)", &s.riverMistRadius, 10.0f, 2000.0f, 5.0f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist rate", &s.riverMistRate, 0.0f, 20.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist threshold", &s.riverMistThreshold, 0.0f, 0.99f, 0.01f);
	// The rate's shape: an exponent on the whitewater past the threshold (higher = falls far over rapids), and how much
	// a small river's mist shrinks with its size (0 = not at all, 1 = x depth / "Full size depth").
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist curve", &s.riverMistCurve, 0.1f, 8.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist size weight", &s.riverMistSizeWeight, 0.0f, 1.0f, 0.01f);
	// Where across the channel it spawns: 0 = evenly over the whole width, higher = gathered toward the centre line (the
	// offset is |u|^(1 + this) x the half-width).
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist centering", &s.riverMistCentering, 0.0f, 8.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist speed", &s.riverMistSpeed, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist kick (m/s)", &s.riverMistKick, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist height (m)", &s.riverMistHeight, -2.0f, 5.0f, 0.05f);
	// A waterfall's foot mists at full whitewater over this many metres of river (mostly downstream) per metre the water
	// falls: a taller fall throws more. 0 = no plunge mist.
	Tweak::floatVar("Terrain/Rivers/Surface", "Mist plunge", &s.riverMistPlunge, 0.0f, 10.0f, 0.05f);
	// The plunge pool's mist has its own centering, speed, kick and height (the same meaning as the "Mist ..." ones).
	Tweak::floatVar("Terrain/Rivers/Surface", "Plunge mist centering", &s.riverPlungeMistCentering, 0.0f, 8.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Plunge mist speed", &s.riverPlungeMistSpeed, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Plunge mist kick (m/s)", &s.riverPlungeMistKick, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Terrain/Rivers/Surface", "Plunge mist height (m)", &s.riverPlungeMistHeight, -2.0f, 5.0f, 0.05f);
}

// "Tile size", "Spacing" and "Friction" rebuild every collider tile: TerrainCollider::initialize attaches that listener.
void Settings::registerTerrainCollider(TerrainColliderSettings& s)
{
	Tweak::boolean("Terrain/Collision", "Enabled", &s.enabled);
	Tweak::floatVar("Terrain/Collision", "Radius", &s.radius, 16.0f, 512.0f, 1.0f);
	Tweak::floatVar("Terrain/Collision", "Tile size", &s.tileSize, 8.0f, 128.0f, 1.0f);
	// 1 m matches the render LOD0 lattice (chunkSize / lod0Res), so collision == drawn surface.
	// Coarser trades fidelity for memory; finer buys nothing the render mesh shows.
	Tweak::floatVar("Terrain/Collision", "Spacing", &s.spacing, 0.25f, 8.0f, 0.05f);
	Tweak::floatVar("Terrain/Collision", "Friction", &s.friction, 0.0f, 2.0f, 0.01f);
}
