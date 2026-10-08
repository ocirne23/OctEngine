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
