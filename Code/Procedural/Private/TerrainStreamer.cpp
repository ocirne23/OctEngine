module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Log;
import Settings;
import Settings.Tweaks;

import RendererVK;
import File;
import Spatial;

import :TerrainStreamer;
import :TerrainSampler;
import :TerrainGenerator;
import :TerrainChunk;
import :OceanGenerator;

namespace
{
	// Chebyshev distance (in chunk units) from the camera to the NEAREST EDGE of a chunk's footprint -
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

	// GEOMETRIC LOD bands over edge distance: lod = floor(log2(1 + d/lodStep)) - lodStep chunks of LOD0,
	// then 2*lodStep of LOD1, 4*lodStep of LOD2, ... capped at maxLod. Each LOD halves mesh density while
	// a feature's screen size halves per distance DOUBLING, so doubling band widths keeps the on-screen
	// triangle density roughly constant (linear bands over-detailed the mid rings). This is THE ring-LOD
	// function: enqueue, queue staleness, result validation and eviction all derive from it.
	uint32 ringLodAt(float edgeDist, float fullRes, float lodStep, uint32 maxLod)
	{
		// Everything whose edge is within fullRes chunks is unconditionally LOD0 - without it a chunk
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
	// shader blends the two best matches per pixel - weight 1 inside the box, Gaussian falloff outside.
	// Leaving an axis at its full range means "this entry does not care about it": alpine bedrock is cold
	// at ANY humidity and simply says so. The old point attractors could not express that, which is why
	// every cold entry used to have to claim one fictional humidity and sit in a hand-tuned ladder.
	//
	// The bands come off the Whittaker diagram (the standard mean-temperature/mean-precipitation plot of
	// what actually grows where). That is also why the hot entries sit at HIGHER precipitation than cold
	// ones covering similar vegetation: 1000 mm on a 25 C plain is seasonal savanna, on a 0 C plain it is
	// boreal forest. Evaporation is not modelled - it is baked into where the bands sit.
	//
	// Beach and Snow are NOT climate entries; they are overlays the shader composites on top (see
	// terrainSplat). Snow especially must not be a ground entry: the rock layer paints over the ground, so
	// a "snow ground type" is overwritten on exactly the peaks it exists for, and they come out gray.
	//
	// Sources are the CC0 sets under Assets/Textures/Terrain (see Assets/THIRD_PARTY_ASSETS.md); they bake
	// once into Assets/Local/TerrainTex as BC .dds so they mip-stream like cooked scene textures.
	enum class ESourceKind : uint8 { Ground, Rock, Beach, Snow };
	// Sentinels for "this axis does not constrain the entry" - past the encodable climate range, so they
	// clamp to a full-width 0..1 box and contribute no distance on that axis.
	constexpr float ANY_COLD = -1000.0f, ANY_HOT = 1000.0f, ANY_DRY = 0.0f, ANY_WET = 100000.0f;
	struct TerrainTexSource
	{
		ESourceKind kind = ESourceKind::Ground;
		// Climate box in REAL units; ignored for Beach/Snow.
		float tempMinC = ANY_COLD, tempMaxC = ANY_HOT;
		float precipMinMm = ANY_DRY, precipMaxMm = ANY_WET;
		// Texture set name under Assets/Textures/Terrain/. Sources default to the Poly Haven layout,
		// <stem>/<stem>_{diff,nor_gl,arm}_2k.jpg + <stem>_disp_2k.png, and bake to
		// Local/TerrainTex/<stem>_{diff,nor,arm,disp}.dds - so the cache is keyed by the texture, and
		// swapping one here can never read a stale bake.
		const char* stem;
		// Only for sets that name their files differently (the ambientCG ones). Relative to Assets/.
		const char* diffSrc = nullptr;
		const char* norSrc = nullptr;
		const char* armSrc = nullptr;
		const char* dispSrc = nullptr;
		// How much GRASS grows where this texture shows (0..1, RendererVK "Procedural grass"): ground entries only.
		float grass = 0.0f;
	};
	const TerrainTexSource TERRAIN_TEX_SOURCES[] =
	{
		// --- Ground: what the climate grows.
		// The boxes ABUT with only a narrow overlap. That is deliberate and is the one thing to preserve
		// when editing: a point inside two boxes scores a perfect 1.0 on both, so a wide overlap is not a
		// wide blend - it is a permanent 50/50 mush in which neither texture ever appears on its own. The
		// blend comes from the Gaussian tails just outside the edges, so entries should MEET, not straddle.
		//                temperature C           precipitation mm/yr
		{ .tempMinC = ANY_COLD, .tempMaxC = -4.0f,   .precipMinMm = ANY_DRY,  .precipMaxMm = ANY_WET, .stem = "gravel_ground_01" },        // polar/alpine scree: frost-shattered rubble. The substrate beside (and under) the snow, at ANY humidity - above the snow line there is no vegetation left for humidity to decide
		{ .tempMinC = -5.0f,    .tempMaxC = 0.0f,    .precipMinMm = 250.0f,   .precipMaxMm = ANY_WET, .stem = "rocky_trail", },             // tundra: moss and lichen over stony ground
		{ .tempMinC = -1.0f,    .tempMaxC = 5.0f,    .precipMinMm = 300.0f,   .precipMaxMm = 1500.0f, .stem = "forest_ground_04", .grass = 0.3f },         // taiga / boreal forest floor: needle litter
		{ .tempMinC = 4.0f,     .tempMaxC = 19.0f,   .precipMinMm = ANY_DRY,  .precipMaxMm = 400.0f,  .stem = "dry_ground_01" },                          // cold desert / dry steppe: cracked earth, no grass
		{ .tempMinC = 4.0f,     .tempMaxC = 20.0f,   .precipMinMm = 400.0f,   .precipMaxMm = 1000.0f, .stem = "Grass001",                  // temperate grassland / prairie
		  .diffSrc = "Textures/Terrain/Grass001/Grass001_2K-JPG_Color.jpg", .norSrc = "Textures/Terrain/Grass001/Grass001_2K-JPG_NormalGL.jpg", .armSrc = "Textures/Terrain/Grass001/Grass001_arm_2k.jpg",
		  .dispSrc = "Textures/Terrain/Grass001/Grass001_2K-JPG_Displacement.jpg", .grass = 1.0f },
		{ .tempMinC = 5.0f,     .tempMaxC = 18.0f,   .precipMinMm = 1000.0f,  .precipMaxMm = 1900.0f, .stem = "forest_floor", .grass = 0.5f },            // temperate seasonal forest: broadleaf litter
		{ .tempMinC = 3.0f,     .tempMaxC = 15.0f,   .precipMinMm = 1800.0f,  .precipMaxMm = ANY_WET, .stem = "Moss002",                   // temperate rainforest: deep moss
		  .diffSrc = "Textures/Terrain/Moss002/Moss002_2K-JPG_Color.jpg", .norSrc = "Textures/Terrain/Moss002/Moss002_2K-JPG_NormalGL.jpg", .armSrc = "Textures/Terrain/Moss002/Moss002_arm_2k.jpg",
		  .dispSrc = "Textures/Terrain/Moss002/Moss002_2K-JPG_Displacement.jpg", .grass = 0.4f },
		{ .tempMinC = 20.0f,    .tempMaxC = ANY_HOT, .precipMinMm = ANY_DRY,  .precipMaxMm = 300.0f,  .stem = "sand_01" },                 // subtropical desert: dune sand
		{ .tempMinC = 19.0f,    .tempMaxC = ANY_HOT, .precipMinMm = 450.0f,   .precipMaxMm = 1300.0f, .stem = "red_laterite_soil_stones", .grass = 0.25f },// savanna / dry tropics: iron-red laterite. 450 floor: at 250 this box bridged the steppe|sand seam and drew a red isotherm sliver across every desert
		{ .tempMinC = 18.0f,    .tempMaxC = ANY_HOT, .precipMinMm = 1300.0f,  .precipMaxMm = 2100.0f, .stem = "leaves_forest_ground", .grass = 0.5f },    // tropical forest floor: leaf litter
		{ .tempMinC = 16.0f,    .tempMaxC = ANY_HOT, .precipMinMm = 2000.0f,  .precipMaxMm = ANY_WET, .stem = "mud_forest", .grass = 0.4f },              // wetland / swamp: saturated mud

		// --- Rock: the bedrock the slope/crag layer exposes. Climate still selects the TYPE - weathering
		// is a climate process - but the cold entry spans all humidity for the same reason the scree does.
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

	// Poly Haven layout; the two ambientCG sets override it per map. map: "diff", "nor_gl", "arm", "disp".
	oc::string terrainTexSrcPath(const TerrainTexSource& src, const char* map)
	{
		const oc::string_view m = map;
		const char* override = m == "diff" ? src.diffSrc : m == "nor_gl" ? src.norSrc : m == "arm" ? src.armSrc : src.dispSrc;
		if (override)
			return override;
		return oc::format("Textures/Terrain/{}/{}_{}_2k.{}", src.stem, src.stem, map, m == "disp" ? "png" : "jpg");
	}

	oc::string terrainTexCachePath(const TerrainTexSource& src, const char* map)
	{
		return oc::format("{}{}_{}.dds", TERRAIN_TEX_CACHE_DIR, src.stem, map);
	}

	// The entry's climate box as TerrainSplatMaterial::climate takes it: temperature normalized to t01,
	// precipitation left in mm/yr. The renderer divides that by the GENERATOR'S live mm-per-full-humidity
	// scale ("Terrain/V3/Precip for full humidity") rather than the shared constant: V3 exposes it as a tweak,
	// and if the two ever drift the whole table slides along the humidity axis while still looking
	// perfectly reasonable in this file.
	glm::vec4 terrainSplatClimate(const TerrainTexSource& src)
	{
		return glm::vec4(Procedural::temperatureTo01(src.tempMinC), Procedural::temperatureTo01(src.tempMaxC),
		                 src.precipMinMm, src.precipMaxMm);
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

		// THREE textures per material (one texture fetch fewer per splat layer than the sources' four; the fetch
		// latency is the Static meshes range's top stall): the ARM source is split over the other two.
		//   diffr = albedo (sRGB) + ROUGHNESS in the alpha (the ARM's G), BC3
		//   nor   = the normal, BC5
		//   hao   = HEIGHT (the disp's R; 0.5 = flat without a disp source) + AO (the ARM's R), BC5
		// Metalness is dropped: terrain is never metallic, the splat uses 0.
		struct BakeTask { const TerrainTexSource* src; int map; }; // map: 0 = diffr, 1 = nor, 2 = hao
		oc::vector<BakeTask> tasks;
		for (const TerrainTexSource& src : TERRAIN_TEX_SOURCES)
		{
			const oc::string arm = terrainTexSrcPath(src, "arm");
			const oc::string diffr = terrainTexCachePath(src, "diffr");
			if (!terrainTexCacheFresh(diffr, terrainTexSrcPath(src, "diff")) || !terrainTexCacheFresh(diffr, arm))
				tasks.push_back({ &src, 0 });
			if (!terrainTexCacheFresh(terrainTexCachePath(src, "nor"), terrainTexSrcPath(src, "nor_gl")))
				tasks.push_back({ &src, 1 });
			const oc::string hao = terrainTexCachePath(src, "hao");
			if (!terrainTexCacheFresh(hao, terrainTexSrcPath(src, "disp")) || !terrainTexCacheFresh(hao, arm))
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
				const oc::string arm = terrainTexSrcPath(src, "arm");
				bool ok = false;
				switch (tasks[i].map)
				{
				case 0:
				{
					const oc::string diff = terrainTexSrcPath(src, "diff");
					const TextureConvert::PackChannel channels[4] = { { diff.c_str(), 0 }, { diff.c_str(), 1 }, { diff.c_str(), 2 }, { arm.c_str(), 1 } };
					ok = TextureConvert::convertChannelsToDds(channels, TextureConvert::EUsage::ColorAlpha, terrainTexCachePath(src, "diffr").c_str());
					break;
				}
				case 1: ok = TextureConvert::convertToDds(terrainTexSrcPath(src, "nor_gl").c_str(), TextureConvert::EUsage::NormalMap, terrainTexCachePath(src, "nor").c_str()); break;
				case 2:
				{
					// The disp source is optional: without it the height channel is flat (0.5 = the mesh).
					const oc::string disp = terrainTexSrcPath(src, "disp");
					const bool hasDisp = FileSystem::exists(disp);
					const TextureConvert::PackChannel channels[4] = { { hasDisp ? disp.c_str() : nullptr, 0, 128 }, { arm.c_str(), 0 }, {}, {} };
					ok = TextureConvert::convertChannelsToDds(channels, TextureConvert::EUsage::TwoChannel, terrainTexCachePath(src, "hao").c_str());
					break;
				}
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
		clearResidents(); // main thread: free RenderNodes/containers while the renderer is still alive (joins the upload job)
	}

	void TerrainStreamer::initialize()
	{
		ProfileScope scope("TerrainStreamer::initialize", EProfileCategory::Procedural);
		auto dirty = [this]() { m_configDirty = true; };

		// The cull job's Main stamp hands over the main-visible chunks AND ocean sectors as userData
		// values (see render), so neither push walks its whole set.
		Globals::spatialIndex.setVisibleCollect(SpatialLayer_Terrain, 1, SpatialIndex::ECollect::UserData);

		// The generator config (Settings::registerTerrain): a change rebuilds the maps. Enabled included: enabling is
		// what kicks the V3 model load (disabled terrain never loads the 2.28 GB of models onto the GPU).
		const TerrainSettings& s = m_settings;
		Tweak::onChange(s.enabled, this, dirty);
		Tweak::onChange(s.v3LoadModels, this, dirty);
		Tweak::onChange(s.seed, this, dirty);
		Tweak::onChange(s.originX, this, dirty);
		Tweak::onChange(s.originZ, this, dirty);
		Tweak::onChange(s.chunkSize, this, dirty);
		Tweak::onChange(s.lod0Res, this, dirty);
		Tweak::onChange(s.seaLevel, this, dirty);
		Tweak::onChange(s.skirtDepth, this, dirty);
		Tweak::onChange(s.v3MetersPerPixel, this, dirty);
		Tweak::onChange(s.v3HeightScale, this, dirty);
		Tweak::onChange(s.v3TemperatureOffset, this, dirty);
		Tweak::onChange(s.v3LapseRate, this, dirty);
		Tweak::onChange(s.v3ClimateNoiseC, this, dirty);
		Tweak::onChange(s.v3ClimateNoiseWavelength, this, dirty);
		Tweak::onChange(s.v3ClimateNoiseOctaves, this, dirty);
		Tweak::onChange(s.v3DetailSlopeGain, this, dirty);
		Tweak::onChange(s.v3DetailWavelengthA, this, dirty);
		Tweak::onChange(s.v3DetailAmplitudeA, this, dirty);
		Tweak::onChange(s.v3DetailOctavesA, this, dirty);
		Tweak::onChange(s.v3DetailWavelengthB, this, dirty);
		Tweak::onChange(s.v3DetailAmplitudeB, this, dirty);
		Tweak::onChange(s.v3DetailOctavesB, this, dirty);
		Tweak::onChange(s.v3PrecipFullHumidity, this, dirty);
		Tweak::onChange(s.v3HumidityOffset, this, dirty);
		Tweak::onChange(s.v3HumidFog, this, dirty);
		Tweak::onChange(s.v3ValleyFog, this, dirty);
		Tweak::onChange(s.v3MaxTiles, this, dirty);
		Tweak::onChange(s.v3Fp16, this, dirty);

		rebuildMaps();  // (a no-op while disabled: no generator, no model load)
		kickTexBake();  // (likewise gated: a disabled terrain reads no source images)
	}

	// Bake the biome splat textures to the DDS cache in the background (no-op when fresh); chunks
	// render with the flat-color fallback until updateTerrainTextures registers the finished set.
	// ONLY while the terrain is enabled - a game whose terrain is off must not touch ~19 source
	// image sets at startup - and once: enabling later kicks it from updateTerrainTextures.
	void TerrainStreamer::kickTexBake()
	{
		if (!m_settings.enabled || m_texBakeKicked)
			return;
		m_texBakeKicked = true;
		Globals::jobSystem.submit([this]
		{
			bakeTerrainTexCache(m_texBakeStop);
			m_texBakeDone.store(!m_texBakeStop.load(oc::memory_order_relaxed), oc::memory_order_release);
		}, { "TerrainStreamer::bakeTerrainTexCache", EProfileCategory::Procedural }, EJobPriority::Low, &m_texBakeCounter);
	}

	void TerrainStreamer::updateTerrainTextures(Renderer& renderer)
	{
        ProfileScope profileScope("updateTerrainTextures", EProfileCategory::Procedural);

		// The crag thresholds are the ONE shader parameter denominated in the same units the generator's
		// elevation is, so they alone have to follow V3's uniform world scale. Crag relief is a height
		// difference, so it shrinks with "Meters per pixel" exactly like everything else: the same mountain
		// measures ~15 m of relief at mpp=30 and ~1.5 m at mpp=3. Left absolute, a 12 m start is unreachable
		// below mpp≈25 and the near field simply never grows rock (measured: mean crag 1.5 m, rock weight
		// 0.019 at mpp=3). Scaling keeps the tweak meaning "metres at the model's true scale", which is the same
		// convention the detail wavelengths already use. The renderer reads every other terrain texture / water
		// setting straight from Globals::settings.terrain.
		renderer.setTerrainCragScale(TerrainGenV3::worldScale(m_settings.v3MetersPerPixel));

		// Until the bake finishes, chunks draw with the flat-color fallback. A bake with nothing usable
		// leaves the set unregistered; registerTerrainTextures' warnings say what.
		if (!m_texSetRegistered)
		{
			kickTexBake(); // terrain enabled after startup: the bake starts here, once
			if (m_texBakeDone.load(oc::memory_order_acquire))
				registerTerrainTextures(renderer);
		}
	}

	// Hands the baked DDS set to the renderer as [ground][rock][beach?][snow?], the slot order
	// Renderer::setTerrainSplatMaterials expects (NOT the composite order). Built in four passes over the table
	// rather than trusting its declaration order, so entries stay grouped however reads best there.
	// --
	// Renderer::setTerrainSplatMaterials docs
	// Registers the terrain splat set - textures AND climate boxes - as ONE contiguous material range.
	// counts must describe the whole span.
	//
	//   SLOT ORDER - mats[i] is material base + i
	//
	//   0                   numGround           numGround+numRock
	//   v                   v                   v
	//   +-------------------+-------------------+---------+---------+
	//   | GROUND            | ROCK              | BEACH   | SNOW    |
	//   | numGround entries | numRock entries   | 0 or 1  | 0 or 1  |
	//   | climate-picked    | climate-picked    | overlay | overlay |
	//   | world-XZ UV       | triplanar UV      | XZ UV   | XZ UV   |
	//   +-------------------+-------------------+---------+---------+
	//
	//   DRAW ORDER - terrainSplat composites bottom-up; this is NOT the slot order
	//
	//   4  SNOW     over everything cold enough; slides off steep slopes
	//   3  ROCK     steep slope or crag; covers the beach
	//   2  BEACH    band just above the waterline
	//   1  GROUND   the base layer
	//
	//   TEXTURES PER MATERIAL
	//
	//   field        -> index (UBO, per slot)  space    channels                        fallback *
	//   diffuseDds   -> terrainSplatTex        sRGB     RGB albedo, A roughness (BC3)  white, rough 0.9
	//   normalDds    -> terrainSplatTex        linear   tangent normal (BC5 **)        flat normal
	//   heightDds    -> terrainSplatHeightTex  linear   R height, G AO (BC5)           flat (0.5) + AO 1: no
	//                                                                                  parallax, linear blend
	//   Metalness is always 0 (terrain is never metallic).
	//
	//   *  on an empty path or a failed upload
	//   ** a BC5 file sets MATERIAL_FLAG_BC5_NORMAL; the shader rebuilds Z
	//
	//   CLIMATE BOX PER MATERIAL (ground and rock only; beach and snow ignore it)
	//
	//   component    range of        units
	//   climate.xy   temperature     t01, normalized by the caller
	//   climate.zw   precipitation   mm/yr; buildUboTerrain divides it by Globals::settings.terrain.v3PrecipFullHumidity
	//                                every frame, so that live tweak needs no re-register
	//
	//   Weight is 1 inside the box and a Gaussian falloff outside it; a full-width axis means "this axis
	//   does not matter" for the entry.
	//
	// Re-registering frees the old textures but leaks the old material slots - a config refresh, not a
	// per-frame path.
	void TerrainStreamer::registerTerrainTextures(Renderer& renderer)
	{
		// One-shot (the F10 pattern): setTerrainSplatMaterials uploads the DDS set, and the texture
		// upload reads each file on THIS thread. Covered by main's startup scope when terrain starts
		// enabled; a runtime enable (the tweak panel, the sandbox's override) needs its own.
		FileSystem::AllowMainThreadIO registerIo;

		auto tryBuildMat = [](const TerrainTexSource& src) -> oc::optional<Renderer::TerrainSplatMaterial>
		{
			Renderer::TerrainSplatMaterial mat;
			mat.diffuseDds = terrainTexCachePath(src, "diffr");
			mat.normalDds = terrainTexCachePath(src, "nor");
			mat.heightDds = terrainTexCachePath(src, "hao");
			mat.climate = terrainSplatClimate(src);
			mat.grass = src.kind == ESourceKind::Ground ? src.grass : 0.0f;
			if (!FileSystem::exists(mat.diffuseDds, /*allowMainThread*/ true) || !FileSystem::exists(mat.normalDds, true)
				|| !FileSystem::exists(mat.heightDds, true))
			{
				// Source images missing / bake failed: drop the entry. A ground or rock entry just leaves
				// its climate to the neighbouring boxes; a missing beach or snow entry disables that
				// overlay outright, which is worth saying out loud - a world with no snow layer looks like
				// a climate bug rather than a missing file.
				Log::warning(oc::format("Terrain: splat set '{}' incomplete, skipping", src.stem));
				return oc::nullopt;
			}
			return mat;
		};

		oc::vector<Renderer::TerrainSplatMaterial> mats;
		Renderer::TerrainSplatCounts counts;
		const auto append = [&](ESourceKind kind, uint32* count)
		{
			for (const TerrainTexSource& src : TERRAIN_TEX_SOURCES)
			{
				if (src.kind != kind)
					continue;
				if (auto mat = tryBuildMat(src))
				{
					mats.push_back(oc::move(*mat));
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
		renderer.setTerrainSplatMaterials(mats, counts);
		m_texSetRegistered = true;
	}

	void TerrainStreamer::rebuildMaps()
	{
        ProfileScope profileScope("rebuildMaps", EProfileCategory::Procedural);
		// A one-shot on a config change (the F10 pattern): constructing the generator below probes its
		// model assets on this thread before the background load starts. At startup main's own scope
		// covers it; a runtime enable (the tweak panel, the sandbox's override) needs this one.
		FileSystem::AllowMainThreadIO rebuildIo;

		// Disabled = no generator and, crucially, NO MODEL KICK: constructing TerrainGenV3 (even the throwaway
		// "kick" below) is what starts the 2.28 GB GPU load, so a disabled terrain must never reach it. The
		// Enabled tweak is registered dirty, so flipping it on lands back here and starts the load then.
		// There is no unload API - models loaded during an earlier enabled stretch stay resident.
		if (!m_settings.enabled)
		{
			m_v3AwaitingModels = false;
			std::lock_guard<std::mutex> lk(m_mutex);
			m_maps = nullptr;
			++m_generation;
			m_requests.clear();
			return;
		}

		// The generator can't sample anything until its models are resident. Constructing it is what kicks
		// the download/load off, so do that unconditionally, but only PUBLISH it once ready - otherwise
		// every chunk built in the meantime would bake a flat sea-level world into the resident cache and
		// never be revisited.
		TerrainConfigV3 cfg;
		cfg.seed = (uint32)m_settings.seed;
		cfg.seaLevel = m_settings.seaLevel;
		cfg.originX = m_settings.originX;
		cfg.originZ = m_settings.originZ;
		cfg.bounded = m_bounded;
		cfg.boundsMinX = m_boundsMin.x;
		cfg.boundsMinZ = m_boundsMin.y;
		cfg.boundsMaxX = m_boundsMax.x;
		cfg.boundsMaxZ = m_boundsMax.y;
		cfg.metersPerPixel = m_settings.v3MetersPerPixel;
		cfg.heightScale = m_settings.v3HeightScale;
		cfg.detailSlopeGain = m_settings.v3DetailSlopeGain;
		cfg.detailWavelengthA = m_settings.v3DetailWavelengthA;
		cfg.detailAmplitudeA = m_settings.v3DetailAmplitudeA;
		cfg.detailOctavesA = (uint32)glm::max(1, m_settings.v3DetailOctavesA);
		cfg.detailWavelengthB = m_settings.v3DetailWavelengthB;
		cfg.detailAmplitudeB = m_settings.v3DetailAmplitudeB;
		cfg.detailOctavesB = (uint32)glm::max(1, m_settings.v3DetailOctavesB);
		cfg.precipForFullHumidity = m_settings.v3PrecipFullHumidity;
		cfg.humidityOffset = m_settings.v3HumidityOffset;
		cfg.temperatureOffset = m_settings.v3TemperatureOffset;
		cfg.lapseRate = m_settings.v3LapseRate;
		cfg.climateNoiseAmplitudeC = m_settings.v3ClimateNoiseC;
		cfg.climateNoiseWavelength = m_settings.v3ClimateNoiseWavelength;
		cfg.climateNoiseOctaves = (uint32)glm::max(1, m_settings.v3ClimateNoiseOctaves);
		cfg.humidFogAmount = m_settings.v3HumidFog;
		cfg.valleyFogAmount = m_settings.v3ValleyFog;
		cfg.maxResidentTiles = m_settings.v3MaxTiles;
		cfg.useFp16 = m_settings.v3Fp16;

		// BEFORE the isReady() check, not inside the generator: switching precision reloads the models
		// and drops isReady(), so a check taken before it would publish a generator whose pipeline is
		// being torn down underneath it - and every chunk built meanwhile would bake a flat sea-level
		// world into the resident cache and never be revisited.
		TerrainGenV3::setModelLoadingEnabled(m_settings.v3LoadModels); // before any construction below kicks a load
		TerrainGenV3::setPrecision(m_settings.v3Fp16);

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

	TerrainStreamer::StreamStatus TerrainStreamer::streamStatus() const
	{
		StreamStatus s;
		s.enabled = m_settings.enabled;
		s.failed = TerrainGenV3::hasFailed();
		s.modelsReady = m_settings.enabled && !m_v3AwaitingModels && !s.failed;
		s.resident = (uint32)m_residents.size();
		s.pending = (uint32)m_pending.size();
		s.mapShipped = m_terrainMapUploaded;
		return s;
	}

	void TerrainStreamer::joinRender()
	{
		Globals::jobSystem.wait(m_renderCounter); // main helps; a no-op once the job is done
	}

	void TerrainStreamer::joinRingScan()
	{
		Globals::jobSystem.wait(m_ringScanCounter);
	}

	void TerrainStreamer::joinEvictScan()
	{
		Globals::jobSystem.wait(m_evictScanCounter);
	}

	void TerrainStreamer::kickUploads(Renderer& renderer)
	{
		if (m_uploads.empty())
			return;
		Globals::jobSystem.submit([this, &renderer]
		{
			auto uploadOne = [&](Upload& upload)
			{
				if (upload.mesh.isValid() || upload.result.mesh.indices.empty())
					return; // uploaded (or failed) by an earlier kick
				upload.mesh = renderer.createMesh(upload.result.mesh);
				upload.result.mesh = RenderMeshData{}; // the copy is in the staging ring now
			};
			// The batch's first shared-buffer write drains the GPU under the staging mutex, so the first chunk goes
			// alone: a fan-out would block one worker per chunk behind that wait. The rest copy in parallel, one
			// chunk per task (the staging memcpy runs outside its mutex).
			uploadOne(m_uploads[0]);
			Globals::jobSystem.parallelFor(1, (uint32)m_uploads.size(), 1, { "terrainUploadMesh", EProfileCategory::Procedural },
				[&](uint32 begin, uint32 end)
				{
					for (uint32 i = begin; i < end; ++i)
						uploadOne(m_uploads[i]);
				});
		}, { "terrainUpload", EProfileCategory::Procedural }, EJobPriority::Normal, &m_uploadCounter);
	}

	void TerrainStreamer::joinUploads()
	{
		Globals::jobSystem.wait(m_uploadCounter); // main helps; a no-op once the job is done
	}

	void TerrainStreamer::clearResidents()
	{
		joinRender();   // the render job reads m_residents
		joinRingScan(); // so does the ring scan; its output is against the residents being cleared
		joinEvictScan(); // and the eviction walk
		joinUploads();  // writes m_uploads
		m_uploads.clear();
		m_ringScanOut.clear();
		m_evictScanOut.clear();
		m_evictScanReady = false;
		for (auto& entry : m_residents)
			retireResident(oc::move(entry.second));
		m_residents.clear();
		m_evictCandidates.clear();
		m_pending.clear();
		m_readyBacklog.clear();
		m_ringScanNeeded = true;
	}

	// Everything that draws is released NOW (the node - which the hand-over push then skips -, the
	// mesh, the culling entry); only the Resident's memory waits for the next real stamp, because the
	// current hand-over list may still hold &node.
	void TerrainStreamer::retireResident(oc::unique_ptr<Resident> resident)
	{
		resident->spatialEntry.reset();
		resident->node.destroy();
		resident->mesh.destroy();
		m_retired.push_back({ Globals::spatialIndex.visibleCollectGeneration(), oc::move(resident) });
	}

	void TerrainStreamer::kickPump(size_t numNew)
	{
		// Top the pump pool up to min(cap, active + new work): each CAS claims one slot. Spawning
		// more pumps than requests is impossible this way; a pump finding the pool already drained
		// just exits through its recheck.
		const int32 cap = glm::clamp(m_settings.maxGenJobs, 1, 16);
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
			// Between chunks (the finer points sit inside generateChunk / sampleGrid): a Low pump
			// that has a queue of chunks ahead of it must not hold a worker against High work.
			Globals::jobSystem.preemptionPoint();

			Request req;
			bool haveWork = false;
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				// Take the request NEAREST THE CAMERA, re-measured against the ring state as it is RIGHT NOW.
				// Deliberately not a queue: FIFO order is enqueue TIME, which stops matching distance the
				// moment the camera moves. Chunks arrive over many frames, and a chunk whose LOD band
				// changes has its queued request invalidated and re-appended - landing BEHIND the far
				// leading-edge chunks queued on earlier frames. The ground under the camera then arrives
				// after the horizon does, which at V3's seconds-per-chunk is impossible to miss.
				// Sorting each batch on the way in cannot fix that: it only orders WITHIN a batch, and the
				// camera has moved by the time the next one is picked up. Only choosing at dequeue, against
				// the live camera, is order-independent.
				//
				// Lazy staleness rides along in the same pass: requests that fell out of the ring are
				// dropped HERE - the main thread never rewrites the queue - and bounce back as an EMPTY
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
				const int32 cap = glm::clamp(m_settings.maxGenJobs, 1, 16);
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
					ProfileScope meshScope("TerrainStreamer::buildMesh", EProfileCategory::Procedural); // pure, never parks
					MeshGeometryDesc geom;
					geom.positions = mesh.positions.data();
					geom.normals = mesh.normals.data();
					geom.tangents = mesh.tangents.data();
					geom.bitangents = mesh.bitangents.data();
					geom.texCoords = mesh.texCoords.data();
					geom.numVertices = (uint32)mesh.positions.size();
					geom.indices = mesh.indices.data();
					geom.numIndices = (uint32)mesh.indices.size();
					res.mesh.build(geom); // the upload layout; the source arrays die here, halving what ships
				}
			}

			{
				std::lock_guard<std::mutex> lk(m_mutex);
				m_results.push_back(oc::move(res));
			}
		}
	}

	// Shared terrain-data map: FOG_TERRAIN_CASCADES camera-centered snapshots (HeightMapBaker, sampled
	// from the SAME sampler the chunks render from) - a near cascade over m_settings.terrainMapRange meters at
	// fine texels and a far cascade over m_settings.terrainMapFarRange at the same resolution (coarse texels are
	// fine at those distances, so long-range data costs no extra memory). Per texel: height, water level,
	// packed fog|falloff|temp|hum, macro altitude. Consumers: the volumetric fog's terrain follow + regional
	// thickness, the ocean's shore-map fallback, and the TERRAIN pipeline's coloring. (The renderer-side
	// API keeps the historical "FogTerrainHeightMap" name - fog was its first consumer.)
	void TerrainStreamer::updateFogHeightMap(Renderer& renderer, const Camera& camera, const oc::shared_ptr<const ITerrainSampler>& maps, float farRange)
	{
        ProfileScope profileScope("updateFogHeightMap", EProfileCategory::Procedural);

		const bool active = m_settings.terrainMapEnabled && maps != nullptr;
		HeightMapBaker::Baked baked;
		const WaterReach* reach = m_settings.waterReachEnabled ? &m_settings.waterReach : nullptr;
		if (m_terrainMapBaker.update(baked, active, maps, glm::vec2(camera.position.x, camera.position.z),
			glm::vec2(glm::max(m_settings.terrainMapRange, 256.0f), farRange),
			RendererVKLayout::FOG_TERRAIN_RES, RendererVKLayout::FOG_TERRAIN_CASCADES, 4, // RGBA: height, water level, fog|flow|temp|hum, altitude
			reach, activeFlowField()))
		{
			// Decode what actually went into the packed climate channel, per cascade, exactly as
			// terrain_height.inc.glsl does. The bake is the one link in the chain nothing else can see: a
			// wrong sampler, a wrong pack and a wrong upload all look identical from the shader.
			if (m_settings.terrainMapDebugLog)
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
						const uint32 packed = oc::bitCast<uint32>(t[2]);
						const float temp = float((packed >> 16) & 255u) * (75.0f / 255.0f) - 25.0f;
						const float hum = float((packed >> 24) & 255u) * (1.0f / 255.0f);
						tMin = glm::min(tMin, temp); tMax = glm::max(tMax, temp);
						hMin = glm::min(hMin, hum);  hMax = glm::max(hMax, hum);
						yMin = glm::min(yMin, t[0]); yMax = glm::max(yMax, t[0]);
						aMin = glm::min(aMin, t[3]); aMax = glm::max(aMax, t[3]);
					}
					Log::info(oc::format("[Terrain] baked cascade {} ({:.0f} m, {}): height {:.0f}..{:.0f} | "
					                      "altitude {:.0f}..{:.0f} | temp {:.1f}..{:.1f} C | humidity {:.2f}..{:.2f}",
					                      c, baked.ranges[(int)c], c != 0 ? "coarse" : (m_terrainMapBaker.activeNearIsFull() ? "full" : "quick pass"),
					                      yMin, yMax, aMin, aMax, tMin, tMax, hMin, hMax));
				}
			}
			renderer.setFogTerrainHeightMap(baked.texels, baked.center, baked.ranges, maps->seaLevel());
			// Keep the shipped texels as the CPU copy (activeTerrainData): the ocean samples them for
			// buoyancy and wind steering. A NEW object per bake - consumers' shared_ptrs stay coherent.
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

	void TerrainStreamer::updateDisabled(Renderer& renderer, const Camera& camera)
	{
		if (m_configDirty)
		{
			m_configDirty = false;
			rebuildMaps();    // disabled: null maps, ++generation, drop queued requests, stop the model wait
			clearResidents();
		}
		if (!m_residents.empty() || !m_pending.empty())
			clearResidents();

		// Starve the pumps and drop late results (a V3 chunk is seconds of work, so chunks keep landing
		// for a while after the disable). Read the pump count BEFORE the drain: a pump only pushes while
		// it holds its slot, so "no pumps, then results drained" is final - nothing can appear after.
		const bool pumpsIdle = m_numPumps.load(oc::memory_order_acquire) == 0;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_requests.clear();
			m_results.clear();
		}

		// Polls/drops the in-flight terrain-data bake and clears the renderer-side map once (active=false).
		updateFogHeightMap(renderer, camera, nullptr, m_settings.terrainMapFarRange); // no terrain -> no terrain-following fog
		renderer.setTerrainParams(0.0f, m_settings.seaLevel);   // no mesh up: disables the ocean land cull
		renderer.setCameraGround(std::numeric_limits<float>::quiet_NaN());

		m_disabledIdle = pumpsIdle && !m_terrainMapBaker.inFlight() && !m_terrainMapUploaded;
	}

	// Push resident chunks through the spatial culling gate, WITHOUT walking every resident (the ring
	// holds thousands of chunks; a handful are on screen):
	//  1. Main-visible chunks come straight from the cull job's Main stamp, which collects the
	//     SpatialLayer_Terrain userData values for us (setVisibleCollect slot 1, installed in
	//     initialize). Every value is a RenderNode* (an ocean sector's tagged with
	//     SpatialTerrainTag_Ocean), so the job's ONE walk pushes chunks and sectors alike, each with
	//     its own node pass mask (chunks PASS_ALL, sectors PASS_MAIN or 0 when dry) - no lookup, no
	//     routing. These chunks feed every pass.
	//  2. Main-culled chunks keep their shadow/GI passes - terrain is the ground everywhere, and
	//     dropping the ground behind the camera from the TLAS/shadow maps visibly breaks GI and
	//     long sun shadows - but only within the range those GPU consumers actually read: the
	//     TLAS range bound, and the sun cascades' maxDistance plus the up-sun casterPad, both
	//     measured from the scene focus. A sphere query of that radius replaces the full walk;
	//     the GPU shadow cull and the TLAS range bound would have dropped anything past it.
	//     Past that, the terrain sun march shades from the baked height map, chunk-free.
	//     MainOnly debug mode drops main-culled chunks entirely, like entities.
	// A chunk in both sets is pushed once: the sphere pass skips main-stamped entries. Every chunk
	// registers with spawnVisible = false, so a fresh chunk simply waits one stamp for its main
	// pass and rides the sphere pass meanwhile. A node that died between the stamp and this push
	// (an evicted chunk, a rebuilt ocean grid) is still addressable - its owner retires the memory
	// until the next real stamp - and renderNode skips it, being destroyed. Culling Off (or headless:
	// no stamps, empty hand-over) keeps the plain walk, so nothing is ever culled there; the ocean then
	// pushes its own sectors.
	//
	// The walk and the pushes run on WORKERS (renderNode is lock-free from any job between
	// beginFrame and present, for different nodes): the list fans out over a parallelFor and the sphere
	// query runs beside it, the two sets disjoint. The list holds VALUES (it reads no pool row the scatter's registrations
	// on main could reallocate) and holds until the next spatial kick. main.cpp joins m_renderCounter
	// right before present, and update/clearResidents join it before touching m_residents (nothing
	// mutates it until then; this frame's ring scan only reads it). The ocean's sectors, their pass
	// masks and transforms hold until its next update, which follows that same join. The sphere query
	// locks the index itself.
	void TerrainStreamer::render(Renderer& renderer, OceanGenerator& ocean)
	{
		const SpatialCullingConfig& culling = Globals::spatialIndex.getCullingConfig();
		const bool gate = culling.mode >= int(ESpatialCullMode::Cull);
		const bool chunks = m_renderReady;
		m_renderReady = false;
		// The chunks' vegetation rides this walk whenever the chunks do (the owner submits it otherwise).
		m_vegRouted = chunks && m_vegSink;
		ocean.render(gate); // its own job: the readbacks, and every sector itself while the hand-over cannot name them
		if (!gate)
		{
			if (chunks)
				Globals::jobSystem.submit([this, &renderer]
				{
					for (const auto& entry : m_residents)
					{
						renderer.renderNode(entry.second->node);
						noteVegetation(*entry.second, RendererVKLayout::PASS_ALL);
					}
					flushVegetation();
				}, { "terrainRenderPush", EProfileCategory::Procedural }, EJobPriority::High, &m_renderCounter);
			return;
		}

		const bool sphere = chunks && culling.mode != int(ESpatialCullMode::MainOnly);
		const ShadowParams& shadow = renderer.shadowParams();
		const float reach = glm::max(shadow.maxDistance + shadow.casterPad, renderer.giTlasRange());
		const glm::dvec3 focus = glm::dvec3(renderer.sceneFocusOrCamera());
		Globals::jobSystem.submit([this, &renderer, sphere, reach, focus]
		{
			// The shadow/GI sphere runs BESIDE the hand-over fan-out: it skips every main-stamped entry, so the two
			// never push the same node (renderNode writes the node's upload state).
			JobCounter sphereCounter;
			if (sphere)
			{
				Globals::jobSystem.submit([this, &renderer, reach, focus]
				{
					Globals::spatialIndex.forEachInSphere(focus, reach, SpatialLayer_Terrain, [&](uint64 userData)
					{
						if (userData && !(userData & SpatialTerrainTag_Ocean)) // chunks only (not scatter groups, not sectors)
						{
							constexpr uint32 SHADOW_AND_GI = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI;
							const Resident& resident = *reinterpret_cast<const Resident*>(userData);
							renderer.renderNode(resident.node, SHADOW_AND_GI);
							noteVegetation(resident, SHADOW_AND_GI);
						}
					}, SpatialPassBit_Main); // main-stamped: pushed by the fan-out
				}, { "terrainRenderPushSphere", EProfileCategory::Procedural }, EJobPriority::High, &sphereCounter);
			}
			// The ONE walk of the hand-over (zeros - scatter groups - dropped at collect): an ocean sector's
			// tagged RenderNode*, or a chunk's Resident* (its node and its vegetation). Every entry is a
			// different node, so the list fans out.
			const oc::vector<uint64>& visible = Globals::spatialIndex.visibleUserData(1);
			Globals::jobSystem.parallelFor(0, (uint32)visible.size(), m_renderPushCost, { "terrainRenderPushChunk", EProfileCategory::Procedural },
				[&](uint32 begin, uint32 end)
				{
					for (uint32 i = begin; i < end; ++i)
					{
						const uint64 userData = visible[i];
						if (userData & SpatialTerrainTag_Ocean)
							renderer.renderNode(*reinterpret_cast<const RenderNode*>(userData & ~SpatialTerrainTag_Ocean));
						else
						{
							const Resident& resident = *reinterpret_cast<const Resident*>(userData);
							renderer.renderNode(resident.node);
							noteVegetation(resident, RendererVKLayout::PASS_ALL);
						}
					}
				}, EJobPriority::High);
			Globals::jobSystem.wait(sphereCounter);
			flushVegetation();
		}, { "terrainRenderPush", EProfileCategory::Procedural }, EJobPriority::High, &m_renderCounter);
	}

	// The walk's pushes (on several workers): a chunk's vegetation, merged per vegetation chunk (a LOD handover pushes
	// the old and the new resident of one coordinate - the trees must not draw twice).
	void TerrainStreamer::noteVegetation(const Resident& resident, uint32 passMask)
	{
		const int32 vegetation = resident.vegetation.load(oc::memory_order_relaxed);
		if (!m_vegRouted || vegetation < 0 || (size_t)vegetation >= m_vegMasks.size())
			return;
		const uint8 bits = (uint8)passMask;
		const oc::atomic_ref<uint8> mask(m_vegMasks[(size_t)vegetation]);
		if ((mask.load(oc::memory_order_relaxed) & bits) == bits)
			return;
		if (mask.fetch_or(bits, oc::memory_order_relaxed) == 0) // the first push of this chunk lists it
			m_vegTouched[m_vegTouchedCount.fetch_add(1, oc::memory_order_relaxed)] = (uint32)vegetation;
	}

	// The walk's end, after its pushes joined.
	void TerrainStreamer::flushVegetation()
	{
		if (!m_vegRouted) // the owner submits its vegetation itself this frame
			return;
		m_vegDraws.clear();
		const uint32 numTouched = m_vegTouchedCount.load(oc::memory_order_relaxed);
		for (uint32 i = 0; i < numTouched; ++i)
		{
			const uint32 chunk = m_vegTouched[i];
			m_vegDraws.push_back({ chunk, m_vegMasks[chunk] });
			m_vegMasks[chunk] = 0;
		}
		m_vegTouchedCount.store(0, oc::memory_order_relaxed);
		m_vegSink(oc::span<const VegetationDraw>(m_vegDraws.data(), m_vegDraws.size()));
	}

	void TerrainStreamer::setVegetation(oc::function<int32(glm::ivec2)> lookup, oc::function<void(oc::span<const VegetationDraw>)> sink,
		uint32 numChunks)
	{
		joinRender(); // the walk reads the hooks and every resident's index
		m_vegLookup = oc::move(lookup);
		m_vegSink = oc::move(sink);
		m_vegMasks.assign(m_vegSink ? numChunks : 0u, 0);
		m_vegTouched.assign(m_vegMasks.size(), 0u); // a chunk is listed at most once per walk
		m_vegTouchedCount.store(0, oc::memory_order_relaxed);
		for (auto& entry : m_residents)
			entry.second->vegetation.store(m_vegLookup ? m_vegLookup(entry.second->coord) : -1, oc::memory_order_relaxed);
		for (RetiredResident& retired : m_retired) // a hand-over list may still name them: no stale index
			retired.resident->vegetation.store(-1, oc::memory_order_relaxed);
	}

	void TerrainStreamer::restampVegetation(glm::ivec2 coord)
	{
		const int32 vegetation = m_vegLookup ? m_vegLookup(coord) : -1;
		for (uint32 lod = 0; lod < 16; ++lod) // every LOD a resident can have (the ring's max is the scan job's)
			if (const auto it = m_residents.find(chunkKey(coord, lod)); it != m_residents.end())
				it->second->vegetation.store(vegetation, oc::memory_order_relaxed);
		for (RetiredResident& retired : m_retired)
			if (retired.resident->coord == coord)
				retired.resident->vegetation.store(-1, oc::memory_order_relaxed);
	}

	void TerrainStreamer::update(Renderer& renderer, const Camera& camera)
	{
		joinRender();   // last frame's render job (already joined before present; a cheap no-op)
		joinRingScan(); // last frame's ring scan: applied below (enabled) or dropped here
		joinEvictScan(); // last frame's eviction walk: the same
		joinUploads();  // already joined before the frame kicks; a cheap no-op
		m_renderReady = false;
		{
			// Retired in increasing generation order: free the prefix no hand-over list can name any more.
			const uint32 generation = Globals::spatialIndex.visibleCollectGeneration();
			size_t freed = 0;
			while (freed < m_retired.size() && m_retired[freed].generation != generation)
				++freed;
			if (freed > 0)
				m_retired.erase(m_retired.begin(), m_retired.begin() + freed);
		}
		if (!m_settings.enabled)
		{
			m_ringScanOut.clear();
			m_evictScanReady = false;
			// Disabled: no profile scope, no per-frame work. The transition (and any tweak that
			// re-dirties the config while parked, e.g. sea level) drains through updateDisabled
			// until nothing is left in flight.
			if (m_configDirty)
				m_disabledIdle = false;
			if (!m_disabledIdle)
				updateDisabled(renderer, camera);
			return;
		}
		m_disabledIdle = false;

		ProfileScope profileScope("Terrain", EProfileCategory::Procedural);
		//m_settings.terrainMapFarRange = camera.far;
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

		updateTerrainTextures(renderer);

		const float chunkSize = (float)m_settings.chunkSize;
		const int camCX = (int)std::floor(camera.position.x / chunkSize);
		const int camCZ = (int)std::floor(camera.position.z / chunkSize);
		const int R = glm::max(1, m_settings.ringRadius);
		const float lodStep = glm::max(0.01f, m_settings.lodStep);
		const float fullRes = glm::max(0.0f, m_settings.fullResDist);
		const uint32 maxLod = (uint32)glm::max(0, m_settings.maxLod);
		// Camera position in chunk units, SNAPPED to a quarter-chunk lattice: edge-distance LOD makes the
		// wanted set a function of the camera POSITION, not just its chunk, and the snap both bounds how
		// often the scan re-runs and gives the worker one exact value to judge staleness against.
		const glm::vec2 camChunks(std::floor(camera.position.x / chunkSize * 4.0f) * 0.25f,
		                          std::floor(camera.position.z / chunkSize * 4.0f) * 0.25f);

		// Chunk mesh coverage: chunks span +-R around the camera's chunk, so a disk of R*chunkSize around
		// the camera is guaranteed resident whatever its position within its own chunk - the fence for
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
		// into a temperature at any height, so it must be the rate the generator ACTUALLY used - hence read
		// from the sampler rather than from the tweak beside it. 0 with no terrain, and 0 from a generator
		// that folds its own lapse into the temperature it reports: there the baseline IS the temperature
		// and lapsing it again would double-count.
		float lapsePerWorldM = 0.0f;
		if (maps)
		{
			const float vertScale = TerrainGenV3::worldScale(m_settings.v3MetersPerPixel) * glm::max(m_settings.v3HeightScale, 0.01f);
			lapsePerWorldM = maps->lapseRatePerMetre() / glm::max(vertScale, 1e-4f);
		}
		renderer.setTerrainParams((float)R * chunkSize, m_settings.seaLevel, lapsePerWorldM);
		// The ground under the camera (the near grass shadow cascade's placement). The sample may read the tile under the
		// camera from the disk cache once (a cold start or a jump): one tile, which the chunks there need anyway.
		{
			const FileSystem::AllowMainThreadIO cameraGroundIo;
			renderer.setCameraGround(maps ? maps->sampleHeight(camera.position.x, camera.position.z) : std::numeric_limits<float>::quiet_NaN());
		}

		// The far cascade must cover the whole resident mesh, else terrain past its edge reads clamp-to-edge
		// (frozen altitude/temperature -> distant inland looks uniformly cold/snowy, ignoring the inland
		// rise). The mesh reaches ~(R+1)*chunkSize from the camera on each axis; the map is a centered
		// square of side = range, so range must be at least twice that. Keep the tweak as a floor.
		const float meshHalfExtent = (float)(R + 1) * chunkSize;
		const float farRange = glm::max(m_settings.terrainMapFarRange, 2.0f * meshHalfExtent);
		updateFogHeightMap(renderer, camera, maps, farRange);

		// Ring membership is CLOSED FORM: the column at Chebyshev distance cheb from the camera chunk
		// wants ringLodAt(cheb) (geometric bands), nothing outside R. Every "is this still wanted"
		// question below (result validation, eviction) is this arithmetic - no desired-key sets.
		const auto ringLod = [&](glm::ivec2 coord) -> int
		{
			const int cheb = glm::max(glm::abs(coord.x - camCX), glm::abs(coord.y - camCZ));
			return cheb <= R ? (int)ringLodAt(chunkEdgeDist(camChunks, coord), fullRes, lodStep, maxLod) : -1;
		};

		const bool ringMoved = camChunks != m_lastRingCamPos || camCX != m_lastRingCX || camCZ != m_lastRingCZ
			|| R != m_lastRingR || lodStep != m_lastRingLodStep || fullRes != m_lastRingFullRes || maxLod != m_lastRingMaxLod;

		// --- Apply LAST frame's ring scan (the job kicked at the end of update, joined above): enqueue
		// every ring chunk it found neither resident nor in flight. The scan is a pure function of
		// (ring, residents, pending), so it only re-runs when one of them changed - and it ran against
		// last frame's ring, so a key that became pending or resident since is skipped here and a
		// request the ring has since moved away from is dropped by the pump like any stale one.
		oc::vector<Request> newRequests = oc::move(m_ringScanOut);
		m_ringScanOut.clear();
		for (size_t i = 0; i < newRequests.size(); )
		{
			const uint64 key = newRequests[i].key;
			if (m_residents.count(key) || m_pending.count(key))
			{
				newRequests[i] = oc::move(newRequests.back());
				newRequests.pop_back();
				continue;
			}
			m_pending.insert(key);
			++i;
		}

		// The queue is an unordered POOL, not a queue: the worker rescans it on every dequeue and takes the
		// chunk nearest the camera at that moment (see workerLoop), and drops out-of-range entries in the
		// same pass. So the main thread neither sorts nor prunes - appending in any order is correct, and
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
        {
            // --- Adopt LAST frame's upload batch (the job ran through present and the frame-pacing wait
            // and was joined before this frame's kicks): only the node spawn and the culling
            // registration are left, both cheap.
            ProfileScope profileScope2("adoptUploads", EProfileCategory::Procedural);
            size_t kept = 0;
            for (Upload& upload : m_uploads)
            {
                Result& res = upload.result;
                if (!upload.mesh.isValid() && !res.mesh.indices.empty())
                {
                    if (&m_uploads[kept] != &upload)
                        m_uploads[kept] = oc::move(upload); // not kicked yet: stays in the batch
                    ++kept;
                    continue;
                }
                m_pending.erase(res.key);
                m_ringScanNeeded = true;
                const bool valid = upload.mesh.isValid() && res.generation == generation
                    && (int)res.lod == ringLod(res.coord) && !m_residents.count(res.key);
                if (!valid)
                    continue; // failed / stale / no longer wanted: the mesh frees here, on main

                // ONE mesh per chunk (no ObjectContainer: no per-chunk material, names or node tables) on the
                // terrain pipeline variant (procedural height/slope albedo), with the material every chunk
                // shares. No LOD chain: the streamer already LODs by ring distance.
                if (m_material == UINT16_MAX)
                    m_material = renderer.createMeshMaterial(RendererVKLayout::EPipelineIndex::TerrainLit, true);
                auto resident = oc::make_unique<Resident>();
                resident->mesh = oc::move(upload.mesh);
                const Transform transform(
                    glm::vec3((float)res.coord.x * chunkSize, 0.0f, (float)res.coord.y * chunkSize),
                    1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                resident->node = renderer.spawnMeshNode(resident->mesh, m_material, RendererVKLayout::EPipelineIndex::TerrainLit, transform);
                resident->coord = res.coord;
                resident->lod = res.lod;
                resident->vegetation.store(m_vegLookup ? m_vegLookup(res.coord) : -1, oc::memory_order_relaxed);
                // Culling registration: chunks live in the SpatialIndex like entity render components, but on
                // their own layer (the userData is the chunk's Resident*, NOT an Entity* - gameplay queries
                // must not see them - so the hand-over push needs no lookup and reaches the chunk's node AND its
                // vegetation; the Resident is heap-held and retired, never freed, while a list may still name it),
                // registered ONCE (chunks never move, so they promote straight into the static tier), and WITHOUT
                // the spawn-visibility guard (chunks stream in off-screen constantly; the guard would pin each one
                // in the main pass until it first enters the frustum).
                // The sphere also holds the chunk's vegetation: a plant stands up to VEG_HEIGHT above the ground and
                // its crown reaches up to VEG_OVERHANG past the chunk's edge (its piece sorts by its centre).
                constexpr float VEG_HEIGHT = 64.0f, VEG_OVERHANG = 16.0f;
                const Sphere bounds = resident->node.getWorldBounds();
                const float radius = std::sqrt(bounds.radius * bounds.radius + VEG_HEIGHT * VEG_HEIGHT) + VEG_OVERHANG;
                resident->spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(
                    glm::dvec3(bounds.pos), radius, (uint64)resident.get(), SpatialLayer_Terrain, false));
                m_residents.emplace(res.key, oc::move(resident));
                ++uploads;
            }
            m_uploads.resize(kept);
        }
        {
            // --- Pick THIS frame's upload batch (kickUploads, after present). The picks stay "pending"
            // until they are adopted, so the ring scan does not re-request them.
            ProfileScope profileScope2("processResult", EProfileCategory::Procedural);
            const size_t maxBytes = (size_t)(glm::max(m_settings.maxUploadMBPerFrame, 1.0f) * 1024.0f * 1024.0f);
            size_t batchBytes = 0;
            for (Result& res : ready)
            {
                if (res.mesh.indices.empty()) // pump-dropped (stale at dequeue) or generation failed: just release the key
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
                const size_t bytes = res.mesh.vertices.size() * sizeof(res.mesh.vertices[0])
                    + res.mesh.indices.size() * sizeof(res.mesh.indices[0]);
                if ((int)m_uploads.size() >= m_settings.maxUploadsPerFrame || (!m_uploads.empty() && batchBytes + bytes > maxBytes))
                {
                    m_readyBacklog.push_back(oc::move(res)); // stays "pending" so it isn't re-requested
                    continue;
                }
                batchBytes += bytes;
                m_uploads.push_back(Upload{ oc::move(res), RenderMesh() });
            }
        }

		const SpatialCullingConfig& culling = Globals::spatialIndex.getCullingConfig();
		const bool gate = culling.mode >= int(ESpatialCullMode::Cull);

		// --- Evict residents, but keep a column's current chunk until its replacement is ready (no hole
		// while a new LOD streams in). Evict a resident only if its column left the ring entirely, or its
		// column now wants a different LOD AND that replacement chunk is resident AND can take over ON
		// SCREEN: a freshly registered replacement isn't main-stamped until the next markVisibleSet, so
		// evicting on residency alone opened a one-frame hole on every in-view LOD change (and while the
		// culling is FROZEN the replacement never gets stamped - the old chunk just stays).
		//
		// No per-frame walk of the ring: which residents are unwanted (want < 0 or want != lod) depends
		// on the ring and the resident set only, so that list is rebuilt by one walk when the ring moved
		// or a chunk uploaded (m_evictCandidates; the handover replacement can only appear through an
		// upload, and an eviction never creates a candidate). The walk is the "terrainEvictScan" job,
		// kicked at the end of the previous update: its list is one ring old, so every frame judges each
		// candidate by THIS frame's ring and checks it against the stamps.
        {
            ProfileScope profileScope2("evictResidents", EProfileCategory::Procedural);
            if (m_evictScanReady)
            {
                m_evictScanReady = false;
                m_evictCandidates.swap(m_evictScanOut);
            }

            for (size_t i = 0; i < m_evictCandidates.size(); )
            {
                const auto it = m_residents.find(m_evictCandidates[i]);
                bool drop = it == m_residents.end(); // already gone: drop the candidate
                if (!drop)
                {
                    const Resident& res = *it->second;
                    const int want = ringLod(res.coord);
                    bool evict = false;
                    if (want < 0)
                        evict = true; // column outside the ring
                    else if ((uint32)want == res.lod)
                        drop = true;  // wanted again (the ring moved back since the walk)
                    else
                    {
                        const auto repIt = m_residents.find(chunkKey(res.coord, (uint32)want));
                        evict = repIt != m_residents.end();
                        if (evict && gate)
                        {
                            // Hole-free handover: the replacement is main-visible, or the old chunk isn't
                            // on screen either (an off-screen swap can't show a hole).
                            const bool newVis = repIt->second->spatialEntry.isValid()
                                && (Globals::spatialIndex.getPassMask(repIt->second->spatialEntry.handle()) & SpatialPassBit_Main);
                            const bool oldVis = res.spatialEntry.isValid()
                                && (Globals::spatialIndex.getPassMask(res.spatialEntry.handle()) & SpatialPassBit_Main);
                            evict = newVis || !oldVis;
                        }
                    }
                    if (evict)
                    {
                        retireResident(oc::move(it->second)); // this frame's hand-over may still hold &node
                        m_residents.erase(it);
                        drop = true;
                    }
                }

                if (drop)
                {
                    m_evictCandidates[i] = m_evictCandidates.back();
                    m_evictCandidates.pop_back();
                }
                else
                    ++i;
            }
        }

		m_renderReady = true; // the chunk push is render()'s (after the ocean's update)

		// The GRASS stands on these chunks (Renderer::setGrassGround): its blades read the mesh vertices. Only the
		// columns within its range - a handful of lookups per LOD, not a walk of the ring. The finest resident per
		// column wins in the renderer (a LOD hand-over keeps two).
		{
			const float grassRange = renderer.grassRange();
			m_grassGround.clear();
			if (grassRange > 0.0f)
			{
				const glm::ivec2 c0(std::floor((camera.position.x - grassRange) / chunkSize), std::floor((camera.position.z - grassRange) / chunkSize));
				const glm::ivec2 c1(std::floor((camera.position.x + grassRange) / chunkSize), std::floor((camera.position.z + grassRange) / chunkSize));
				const uint32 lod0Res = (uint32)glm::max(1, m_settings.lod0Res);
				for (int z = c0.y; z <= c1.y; ++z)
					for (int x = c0.x; x <= c1.x; ++x)
						for (uint32 lod = 0; lod <= maxLod; ++lod)
						{
							const auto it = m_residents.find(chunkKey(glm::ivec2(x, z), lod));
							if (it != m_residents.end() && it->second->mesh.isValid())
								m_grassGround.push_back({ glm::ivec2(x, z), it->second->mesh.getFirstVertex(), glm::max(1u, lod0Res >> lod) });
						}
			}
			renderer.setGrassGround(chunkSize, oc::span<const Renderer::GrassGroundChunk>(m_grassGround.data(), m_grassGround.size()));
		}

		// --- Kick THIS frame's ring scan for the next update to apply. Last in update on purpose: the
		// drain and the eviction above were the frame's last writers of m_residents / m_pending, and
		// the job only reads them (its output goes to m_ringScanOut, consumed after the join at the
		// top). Every input is SNAPSHOT into m_ringScanIn, which only main writes and only after the
		// joins, so a tweak or setGeneratedBounds edit on main cannot race it. The eviction walk reads the
		// same snapshot, in a job of its own.
		const bool ringScan = ringMoved || m_ringScanNeeded;
		const bool evictScan = ringMoved || uploads > 0;
		if (ringScan || evictScan)
		{
			RingScanInput& in = m_ringScanIn;
			in.camCX = camCX; in.camCZ = camCZ; in.R = R;
			in.camChunks = camChunks;
			in.chunkSize = chunkSize; in.fullRes = fullRes; in.lodStep = lodStep; in.skirtDepth = m_settings.skirtDepth;
			in.maxLod = maxLod; in.lod0Res = (uint32)glm::max(1, m_settings.lod0Res); in.generation = generation;
			in.bounded = m_bounded; in.boundsMin = m_boundsMin; in.boundsMax = m_boundsMax;
			in.maps = maps;
		}
		if (evictScan)
		{
			m_evictScanReady = true;
			Globals::jobSystem.submit([this]
			{
				const RingScanInput& s = m_ringScanIn;
				m_evictScanOut.clear();
				for (const auto& entry : m_residents)
				{
					const Resident& res = *entry.second;
					const int cheb = glm::max(glm::abs(res.coord.x - s.camCX), glm::abs(res.coord.y - s.camCZ));
					if (cheb > s.R || ringLodAt(chunkEdgeDist(s.camChunks, res.coord), s.fullRes, s.lodStep, s.maxLod) != res.lod)
						m_evictScanOut.push_back(entry.first);
				}
			}, { "terrainEvictScan", EProfileCategory::Procedural }, EJobPriority::Normal, &m_evictScanCounter);
		}
		if (ringScan)
		{
			m_ringScanNeeded = false;
			Globals::jobSystem.submit([this]
			{
				const RingScanInput& s = m_ringScanIn;
				for (int dz = -s.R; dz <= s.R; ++dz)
				{
					for (int dx = -s.R; dx <= s.R; ++dx)
					{
						const glm::ivec2 coord(s.camCX + dx, s.camCZ + dz);
						// Generated bounds: a chunk that does not touch the seeded area is never requested
						// (its tiles were never generated; see setGeneratedBounds).
						if (s.bounded)
						{
							const float x0 = (float)coord.x * s.chunkSize, z0 = (float)coord.y * s.chunkSize;
							if (x0 + s.chunkSize <= s.boundsMin.x || x0 >= s.boundsMax.x
								|| z0 + s.chunkSize <= s.boundsMin.y || z0 >= s.boundsMax.y)
								continue;
						}
						const uint32 lod = ringLodAt(chunkEdgeDist(s.camChunks, coord), s.fullRes, s.lodStep, s.maxLod);
						const uint64 key = chunkKey(coord, lod);
						if (m_residents.count(key) || m_pending.count(key))
							continue;

						Request req;
						req.key = key;
						req.generation = s.generation;
						req.maps = s.maps;
						req.params.coord = coord;
						req.params.lod = lod;
						req.params.chunkSize = s.chunkSize;
						req.params.lod0Res = s.lod0Res;
						req.params.skirtDepth = s.skirtDepth;
						m_ringScanOut.push_back(oc::move(req));
					}
				}
			}, { "terrainRingScan", EProfileCategory::Procedural }, EJobPriority::Normal, &m_ringScanCounter);
		}
	}
}
