export module Procedural:HeightMapBaker;

import Core;
import Core.glm;
import Threading;
import :TerrainSampler;

export namespace Procedural
{
	// A shipped terrain-data bake, shared out for CPU consumers (the ocean's buoyancy sampling and
	// wind-steering votes read the near cascade). Held by shared_ptr: the streamer replaces the whole
	// object when a re-bake ships, so a consumer's copy of the pointer stays valid across the swap.
	// Texel layout matches the GPU map (terrain_height.inc.glsl): cascade-major, res^2 RGBA floats per
	// cascade — R = terrain height (world Y), G = water surface level, B = 4x8 packed bits (flow
	// direction in bits 8-15; bit-cast, never float arithmetic), A = macro altitude.
	struct BakedTerrainData
	{
		oc::vector<float> texels;
		glm::vec2 center = glm::vec2(0.0f);
		glm::vec2 ranges = glm::vec2(0.0f); // world size per cascade
		uint32 res = 0;
		uint32 cascades = 0;
	};

	// Sea level everywhere is a lie the ocean believes. A generator that models no lakes (V3) reports the
	// ocean's level at every point on the planet, so every scrap of terrain within a metre of it — an
	// inland hollow, a river flat, anything — reads as shoreline and the ocean runs swash up it.
	//
	// The swash already has the right gate: it fades out where the baked water level departs from sea
	// level, which is how landlocked water (lakes at altitude) is meant to be excluded. So the fix is not a
	// new rule, it is telling the truth — drop the water level far below any ground the ocean cannot
	// actually reach, and the existing gate does the rest. The terrain shader's beach overlay keys on the
	// same field, so inland sand goes with it.
	//
	// "Can reach" is a DISTANCE question, and this is the only place it is cheap to answer: the bake owns
	// the whole camera-centred height grid, so one chamfer pass gives every texel its distance to real
	// ocean in world metres. A per-point generator query cannot do this — it would have to search its own
	// field per sample — and the coarse detail level has no fine elevation to search at all.
	//
	// Both cascades measure the same world distance (just at their own texel size), so near and far agree.
	//
	// This applies to SUBMERGED ground as much as to dry land: a plateau at -0.05 m is under water by
	// definition, so a land-only rule could never drain one, and those are exactly what V3 produces —
	// kilometres of sea-level film with no ocean within reach of it. The clipmap rides the baked level
	// (the water-table lift in the ocean VS), so sinking it here actually removes the water rather than
	// merely muting its swash.
	// A real shelving bay is kept wet by the RADIUS instead: its apron is too shallow to seed itself, but it
	// sits close to water that does. Radius shorter than the apron = the sea retreats off it.
	//
	// LAKES are never touched: a lake's surface already differs from sea level, so lakes pass through
	// and keep their own shoreline.
	struct WaterReach
	{
		float radius = 4.0f;   // world metres of ocean proximity that still counts as shore
		float feather = 70.0f;  // world metres over which the drop eases in (no hard line on a beach)
		float drop = 1.0f;      // how far below sea level to sink unreachable water. Must clear the swash
		                        // gate's 1 m fade with room to spare; also pushes the beach overlay off.
		// How deep water must be to count as OCEAN and seed reach for the ground around it. Without it any
		// texel a hair under sea level qualifies, so one shallow inland dip vouches for every hollow within
		// the radius of it — the thing this whole pass exists to stop, reintroduced by a puddle. Swell needs
		// a real body of water behind it; this is where that line is drawn. 0 = any water below sea level.
		float swashDepth = 1.0f;

		// Compared to decide whether a baked map went stale (HeightMapBaker::update). Defaulted rather than
		// hand-written so a field added above is covered without anyone remembering to extend it — a missed
		// one would silently leave the tweak doing nothing until the camera happened to move far enough.
		bool operator==(const WaterReach&) const = default;
	};

