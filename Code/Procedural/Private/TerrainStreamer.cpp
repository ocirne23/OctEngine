module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Tweaks;
import Core.Log;

import RendererVK;
import File;
import Spatial;

import :TerrainStreamer;
import :TerrainSampler;
import :TerrainGenerator;
import :TerrainChunk;

namespace
{
	// Chebyshev distance (in chunk units) from the camera to the NEAREST EDGE of a chunk's footprint —
	// 0 while the camera stands inside/on it. Distance to the edge, not to the center ring: a neighbor's
	// near boundary can be a whole chunk away when the camera sits centered, or right underfoot at the
	// boundary, and the LOD should follow that continuously instead of stepping per camera-chunk crossing.
	// camChunks must be the SNAPPED camera position (see update()) so every caller sees the same value.
	float chunkEdgeDist(glm::vec2 camChunks, glm::ivec2 coord)
	{
		const float dx = glm::max(glm::max((float)coord.x - camChunks.x, camChunks.x - (float)(coord.x + 1)), 0.0f);
		const float dz = glm::max(glm::max((float)coord.y - camChunks.y, camChunks.y - (float)(coord.y + 1)), 0.0f);
		return glm::max(dx, dz);
	}

	// GEOMETRIC LOD bands over edge distance: lod = floor(log2(1 + d/lodStep)) — lodStep chunks of LOD0,
	// then 2*lodStep of LOD1, 4*lodStep of LOD2, ... capped at maxLod. Each LOD halves mesh density while
	// a feature's screen size halves per distance DOUBLING, so doubling band widths keeps the on-screen
	// triangle density roughly constant (linear bands over-detailed the mid rings). This is THE ring-LOD
	// function: enqueue, queue staleness, result validation and eviction all derive from it.
	uint32 ringLodAt(float edgeDist, float fullRes, float lodStep, uint32 maxLod)
	{
		// Everything whose edge is within fullRes chunks is unconditionally LOD0 — without it a chunk
		// whose boundary you are standing on could already be a level down.
		const float k = glm::max(edgeDist - fullRes, 0.0f) / glm::max(lodStep, 0.01f);
		const int lod = (int)std::floor(std::log2(1.0f + k));
		return glm::min(maxLod, (uint32)glm::max(lod, 0));
	}

	// Pack a chunk coordinate + LOD into a stable 64-bit key. 28 bits each for X/Z covers +-134M chunks.
	uint64 chunkKey(glm::ivec2 coord, uint32 lod)
	{
		const uint64 x = (uint64)(uint32)coord.x & 0xFFFFFFFull;
		const uint64 z = (uint64)(uint32)coord.y & 0xFFFFFFFull;
		return (x << 36) | (z << 8) | (uint64)(lod & 0xFFu);
	}

	// --- Terrain splat texture sources -----------------------------------------------------------------
	// THERE ARE NO BIOMES HERE. Climate picks textures directly: every Ground and Rock entry declares the
	// CLIMATE BOX it covers in real units (mean annual temperature C, annual precipitation mm/yr), and the
	// shader blends the two best matches per pixel — weight 1 inside the box, Gaussian falloff outside.
	// Leaving an axis at its full range means "this entry does not care about it": alpine bedrock is cold
	// at ANY humidity and simply says so. The old point attractors could not express that, which is why
	// every cold entry used to have to claim one fictional humidity and sit in a hand-tuned ladder.
	//
	// The bands come off the Whittaker diagram (the standard mean-temperature/mean-precipitation plot of
	// what actually grows where). That is also why the hot entries sit at HIGHER precipitation than cold
	// ones covering similar vegetation: 1000 mm on a 25 C plain is seasonal savanna, on a 0 C plain it is
	// boreal forest. Evaporation is not modelled — it is baked into where the bands sit.
	//
	// Beach and Snow are NOT climate entries; they are overlays the shader composites on top (see
	// terrainSplat). Snow especially must not be a ground entry: the rock layer paints over the ground, so
	// a "snow ground type" is overwritten on exactly the peaks it exists for, and they come out gray.
	//
	// Sources are the CC0 sets under Assets/Textures/Terrain (see Assets/THIRD_PARTY_ASSETS.md); they bake
	// once into Assets/Local/TerrainTex as BC .dds so they mip-stream like cooked scene textures.
	enum class ESourceKind : uint8 { Ground, Rock, Beach, Snow };
	// Sentinels for "this axis does not constrain the entry" — past the encodable climate range, so they
	// clamp to a full-width 0..1 box and contribute no distance on that axis.
	constexpr float ANY_COLD = -1000.0f, ANY_HOT = 1000.0f, ANY_DRY = 0.0f, ANY_WET = 100000.0f;
	struct TerrainTexSource
	{
		ESourceKind kind = ESourceKind::Ground;
		// Climate box in REAL units; ignored for Beach/Snow.
		float tempMinC = ANY_COLD, tempMaxC = ANY_HOT;
		float precipMinMm = ANY_DRY, precipMaxMm = ANY_WET;
		// Texture set name under Assets/Textures/Terrain/. Sources default to the Poly Haven layout,
		// <stem>/<stem>_{diff,nor_gl,arm}_2k.jpg, and bake to Local/TerrainTex/<stem>_{diff,nor,arm}.dds —
		// so the cache is keyed by the texture, and swapping one here can never read a stale bake.
		const char* stem;
		// Only for sets that name their files differently (the ambientCG ones). Relative to Assets/.
		const char* diffSrc = nullptr;
		const char* norSrc = nullptr;
		const char* armSrc = nullptr;
	};
	const TerrainTexSource TERRAIN_TEX_SOURCES[] =
	{
		// --- Ground: what the climate grows.
		// The boxes ABUT with only a narrow overlap. That is deliberate and is the one thing to preserve
		// when editing: a point inside two boxes scores a perfect 1.0 on both, so a wide overlap is not a
		// wide blend — it is a permanent 50/50 mush in which neither texture ever appears on its own. The
		// blend comes from the Gaussian tails just outside the edges, so entries should MEET, not straddle.
		//                temperature C           precipitation mm/yr
		{ .tempMinC = ANY_COLD, .tempMaxC = -4.0f,   .precipMinMm = ANY_DRY,  .precipMaxMm = ANY_WET, .stem = "gravel_ground_01" },        // polar/alpine scree: frost-shattered rubble. The substrate beside (and under) the snow, at ANY humidity — above the snow line there is no vegetation left for humidity to decide
		{ .tempMinC = -5.0f,    .tempMaxC = 0.0f,    .precipMinMm = 250.0f,   .precipMaxMm = ANY_WET, .stem = "rocky_trail" },             // tundra: moss and lichen over stony ground
		{ .tempMinC = -1.0f,    .tempMaxC = 5.0f,    .precipMinMm = 300.0f,   .precipMaxMm = 1500.0f, .stem = "forest_ground_04" },        // taiga / boreal forest floor: needle litter
		{ .tempMinC = 4.0f,     .tempMaxC = 19.0f,   .precipMinMm = ANY_DRY,  .precipMaxMm = 400.0f,  .stem = "dry_ground_01" },           // cold desert / dry steppe
		{ .tempMinC = 4.0f,     .tempMaxC = 20.0f,   .precipMinMm = 400.0f,   .precipMaxMm = 1000.0f, .stem = "Grass001",                  // temperate grassland / prairie
		  .diffSrc = "Textures/Terrain/Grass001/Grass001_2K-JPG_Color.jpg", .norSrc = "Textures/Terrain/Grass001/Grass001_2K-JPG_NormalGL.jpg", .armSrc = "Textures/Terrain/Grass001/Grass001_arm_2k.jpg" },
		{ .tempMinC = 5.0f,     .tempMaxC = 18.0f,   .precipMinMm = 1000.0f,  .precipMaxMm = 1900.0f, .stem = "forest_floor" },            // temperate seasonal forest: broadleaf litter
		{ .tempMinC = 3.0f,     .tempMaxC = 15.0f,   .precipMinMm = 1800.0f,  .precipMaxMm = ANY_WET, .stem = "Moss002",                   // temperate rainforest: deep moss
		  .diffSrc = "Textures/Terrain/Moss002/Moss002_2K-JPG_Color.jpg", .norSrc = "Textures/Terrain/Moss002/Moss002_2K-JPG_NormalGL.jpg", .armSrc = "Textures/Terrain/Moss002/Moss002_arm_2k.jpg" },
		{ .tempMinC = 20.0f,    .tempMaxC = ANY_HOT, .precipMinMm = ANY_DRY,  .precipMaxMm = 300.0f,  .stem = "sand_01" },                 // subtropical desert: dune sand
		{ .tempMinC = 19.0f,    .tempMaxC = ANY_HOT, .precipMinMm = 450.0f,   .precipMaxMm = 1300.0f, .stem = "red_laterite_soil_stones" },// savanna / dry tropics: iron-red laterite. 450 floor: at 250 this box bridged the steppe|sand seam and drew a red isotherm sliver across every desert
		{ .tempMinC = 18.0f,    .tempMaxC = ANY_HOT, .precipMinMm = 1300.0f,  .precipMaxMm = 2100.0f, .stem = "leaves_forest_ground" },    // tropical forest floor: leaf litter
		{ .tempMinC = 16.0f,    .tempMaxC = ANY_HOT, .precipMinMm = 2000.0f,  .precipMaxMm = ANY_WET, .stem = "mud_forest" },              // wetland / swamp: saturated mud

		// --- Rock: the bedrock the slope/crag layer exposes. Climate still selects the TYPE — weathering
		// is a climate process — but the cold entry spans all humidity for the same reason the scree does.
		{ .kind = ESourceKind::Rock, .tempMinC = ANY_COLD, .tempMaxC = 1.0f,    .precipMinMm = ANY_DRY, .precipMaxMm = ANY_WET, .stem = "gray_rocks" },           // alpine/polar granite: the rock the snow caps sit on and the faces it slides off
		{ .kind = ESourceKind::Rock, .tempMinC = 0.0f,     .tempMaxC = 13.0f,   .precipMinMm = 1100.0f, .precipMaxMm = ANY_WET, .stem = "rock_pitted_mossy" },    // cool + wet: lichened, moss-pitted
		{ .kind = ESourceKind::Rock, .tempMinC = 0.0f,     .tempMaxC = 16.0f,   .precipMinMm = 250.0f,  .precipMaxMm = 1100.0f, .stem = "rock_3" },               // temperate: plain weathered stone
		{ .kind = ESourceKind::Rock, .tempMinC = 15.0f,    .tempMaxC = 24.0f,   .precipMinMm = 200.0f,  .precipMaxMm = 900.0f,  .stem = "worn_rock_natural_01" }, // warm + dry: wind-worn sandstone
		{ .kind = ESourceKind::Rock, .tempMinC = 23.0f,    .tempMaxC = ANY_HOT, .precipMinMm = ANY_DRY, .precipMaxMm = 450.0f,  .stem = "terrain_red_01" },       // hot + arid: oxidised red rock
		{ .kind = ESourceKind::Rock, .tempMinC = 15.0f,    .tempMaxC = ANY_HOT, .precipMinMm = 900.0f,  .precipMaxMm = ANY_WET, .stem = "dark_rock" },            // warm + wet: dark basalt

		// --- Overlays, in composite order. Both are climate-independent; their boxes are never read.
		{ .kind = ESourceKind::Beach, .stem = "coast_sand_01" },
		{ .kind = ESourceKind::Snow,  .stem = "snow_02" },
	};

