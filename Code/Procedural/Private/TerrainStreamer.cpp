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
	// Chebyshev distance (in BASE chunk units) from the camera to the NEAREST EDGE of a quadtree node's footprint -
	// 0 while the camera stands inside/on it. Distance to the edge, not to the center: a neighbour's near boundary
	// can be a whole chunk away when the camera sits centered, or right underfoot at the boundary, and the LOD
	// should follow that continuously instead of stepping per camera-chunk crossing. `cam` is the SNAPPED camera
	// (see update()) so every caller sees the same value.
	//
	// MIRRORED BIT FOR BIT by terrainNodeEdgeDist in instanced_indirect_terrain.vs.glsl (edge stitching), like
	// ringLodAt and Ring::leafLodAt: the same float operations in the same order (every int -> float exact).
	float nodeEdgeDist(glm::vec2 cam, glm::ivec2 coord, uint32 lod)
	{
		const int span = 1 << lod;
		const float size = (float)span;
		const float x0 = (float)(coord.x * span), z0 = (float)(coord.y * span);
		const float dx = glm::max(glm::max(x0 - cam.x, cam.x - (x0 + size)), 0.0f);
		const float dz = glm::max(glm::max(z0 - cam.y, cam.y - (z0 + size)), 0.0f);
		return glm::max(dx, dz);
	}

	// GEOMETRIC LOD bands over edge distance: lod = floor(log2(1 + d/lodStep)) - lodStep chunks of LOD0,
	// then 2*lodStep of LOD1, 4*lodStep of LOD2, ... capped at maxLod. Each LOD halves mesh density while
	// a feature's screen size halves per distance DOUBLING, so doubling band widths keeps the on-screen
	// triangle density roughly constant (linear bands over-detailed the mid rings). The quadtree splits a node
	// while this is below its lod (Ring::leafLodAt).
	//
	// MIRRORED BIT FOR BIT by terrainRingLod in instanced_indirect_terrain.vs.glsl (edge stitching): the same float
	// operations in the same order - so no log2 - since both sides of every node edge must agree on its LOD.
	// lod >= l + 1 exactly when k >= (2^(l+1) - 1) * lodStep; the threshold doubles plus lodStep per level (the * 2
	// is exact, so an FMA contraction cannot change it).
	uint32 ringLodAt(float edgeDist, float fullRes, float lodStep, uint32 maxLod)
	{
		// Everything whose edge is within fullRes chunks is unconditionally LOD0 - without it a chunk
		// whose boundary you are standing on could already be a level down.
		const float k = glm::max(edgeDist - fullRes, 0.0f);
		const float step = glm::max(lodStep, 0.01f);
		float threshold = step;
		uint32 lod = 0;
		while (lod < maxLod && k >= threshold)
		{
			++lod;
			threshold = threshold * 2.0f + step;
		}
		return lod;
	}

	// Pack a node coordinate (at its lod) + the LOD into a stable 64-bit key. 28 bits each for X/Z covers +-134M nodes.
	uint64 chunkKey(glm::ivec2 coord, uint32 lod)
	{
		const uint64 x = (uint64)(uint32)coord.x & 0xFFFFFFFull;
		const uint64 z = (uint64)(uint32)coord.y & 0xFFFFFFFull;
		return (x << 36) | (z << 8) | (uint64)(lod & 0xFFu);
	}
	glm::ivec2 keyCoord(uint64 key)
	{
		// Sign-extend the 28-bit fields.
		const int32 x = (int32)((uint32)(key >> 36) << 4) >> 4;
		const int32 z = (int32)((uint32)((key >> 8) & 0xFFFFFFFull) << 4) >> 4;
		return glm::ivec2(x, z);
	}
	uint32 keyLod(uint64 key) { return (uint32)(key & 0xFFu); }

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

	// ---- The quadtree (see Ring in the header) ----

	// The LOD of the quadtree leaf over a base chunk: from maxLod down, the first node over it whose band reaches its
	// lod. MIRRORED BIT FOR BIT by terrainLeafLod in instanced_indirect_terrain.vs.glsl (edge stitching).
	uint32 TerrainStreamer::Ring::leafLodAt(glm::ivec2 base) const
	{
		for (uint32 lod = maxLod; lod > 0; --lod)
		{
			const glm::ivec2 node(base.x >> lod, base.y >> lod); // arithmetic shift: floor, also for negatives
			if (ringLodAt(nodeEdgeDist(cam, node, lod), fullRes, lodStep, maxLod) >= lod)
				return lod;
		}
		return 0;
	}

	bool TerrainStreamer::Ring::touches(glm::ivec2 coord, uint32 lod) const
	{
		const int span = 1 << lod;
		const glm::ivec2 lo = coord * span, hi = lo + span - 1;
		return hi.x >= camChunk.x - R && lo.x <= camChunk.x + R && hi.y >= camChunk.y - R && lo.y <= camChunk.y + R;
	}

	float TerrainStreamer::Ring::edgeDist(glm::ivec2 coord, uint32 lod) const
	{
		return nodeEdgeDist(cam, coord, lod);
	}

	bool TerrainStreamer::insideBounds(glm::ivec2 coord, uint32 lod, float chunkSize) const
	{
		if (!m_bounded)
			return true;
		const float size = chunkSize * (float)(1 << lod);
		const float x0 = (float)coord.x * size, z0 = (float)coord.y * size;
		return x0 + size > m_boundsMin.x && x0 < m_boundsMax.x && z0 + size > m_boundsMin.y && z0 < m_boundsMax.y;
	}

	// The wanted leaves of m_ring: from the maxLod nodes over the ring, split while the band asks for finer (the same
	// descent as leafLodAt), keeping the leaves that touch the ring and the generated bounds.
	void TerrainStreamer::rebuildWanted()
	{
		m_wanted.clear();
		m_wantedList.clear();
		m_wantedHalfExtent = 0.0f;
		const Ring& ring = m_ring;
		const float chunkSize = (float)m_settings.chunkSize;
		const auto visit = [&](auto&& self, glm::ivec2 coord, uint32 lod) -> void
		{
			if (!ring.touches(coord, lod) || !insideBounds(coord, lod, chunkSize))
				return;
			if (lod > 0 && ringLodAt(ring.edgeDist(coord, lod), ring.fullRes, ring.lodStep, ring.maxLod) < lod)
			{
				for (int dz = 0; dz < 2; ++dz)
					for (int dx = 0; dx < 2; ++dx)
						self(self, coord * 2 + glm::ivec2(dx, dz), lod - 1);
				return;
			}
			const uint64 key = chunkKey(coord, lod);
			m_wanted.insert(key);
			m_wantedList.push_back(key);
			const int span = 1 << lod;
			const glm::ivec2 lo = coord * span - ring.camChunk, hi = lo + span;
			m_wantedHalfExtent = glm::max(m_wantedHalfExtent, (float)glm::max(glm::max(-lo.x, hi.x), glm::max(-lo.y, hi.y)) * chunkSize);
		};
		const glm::ivec2 rootLo((ring.camChunk.x - ring.R) >> ring.maxLod, (ring.camChunk.y - ring.R) >> ring.maxLod);
		const glm::ivec2 rootHi((ring.camChunk.x + ring.R) >> ring.maxLod, (ring.camChunk.y + ring.R) >> ring.maxLod);
		for (int z = rootLo.y; z <= rootHi.y; ++z)
			for (int x = rootLo.x; x <= rootHi.x; ++x)
				visit(visit, glm::ivec2(x, z), ring.maxLod);
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
		joinUploads();  // writes m_uploads
		m_uploads.clear();
		for (auto& entry : m_residents)
			retireResident(oc::move(entry.second));
		m_residents.clear();
		m_drawCam.valid = false; // nothing drawn: the next update takes the ring camera
		m_ring.valid = false;    // the next update rebuilds the wanted set and re-requests it
		m_pending.clear();
		m_readyBacklog.clear();
	}

	// Culling registration: nodes live in the SpatialIndex like entity render components, but on their own layer (the
	// userData is the node's Resident*, NOT an Entity* - gameplay queries must not see them - so the hand-over push
	// needs no lookup and reaches the node AND its vegetation; the Resident is heap-held and retired, never freed,
	// while a list may still name it), registered ONCE (nodes never move), and WITHOUT the spawn-visibility guard
	// (nodes stream in off-screen constantly; the guard would pin each one in the main pass until it first enters the
	// frustum). The sphere also holds the node's vegetation: a plant stands up to VEG_HEIGHT above the ground and its
	// crown reaches up to VEG_OVERHANG past the node's edge (its piece sorts by its centre).
	void TerrainStreamer::registerResident(Resident& resident)
	{
		constexpr float VEG_HEIGHT = 64.0f, VEG_OVERHANG = 16.0f;
		const Sphere bounds = resident.node.getWorldBounds();
		const float radius = std::sqrt(bounds.radius * bounds.radius + VEG_HEIGHT * VEG_HEIGHT) + VEG_OVERHANG;
		resident.spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(
			glm::dvec3(bounds.pos), radius, (uint64)&resident, SpatialLayer_Terrain, false));
		resident.registeredAt = Globals::spatialIndex.visibleCollectGeneration();
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
				float bestDist = FLT_MAX;
				size_t keep = 0;
				for (size_t i = 0; i < m_requests.size(); ++i)
				{
					Request& r = m_requests[i];
					if (!m_pumpRing.valid || !m_pumpRing.touches(r.params.coord, r.params.lod) || !m_pumpRing.isLeaf(r.params.coord, r.params.lod))
					{
						Result drop;
						drop.key = r.key;
						drop.generation = r.generation;
						drop.coord = r.params.coord;
						drop.lod = r.params.lod;
						m_results.push_back(oc::move(drop)); // empty mesh = dropped
						continue;                             // not carried into the kept prefix
					}
					// The node's nearest edge: the ground under the camera first, a far node (a big one) by its near side.
					const float dist = m_pumpRing.edgeDist(r.params.coord, r.params.lod);
					if (dist < bestDist)
					{
						bestDist = dist;
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
					geom.texCoords = mesh.texCoords.data();
					geom.numVertices = (uint32)mesh.positions.size();
					geom.indices = mesh.indices.data();
					geom.numIndices = (uint32)mesh.indices.size();
					res.mesh.build(geom); // the upload layout; the source arrays die here, halving what ships
					for (size_t v = 0; v < res.mesh.vertices.size(); ++v)
						res.mesh.vertices[v].tangent = mesh.stitch[v]; // the edge stitch rides the tangent slot (TerrainChunkMesh)
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
		renderer.setTerrainStitch(0.0f, glm::vec2(0.0f), 0.0f, 1.0f, 0); // no chunks: no edge stitching

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
	// query runs beside it, the two sets disjoint. The list holds VALUES and holds until the next spatial kick.
	// main.cpp joins m_renderCounter right before present, and update/clearResidents join it before touching
	// m_residents (nothing mutates it until then). The ocean's sectors, their pass masks and transforms hold until its
	// next update, which follows that same join.
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
						if (!entry.second->spatialEntry.isValid())
							continue; // held: too coarse for the draw camera (edge stitching), not drawn yet
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
						if (userData && !(userData & SpatialTerrainTag_Ocean)) // chunks only (not a 0 userData, not sectors)
						{
							constexpr uint32 SHADOW_AND_GI = RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI;
							const Resident& resident = *reinterpret_cast<const Resident*>(userData);
							renderer.renderNode(resident.node, SHADOW_AND_GI);
							noteVegetation(resident, SHADOW_AND_GI);
						}
					}, SpatialPassBit_Main); // main-stamped: pushed by the fan-out
				}, { "terrainRenderPushSphere", EProfileCategory::Procedural }, EJobPriority::High, &sphereCounter);
			}
			// The ONE walk of the hand-over (zeros dropped at collect): an ocean sector's
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

	// The walk's pushes (on several workers): a node's vegetation, merged per vegetation chunk (a swap pushes
	// the old and the new resident of one coordinate - the trees must not draw twice).
	void TerrainStreamer::noteVegetation(const Resident& resident, uint32 passMask)
	{
		if (!m_vegRouted || !resident.vegetation || resident.vegetationCount.load(oc::memory_order_relaxed) == 0)
			return;
		const uint8 bits = (uint8)passMask;
		const uint32 numBase = 1u << (2 * resident.lod);
		for (uint32 i = 0; i < numBase; ++i)
		{
			const int32 vegetation = resident.vegetation[i].load(oc::memory_order_relaxed);
			if (vegetation < 0 || (size_t)vegetation >= m_vegMasks.size())
				continue;
			const oc::atomic_ref<uint8> mask(m_vegMasks[(size_t)vegetation]);
			if ((mask.load(oc::memory_order_relaxed) & bits) == bits)
				continue;
			if (mask.fetch_or(bits, oc::memory_order_relaxed) == 0) // the first push of this chunk lists it
				m_vegTouched[m_vegTouchedCount.fetch_add(1, oc::memory_order_relaxed)] = (uint32)vegetation;
		}
	}

	// Every base chunk the node covers, from the owner's lookup (main thread). The vegetation's chunks ARE the base chunks.
	void TerrainStreamer::stampVegetation(Resident& resident)
	{
		const uint32 span = 1u << resident.lod;
		if (!resident.vegetation)
			resident.vegetation = oc::make_unique<oc::atomic<int32>[]>(span * span);
		const glm::ivec2 base = resident.firstBase();
		uint32 count = 0;
		for (uint32 z = 0; z < span; ++z)
			for (uint32 x = 0; x < span; ++x)
			{
				const int32 vegetation = m_vegLookup ? m_vegLookup(base + glm::ivec2(x, z)) : -1;
				resident.vegetation[z * span + x].store(vegetation, oc::memory_order_relaxed);
				count += vegetation >= 0 ? 1u : 0u;
			}
		resident.vegetationCount.store(count, oc::memory_order_relaxed);
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
			stampVegetation(*entry.second);
		for (RetiredResident& retired : m_retired) // a hand-over list may still name them: no stale index
			retired.resident->vegetationCount.store(0, oc::memory_order_relaxed);
	}

	// `coord` is a BASE chunk: one entry of every resident node over it changes.
	void TerrainStreamer::restampVegetation(glm::ivec2 coord)
	{
		const int32 vegetation = m_vegLookup ? m_vegLookup(coord) : -1;
		const auto stamp = [&](Resident& resident, int32 value)
		{
			if (!resident.vegetation)
				return;
			const uint32 span = 1u << resident.lod;
			const glm::ivec2 local = coord - resident.firstBase();
			const int32 old = resident.vegetation[(uint32)local.y * span + (uint32)local.x].exchange(value, oc::memory_order_relaxed);
			if ((old >= 0) != (value >= 0))
				resident.vegetationCount.fetch_add(value >= 0 ? 1u : ~0u, oc::memory_order_relaxed);
		};
		for (uint32 lod = 0; lod < 16; ++lod) // every LOD a node can have
			if (const auto it = m_residents.find(chunkKey(glm::ivec2(coord.x >> lod, coord.y >> lod), lod)); it != m_residents.end())
				stamp(*it->second, vegetation);
		for (RetiredResident& retired : m_retired)
		{
			const uint32 lod = retired.resident->lod;
			if (retired.resident->coord == glm::ivec2(coord.x >> lod, coord.y >> lod))
				stamp(*retired.resident, -1);
		}
	}

	void TerrainStreamer::update(Renderer& renderer, const Camera& camera)
	{
		joinRender();   // last frame's render job (already joined before present; a cheap no-op)
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

		// --- THE RING (the quadtree's wanted leaves around the camera; see Ring). Rebuilt when it moves: ~100-200 nodes,
		// so on main, no job.
		const Ring ring{ camChunks, glm::ivec2(camCX, camCZ), R, fullRes, lodStep, maxLod, true };
		const bool ringMoved = !(ring == m_ring);
		if (ringMoved)
		{
			m_ring = ring;
			rebuildWanted();
			m_wantedDirty = true;
		}
		if (!m_drawCam.valid)
			m_drawCam = ring; // nothing drawn (start, or after a clear): no invariant to keep

		// The far cascade must cover the whole resident mesh, else terrain past its edge reads clamp-to-edge
		// (frozen altitude/temperature -> distant inland looks uniformly cold/snowy, ignoring the inland
		// rise). The wanted nodes reach m_wantedHalfExtent past the camera's chunk (a far node can stick out of the ring
		// by up to its own size); the map is a centered square of side = range, so range must be at least twice that
		// (+ a chunk: the camera stands anywhere in its chunk). Keep the tweak as a floor.
		const float meshHalfExtent = glm::max(m_wantedHalfExtent, (float)R * chunkSize) + chunkSize;
		const float farRange = glm::max(m_settings.terrainMapFarRange, 2.0f * meshHalfExtent);
		updateFogHeightMap(renderer, camera, maps, farRange);

		// --- Request every wanted leaf neither resident nor in flight (when the ring moved, or a request ended without a
		// resident). The queue is an unordered POOL, not a queue: the worker rescans it on every dequeue and takes the
		// node nearest the camera at that moment, and drops the ones no longer wanted in the same pass. What it DOES
		// owe the worker is the ring to judge against, published under the same lock before any of its requests.
		if (m_wantedDirty)
		{
			m_wantedDirty = false;
			oc::vector<Request> newRequests;
			for (const uint64 key : m_wantedList)
			{
				if (m_residents.count(key) || m_pending.count(key))
					continue;
				m_pending.insert(key);
				Request req;
				req.key = key;
				req.generation = generation;
				req.maps = maps;
				req.params.coord = keyCoord(key);
				req.params.lod = keyLod(key);
				req.params.chunkSize = chunkSize;
				req.params.lod0Res = (uint32)glm::max(1, m_settings.lod0Res);
				newRequests.push_back(oc::move(req));
			}
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				m_pumpRing = m_ring;
				for (Request& r : newRequests)
					m_requests.push_back(oc::move(r));
			}
			if (!newRequests.empty())
				kickPump(newRequests.size());
		}
		const auto wantedLeaf = [&](uint64 key) { return m_wanted.count(key) != 0; };

		// --- Drain generated chunks (backlog first, then whatever the worker finished), budget-limited.
		oc::vector<Result> ready = oc::move(m_readyBacklog);
		m_readyBacklog.clear();
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			for (Result& r : m_results)
				ready.push_back(oc::move(r));
			m_results.clear();
		}

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
                const bool valid = upload.mesh.isValid() && res.generation == generation
                    && wantedLeaf(res.key) && !m_residents.count(res.key);
                if (!valid)
                {
                    m_wantedDirty = true; // a still-wanted node (a failed upload, an old generation) is requested again
                    continue;             // failed / stale / no longer wanted: the mesh frees here, on main
                }

                // ONE mesh per node (no ObjectContainer: no per-node material, names or node tables) on the
                // terrain pipeline variant (procedural height/slope albedo), with the material every node
                // shares. No LOD chain: the quadtree IS the LOD.
                if (m_material == UINT16_MAX)
                    m_material = renderer.createMeshMaterial(RendererVKLayout::EPipelineIndex::TerrainLit, true);
                auto resident = oc::make_unique<Resident>();
                resident->mesh = oc::move(upload.mesh);
                const float nodeSize = chunkSize * (float)(1 << res.lod);
                const Transform transform(
                    glm::vec3((float)res.coord.x * nodeSize, 0.0f, (float)res.coord.y * nodeSize),
                    1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                resident->node = renderer.spawnMeshNode(resident->mesh, m_material, RendererVKLayout::EPipelineIndex::TerrainLit, transform);
                resident->coord = res.coord;
                resident->lod = res.lod;
                stampVegetation(*resident);
                // NOT registered (drawn) here: updateResidency decides that (the draw camera, the swap group).
                m_residents.emplace(res.key, oc::move(resident));
            }
            m_uploads.resize(kept);
        }
        {
            // --- Pick THIS frame's upload batch (kickUploads, after present). The picks stay "pending"
            // until they are adopted, so they are not requested again.
            ProfileScope profileScope2("processResult", EProfileCategory::Procedural);
            const size_t maxBytes = (size_t)(glm::max(m_settings.maxUploadMBPerFrame, 1.0f) * 1024.0f * 1024.0f);
            size_t batchBytes = 0;
            for (Result& res : ready)
            {
                if (res.mesh.indices.empty()) // pump-dropped (stale at dequeue) or generation failed: just release the key
                {
                    m_pending.erase(res.key); // re-enters the ring as a fresh request if wanted again
                    m_wantedDirty = true;
                    continue;
                }
                const bool valid = (res.generation == generation) && wantedLeaf(res.key) && !m_residents.count(res.key);
                if (!valid)
                {
                    m_pending.erase(res.key); // stale / no longer wanted / duplicate: done with it
                    m_wantedDirty = true;
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

		updateResidency(gate);
		renderer.setTerrainStitch(m_settings.edgeStitch ? chunkSize : 0.0f, m_drawCam.cam,
			m_drawCam.fullRes, glm::max(m_drawCam.lodStep, 0.01f), m_drawCam.maxLod);

		m_renderReady = true; // the chunk push is render()'s (after the ocean's update)

		// The GRASS and the GROUND CLUTTER stand on these nodes (Renderer::setGrassGround): they read the mesh vertices, per
		// BASE chunk. Only the base chunks within their range - a lookup per LOD each, not a walk. A base chunk inside a
		// bigger node is that node's sub-grid (lod0Res >> lod cells, the node's row stride); the finest drawn node wins.
		{
			const float groundRange = renderer.groundRange();
			const float grassRange = groundRange + 16.0f; // + the largest patch
			const uint32 lod0Res = (uint32)glm::max(1, m_settings.lod0Res);
			m_grassGround.clear();
			if (groundRange > 0.0f)
			{
				const glm::ivec2 c0(std::floor((camera.position.x - grassRange) / chunkSize), std::floor((camera.position.z - grassRange) / chunkSize));
				const glm::ivec2 c1(std::floor((camera.position.x + grassRange) / chunkSize), std::floor((camera.position.z + grassRange) / chunkSize));
				for (int z = c0.y; z <= c1.y; ++z)
					for (int x = c0.x; x <= c1.x; ++x)
						for (uint32 lod = 0; lod <= maxLod; ++lod)
						{
							const glm::ivec2 base(x, z);
							const auto it = m_residents.find(chunkKey(glm::ivec2(x >> lod, z >> lod), lod));
							if (it == m_residents.end() || !it->second->mesh.isValid() || !it->second->spatialEntry.isValid())
								continue;
							const uint32 cells = glm::max(1u, lod0Res >> lod);
							const glm::ivec2 sub = base - it->second->firstBase();
							m_grassGround.push_back({ base,
								it->second->mesh.getFirstVertex() + (uint32)sub.y * cells * (lod0Res + 1) + (uint32)sub.x * cells, cells });
							break;
						}
			}
			renderer.setGrassGround(chunkSize, lod0Res + 1, oc::span<const Renderer::GrassGroundChunk>(m_grassGround.data(), m_grassGround.size()));
		}
	}

	// ---- Residency: what draws, per frame, on main (the node counts are small) ----

	// `key` (a wanted leaf) is drawn and can take over from a node being replaced: registered, and - with the culling
	// gate - main-visible, or CULLED by a Main stamp since its registration (it is off screen: no hole can show there;
	// one old node is replaced by many leaves, and the off-screen ones are never main-visible), or the old node is not
	// on screen either. A freshly registered node is not stamped until the next markVisibleSet, so swapping on
	// residency alone opened a one-frame hole on every in-view swap (and while the culling is FROZEN no stamp comes -
	// the old node just stays).
	bool TerrainStreamer::drawnReady(uint64 key, bool oldVisible, bool gate) const
	{
		const auto it = m_residents.find(key);
		if (it == m_residents.end() || !it->second->spatialEntry.isValid())
			return false;
		if (!gate || !oldVisible)
			return true;
		const Resident& leaf = *it->second;
		return (Globals::spatialIndex.getPassMask(leaf.spatialEntry.handle()) & SpatialPassBit_Main) != 0
			|| Globals::spatialIndex.visibleCollectGeneration() != leaf.registeredAt;
	}

	// A resident that is no longer a wanted leaf can go once its whole area is drawn by wanted ones: its wanted
	// ANCESTOR (a coarsening), or every wanted leaf under it (a refinement, down several levels if the bands say so).
	// Parts of it outside the ring or the generated bounds need no cover.
	bool TerrainStreamer::coveredFor(const Resident& old, bool oldVisible, bool gate) const
	{
		const Ring& ring = m_ring;
		for (uint32 lod = old.lod + 1; lod <= ring.maxLod; ++lod)
		{
			const uint64 ancestor = chunkKey(glm::ivec2(old.coord.x >> (lod - old.lod), old.coord.y >> (lod - old.lod)), lod);
			if (m_wanted.count(ancestor))
				return drawnReady(ancestor, oldVisible, gate);
		}
		const float chunkSize = (float)m_settings.chunkSize;
		const auto below = [&](auto&& self, glm::ivec2 coord, uint32 lod) -> bool
		{
			if (!ring.touches(coord, lod) || !insideBounds(coord, lod, chunkSize))
				return true;
			const uint64 key = chunkKey(coord, lod);
			if (m_wanted.count(key))
				return drawnReady(key, oldVisible, gate);
			if (lod == 0)
				return false;
			for (int dz = 0; dz < 2; ++dz)
				for (int dx = 0; dx < 2; ++dx)
					if (!self(self, coord * 2 + glm::ivec2(dx, dz), lod - 1))
						return false;
			return true;
		};
		if (old.lod == 0)
			return !ring.touches(old.coord, 0);
		for (int dz = 0; dz < 2; ++dz)
			for (int dx = 0; dx < 2; ++dx)
				if (!below(below, old.coord * 2 + glm::ivec2(dx, dz), old.lod - 1))
					return false;
		return true;
	}

	// Per frame: (1) retire the residents whose area wanted ones draw; (2) move the draw camera (edge stitching) as far
	// toward the ring camera as the drawn residents allow; (3) register (draw) the wanted residents the draw camera
	// allows - a refinement's leaves all at once, when the last of them landed, so the old node and its replacements
	// never overlap for longer than the one-frame stamp latency. Registrations go LAST: they draw in the main pass from
	// next frame on, when the UBO carries this draw camera too (the begin-frame job that builds it runs before update).
	void TerrainStreamer::updateResidency(bool gate)
	{
		ProfileScope profileScope("Terrain residency", EProfileCategory::Procedural);
		const auto mainVisible = [](const Resident& r)
		{
			return r.spatialEntry.isValid() && (Globals::spatialIndex.getPassMask(r.spatialEntry.handle()) & SpatialPassBit_Main) != 0;
		};

		// (1) Retire.
		m_scratchKeys.clear();
		for (const auto& entry : m_residents)
			if (!m_wanted.count(entry.first))
				m_scratchKeys.push_back(entry.first);
		for (const uint64 key : m_scratchKeys)
		{
			const auto it = m_residents.find(key);
			const Resident& old = *it->second;
			// A held one never drew: nothing to cover.
			if (!old.spatialEntry.isValid() || !m_ring.touches(old.coord, old.lod) || coveredFor(old, mainVisible(old), gate))
			{
				retireResident(oc::move(it->second)); // this frame's hand-over may still hold &node
				m_residents.erase(it);
			}
		}

		// (2) The draw camera: DRAW_CAM_STEPS points from it to the ring camera (the last IS it), on the quarter-chunk
		// lattice, with the ring's bands; the farthest at which every drawn resident lies inside a leaf of that tree.
		if (!(m_drawCam == m_ring))
		{
			Ring cams[DRAW_CAM_STEPS];
			for (uint32 i = 0; i < DRAW_CAM_STEPS; ++i)
			{
				cams[i] = m_ring;
				if (i + 1 < DRAW_CAM_STEPS)
					cams[i].cam = glm::round(glm::mix(m_drawCam.cam, m_ring.cam, (float)(i + 1) / (float)DRAW_CAM_STEPS) * 4.0f) * 0.25f;
			}
			uint32 feasible = (1u << DRAW_CAM_STEPS) - 1u;
			for (const auto& entry : m_residents)
			{
				const Resident& r = *entry.second;
				if (r.lod == 0 || !r.spatialEntry.isValid())
					continue; // LOD0 fits any camera; a held resident is not drawn
				for (uint32 bits = feasible; bits != 0; bits &= bits - 1)
				{
					const uint32 i = oc::tzcnt(bits);
					if (r.lod > cams[i].leafLodAt(r.firstBase()))
						feasible &= ~(1u << i);
				}
				if (feasible == 0)
					break;
			}
			if (feasible != 0)
				m_drawCam = cams[31 - oc::lzcnt(feasible)];
		}

		// (3) Register. A held wanted leaf under a drawn, unwanted ancestor (a refinement) waits for its whole group: every
		// wanted leaf under that ancestor resident and allowed by the draw camera.
		const auto allowed = [&](const Resident& r) { return r.lod <= m_drawCam.leafLodAt(r.firstBase()); };
		const float chunkSize = (float)m_settings.chunkSize;
		for (const uint64 key : m_wantedList)
		{
			const auto it = m_residents.find(key);
			if (it == m_residents.end() || it->second->spatialEntry.isValid() || !allowed(*it->second))
				continue;
			const Resident& leaf = *it->second;
			const Resident* group = nullptr; // the drawn ancestor this leaf replaces
			for (uint32 lod = leaf.lod + 1; lod <= m_ring.maxLod && !group; ++lod)
			{
				const auto a = m_residents.find(chunkKey(glm::ivec2(leaf.coord.x >> (lod - leaf.lod), leaf.coord.y >> (lod - leaf.lod)), lod));
				if (a != m_residents.end() && a->second->spatialEntry.isValid())
					group = a->second.get();
			}
			if (!group)
			{
				registerResident(*it->second);
				continue;
			}
			// Every wanted leaf under the group's node: resident and allowed? Then all of them register now.
			bool complete = true;
			m_scratchKeys.clear();
			const auto collect = [&](auto&& self, glm::ivec2 coord, uint32 lod) -> void
			{
				if (!complete || !m_ring.touches(coord, lod) || !insideBounds(coord, lod, chunkSize))
					return;
				const uint64 k = chunkKey(coord, lod);
				if (m_wanted.count(k))
				{
					const auto r = m_residents.find(k);
					if (r == m_residents.end() || !allowed(*r->second))
						complete = false;
					else if (!r->second->spatialEntry.isValid())
						m_scratchKeys.push_back(k);
					return;
				}
				if (lod == 0)
					return;
				for (int dz = 0; dz < 2; ++dz)
					for (int dx = 0; dx < 2; ++dx)
						self(self, coord * 2 + glm::ivec2(dx, dz), lod - 1);
			};
			for (int dz = 0; dz < 2; ++dz)
				for (int dx = 0; dx < 2; ++dx)
					collect(collect, group->coord * 2 + glm::ivec2(dx, dz), group->lod - 1);
			if (complete)
				for (const uint64 k : m_scratchKeys)
					registerResident(*m_residents.find(k)->second);
		}
	}
}