	// Sinks the water level of texels that no ocean reaches. `texels` is one cascade, res*res*channels with
	// height at [0] and water level at [1]; a chamfer distance transform over the ocean texels drives it.
	inline void applyWaterReach(float* texels, uint32 res, uint32 channels, double texelSize,
	                            float seaLevel, const WaterReach& cfg)
	{
		if (channels < 2 || cfg.drop <= 0.0f || cfg.radius <= 0.0f)
			return;
		const size_t n = (size_t)res * res;
		// Seed: a texel is OCEAN if it is submerged BY AT LEAST swashDepth of water sitting at sea level.
		// Two tests, each load-bearing. The water-level one keeps a lake from seeding reach for its own
		// shore (and from rescuing the plain around it). The depth one keeps a puddle from doing the same:
		// terrain a centimetre under sea level is not something swell arrives from.
		constexpr float kSeaEps = 0.05f; // matches the shader's seaFade toe (ocean_wave.inc.glsl)
		const float seedLevel = seaLevel - oc::max(cfg.swashDepth, 0.0f);
		oc::vector<float> dist(n, FLT_MAX);
		for (size_t i = 0; i < n; ++i)
		{
			const float* t = texels + i * channels;
			if (t[0] <= seedLevel && std::abs(t[1] - seaLevel) <= kSeaEps)
				dist[i] = 0.0f;
		}

		// Two-pass chamfer. Measured worst case ~8% over true Euclidean, so the reach boundary can sit up to
		// ~8% of the radius off where a true Euclidean one would; keep the feather wider than that and it
		// never shows. Distances are in TEXELS here and converted to world metres once at the end.
		const float d1 = 1.0f, d2 = 1.41421356f;
		const auto relax = [&](size_t idx, size_t from, float w)
		{
			const float v = dist[from];
			if (v != FLT_MAX && v + w < dist[idx])
				dist[idx] = v + w;
		};
		for (uint32 j = 0; j < res; ++j)
			for (uint32 i = 0; i < res; ++i)
			{
				const size_t k = (size_t)j * res + i;
				if (j > 0)             relax(k, k - res, d1);
				if (i > 0)             relax(k, k - 1, d1);
				if (j > 0 && i > 0)    relax(k, k - res - 1, d2);
				if (j > 0 && i + 1 < res) relax(k, k - res + 1, d2);
			}
		for (uint32 j = res; j-- > 0; )
			for (uint32 i = res; i-- > 0; )
			{
				const size_t k = (size_t)j * res + i;
				if (j + 1 < res)              relax(k, k + res, d1);
				if (i + 1 < res)              relax(k, k + 1, d1);
				if (j + 1 < res && i + 1 < res) relax(k, k + res + 1, d2);
				if (j + 1 < res && i > 0)      relax(k, k + res - 1, d2);
			}

		const float r0 = cfg.radius;
		const float r1 = cfg.radius + oc::max(cfg.feather, 1.0f);
		for (size_t i = 0; i < n; ++i)
		{
			float* t = texels + i * channels;
			if (std::abs(t[1] - seaLevel) > kSeaEps) // not the ocean's level: a lake, leave it alone
				continue;
			// SUBMERGED GROUND IS DROPPED TOO, deliberately. It is the whole point: a plateau sitting at
			// -0.05 m is under water by definition, so a land-only rule can never take its water away, and
			// V3 grows plenty of them — vast films of sea, kilometres inland, that no swell has ever reached.
			// The ocean's clipmap rides the baked level (the VS water-table lift = level - sea level), so sinking it
			// here genuinely drains them rather than just muting their swash.
			//
			// What protects a REAL shelving bay is the reach RADIUS, not a land test: its apron is shallow
			// (so it never seeds itself) but it is close to water that does, and stays wet on that basis.
			// Set the radius shorter than the apron and the sea WILL retreat off it — that is the knob, and
			// it is the same trade in both directions.
			//
			// Sagging is the risk to watch, and the reason a blanket land-burial was removed from this bake
			// once before: the clipmap's coarse outer rings vertex-sample the level, and a triangle bridging
			// flat sea to a dropped texel tilts the surface. The feather is what keeps that a gentle ramp
			// instead of a cliff — and it only ever happens out past the radius, where the water is meant to
			// be leaving anyway.

			// No ocean anywhere in this map reads as FLT_MAX -> fully dropped, which is the right answer for
			// a map that is all inland. A texel whose nearest ocean lies just outside the map is wrongly
			// dropped, but that is the map's outer edge, and the near cascade crossfades to the far one
			// (which sees much further) exactly there.
			const float dWorld = dist[i] == FLT_MAX ? FLT_MAX : dist[i] * (float)texelSize;
			const float k = glm::clamp((dWorld - r0) / (r1 - r0), 0.0f, 1.0f);
			t[1] = seaLevel - cfg.drop * (k * k * (3.0f - 2.0f * k));
		}
	}