	static_assert(oc::size(TERRAIN_TEX_SOURCES) <= RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS,
		"the splat table outgrew the UBO's climate array - raise MAX_TERRAIN_SPLAT_MATERIALS");

	constexpr const char* TERRAIN_TEX_CACHE_DIR = "Local/TerrainTex/";

	// Poly Haven layout; the two ambientCG sets override it per map.
	oc::string terrainTexSrcPath(const TerrainTexSource& src, const char* map)
	{
		const char* const overrides[] = { src.diffSrc, src.norSrc, src.armSrc };
		const int slot = map[0] == 'd' ? 0 : (map[0] == 'n' ? 1 : 2);
		if (overrides[slot])
			return overrides[slot];
		return oc::format("Textures/Terrain/{}/{}_{}_2k.jpg", src.stem, src.stem, map);
	}

	oc::string terrainTexCachePath(const TerrainTexSource& src, const char* map)
	{
		return oc::format("{}{}_{}.dds", TERRAIN_TEX_CACHE_DIR, src.stem, map);
	}

	// The entry's climate in REAL units, as the streamer stores it: (tempMinC, tempMaxC, precipMinMm,
	// precipMaxMm).
	glm::vec4 terrainClimateReal(const TerrainTexSource& src)
	{
		return glm::vec4(src.tempMinC, src.tempMaxC, src.precipMinMm, src.precipMaxMm);
	}

	// Real climate -> the normalized (t01, h01) box the shader tests against. Precipitation converts
	// through the GENERATOR'S live mm-per-full-humidity scale rather than the shared constant: V3 exposes
	// that as a tweak, and if the two ever drift the whole table slides along the humidity axis while
	// still looking perfectly reasonable in this file.
	glm::vec4 terrainClimateBox(const glm::vec4& real, float precipFullMm)
	{
		const float invFull = 1.0f / glm::max(precipFullMm, 1.0f);
		return glm::vec4(Procedural::temperatureTo01(real.x), Procedural::temperatureTo01(real.y),
		                 glm::clamp(real.z * invFull, 0.0f, 1.0f),
		                 glm::clamp(real.w * invFull, 0.0f, 1.0f));
	}

	// True when outPath exists and is newer than its source (a missing source doesn't invalidate a bake).
	bool terrainTexCacheFresh(const oc::string& outPath, const oc::string& source)
	{
		// Runs on the startup bake thread (never the main thread).
		const int64 outTime = FileSystem::lastWriteTimeSec(outPath);
		if (outTime == 0)
			return false;
		const int64 srcTime = FileSystem::lastWriteTimeSec(source);
		return srcTime == 0 || srcTime <= outTime;
	}

	// Bakes every stale terrain texture into the DDS cache. Runs on a background jthread at startup;
	// one conversion is seconds of CPU (mips + BC compression), so stale entries fan out over a small
	// thread pool and the stop token is checked between files (app shutdown mid-first-bake).
	void bakeTerrainTexCache(const oc::atomic<bool>& stopRequested)
	{
		FileSystem::createDirectories(TERRAIN_TEX_CACHE_DIR);

		struct BakeTask { const TerrainTexSource* src; int map; }; // map: 0 = diff, 1 = nor, 2 = arm
		oc::vector<BakeTask> tasks;
		for (const TerrainTexSource& src : TERRAIN_TEX_SOURCES)
		{
			if (!terrainTexCacheFresh(terrainTexCachePath(src, "diff"), terrainTexSrcPath(src, "diff")))
				tasks.push_back({ &src, 0 });
			if (!terrainTexCacheFresh(terrainTexCachePath(src, "nor"), terrainTexSrcPath(src, "nor_gl")))
				tasks.push_back({ &src, 1 });
			if (!terrainTexCacheFresh(terrainTexCachePath(src, "arm"), terrainTexSrcPath(src, "arm")))
				tasks.push_back({ &src, 2 });
		}
		if (tasks.empty())
			return;

		const auto bakeStart = std::chrono::steady_clock::now();
		// grain 1: each conversion is a whole image load + BC compress, seconds apart in cost
		Globals::jobSystem.parallelFor(0, (uint32)tasks.size(), 1, { "Terrain tex bake", EProfileCategory::Procedural },
			[&](uint32 begin, uint32 end)
		{
			for (uint32 i = begin; i < end; ++i)
			{
				if (stopRequested.load(oc::memory_order_relaxed))
					return;
				const TerrainTexSource& src = *tasks[i].src;
				bool ok = false;
				switch (tasks[i].map)
				{
				case 0: ok = TextureConvert::convertToDds(terrainTexSrcPath(src, "diff").c_str(), TextureConvert::EUsage::Color, terrainTexCachePath(src, "diff").c_str()); break;
				case 1: ok = TextureConvert::convertToDds(terrainTexSrcPath(src, "nor_gl").c_str(), TextureConvert::EUsage::NormalMap, terrainTexCachePath(src, "nor").c_str()); break;
				case 2: ok = TextureConvert::convertToDds(terrainTexSrcPath(src, "arm").c_str(), TextureConvert::EUsage::Data, terrainTexCachePath(src, "arm").c_str()); break;
				}
				if (!ok)
					Log::warning(oc::format("Terrain: failed to bake splat texture '{}' map {}", src.stem, tasks[i].map));
			}
		}, EJobPriority::Low);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bakeStart).count();
		Log::info(oc::format("Terrain: baked {} splat textures into {} in {} ms", tasks.size(), TERRAIN_TEX_CACHE_DIR, ms));
	}
}