	// The 8-bit flow-direction encoding both baked layouts share (and the shaders' decode mirrors):
	// 0 = no direction, 1..255 = angle / 2pi over [0, 1) in the world XZ plane (0 = +X). Negative input
	// (the sampler's "no direction") encodes 0.
	inline uint32 encodeFlowAngle01(float angle01)
	{
		return angle01 < 0.0f ? 0u : 1u + (uint32)(glm::clamp(angle01, 0.0f, 1.0f) * 254.0f + 0.5f);
	}
	inline uint32 encodeFlowDir(glm::vec2 dirXZ) // need not be normalized
	{
		const float a01 = std::atan2(dirXZ.y, dirXZ.x) * (1.0f / 6.283185307f);
		return encodeFlowAngle01(a01 - std::floor(a01));
	}

	// Direction assignment for the baked 8-bit flow channel — where the local water MOVES. Ocean texels
	// (submerged, water at sea level) within `oceanRange` of land point AT that land: this is what carries
	// waves inland at the coast (the water shader rotates its wave field along it). Everything else — dry
	// ground, lakes, reach-drained flats — points downhill, for rivers/water simulation to build on.
	struct FlowField
	{
		float oceanRange = 250.0f;   // world m: how far offshore the toward-land direction still applies
		// World m at the END of that range over which the direction eases back to the WIND heading, so the
		// encoded field meets the unencoded (= wind-driven) open ocean without a visible turn: past the
		// range "no direction" and "the wind's direction" are the same answer to 8-bit precision.
		float oceanFade = 120.0f;
		// World m of box averaging over the shore directions. The raw nearest-land field is Voronoi
		// piecewise-constant — a jagged coastline flips it texel to texel, and waves would visibly change
		// travel direction along the beach. Averaged as VECTORS, so opposing shores cancel to "none"
		// rather than to a bogus average angle.
		float smoothRadius = 40.0f;
		float minSlope = 0.02f;      // land: slopes flatter than this (m per m) carry no direction
		float windAngle = 0.0f;      // radians in XZ: the swell heading the fade band returns to (the ocean's)

		// Staleness comparison (HeightMapBaker::update), same reasoning as WaterReach's.
		bool operator==(const FlowField&) const = default;
	};

	// Fills the 8-bit flow channel of one baked cascade. Runs AFTER applyWaterReach on purpose: reach
	// drains inland sea-level films, so they classify as land here and get downhill directions instead of
	// pointing at a coast they cannot see. `texels` as in applyWaterReach; flow bits already non-zero
	// (the generator authored a river direction there) are kept, everything else is computed:
	//   - submerged at ocean level, land within reach -> at the nearest land, eased to the wind offshore
	//   - anywhere else                               -> downhill (none where flatter than cfg.minSlope)
	// Cost is a handful of O(n) sweeps (feature transform, running-window blur), independent of the radii.
	inline void applyFlowField(float* texels, uint32 res, uint32 channels, double texelSize,
	                           float seaLevel, const FlowField& cfg)
	{
		if (channels < 4 || cfg.oceanRange <= 0.0f)
			return;
		const size_t n = (size_t)res * res;
		constexpr float kSeaEps = 0.05f; // the same "is this the ocean's level" test as applyWaterReach

		// The 8 flow bits ride bits 8-15 of the bit-cast climate channel (bit ops only — the packed
		// value may form NaN patterns, so no float arithmetic may ever touch it).
		const auto readEnc = [](const float* t) -> uint32
		{
			return (std::bit_cast<uint32>(t[2]) >> 8) & 255u;
		};
		const auto writeEnc = [](float* t, uint32 enc)
		{
			t[2] = std::bit_cast<float>((std::bit_cast<uint32>(t[2]) & ~0x0000FF00u) | (enc << 8));
		};

		// Nearest-land feature transform, dead-reckoning style: the two chamfer sweeps of applyWaterReach,
		// but PROPAGATING the nearest dry texel's coords and scoring candidates by true euclidean distance
		// to them. Directions come out of this — chamfer distances alone visibly bend them near corners.
		constexpr uint32 kNoSeed = 0xFFFFFFFFu;
		oc::vector<uint32> seed(n);  // packed (y << 16 | x) of the nearest non-ocean texel
		oc::vector<float> distSq(n); // squared distance to it, in texels
		enum : uint8 { Land = 0, Shore = 1, OpenOcean = 2 };
		oc::vector<uint8> cls(n);
		for (uint32 j = 0; j < res; ++j)
			for (uint32 i = 0; i < res; ++i)
			{
				const size_t k = (size_t)j * res + i;
				const float* t = texels + k * channels;
				const bool ocean = t[0] < t[1] && std::abs(t[1] - seaLevel) <= kSeaEps;
				seed[k] = ocean ? kNoSeed : (j << 16 | i);
				distSq[k] = ocean ? FLT_MAX : 0.0f;
				cls[k] = ocean ? OpenOcean : Land; // Shore resolves after the transform
			}
		const auto relax = [&](size_t k, int32 x, int32 y, size_t from)
		{
			const uint32 s = seed[from];
			if (s == kNoSeed)
				return;
			const int32 dx = x - (int32)(s & 0xFFFFu), dy = y - (int32)(s >> 16);
			const float d = (float)(dx * dx + dy * dy);
			if (d < distSq[k])
			{
				distSq[k] = d;
				seed[k] = s;
			}
		};
		for (uint32 j = 0; j < res; ++j)
			for (uint32 i = 0; i < res; ++i)
			{
				const size_t k = (size_t)j * res + i;
				if (distSq[k] == 0.0f)
					continue;
				if (j > 0)                relax(k, i, j, k - res);
				if (i > 0)                relax(k, i, j, k - 1);
				if (j > 0 && i > 0)       relax(k, i, j, k - res - 1);
				if (j > 0 && i + 1 < res) relax(k, i, j, k - res + 1);
			}
		for (uint32 j = res; j-- > 0; )
			for (uint32 i = res; i-- > 0; )
			{
				const size_t k = (size_t)j * res + i;
				if (distSq[k] == 0.0f)
					continue;
				if (j + 1 < res)                relax(k, i, j, k + res);
				if (i + 1 < res)                relax(k, i, j, k + 1);
				if (j + 1 < res && i + 1 < res) relax(k, i, j, k + res + 1);
				if (j + 1 < res && i > 0)       relax(k, i, j, k + res - 1);
			}

		// Raw direction per SHORE texel, as (vector, weight). Land and out-of-reach ocean carry weight 0:
		// the averaging below must never pull shore directions toward the beach's own downhill — that
		// points the OPPOSITE way, and the two would cancel exactly at the waterline. Authored (river)
		// directions on shore texels do participate, so a river mouth blends into the surrounding surf.
		const float r1 = cfg.oceanRange;
		const float r0 = glm::max(r1 - glm::max(cfg.oceanFade, 0.0f), 0.0f);
		const glm::vec2 windDir(std::cos(cfg.windAngle), std::sin(cfg.windAngle));
		oc::vector<glm::vec3> field(n, glm::vec3(0.0f));
		for (uint32 j = 0; j < res; ++j)
			for (uint32 i = 0; i < res; ++i)
			{
				const size_t k = (size_t)j * res + i;
				if (distSq[k] == 0.0f || seed[k] == kNoSeed) // land / a map with no land at all
					continue;
				const float dWorld = std::sqrt(distSq[k]) * (float)texelSize;
				if (dWorld > r1)
					continue;
				cls[k] = Shore;
				glm::vec2 dir;
				if (const uint32 enc = readEnc(texels + k * channels); enc != 0)
				{
					const float a = (float)(enc - 1u) * (6.283185307f / 254.0f);
					dir = glm::vec2(std::cos(a), std::sin(a));
				}
				else
				{
					const uint32 s = seed[k];
					dir = glm::normalize(glm::vec2((float)((int32)(s & 0xFFFFu) - (int32)i),
					                               (float)((int32)(s >> 16) - (int32)j)));
					const float f = glm::clamp((dWorld - r0) / glm::max(r1 - r0, 1e-3f), 0.0f, 1.0f);
					const float e = f * f * (3.0f - 2.0f * f);
					dir = dir * (1.0f - e) + windDir * e;
				}
				field[k] = glm::vec3(dir, 1.0f);
			}

		// Separable box average over the shore vectors: windowed running SUMS per axis (O(n) at any
		// radius), mean = sum / summed weight at the end — no per-window normalization needed, and land's
		// zero weight keeps the mask exact.
		const int32 radius = (int32)glm::clamp(cfg.smoothRadius / (float)texelSize + 0.5f, 0.0f, (float)(res / 4));
		if (radius > 0)
		{
			oc::vector<glm::vec3> tmp(n);
			for (uint32 j = 0; j < res; ++j)
			{
				const glm::vec3* src = field.data() + (size_t)j * res;
				glm::vec3* dst = tmp.data() + (size_t)j * res;
				glm::vec3 sum(0.0f);
				for (int32 i = -radius; i < (int32)res; ++i)
				{
					if (i + radius < (int32)res) sum += src[i + radius];
					if (i - radius - 1 >= 0)     sum -= src[i - radius - 1];
					if (i >= 0)                  dst[i] = sum;
				}
			}
			oc::vector<glm::vec3> acc(res, glm::vec3(0.0f)); // per-column running sums, streamed row-major
			for (int32 j = -radius; j < (int32)res; ++j)
			{
				if (j + radius < (int32)res)
				{
					const glm::vec3* row = tmp.data() + (size_t)(j + radius) * res;
					for (uint32 i = 0; i < res; ++i) acc[i] += row[i];
				}
				if (j - radius - 1 >= 0)
				{
					const glm::vec3* row = tmp.data() + (size_t)(j - radius - 1) * res;
					for (uint32 i = 0; i < res; ++i) acc[i] -= row[i];
				}
				if (j >= 0)
					oc::copy(acc.begin(), acc.end(), field.begin() + (size_t)j * res);
			}
		}

		const float gradScale = 1.0f / (2.0f * (float)texelSize);
		for (uint32 j = 0; j < res; ++j)
			for (uint32 i = 0; i < res; ++i)
			{
				const size_t k = (size_t)j * res + i;
				float* t = texels + k * channels;
				if (readEnc(t) != 0)
					continue; // generator-authored flow (rivers) is ground truth — never overwritten
				uint32 enc = 0;
				if (cls[k] == Shore)
				{
					const glm::vec3 f = field[k];
					const glm::vec2 dir = f.z > 0.0f ? glm::vec2(f) * (1.0f / f.z) : glm::vec2(0.0f);
					// Opposing shores (a strait, the middle of a bay) cancel toward zero: that genuinely is
					// "no one direction", and the wind keeps those texels.
					if (glm::dot(dir, dir) > 0.15f * 0.15f)
						enc = encodeFlowDir(dir);
				}
				else if (cls[k] == Land)
				{
					// Downhill of the baked surface (clamped central differences; halved but correctly-aimed
					// gradients on the map's border texels are fine for a direction).
					const float hL = texels[((size_t)j * res + (i > 0 ? i - 1 : 0)) * channels];
					const float hR = texels[((size_t)j * res + (i + 1 < res ? i + 1 : res - 1)) * channels];
					const float hD = texels[((size_t)(j > 0 ? j - 1 : 0) * res + i) * channels];
					const float hU = texels[((size_t)(j + 1 < res ? j + 1 : res - 1) * res + i) * channels];
					const glm::vec2 g = glm::vec2(hR - hL, hU - hD) * gradScale;
					if (glm::dot(g, g) >= cfg.minSlope * cfg.minSlope)
						enc = encodeFlowDir(-g);
				}
				if (enc != 0)
					writeEnc(t, enc);
			}
	}