namespace Procedural
{
	TerrainStreamer::~TerrainStreamer()
	{
		{
			// starve the pump: it exits once the pool is empty, and nothing re-kicks it
			std::lock_guard<std::mutex> lk(m_mutex);
			m_requests.clear();
		}
		m_texBakeStop.store(true, oc::memory_order_relaxed);
		Globals::jobSystem.wait(m_pumpCounter);   // main thread helps while waiting
		Globals::jobSystem.wait(m_texBakeCounter);
		clearResidents(); // main thread: free RenderNodes/containers while the renderer is still alive
	}

	void TerrainStreamer::initialize()
	{
		auto dirty = [this]() { m_configDirty = true; };

		// Dirty so rebuildMaps runs on toggle: enabling is what kicks the V3 model load (disabled terrain
		// never loads the 2.28 GB of models onto the GPU).
		Tweak::boolean("Terrain", "Enabled", &m_enabled, dirty);
		Tweak::intVar("Terrain", "Seed", &m_seed, 0, 1000000, 1.0f, dirty);
		Tweak::intVar("Terrain", "Chunk size (m)", &m_chunkSize, 128, 1024, 1.0f, dirty);
		Tweak::intVar("Terrain", "LOD0 resolution", &m_lod0Res, 128, 1024, 1.0f, dirty);
		Tweak::intVar("Terrain", "Range (chunks)", &m_ringRadius, 1, 64, 1.0f); // max generation radius from the camera chunk
		Tweak::floatVar("Terrain", "LOD step (chunks)", &m_lodStep, 0.1f, 4.0f, 0.1f); // LOD0 band width; each next band doubles
		Tweak::floatVar("Terrain", "Full-res distance (chunks)", &m_fullResDist, 0.0f, 8.0f, 0.05f); // edge distance forced to LOD0 before bands begin
		Tweak::intVar("Terrain", "Max LOD", &m_maxLod, 0, 6, 1.0f);
		Tweak::intVar("Terrain", "Uploads/frame", &m_maxUploadsPerFrame, 1, 32, 1.0f);
		// Concurrent generation jobs: >1 lets warm-tile mesh builds overlap a cold V3 tile wait
		// (inference itself still serializes on the pipeline lock).
		Tweak::intVar("Terrain", "Gen jobs", &m_maxGenJobs, 1, 16, 1.0f);
		Tweak::floatVar("Terrain", "Sea level (m)", &m_seaLevel, -200.0f, 200.0f, 0.5f, dirty);
		Tweak::floatVar("Terrain", "Skirt depth (m)", &m_skirtDepth, 0.0f, 64.0f, 0.5f, dirty);
		// ONE shared baked terrain-data map (height, water level, fog|falloff|temp|hum, altitude): the volumetric
		// fog's terrain follow + regional thickness, the ocean's far shore fallback, and the terrain
		// coloring all read these cascades. Disabling it degrades all three.
		Tweak::boolean("Terrain", "Terrain data map", &m_terrainMapEnabled);
		// Diagnostic: prints what each baked cascade actually contains, decoded the way the shader
		// decodes it. The bake is otherwise invisible — a wrong sampler, pack or upload all look
		// the same from the shader side.
		Tweak::boolean("Terrain", "Log baked data map", &m_terrainMapDebugLog);
		// The ocean runs swash onto anything within ~1 m of the water level, and V3 reports sea level
		// EVERYWHERE (it models no lakes), so inland ground that happens to sit at 0..1 m reads as beach and
		// gets waves. These sink the baked water level well below any ground the ocean cannot reach, which
		// the swash's existing landlocked-water gate then takes care of; the beach texture overlay keys on
		// the same field, so inland sand goes with it. Off = the raw sampler water level.
		Tweak::boolean("Terrain", "Ocean reach limit", &m_waterReachEnabled);
		// How deep water must be before it counts as ocean at all. Guards the reach test against shallow
		// inland dips: one texel a hair under sea level would otherwise vouch for every hollow within the
		// reach radius of it. Raise it if puddles still rescue ground they should not.
		Tweak::floatVar("Terrain", "Ocean swash depth", &m_waterReach.swashDepth, 0.0f, 20.0f, 0.1f);
		Tweak::floatVar("Terrain", "Ocean reach (m)", &m_waterReach.radius, 0.0f, 100.0f, 0.1f);
		Tweak::floatVar("Terrain", "Ocean reach feather (m)", &m_waterReach.feather, 1.0f, 100.0f, 0.1f);
		Tweak::floatVar("Terrain", "Ocean reach drop (m)", &m_waterReach.drop, 0.0f, 50.0f, 0.5f);
		// Baked per-texel flow direction (the data map's 8 packed bits + the ocean shore map's B channel):
		// toward the nearest land through the surf zone — the waves' travel direction at the coast — and
		// downhill everywhere else (future rivers/water simulation). See applyFlowField for each knob's
		// role; changing one re-bakes both maps, like the reach settings above.
		Tweak::boolean("Terrain", "Flow direction", &m_flowFieldEnabled);
		Tweak::floatVar("Terrain", "Flow shore range (m)", &m_flowField.oceanRange, 0.0f, 2000.0f, 10.0f);
		Tweak::floatVar("Terrain", "Flow shore fade (m)", &m_flowField.oceanFade, 0.0f, 1000.0f, 10.0f);
		Tweak::floatVar("Terrain", "Flow smoothing (m)", &m_flowField.smoothRadius, 0.0f, 200.0f, 1.0f);
		Tweak::floatVar("Terrain", "Flow min slope", &m_flowField.minSlope, 0.0f, 0.5f, 0.005f);
		Tweak::floatVar("Terrain", "Data map range (m)", &m_terrainMapRange, 256.0f, 8192.0f, 32.0f);
		// Floor for the far cascade world size: the actual range is raised to cover the resident mesh ring
		// (2*(R+1)*chunkSize) so distant terrain never reads clamp-to-edge frozen altitude/temperature.
		Tweak::floatVar("Terrain", "Data map far range (m)", &m_terrainMapFarRange, 1024.0f, 65536.0f, 256.0f);

		// Terrain texture splatting (TERRAIN pipeline variant; pushed to the renderer every frame from
		// updateTerrainTextures via Renderer::setTerrainTextureParams). The surface composites bottom-up
		// as ground -> beach -> rock -> snow; these shape where each layer takes over.
		Tweak::floatVar("Terrain/Textures", "Ground uv scale (1/m)", &m_texUvScaleGround, 0.005f, 2.0f);
		Tweak::floatVar("Terrain/Textures", "Rock uv scale (1/m)", &m_texUvScaleRock, 0.005f, 2.0f);
		Tweak::floatVar("Terrain/Textures", "Snow uv scale (1/m)", &m_texUvScaleSnow, 0.005f, 2.0f);
		// How far outside its climate box an entry still competes. Low = crisp climate borders.
		Tweak::floatVar("Terrain/Textures", "Climate blend", &m_texClimateBlend, 0.01f, 0.5f, 0.005f);
		// Slope = 1 - N.y, so 0.30 = 45 deg, 0.55 = 63. NOTE for V3: the diffusion field is 30 m/px and
		// barely reaches 45 deg on its own, so most rock arrives via the crag test below, not this one.
		Tweak::floatVar("Terrain/Textures", "Slope rock start", &m_texSlopeRockStart, 0.0f, 1.0f);
		Tweak::floatVar("Terrain/Textures", "Slope rock full", &m_texSlopeRockFull, 0.0f, 1.0f);
		// Relief above the MACRO altitude: what separates a mountain from flat ground that merely sits
		// high. This is the main rock control under V3.
		// V3's macro altitude is the coarse stage's 7.68 km surface — nearly flat across one mountain — so
		// crag relief is essentially (height - constant) and the rock boundary traces an ELEVATION CONTOUR:
		// grass gives way to stone at one height right across a range. This wanders it up and down.
		// Scaled by the local relief in the shader, so it cannot rock a plain however high it is set; safe
		// to push. 0 = off (contour lines). Both are metres at the model's true scale, like the thresholds
		// below, so they hold their look across "Meters per pixel".
		Tweak::floatVar("Terrain/Textures", "Crag wander (m)", &m_texCragWanderAmp, 0.0f, 600.0f, 5.0f);
		Tweak::floatVar("Terrain/Textures", "Crag wander wavelength (m)", &m_texCragWanderWavelength, 200.0f, 20000.0f, 100.0f);
		// Beyond this view distance the splat layers fetch albedo only (no normal/ARM taps, geometric
		// normal shading) — the minified detail mips carry no visible signal there. 0 = never simplify.
		Tweak::floatVar("Terrain/Textures", "Crag relief start (m)", &m_texCragStart, 0.0f, 200.0f);
		Tweak::floatVar("Terrain/Textures", "Crag relief full (m)", &m_texCragFull, 0.0f, 400.0f);
		Tweak::floatVar("Terrain/Textures", "Beach band (m)", &m_texBeachBand, 0.0f, 20.0f);
		// Snow cover. Temperature sets the snow LINE (the generator's lapse rate is what makes it follow
		// the mountains); slope decides how much of the mountain's own rock shows through it.
		Tweak::floatVar("Terrain/Textures", "Snow temp full (C)", &m_texSnowTempFull, -30.0f, 20.0f, 0.5f);
		Tweak::floatVar("Terrain/Textures", "Snow temp none (C)", &m_texSnowTempNone, -30.0f, 20.0f, 0.5f);
		Tweak::floatVar("Terrain/Textures", "Snow slope start", &m_texSnowSlopeStart, 0.0f, 1.0f);
		Tweak::floatVar("Terrain/Textures", "Snow slope full", &m_texSnowSlopeFull, 0.0f, 1.0f);
		Tweak::floatVar("Terrain/Textures", "Snow min humidity", &m_texSnowAridity, 0.0f, 0.5f, 0.005f);


		// V3 (Terrain Diffusion). The model resolves 30 m/px, which is what makes its continents
		// continent-sized; everything below that is the slope-masked detail layer below.
		// "Meters per pixel" is a UNIFORM world scale: heights and detail shrink with it, so the model's
		// proportions survive and "Height scale" stays a pure exaggeration on top (1 = real proportions).
		// NOTE ON COST: lowering it compresses the world (mountains become reachable sooner) but is
		// quadratically MORE expensive — the same view distance then spans more model pixels, so more tiles
		// must be generated. 30 -> 15 is ~4x the inference work for the same ring radius.
		Tweak::floatVar("Terrain/V3", "Meters per pixel", &m_v3MetersPerPixel, 0.05f, 30.0f, 0.05f, dirty); // 30 = true scale
		Tweak::floatVar("Terrain/V3", "Height scale", &m_v3HeightScale, 0.0f, 4.0f, 0.05f, dirty);
		// The model's climate is real-world calibrated and the biome attractors are expressed in real units
		// to match, so both of these default to 0 (= the model's own climate). "Extra lapse" pushes the
		// snow line further down the mountains than reality; "Temp offset" makes the whole planet warmer or
		// colder.
		Tweak::floatVar("Terrain/V3", "Temp offset (C)", &m_v3TemperatureOffset, -30.0f, 30.0f, 0.5f, dirty);
		// THE snow-line dial: how fast it cools with altitude, C per MODEL metre (so it rides "Meters per
		// pixel" like every other vertical quantity). More negative = snow lower down the mountains.
		// One number for the world, not the model's own per-region rate. That was baked into the terrain-data
		// map and measured as buying nothing: both cascades regress the same data, so their rates agreed and
		// dropping it left near-vs-far disagreement unchanged at 0.15 C max. It only bought fidelity to the
		// model's absolute temperature, which nothing consumes. -0.008 is the mean of what it regressed
		// (Earth's environmental lapse is ~-0.0065).
		Tweak::floatVar("Terrain/V3", "Lapse rate (C/m)", &m_v3LapseRate, -0.03f, 0.0f, 0.0005f, dirty);
		// The model's temperature is a function of ELEVATION, so every climate boundary it draws is an
		// isotherm and therefore a contour line: grass gives way to rock at one height right across a range,
		// and the snow line is a perfect ring. This wanders the temperature so those boundaries ride up and
		// down instead. In degrees, so it breaks up the ground transitions and the snow line with one field,
		// and it goes into the climate the scatter system reads too — trees keep marking the treeline the
		// textures draw. 0 = the model's own banded climate.
		// Roughly: at the model's lapse (~0.005 C/m in model metres) 1 C of wander moves a boundary ~200
		// model metres up or down.
		Tweak::floatVar("Terrain/V3", "Climate wander (C)", &m_v3ClimateNoiseC, 0.0f, 8.0f, 0.1f, dirty);
		// Model metres, like the detail wavelengths. Floored well above the far terrain-data cascade's texel
		// (~16 m world): this is BAKED, so a short wavelength aliases and near/far stop agreeing.
		Tweak::floatVar("Terrain/V3", "Climate wander wavelength (m)", &m_v3ClimateNoiseWavelength, 500.0f, 20000.0f, 100.0f, dirty);
		Tweak::intVar("Terrain/V3", "Climate wander octaves", &m_v3ClimateNoiseOctaves, 1, 6, 1.0f, dirty);
		Tweak::floatVar("Terrain/V3", "Detail slope gain", &m_v3DetailSlopeGain, 0.0f, 4.0f, 0.05f, dirty); // higher = detail on gentler slopes
		// THE resolution dial. The model resolves 30 m/px and nothing will make it resolve less, so every
		// feature below that is these two fBm layers — and how far down they reach is set by the OCTAVE
		// count, not the wavelength: finest feature = wavelength / 2^(octaves-1), in MODEL metres, then
		// scaled by metersPerPixel/30 like everything else.
		//
		// At the defaults (A 220 m/4, B 45 m/3) the finest thing in the field is 45/4 = 11 model metres.
		// That is why lowering "Meters per pixel" looks like it adds detail: at mpp=3 those 11 model metres
		// become 1.1 world metres, which finally matches the 1 m vertex grid (chunk size / LOD0 res). It is
		// not generating more — it is shrinking the world until the existing detail reaches the vertices.
		// To get the same crispness at FULL scale (mpp=30), raise the octaves instead:
		//     B 3 -> 5   finest 11.25 m -> 2.81 m
		//     A 4 -> 6   finest 27.50 m -> 6.88 m
		// Do not chase the last factor of two: below ~2 m the detail is finer than the 1 m vertex grid can
		// represent and simply aliases into shimmer. fbm() is amplitude-normalised, so extra octaves add
		// roughness without inflating the relief, and each one is a single noise lookup against a diffusion
		// tile resolve — the cost is not measurable next to inference.
		Tweak::floatVar("Terrain/V3", "Detail A wavelength (m)", &m_v3DetailWavelengthA, 20.0f, 1000.0f, 5.0f, dirty);
		Tweak::floatVar("Terrain/V3", "Detail A amp (m)", &m_v3DetailAmplitudeA, 0.0f, 200.0f, 1.0f, dirty);
		Tweak::intVar("Terrain/V3", "Detail A octaves", &m_v3DetailOctavesA, 1, 8, 1.0f, dirty);
		Tweak::floatVar("Terrain/V3", "Detail B wavelength (m)", &m_v3DetailWavelengthB, 5.0f, 300.0f, 1.0f, dirty);
		Tweak::floatVar("Terrain/V3", "Detail B amp (m)", &m_v3DetailAmplitudeB, 0.0f, 80.0f, 0.5f, dirty);
		Tweak::intVar("Terrain/V3", "Detail B octaves", &m_v3DetailOctavesB, 1, 8, 1.0f, dirty);
		Tweak::floatVar("Terrain/V3", "Precip for full humidity", &m_v3PrecipFullHumidity, 200.0f, 6000.0f, 50.0f, dirty);
		Tweak::floatVar("Terrain/V3", "Humidity offset", &m_v3HumidityOffset, -1.0f, 1.0f, 0.01f, dirty);
		Tweak::floatVar("Terrain/V3", "Humid fog", &m_v3HumidFog, 0.0f, 1.0f, 0.01f, dirty);
		Tweak::floatVar("Terrain/V3", "Valley fog", &m_v3ValleyFog, 0.0f, 1.0f, 0.01f, dirty);
		Tweak::intVar("Terrain/V3", "Resident tiles", &m_v3MaxTiles, 4, 256, 1.0f, dirty); // ~800 KB each
		// Half-precision inference. This does NOT generate terrain faster: measured at mpp=3, the near
		// cascade is unchanged (4.03 s -> 4.02 s) and the coarse/far path is ~15% SLOWER (1.12 s -> 1.30 s).
		// The pipeline is dispatch-bound, not compute-bound, so halving the FLOPs buys nothing and the
		// boundary Casts cost a little. What it does buy: model VRAM drops ~2.28 GB -> ~1.1 GB (the
		// renderer wants that back) and the models load ~1.8x faster (2.14 s -> 1.19 s for base).
		// Needs the optional fp16 models (Tools/convert_models_fp16.py); without them this does nothing and
		// the log says so. Flipping it RELOADS the weights and regenerates the world - fp16 is not
		// bit-identical, so the same seed grows visibly different fine detail.
		Tweak::boolean("Terrain/V3", "FP16 inference", &m_v3Fp16, dirty);

		rebuildMaps();

		// Bake the biome splat textures to the DDS cache in the background (no-op when fresh); chunks
		// render with the flat-color fallback until updateTerrainTextures registers the finished set.
		Globals::jobSystem.submit([this]
		{
			bakeTerrainTexCache(m_texBakeStop);
			m_texBakeDone.store(!m_texBakeStop.load(oc::memory_order_relaxed), oc::memory_order_release);
		}, { "TerrainStreamer::bakeTerrainTexCache", EProfileCategory::Procedural }, EJobPriority::Low, &m_texBakeCounter);
	}