	// Async single-bake driver for camera-centered terrain height snapshots: res^2 floats of raw surface
	// height (world Y, m) per cascade, cascade-major, sampled from any ITerrainSampler. The terrain
	// streamer bakes the shared terrain-data map through this and ships the result to a renderer-side
	// BakedWorldMap. One bake in flight at a time; a bake pins its sampler via the
	// captured shared_ptr, and the active map keeps working until the replacement lands. Re-bakes when the
	// maps identity or the ranges change, or the camera strays a quarter of the FINEST range from the
	// active center; centers snap to the COARSEST cascade's texel lattice so no cascade's features ever
	// swim between re-bakes.
	class HeightMapBaker
	{
	public:
		struct Baked
		{
			oc::vector<float> texels;
			glm::vec2 center = glm::vec2(0.0f);
			glm::vec2 ranges = glm::vec2(0.0f); // world size per cascade (y = x when single-cascade)
		};

		// Polls the in-flight bake and starts a new one when the active map went stale. Returns true when
		// a completed bake should ship to the renderer (out is filled). active = false drops any finished
		// bake and invalidates (the caller clears its renderer-side map); maps must be non-null while
		// active. numCascades <= 2; ranges.y is ignored when numCascades == 1. channels (interleaved per
		// texel): 1 = terrain height; 2 = height + water level; 4 = the terrain-data layout (4 not 3:
		// RGB32F is not a sampled-image format on most GPUs): (height, water level, PACKED fog thickness
		// | flow direction | temperature | humidity 4x8 bits BIT-CAST into the float texel, MACRO
		// ALTITUDE). The packed channel tolerates no bilinear filtering and no float arithmetic (NaN
		// patterns possible) — shaders decode texels via floatBitsToUint (terrainClimateAt); altitude is
		// a plain float (bilinear-safe). The flow bits carry the generator's authored angle where it has
		// one, the computed toward-land/downhill field where flowField is non-null (applyFlowField).
		// A bake job is outstanding — keep polling update() (active or not) until it drains, so its
		// result is consumed the frame it lands and a disabled consumer knows when it can stop calling.
		bool inFlight() const { return m_bakeInFlight; }