	void TerrainStreamer::updateTerrainTextures(Renderer& renderer)
	{
		// The crag thresholds are the ONE shader parameter denominated in the same units the generator's
		// elevation is, so they alone have to follow V3's uniform world scale. Crag relief is a height
		// difference, so it shrinks with "Meters per pixel" exactly like everything else: the same mountain
		// measures ~15 m of relief at mpp=30 and ~1.5 m at mpp=3. Left absolute, a 12 m start is unreachable
		// below mpp≈25 and the near field simply never grows rock (measured: mean crag 1.5 m, rock weight
		// 0.019 at mpp=3). Scaling here keeps the tweak meaning "metres at the model's true scale", which is
		// the same convention the detail wavelengths already use.
		const float cragScale = TerrainGenV3::worldScale(m_v3MetersPerPixel);
		renderer.setTerrainTextureParams({
			.uvScaleGround = m_texUvScaleGround,
			.uvScaleRock = m_texUvScaleRock,
			.uvScaleSnow = m_texUvScaleSnow,
			.climateBlend = m_texClimateBlend,
			.slopeRockStart = m_texSlopeRockStart,
			.slopeRockFull = m_texSlopeRockFull,
			.cragStart = m_texCragStart * cragScale,
			.cragFull = m_texCragFull * cragScale,
			.beachBand = m_texBeachBand,
			.snowTempFull = m_texSnowTempFull,
			.snowTempNone = m_texSnowTempNone,
			.snowSlopeStart = m_texSnowSlopeStart,
			.snowSlopeFull = m_texSnowSlopeFull,
			.snowAridity = m_texSnowAridity,
			.cragWanderAmp = m_texCragWanderAmp * cragScale,
			.cragWanderWavelength = m_texCragWanderWavelength * cragScale,
		});

		if (!m_texSetRegistered)
		{
			if (!m_texBakeDone.load(oc::memory_order_acquire))
				return; // still baking: chunks draw with the flat-color fallback
			registerTerrainTextures(renderer);
			if (!m_texSetRegistered)
				return; // nothing usable baked; the warnings above say what
		}

		// Live: the boxes are authored in mm/yr and divided by the generator's precipitation scale, which
		// is a tweak. Pushing them every frame is a memcpy of two dozen vec4s and means the table can never
		// be reading a scale the generator has already moved off.
		const float precipFullMm = m_v3PrecipFullHumidity;
		for (size_t i = 0; i < m_texClimateReal.size(); ++i)
			m_texClimateBoxes[i] = terrainClimateBox(m_texClimateReal[i], precipFullMm);
		renderer.setTerrainSplatClimate(m_texClimateBoxes);
	}

	// Hands the baked DDS set to the renderer as [ground][rock][beach?][snow?], which is the order the
	// shader composites it in (Renderer::setTerrainSplatMaterials). Built in four passes over the table
	// rather than trusting its declaration order, so entries stay grouped however reads best there.
	void TerrainStreamer::registerTerrainTextures(Renderer& renderer)
	{
		auto tryBuildMat = [](const TerrainTexSource& src) -> oc::optional<Renderer::TerrainSplatMaterial>
		{
			Renderer::TerrainSplatMaterial mat;
			mat.diffuseDds = terrainTexCachePath(src, "diff");
			mat.normalDds = terrainTexCachePath(src, "nor");
			mat.armDds = terrainTexCachePath(src, "arm");
			if (!FileSystem::exists(mat.diffuseDds, /*allowMainThread*/ true) || !FileSystem::exists(mat.normalDds, true)
				|| !FileSystem::exists(mat.armDds, true))
			{
				// Source images missing / bake failed: drop the entry. A ground or rock entry just leaves
				// its climate to the neighbouring boxes; a missing beach or snow entry disables that
				// overlay outright, which is worth saying out loud — a world with no snow layer looks like
				// a climate bug rather than a missing file.
				Log::warning(oc::format("Terrain: splat set '{}' incomplete, skipping", src.stem));
				return oc::nullopt;
			}
			return mat;
		};

		oc::vector<Renderer::TerrainSplatMaterial> mats;
		Renderer::TerrainSplatCounts counts;
		m_texClimateReal.clear();
		const auto append = [&](ESourceKind kind, uint32* count)
		{
			for (const TerrainTexSource& src : TERRAIN_TEX_SOURCES)
			{
				if (src.kind != kind)
					continue;
				if (auto mat = tryBuildMat(src))
				{
					mats.push_back(oc::move(*mat));
					// Parallel to mats, INCLUDING the overlays (whose climate is never read): the shader
					// indexes climate boxes by material slot, so a skipped entry has to drop out of both
					// or every box after it describes the wrong texture.
					m_texClimateReal.push_back(terrainClimateReal(src));
					if (count)
						(*count)++;
				}
			}
		};
		append(ESourceKind::Ground, &counts.numGround);
		append(ESourceKind::Rock, &counts.numRock);
		const size_t beforeBeach = mats.size();
		append(ESourceKind::Beach, nullptr);
		counts.hasBeach = mats.size() > beforeBeach;
		const size_t beforeSnow = mats.size();
		append(ESourceKind::Snow, nullptr);
		counts.hasSnow = mats.size() > beforeSnow;

		if (counts.numGround == 0)
		{
			Log::error("Terrain: no ground splat textures baked - terrain stays flat-shaded");
			return;
		}
		m_texClimateBoxes.assign(mats.size(), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
		renderer.setTerrainSplatMaterials(mats, counts);
		m_texSetRegistered = true;
	}

	void TerrainStreamer::rebuildMaps()
	{
		// Disabled = no generator and, crucially, NO MODEL KICK: constructing TerrainGenV3 (even the throwaway
		// "kick" below) is what starts the 2.28 GB GPU load, so a disabled terrain must never reach it. The
		// Enabled tweak is registered dirty, so flipping it on lands back here and starts the load then.
		// There is no unload API — models loaded during an earlier enabled stretch stay resident.
		if (!m_enabled)
		{
			m_v3AwaitingModels = false;
			std::lock_guard<std::mutex> lk(m_mutex);
			m_maps = nullptr;
			++m_generation;
			m_requests.clear();
			return;
		}

		// The generator can't sample anything until its models are resident. Constructing it is what kicks
		// the download/load off, so do that unconditionally, but only PUBLISH it once ready — otherwise
		// every chunk built in the meantime would bake a flat sea-level world into the resident cache and
		// never be revisited.
		TerrainConfigV3 cfg;
		cfg.seed = (uint32)m_seed;
		cfg.seaLevel = m_seaLevel;
		cfg.metersPerPixel = m_v3MetersPerPixel;
		cfg.heightScale = m_v3HeightScale;
		cfg.detailSlopeGain = m_v3DetailSlopeGain;
		cfg.detailWavelengthA = m_v3DetailWavelengthA;
		cfg.detailAmplitudeA = m_v3DetailAmplitudeA;
		cfg.detailOctavesA = (uint32)glm::max(1, m_v3DetailOctavesA);
		cfg.detailWavelengthB = m_v3DetailWavelengthB;
		cfg.detailAmplitudeB = m_v3DetailAmplitudeB;
		cfg.detailOctavesB = (uint32)glm::max(1, m_v3DetailOctavesB);
		cfg.precipForFullHumidity = m_v3PrecipFullHumidity;
		cfg.humidityOffset = m_v3HumidityOffset;
		cfg.temperatureOffset = m_v3TemperatureOffset;
		cfg.lapseRate = m_v3LapseRate;
		cfg.climateNoiseAmplitudeC = m_v3ClimateNoiseC;
		cfg.climateNoiseWavelength = m_v3ClimateNoiseWavelength;
		cfg.climateNoiseOctaves = (uint32)glm::max(1, m_v3ClimateNoiseOctaves);
		cfg.humidFogAmount = m_v3HumidFog;
		cfg.valleyFogAmount = m_v3ValleyFog;
		cfg.maxResidentTiles = m_v3MaxTiles;
		cfg.useFp16 = m_v3Fp16;

		// BEFORE the isReady() check, not inside the generator: switching precision reloads the models
		// and drops isReady(), so a check taken before it would publish a generator whose pipeline is
		// being torn down underneath it — and every chunk built meanwhile would bake a flat sea-level
		// world into the resident cache and never be revisited.
		TerrainGenV3::setPrecision(m_v3Fp16);

		oc::shared_ptr<const ITerrainSampler> maps;
		if (TerrainGenV3::isReady())
		{
			maps = oc::make_shared<const TerrainGenV3>(cfg);
			m_v3AwaitingModels = false;
		}
		else
		{
			// Cheap: the ctor only registers interest and starts the background load.
			const TerrainGenV3 kick(cfg);
			m_v3AwaitingModels = !TerrainGenV3::hasFailed();
			if (TerrainGenV3::hasFailed())
				Log::error("[Terrain] unavailable (model load failed) - staying on an empty world");
		}

		std::lock_guard<std::mutex> lk(m_mutex);
		m_maps = oc::move(maps);
		++m_generation;
		m_requests.clear(); // drop queued work built against the old config
	}

	oc::shared_ptr<const ITerrainSampler> TerrainStreamer::currentMaps()
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		return m_maps;
	}