		bool update(Baked& out, bool active, const oc::shared_ptr<const ITerrainSampler>& maps,
			const glm::vec2& camXZ, glm::vec2 ranges, uint32 res, uint32 numCascades, uint32 channels = 1,
			const WaterReach* waterReach = nullptr, const FlowField* flowField = nullptr)
		{
			bool shipped = false;
			if (m_bakeInFlight && m_bakeCounter.isDone())
			{
				oc::vector<float> texels = oc::move(m_bake->result);
				m_bakeInFlight = false;
				if (active) // a bake finishing after its consumer got disabled is dropped
				{
					out.texels = oc::move(texels);
					out.center = m_pendingCenter;
					out.ranges = m_pendingRanges;
					m_activeCenter = m_pendingCenter;
					m_activeRanges = m_pendingRanges;
					m_activeMaps = m_pendingMaps;
					m_activeReach = m_pendingReach;
					m_activeReachOn = m_pendingReachOn;
					m_activeFlow = m_pendingFlow;
					m_activeFlowOn = m_pendingFlowOn;
					m_valid = true;
					shipped = true;
				}
			}
			if (!active)
			{
				m_valid = false;
				return false;
			}
			if (m_bakeInFlight)
				return shipped; // one bake in flight at a time

			ranges.x = glm::max(ranges.x, 1.0f);
			ranges.y = numCascades > 1 ? glm::max(ranges.y, ranges.x) : ranges.x;
			// The reach rule is BAKED INTO the water-level channel, so changing it invalidates the map the
			// same way a new sampler or range does. Without this the tweaks appear dead: nothing re-bakes
			// until the camera drifts a quarter of a range, and a slider you have to fly away from to test
			// is worse than no slider.
			const WaterReach reachNow = waterReach ? *waterReach : WaterReach{};
			const bool reachOnNow = waterReach != nullptr;
			const FlowField flowNow = flowField ? *flowField : FlowField{};
			const bool flowOnNow = flowField != nullptr;
			const glm::vec2 drift = glm::abs(camXZ - m_activeCenter);
			const bool stale = !m_valid
				|| m_activeMaps != maps.get()
				|| m_activeRanges != ranges
				|| m_activeReachOn != reachOnNow
				|| (reachOnNow && !(m_activeReach == reachNow))
				|| m_activeFlowOn != flowOnNow
				|| (flowOnNow && !(m_activeFlow == flowNow))
				|| glm::max(drift.x, drift.y) > ranges.x * 0.25f;
			if (!stale)
				return shipped;

			// Snap the shared center to the COARSEST cascade's texel lattice: every cascade's texels then
			// re-land on the exact same world positions bake after bake (with the default range ratio the
			// finer lattice divides the coarser), so re-bakes reproduce identical values where the terrain
			// is unchanged. Snapping to the finest lattice instead let the coarse cascade's texels shift
			// sub-coarse-texel per bake — on steep coasts a 16 m texel's height then jumped meters between
			// bakes, and consumers thresholding the field (the ocean land cull) flipped visibly.
			const float texel = ranges[numCascades > 1 ? 1 : 0] / float(res);
			const glm::vec2 center = glm::floor(camXZ / texel + 0.5f) * texel;
			m_pendingCenter = center;
			m_pendingRanges = ranges;
			m_pendingMaps = maps.get();
			m_pendingReach = reachNow;
			m_pendingReachOn = reachOnNow;
			m_pendingFlow = flowNow;
			m_pendingFlowOn = flowOnNow;
			// The bake parameters exceed the job's 64-byte inline capture - box them (result rides
			// in the same box, written before the counter signals; read only after isDone).
			m_bake = oc::make_shared<BakeJob>(BakeJob{ maps, center, ranges, res, numCascades, channels,
				reachNow, reachOnNow, flowNow, flowOnNow, {} });
			m_bakeInFlight = true;
			Globals::jobSystem.submit([bake = m_bake]() {

				const oc::shared_ptr<const ITerrainSampler>& maps = bake->maps;
				const glm::vec2 center = bake->center, ranges = bake->ranges;
				const uint32 res = bake->res, numCascades = bake->numCascades, channels = bake->channels;
				const bool applyReach = bake->applyReach, applyFlow = bake->applyFlow;
				const WaterReach reach = bake->reach;
				const FlowField flow = bake->flow;
				oc::vector<float> heights((size_t)numCascades * res * res * channels);
				oc::vector<TerrainPoint> points; // reused across cascades
				for (uint32 c = 0; c < numCascades; ++c)
				{
					float* dst = heights.data() + (size_t)c * res * res * channels;
					const double range = (double)ranges[(int)c];
					const double texelSize = range / (double)res;
					const double x0 = (double)center.x - range * 0.5 + texelSize * 0.5; // texel centers
					const double z0 = (double)center.y - range * 0.5 + texelSize * 0.5;
					// Cascade 0 is the near/fine map; every later cascade spans tens of km at the same texel
					// count, so ask for the cheap approximation there. For a generator with a real per-point
					// cost (V3) that is the difference between a handful of coarse tiles and thousands of
					// full-detail ones covering terrain mostly beyond the mesh ring — see ESampleDetail. The
					// shader crossfades near->far, so the fidelity step blends in instead of seaming.
					const ESampleDetail detail = (c == 0) ? ESampleDetail::Full : ESampleDetail::Coarse;

					// ONE grid call per cascade, not res*res point calls: it lets the sampler resolve
					// whatever it needs (V3: its tile set, under one lock each) before touching a texel.
					points.resize((size_t)res * res);
					maps->sampleGrid(x0, z0, texelSize, res, res, points, detail);

					for (uint32 j = 0; j < res; ++j)
						for (uint32 i = 0; i < res; ++i)
						{
							const TerrainPoint& p = points[(size_t)j * res + i];
							float* texel = dst + ((size_t)j * res + i) * channels;
							texel[0] = p.height;
							if (channels > 1)
								texel[1] = p.waterLevel;
							if (channels > 3)
							{
								// 4x8-bit climate pack: fog thickness [0,1] | FLOW DIRECTION (bits 8-15,
								// encodeFlowAngle01; the sampler's authored angle, applyFlowField fills the
								// computed field where it has none) | SEA-LEVEL TEMPERATURE | humidity [0,1].
								// 32 bits exceed float32's exact-integer range, so the bits are BIT-CAST into
								// the texel (std::bit_cast here, floatBitsToUint in terrain_height.inc.glsl).
								// The whole path — vector moves, staging memcpy, copyBufferToImage, texelFetch
								// — carries raw bits with NO float arithmetic; some packs form NaN bit
								// patterns, which any arithmetic would corrupt. Keep it bit-exact end to end.
								// Temperature is stored as the SEA-LEVEL BASELINE, not as a sample: a sample
								// is only valid at the height it was taken from, and the two cascades bake
								// different heights for the same spot — baking samples left the far cascade
								// 10.6 C too warm at peaks and its texture flipped at the crossfade. A
								// baseline has no anchor to disagree about: consumers evaluate it at the
								// height they shade (terrainTemperatureAt) against the generator's one
								// published lapse rate (u_terrainParams.y).
								const auto q8 = [](float f) { return (uint32)(glm::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
								const uint32 packed = q8(p.fogThickness)
									| (encodeFlowAngle01(p.flowAngle01) << 8)
									| (q8(temperatureTo01(p.temperatureSeaLevel)) << 16)
									| (q8(p.humidity) << 24);
								texel[2] = std::bit_cast<float>(packed);
								texel[3] = p.altitude; // macro elevation (terrain coloring)
							}
						}
					// NOTE: the water-level channel bakes EXACTLY what the sampler reports — no post-processing.
					// A land-burial dive (sinking the level under land texels against standard-Z depth noise)
					// briefly lived here and backfired: the ocean clipmap's coarse outer rings vertex-sample
					// this channel, and one triangle bridging a flat water texel to a deeply dived land texel
					// visibly sagged the far sea near coasts. Reversed-Z made any such separation unnecessary.

					// ... with ONE exception, and it is the reverse case: water the ocean cannot REACH. See
					// applyWaterReach. Done here, after the grid exists, because it is a distance question.
					if (applyReach)
						applyWaterReach(dst, res, channels, texelSize, maps->seaLevel(), reach);
					// Flow directions are a distance question too (toward the nearest land), so they are
					// also computed here — and after the reach pass, so drained inland films point downhill
					// instead of at a coast the ocean never reaches them from.
					if (applyFlow)
						applyFlowField(dst, res, channels, texelSize, maps->seaLevel(), flow);
				}
				bake->result = oc::move(heights);
			}, { "HeightMapBaker::update", EProfileCategory::Procedural }, EJobPriority::Low, &m_bakeCounter);
			return shipped;
		}

		bool hasActiveMap() const { return m_valid; }

		// Callers must destroy this after draining (both owners outlive the frame); the wait covers
		// a bake still in flight at teardown.
		~HeightMapBaker() { Globals::jobSystem.wait(m_bakeCounter); }

	private:
		struct BakeJob
		{
			oc::shared_ptr<const ITerrainSampler> maps;
			glm::vec2 center, ranges;
			uint32 res, numCascades, channels;
			WaterReach reach;
			bool applyReach;
			FlowField flow;
			bool applyFlow;
			oc::vector<float> result;
		};
		oc::shared_ptr<BakeJob> m_bake;
		JobCounter m_bakeCounter;
		bool m_bakeInFlight = false;
		WaterReach m_activeReach;      // the reach rule baked into the live map (see update)
		bool m_activeReachOn = false;
		WaterReach m_pendingReach;     // ... and the one the in-flight bake is using
		bool m_pendingReachOn = false;
		FlowField m_activeFlow;        // same active/pending pair for the flow-direction rule
		bool m_activeFlowOn = false;
		FlowField m_pendingFlow;
		bool m_pendingFlowOn = false;
		glm::vec2 m_pendingCenter = glm::vec2(0.0f); // inputs of the IN-FLIGHT bake
		glm::vec2 m_pendingRanges = glm::vec2(0.0f);
		const ITerrainSampler* m_pendingMaps = nullptr;  // identity only (never dereferenced)
		glm::vec2 m_activeCenter = glm::vec2(0.0f);  // inputs of the ACTIVE (shipped) map
		glm::vec2 m_activeRanges = glm::vec2(0.0f);
		const ITerrainSampler* m_activeMaps = nullptr;   // identity only (never dereferenced)
		bool m_valid = false;
	};
}