	void TerrainStreamer::clearResidents()
	{
		m_residents.clear();
		m_pending.clear();
		m_readyBacklog.clear();
		m_ringScanNeeded = true;
	}

	void TerrainStreamer::kickPump(size_t numNew)
	{
		// Top the pump pool up to min(cap, active + new work): each CAS claims one slot. Spawning
		// more pumps than requests is impossible this way; a pump finding the pool already drained
		// just exits through its recheck.
		const int32 cap = glm::clamp(m_maxGenJobs, 1, 16);
		for (size_t spawned = 0; spawned < numNew; )
		{
			int32 cur = m_numPumps.load(oc::memory_order_relaxed);
			if (cur >= cap)
				return;
			if (m_numPumps.compare_exchange_weak(cur, cur + 1, oc::memory_order_acq_rel))
			{
				Globals::jobSystem.submit([this] { pumpJob(); }, { "terrainChunkPump", EProfileCategory::Procedural }, EJobPriority::Low, &m_pumpCounter);
				++spawned;
			}
		}
	}

	void TerrainStreamer::pumpJob()
	{
		for (;;)
		{
			ProfileScope profileScope("TerrainStreamer::pumpJob", EProfileCategory::Procedural);

			Request req;
			bool haveWork = false;
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				// Take the request NEAREST THE CAMERA, re-measured against the ring state as it is RIGHT NOW.
				// Deliberately not a queue: FIFO order is enqueue TIME, which stops matching distance the
				// moment the camera moves. Chunks arrive over many frames, and a chunk whose LOD band
				// changes has its queued request invalidated and re-appended — landing BEHIND the far
				// leading-edge chunks queued on earlier frames. The ground under the camera then arrives
				// after the horizon does, which at V3's seconds-per-chunk is impossible to miss.
				// Sorting each batch on the way in cannot fix that: it only orders WITHIN a batch, and the
				// camera has moved by the time the next one is picked up. Only choosing at dequeue, against
				// the live camera, is order-independent.
				//
				// Lazy staleness rides along in the same pass: requests that fell out of the ring are
				// dropped HERE — the main thread never rewrites the queue — and bounce back as an EMPTY
				// result so it releases its pending key (m_pending is main-thread-owned).
				//
				// O(queue) per generated chunk. The queue is a few hundred entries and a chunk costs
				// milliseconds to seconds, so the scan does not register.
				size_t best = SIZE_MAX;
				int64 bestDist2 = INT64_MAX;
				size_t keep = 0;
				for (size_t i = 0; i < m_requests.size(); ++i)
				{
					Request& r = m_requests[i];
					const glm::ivec2 d = r.params.coord - m_ringCam;
					const int cheb = glm::max(glm::abs(d.x), glm::abs(d.y));
					if (cheb > m_ringR || r.params.lod != ringLodAt(chunkEdgeDist(m_ringCamPos, r.params.coord), m_ringFullRes, m_ringLodStep, m_ringMaxLod))
					{
						Result drop;
						drop.key = r.key;
						drop.generation = r.generation;
						drop.coord = r.params.coord;
						drop.lod = r.params.lod;
						m_results.push_back(oc::move(drop)); // empty mesh = dropped
						continue;                             // not carried into the kept prefix
					}
					// Euclidean, not the ring's Chebyshev: "nearest" should mean nearest, not same-ring.
					const int64 dist2 = (int64)d.x * d.x + (int64)d.y * d.y;
					if (dist2 < bestDist2)
					{
						bestDist2 = dist2;
						best = keep;
					}
					if (keep != i)
						m_requests[keep] = oc::move(r);
					++keep;
				}
				m_requests.resize(keep);
				if (best != SIZE_MAX)
				{
					// Queue ORDER carries no meaning now (every dequeue rescans the lot), so the winner can
					// come out in O(1) by swapping it to the front rather than erasing from the middle.
					if (best != 0)
						oc::swap(m_requests[best], m_requests[0]);
					req = oc::move(m_requests.front());
					m_requests.pop_front();
					haveWork = true;
				}
			}
			if (!haveWork)
			{
				// The pool drained (or held only stale entries, dropped above). Release the slot,
				// then re-check: an append racing between our scan and the release either sees the
				// slot still held (we re-claim below) or its kick spawns a fresh pump itself.
				m_numPumps.fetch_sub(1, oc::memory_order_release);
				{
					std::lock_guard<std::mutex> lk(m_mutex);
					if (m_requests.empty())
						return;
				}
				const int32 cap = glm::clamp(m_maxGenJobs, 1, 16);
				int32 cur = m_numPumps.load(oc::memory_order_relaxed);
				for (;;)
				{
					if (cur >= cap)
						return; // an appender's kicks refilled the pool; those jobs take over
					if (m_numPumps.compare_exchange_weak(cur, cur + 1, oc::memory_order_acq_rel))
						break;
				}
				continue;
			}

			Result res;
			res.key = req.key;
			res.generation = req.generation;
			res.coord = req.params.coord;
			res.lod = req.params.lod;
			if (req.maps)
			{
				// generateChunk can fiber-park inside (V3 sampler JobMutex / per-tile JobEvent) - safe
				// to scope since the JobSystem migrates the open-scope stack with the fiber; the
				// timeline shows the on-thread segments, parked time is not counted.
				ProfileScope genScope("TerrainStreamer::generateChunk", EProfileCategory::Procedural);
				TerrainChunkMesh mesh;
				generateChunk(*req.maps, req.params, mesh);
				if (!mesh.indices.empty())
				{
					ProfileScope meshScope("TerrainStreamer::createMeshScene", EProfileCategory::Procedural); // pure, never parks
					MeshGeometryDesc geom;
					geom.positions = mesh.positions.data();
					geom.normals = mesh.normals.data();
					geom.tangents = mesh.tangents.data();
					geom.bitangents = mesh.bitangents.data();
					geom.texCoords = mesh.texCoords.data();
					geom.numVertices = (uint32)mesh.positions.size();
					geom.indices = mesh.indices.data();
					geom.numIndices = (uint32)mesh.indices.size();
					geom.name = "TerrainChunk";
					res.scene = ISceneData::createMeshScene(geom); // copies; mesh dies here, halving what ships
				}
			}

			{
				std::lock_guard<std::mutex> lk(m_mutex);
				m_results.push_back(oc::move(res));
			}
		}
	}

	// Shared terrain-data map: FOG_TERRAIN_CASCADES camera-centered snapshots (HeightMapBaker, sampled
	// from the SAME sampler the chunks render from) — a near cascade over m_terrainMapRange meters at
	// fine texels and a far cascade over m_terrainMapFarRange at the same resolution (coarse texels are
	// fine at those distances, so long-range data costs no extra memory). Per texel: height, water level,
	// packed fog|falloff|temp|hum, macro altitude. Consumers: the volumetric fog's terrain follow + regional
	// thickness, the ocean's shore-map fallback, and the TERRAIN pipeline's coloring. (The renderer-side
	// API keeps the historical "FogTerrainHeightMap" name — fog was its first consumer.)
	void TerrainStreamer::updateFogHeightMap(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps, float farRange)
	{
		const bool active = m_terrainMapEnabled && maps != nullptr;
		HeightMapBaker::Baked baked;
		const WaterReach* reach = m_waterReachEnabled ? &m_waterReach : nullptr;
		if (m_terrainMapBaker.update(baked, active, maps, glm::vec2(camera.position.x, camera.position.z),
			glm::vec2(glm::max(m_terrainMapRange, 256.0f), farRange),
			RendererVKLayout::FOG_TERRAIN_RES, RendererVKLayout::FOG_TERRAIN_CASCADES, 4, // RGBA: height, water level, fog|flow|temp|hum, altitude
			reach, activeFlowField()))
		{
			// Decode what actually went into the packed climate channel, per cascade, exactly as
			// terrain_height.inc.glsl does. The bake is the one link in the chain nothing else can see: a
			// wrong sampler, a wrong pack and a wrong upload all look identical from the shader.
			if (m_terrainMapDebugLog)
			{
				const uint32 res = RendererVKLayout::FOG_TERRAIN_RES;
				const size_t perCascade = (size_t)res * res * 4;
				for (uint32 c = 0; c < RendererVKLayout::FOG_TERRAIN_CASCADES; c++)
				{
					float tMin = FLT_MAX, tMax = -FLT_MAX, hMin = FLT_MAX, hMax = -FLT_MAX;
					float yMin = FLT_MAX, yMax = -FLT_MAX, aMin = FLT_MAX, aMax = -FLT_MAX;
					for (size_t i = 0; i < perCascade; i += 4)
					{
						const float* t = baked.texels.data() + (size_t)c * perCascade + i;
						const uint32 packed = std::bit_cast<uint32>(t[2]);
						const float temp = float((packed >> 16) & 255u) * (75.0f / 255.0f) - 25.0f;
						const float hum = float((packed >> 24) & 255u) * (1.0f / 255.0f);
						tMin = glm::min(tMin, temp); tMax = glm::max(tMax, temp);
						hMin = glm::min(hMin, hum);  hMax = glm::max(hMax, hum);
						yMin = glm::min(yMin, t[0]); yMax = glm::max(yMax, t[0]);
						aMin = glm::min(aMin, t[3]); aMax = glm::max(aMax, t[3]);
					}
					Log::info(oc::format("[Terrain] baked cascade {} ({:.0f} m): height {:.0f}..{:.0f} | "
					                      "altitude {:.0f}..{:.0f} | temp {:.1f}..{:.1f} C | humidity {:.2f}..{:.2f}",
					                      c, baked.ranges[(int)c], yMin, yMax, aMin, aMax, tMin, tMax, hMin, hMax));
				}
			}
			renderer.setFogTerrainHeightMap(baked.texels, baked.center, baked.ranges, maps->seaLevel());
			// Keep the shipped texels as the CPU copy (activeTerrainData): the ocean samples them for
			// buoyancy and wind steering. A NEW object per bake — consumers' shared_ptrs stay coherent.
			m_terrainMapData = oc::make_shared<const BakedTerrainData>(BakedTerrainData{
				oc::move(baked.texels), baked.center, baked.ranges,
				RendererVKLayout::FOG_TERRAIN_RES, RendererVKLayout::FOG_TERRAIN_CASCADES });
			m_terrainMapUploaded = true;
		}
		if (!active && m_terrainMapUploaded)
		{
			renderer.clearFogTerrainHeightMap(); // fog reverts to the flat base; ocean/coloring lose their data
			m_terrainMapUploaded = false;
			m_terrainMapData = nullptr;
		}
	}

	void TerrainStreamer::update(Renderer& renderer, const Camera& camera)
	{
		ProfileScope profileScope("Terrain", EProfileCategory::Procedural);
		//m_terrainMapFarRange = camera.far;
		if (m_configDirty)
		{
			m_configDirty = false;
			rebuildMaps();
			clearResidents(); // geometry/climate changed: regenerate everything against the new config
		}

		// V3's 2.28 GB of models load on a background thread; poll for the handover. Until then m_maps is
		// null and nothing is generated, so the world stays empty rather than flat.
		if (m_v3AwaitingModels)
		{
			if (TerrainGenV3::isReady() || TerrainGenV3::hasFailed())
			{
				rebuildMaps();
				clearResidents();
			}
			else
			{
				// Rate-limited: loading 2.28 GB onto the GPU takes a few seconds.
				const double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
				if (now - m_v3LastStatusLog > 1.0)
				{
					m_v3LastStatusLog = now;
					Log::info(oc::format("[Terrain] V3: {}", TerrainGenV3::statusText()));
				}
			}
		}

		if (!m_enabled)
		{
			if (!m_residents.empty() || !m_pending.empty())
				clearResidents();
			updateFogHeightMap(renderer, camera, nullptr, m_terrainMapFarRange); // no terrain -> no terrain-following fog
			renderer.setTerrainParams(0.0f, m_seaLevel);   // no mesh up: disables the ocean land cull
			return;
		}

		updateTerrainTextures(renderer);

		const float chunkSize = (float)m_chunkSize;
		const int camCX = (int)std::floor(camera.position.x / chunkSize);
		const int camCZ = (int)std::floor(camera.position.z / chunkSize);
		const int R = glm::max(1, m_ringRadius);
		const float lodStep = glm::max(0.01f, m_lodStep);
		const float fullRes = glm::max(0.0f, m_fullResDist);
		const uint32 maxLod = (uint32)glm::max(0, m_maxLod);
		// Camera position in chunk units, SNAPPED to a quarter-chunk lattice: edge-distance LOD makes the
		// wanted set a function of the camera POSITION, not just its chunk, and the snap both bounds how
		// often the scan re-runs and gives the worker one exact value to judge staleness against.
		const glm::vec2 camChunks(std::floor(camera.position.x / chunkSize * 4.0f) * 0.25f,
		                          std::floor(camera.position.z / chunkSize * 4.0f) * 0.25f);

		// Chunk mesh coverage: chunks span +-R around the camera's chunk, so a disk of R*chunkSize around
		// the camera is guaranteed resident whatever its position within its own chunk — the fence for
		// the ocean's buried-under-land vertex cull. Also carries the live sea level (terrain coloring).
		oc::shared_ptr<const ITerrainSampler> maps;
		uint32 generation;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			maps = m_maps;
			generation = m_generation;
		}

		// The generator's lapse rate, converted to C per WORLD metre for the shaders (it publishes in its
		// own vertical frame). This is how terrainTemperatureAt turns the map's baked SEA-LEVEL baseline
		// into a temperature at any height, so it must be the rate the generator ACTUALLY used — hence read
		// from the sampler rather than from the tweak beside it. 0 with no terrain, and 0 from a generator
		// that folds its own lapse into the temperature it reports: there the baseline IS the temperature
		// and lapsing it again would double-count.
		float lapsePerWorldM = 0.0f;
		if (maps)
		{
			const float vertScale = TerrainGenV3::worldScale(m_v3MetersPerPixel) * glm::max(m_v3HeightScale, 0.01f);
			lapsePerWorldM = maps->lapseRatePerMetre() / glm::max(vertScale, 1e-4f);
		}
		renderer.setTerrainParams((float)R * chunkSize, m_seaLevel, lapsePerWorldM);

		// The far cascade must cover the whole resident mesh, else terrain past its edge reads clamp-to-edge
		// (frozen altitude/temperature -> distant inland looks uniformly cold/snowy, ignoring the inland
		// rise). The mesh reaches ~(R+1)*chunkSize from the camera on each axis; the map is a centered
		// square of side = range, so range must be at least twice that. Keep the tweak as a floor.
		const float meshHalfExtent = (float)(R + 1) * chunkSize;
		const float farRange = glm::max(m_terrainMapFarRange, 2.0f * meshHalfExtent);
		updateFogHeightMap(renderer, camera, maps, farRange);

		// Ring membership is CLOSED FORM: the column at Chebyshev distance cheb from the camera chunk
		// wants ringLodAt(cheb) (geometric bands), nothing outside R. Every "is this still wanted"
		// question below (result validation, eviction) is this arithmetic — no desired-key sets.
		const auto ringLod = [&](glm::ivec2 coord) -> int
		{
			const int cheb = glm::max(glm::abs(coord.x - camCX), glm::abs(coord.y - camCZ));
			return cheb <= R ? (int)ringLodAt(chunkEdgeDist(camChunks, coord), fullRes, lodStep, maxLod) : -1;
		};

		const bool ringMoved = camChunks != m_lastRingCamPos || camCX != m_lastRingCX || camCZ != m_lastRingCZ
			|| R != m_lastRingR || lodStep != m_lastRingLodStep || fullRes != m_lastRingFullRes || maxLod != m_lastRingMaxLod;

		// --- Enqueue any ring chunk that is neither resident nor already in flight. The scan is a
		// pure function of (ring, residents, pending), so it only re-runs when one of them changed.
		oc::vector<Request> newRequests;
		if (ringMoved || m_ringScanNeeded)
		{
			m_ringScanNeeded = false;
			for (int dz = -R; dz <= R; ++dz)
			{
				for (int dx = -R; dx <= R; ++dx)
				{
					const glm::ivec2 coord(camCX + dx, camCZ + dz);
					const uint32 lod = ringLodAt(chunkEdgeDist(camChunks, coord), fullRes, lodStep, maxLod);
					const uint64 key = chunkKey(coord, lod);
					if (m_residents.count(key) || m_pending.count(key))
						continue;

					Request req;
					req.key = key;
					req.generation = generation;
					req.maps = maps;
					req.params.coord = coord;
					req.params.lod = lod;
					req.params.chunkSize = chunkSize;
					req.params.lod0Res = (uint32)glm::max(1, m_lod0Res);
					req.params.skirtDepth = m_skirtDepth;
					newRequests.push_back(oc::move(req));
					m_pending.insert(key);
				}
			}
		}

		// The queue is an unordered POOL, not a queue: the worker rescans it on every dequeue and takes the
		// chunk nearest the camera at that moment (see workerLoop), and drops out-of-range entries in the
		// same pass. So the main thread neither sorts nor prunes — appending in any order is correct, and
		// sorting here would only be re-deciding, one camera position out of date, something the worker
		// decides properly a moment later.
		// What it DOES owe the worker is the ring state to judge against, published under the same lock.
		m_lastRingCX = camCX; m_lastRingCZ = camCZ;
		m_lastRingR = R; m_lastRingLodStep = lodStep; m_lastRingFullRes = fullRes; m_lastRingMaxLod = maxLod;
		m_lastRingCamPos = camChunks;
		if (ringMoved || !newRequests.empty())
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_ringCam = glm::ivec2(camCX, camCZ);
			m_ringCamPos = camChunks;
			m_ringR = R;
			m_ringLodStep = lodStep;
			m_ringFullRes = fullRes;
			m_ringMaxLod = maxLod;
			for (Request& r : newRequests)
				m_requests.push_back(oc::move(r));
		}
		if (!newRequests.empty())
			kickPump(newRequests.size());

		// --- Drain generated chunks (backlog first, then whatever the worker finished), budget-limited.
		oc::vector<Result> ready = oc::move(m_readyBacklog);
		m_readyBacklog.clear();
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			for (Result& r : m_results)
				ready.push_back(oc::move(r));
			m_results.clear();
		}

		int uploads = 0;
		for (Result& res : ready)
		{
			if (!res.scene) // pump-dropped (stale at dequeue) or generation failed: just release the key
			{
				m_pending.erase(res.key); // re-enters the ring as a fresh request if wanted again
				m_ringScanNeeded = true;
				continue;
			}
			const bool valid = (res.generation == generation) && (int)res.lod == ringLod(res.coord) && !m_residents.count(res.key);
			if (!valid)
			{
				m_pending.erase(res.key); // stale / no longer wanted / duplicate: done with it
				m_ringScanNeeded = true;
				continue;
			}
			if (uploads >= m_maxUploadsPerFrame)
			{
				m_readyBacklog.push_back(oc::move(res)); // stays "pending" so it isn't re-requested
				continue;
			}

			m_pending.erase(res.key);
			m_ringScanNeeded = true; // conservative: covers the failure continue below

			// Route the chunk onto the terrain pipeline variant (procedural height/slope albedo). Keep the
			// material's own texture indices (fallback) — terrain carries no textures.
			ObjectContainer::MaterialOverrides overrides;
			overrides.pipelineIdx = RendererVKLayout::EPipelineIndex::TerrainLit;
			overrides.useSceneTextures = true;
			// Chunks are already LOD'd by the streamer (resolution picked per ring distance), so the
			// renderer's per-chunk meshopt LOD chains are pure redundant churn: 5x the MeshInfos plus LOD
			// groups and BLAS-alias sharing, all recreated on every re-LOD. Disable them so each chunk is a
			// single mesh with one identity-aliased BLAS — far less churn through the mesh/RT free lists.
			overrides.disableGeneratedLods = true;
			auto container = oc::make_unique<ObjectContainer>();
			if (!container->initialize(*res.scene, &overrides))
				continue;

			const Transform transform(
				glm::vec3((float)res.coord.x * chunkSize, 0.0f, (float)res.coord.y * chunkSize),
				1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			RenderNode node = container->spawnRootNode(transform);

			Resident resident;
			resident.container = oc::move(container);
			resident.coord = res.coord;
			resident.lod = res.lod;
			resident.node = oc::move(node);
			// Culling registration: chunks live in the SpatialIndex like entity render components, but on
			// their own layer (no Entity* behind userData — gameplay queries must not see them),
			// registered ONCE (chunks never move, so they promote straight into the static tier), and
			// WITHOUT the spawn-visibility guard (chunks stream in off-screen constantly; the guard
			// would pin each one in the main pass until it first enters the frustum).
			const Sphere bounds = resident.node.getWorldBounds();
			resident.spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(
				glm::dvec3(bounds.pos), bounds.radius, 0ull, SpatialLayer_Terrain, false));
			m_residents.emplace(res.key, oc::move(resident));
			++uploads;
		}

		const SpatialCullingConfig& culling = Globals::spatialIndex.getCullingConfig();
		const bool gate = culling.mode >= int(ESpatialCullMode::Cull);

		// --- Evict residents, but keep a column's current chunk until its replacement is ready (no hole
		// while a new LOD streams in). Evict a resident only if its column left the ring entirely, or its
		// column now wants a different LOD AND that replacement chunk is resident AND can take over ON
		// SCREEN: a freshly registered replacement isn't main-stamped until the next markVisibleSet, so
		// evicting on residency alone opened a one-frame hole on every in-view LOD change (and while the
		// culling is FROZEN the replacement never gets stamped — the old chunk just stays).
		for (auto it = m_residents.begin(); it != m_residents.end(); )
		{
			const Resident& res = it->second;
			const int want = ringLod(res.coord);
			bool evict;
			if (want < 0)
				evict = true; // column outside the ring
			else if ((uint32)want == res.lod)
				evict = false; // this is the wanted LOD
			else
			{
				const auto repIt = m_residents.find(chunkKey(res.coord, (uint32)want));
				evict = repIt != m_residents.end();
				if (evict && gate)
				{
					// Hole-free handover: the replacement is main-visible, or the old chunk isn't
					// on screen either (an off-screen swap can't show a hole).
					const bool newVis = repIt->second.spatialEntry.isValid()
						&& (Globals::spatialIndex.getPassMask(repIt->second.spatialEntry.handle()) & SpatialPassBit_Main);
					const bool oldVis = res.spatialEntry.isValid()
						&& (Globals::spatialIndex.getPassMask(res.spatialEntry.handle()) & SpatialPassBit_Main);
					evict = newVis || !oldVis;
				}
			}

			if (evict)
				it = m_residents.erase(it);
			else
				++it;
		}

		// --- Push resident chunks through the spatial culling gate. Main-visible chunks feed every pass;
		// main-culled chunks KEEP their shadow/GI passes unconditionally (terrain is the ground
		// everywhere — unlike entities, it skips the Near-ball test, because dropping the ground behind
		// the camera from the TLAS/shadow maps visibly breaks GI and long sun shadows; the GPU shadow
		// cull and the TLAS range bound already refine those passes). MainOnly debug mode drops
		// main-culled chunks entirely, like entities.
		for (auto& entry : m_residents)
		{
			Resident& res = entry.second;
			if (!res.node.isValid())
				continue;
			if (gate && res.spatialEntry.isValid())
			{
				uint32 passMask = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI;
				if (Globals::spatialIndex.getPassMask(res.spatialEntry.handle()) & SpatialPassBit_Main)
					passMask = RendererVKLayout::PASS_ALL;
				else if (culling.mode == int(ESpatialCullMode::MainOnly))
					continue;
				renderer.renderNode(res.node, passMask);
			}
			else
				renderer.renderNode(res.node);
		}
	}
}
