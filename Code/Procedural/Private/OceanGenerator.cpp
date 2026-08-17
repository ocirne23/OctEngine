module Procedural;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;
import Core.Tweaks;
import Core.Time;

import RendererVK;
import File;
import Spatial;

import :OceanGenerator;
import :TerrainSampler;

namespace Procedural
{
	OceanGenerator::~OceanGenerator()
	{
		// Free the sectors while the renderer/device are still alive (each node before its container).
		m_sectors.clear();
	}

	void OceanGenerator::initialize()
	{
		auto gridDirty = [this]() { m_gridDirty = true; };

		Tweak::boolean("Ocean", "Enabled", &m_enabled);
		Tweak::floatVar("Ocean", "Ring cell (m)", &m_ringCell, 0.02f, 2.0f, 0.005f, gridDirty);
		Tweak::intVar("Ocean", "Ring resolution", &m_ringRes, 64, 512, 4.0f, gridDirty);
		Tweak::intVar("Ocean", "Rings", &m_rings, 1, 10, 1.0f, gridDirty);
		// One coarse quad band from the outermost ring's edge to the camera far plane, so the sea always
		// reaches the horizon; its geometry is band-limited to the coarsest mips (near-flat), which is
		// exactly what sub-pixel waves at that distance resolve to anyway.
		Tweak::boolean("Ocean", "Horizon band", &m_horizonBand, gridDirty);
		// The band is exempt from the land cull (its triangles far exceed the cull's footprint bound),
		// so it draws over distant terrain: sink it a little and its crests stay under ground sitting
		// near sea level. Only the band moves, so large values leave a step at its inner seam.
		Tweak::floatVar("Ocean", "Horizon level offset (m)", &m_horizonLevelOffset, -20.0f, 5.0f, 0.1f);
		// Negative = displacement sampled finer than the ring's Nyquist (slight shimmer while moving);
		// with fixed-cell rings the default 0 is already motion-stable.
		Tweak::floatVar("Ocean", "Detail bias", &m_detailBias, -2.0f, 2.0f, 0.05f);

		// TMA/JONSWAP spectrum inputs (Horvath 2015); re-evaluated on the GPU every frame, so all live.
		Tweak::floatVar("Ocean/Waves", "Wind speed (m/s)", &m_windSpeed, 0.0f, 40.0f, 0.1f);
		Tweak::floatVar("Ocean/Waves", "Fetch (km)", &m_fetchKm, 1.0f, 2000.0f, 1.0f);
		Tweak::floatVar("Ocean/Waves", "Depth (m)", &m_depth, 1.0f, 500.0f, 0.5f);
		Tweak::floatVar("Ocean/Waves", "Wind angle (rad)", &m_windAngle, 0.0f, 6.2831853f, 0.01f);
		// Flow -> wind steering: near a coast the SIMULATION wind turns toward the baked flow directions
		// (waves roll in toward the local shore); away from any shore it returns to the wind angle above.
		Tweak::boolean("Ocean/Waves", "Flow steers wind", &m_windSteerEnabled);
		Tweak::floatVar("Ocean/Waves", "Steer rate (deg/s)", &m_windSteerRate, 0.0f, 90.0f, 0.5f);
		Tweak::floatVar("Ocean/Waves", "Steer range (m)", &m_windSteerRange, 0.0f, 2000.0f, 10.0f);
		Tweak::floatVar("Ocean/Waves", "Amplitude scale", &m_amplitude, 0.0f, 4.0f, 0.01f);
		Tweak::floatVar("Ocean/Waves", "Choppiness", &m_choppiness, 0.0f, 2.5f, 0.01f);
		Tweak::floatVar("Ocean/Waves", "Normal strength", &m_normalStrength, 0.0f, 4.0f, 0.01f);
		Tweak::floatVar("Ocean/Waves", "Cascade 0 (m)", &m_cascadeSizes.x, 16.0f, 2000.0f, 1.0f);
		Tweak::floatVar("Ocean/Waves", "Cascade 1 (m)", &m_cascadeSizes.y, 4.0f, 500.0f, 0.5f);
		Tweak::floatVar("Ocean/Waves", "Cascade 2 (m)", &m_cascadeSizes.z, 1.0f, 100.0f, 0.1f);

		Tweak::color3("Ocean/Shading", "Absorption (1/m)", &m_absorption);
		Tweak::color3("Ocean/Shading", "Scatter color", &m_scatterColor);
		Tweak::floatVar("Ocean/Shading", "Scatter strength", &m_scatterStrength, 0.0f, 4.0f, 0.01f);
		Tweak::floatVar("Ocean/Shading", "Roughness", &m_roughness, 0.02f, 0.5f, 0.001f);
		// Sharper sun glints: sharpness biases the shading-normal mips finer (some shimmer past ~1.5),
		// filtering scales the roughness-widening variance terms (0 = raw sharp GGX, 1 = fully filtered).
		Tweak::floatVar("Ocean/Shading", "Glint filtering", &m_glintFilter, 0.0f, 2.0f, 0.05f);
		// Crest SSS (Sea of Thieves-style): sun shining through back-lit crests, scaled by wave height.
		Tweak::floatVar("Ocean/Shading", "SSS strength", &m_sssStrength, 0.0f, 4.0f, 0.01f);
		Tweak::floatVar("Ocean/Shading", "SSS power", &m_sssPower, 1.0f, 16.0f, 0.1f);
		Tweak::boolean("Ocean/Shading", "Hit lighting", &m_hitLighting); // lights on geometry seen through/mirrored in the water
		Tweak::color3("Ocean/Shading", "Foam color", &m_foamColor);
		// One instant-foam response (thresholds + softness) draws the crest foam AND injects the
		// turbulence field; decay/spread shape the wake's life, foam boost/turbidity its look.
		Tweak::floatVar("Ocean/Foam", "Fold bias", &m_foamBias, 0.0f, 1.2f, 0.01f);
		Tweak::floatVar("Ocean/Foam", "Break accel (g)", &m_foamBreakAccel, 0.05f, 1.5f, 0.01f);
		Tweak::floatVar("Ocean/Foam", "Softness", &m_foamSoftness, 0.02f, 2.0f, 0.01f);
		Tweak::floatVar("Ocean/Foam", "Turb decay", &m_foamDecay, 0.5f, 0.999f, 0.001f);
		Tweak::floatVar("Ocean/Foam", "Turb spread", &m_foamSpread, 0.0f, 4.0f, 0.05f);
		Tweak::floatVar("Ocean/Foam", "Foam boost", &m_foamBoost, 0.0f, 2.0f, 0.01f);
		Tweak::floatVar("Ocean/Foam", "Turbidity", &m_turbidity, 0.0f, 1.0f, 0.01f);

		// Shore interaction: driven by the terrain streamer's baked terrain-data map (nothing baked here;
		// no data while terrain rendering is disabled — the ocean then behaves as open sea).
		Tweak::floatVar("Ocean/Shore", "Shoal depth scale", &m_shoalScale, 0.0f, 0.1f, 0.001f);
		// Past "range" the waves assume at least "Horizon depth" of water whatever the map says (see the
		// header): distant depth readings all err shallow, and shallow reads as a dead mirror sea. Only
		// the assumed seabed moves — the surface stays put, so this cannot put water over land.
		Tweak::floatVar("Ocean/Shore", "Horizon depth (m)", &m_horizonDepth, 0.0f, 200.0f, 1.0f);
		Tweak::floatVar("Ocean/Shore", "Horizon depth range (m)", &m_horizonDepthRange, 0.0f, 8000.0f, 50.0f);
		// A wave cannot stand taller than the water it is in (H/d ~ 0.78 when it breaks). The shoal fade
		// alone cannot enforce this: it fades a band by its WAVELENGTH, so the mid cascade still runs at
		// full height in shallow water. Scales the shoaled sum, so waves shrink instead of flat-topping.
		Tweak::floatVar("Ocean/Shore", "Wave height limit (x depth)", &m_waveHeightLimit, 0.0f, 2.0f, 0.01f);
		Tweak::floatVar("Ocean/Shore", "Shore foam depth (m)", &m_shoreFoamDepth, 0.0f, 8.0f, 0.05f);
		Tweak::floatVar("Ocean/Shore", "Shore foam max", &m_shoreFoamMax, 0.0f, 1.0f, 0.01f);
		Tweak::floatVar("Ocean/Shore", "Swash amplitude", &m_swashAmp, 0.0f, 2.0f, 0.01f);
		Tweak::floatVar("Ocean/Shore", "Swash drawdown (m)", &m_swashDrawdown, 0.00f, 2.0f, 0.01f);
		// How far above the seabed the TROUGH is held. The clamp measures against the baked
		// depth map, but you see the LOD'd terrain mesh, and the two disagree by decimetres — so the old
		// hard-coded 5 cm let a trough that clears the map's seabed sink under the real ground, which then
		// pokes through the surface. This is the margin for that error, so size it to the disagreement, not
		// to the waves. Tapered in with depth, so the waterline does not lift and retreat.
		Tweak::floatVar("Ocean/Shore", "Trough margin (m)", &m_troughMargin, 0.0f, 1.0f, 0.01f);
		Tweak::floatVar("Ocean/Shore", "Shore foam bias", &m_shoreFoamBias, -1.0f, 1.0f, 0.01f);
		Tweak::floatVar("Ocean/Shore", "Swash backflow", &m_swashFlow, 0.0f, 3.0f, 0.01f);
		// Land cull: clipmap triangles buried deeper than this under the local water level (over their
		// whole footprint) are discarded in the vertex shaders — no displacement sampling, no raster.
		Tweak::floatVar("Ocean/Shore", "Cull margin (m)", &m_cullMargin, 0.0f, 4.0f, 0.05f);
		// Beyond the near terrain cascade (~860 m) the cull uses the FAR cascade with this flat burial
		// error allowance in meters (covers the far mesh LODs' drift off the bake). Narrow rivers the
		// coarse far bake cannot resolve may lose triangles out there — speed over accuracy; raise it
		// if that shows, 0 = never cull from far data.
		Tweak::floatVar("Ocean/Shore", "Far cull error (m)", &m_farCullError, 0.0f, 20.0f, 0.25f);
		// Whole-sector skip when the baked terrain buries a sector's entire footprint (see the header):
		// deep inland the ocean then costs nothing at all.
		Tweak::boolean("Ocean/Shore", "Dry sector cull", &m_drySectorCull);

		// Ray-tracing budget: the water shader traces the scene TLAS per pixel for refraction (seeing
		// geometry through the water) and reflection (scenery mirrored in it). Refraction range = how far
		// underwater stays visible (the ~99% Beer-Lambert extinction bound still applies on top, so this
		// caps the clear-water case); reflection rays skip above the roughness cutoff (a wide lobe cannot
		// be represented by one mirror sample — the blurred sky stands in); the ray cutoff distance stops
		// ALL rays past that camera distance (refraction falls back to the analytic baked-terrain bottom,
		// reflections to the atmosphere — the same paths misses already take), 0 = unlimited.
		Tweak::floatVar("Ocean/RT", "Refraction range (m)", &m_rtRefractionRange, 1.0f, 100.0f, 1.0f);
		Tweak::floatVar("Ocean/RT", "Reflection range (m)", &m_rtReflectionRange, 50.0f, 10000.0f, 50.0f);
		Tweak::floatVar("Ocean/RT", "Reflection max rough", &m_rtReflectionMaxRough, 0.0f, 1.0f, 0.01f);
		Tweak::floatVar("Ocean/RT", "Ray cutoff dist (m)", &m_rtRayCutoffDist, 0.0f, 10000.0f, 50.0f);
	}

	void OceanGenerator::rebuildGrid()
	{
		m_gridDirty = false;
		m_sectors.clear(); // release the previous grid first (nodes before their containers)

		// Geometry clipmap: ring 0 is a full NxN-cell grid at m_ringCell; each outer ring is a square
		// annulus at double the cell size whose hole is the previous ring's coverage. Per vertex, the
		// texcoord carries (ring cell size, morph weight): the vertex shaders read them to pick the
		// ring-matched displacement mip and to run the CDLOD boundary morph (over each ring's outer band,
		// odd vertices collapse onto the next ring's lattice and the mip blends +1, so adjacent rings meet
		// exactly — no stitching geometry needed).
		// Built as SECTORS (terrain-chunk style): ring 0 whole, each annulus as 8 rectangular blocks
		// around its hole, the horizon band as its 4 sides — each its own container/node + spatial entry
		// so both cull paths (Spatial gate + GPU per-instance frustum test) drop off-screen water.
		// Sector borders duplicate identical vertices: same position, cell size and morph -> watertight.
		const int   N = glm::clamp(m_ringRes & ~3, 16, 1024); // multiple of 4: hole/sector edges stay on the lattice
		const float c0 = glm::max(m_ringCell, 0.01f);
		const int   rings = glm::clamp(m_rings, 1, 12);
		constexpr float MORPH_BAND_START = 0.7f; // morph over the outer 30% of each ring

		oc::vector<glm::vec3> positions;
		oc::vector<glm::vec3> normals;
		oc::vector<glm::vec3> texCoords;
		oc::vector<uint32> indices;

		// Wraps the accumulated arrays into one sector: container + node + SpatialIndex registration
		// (SpatialLayer_Terrain like terrain chunks — render culling only, invisible to gameplay
		// queries; no spawn guard, sectors surround the camera and stamp on the next markVisibleSet).
		bool emittingHorizonBand = false;
		const auto emitSector = [&]() {
			if (indices.empty())
				return;
			MeshGeometryDesc geom;
			geom.positions = positions.data();
			geom.normals = normals.data();
			geom.texCoords = texCoords.data();
			geom.numVertices = (uint32)positions.size();
			geom.indices = indices.data();
			geom.numIndices = (uint32)indices.size();
			geom.name = "Ocean";

			oc::unique_ptr<ISceneData> scene = ISceneData::createMeshScene(geom);
			if (scene)
			{
				ObjectContainer::MaterialOverrides overrides;
				overrides.pipelineIdx = RendererVKLayout::EPipelineIndex::Ocean;
				overrides.useSceneTextures = true;
				overrides.excludeFromRayTracing = true; // the animated water surface isn't in the TLAS
				// No meshopt LOD chains (same as terrain chunks): the clipmap IS its own LOD — a generated
				// level would collapse the lattice the CDLOD morph and ring-matched mips depend on. The
				// old single-mesh ocean carried chains too but projected too large to ever leave LOD0;
				// per-sector meshes are small enough that the selector actually used them (stretched
				// triangles, per-sector pops, cracked borders).
				overrides.disableGeneratedLods = true;
				auto container = oc::make_unique<ObjectContainer>();
				if (container->initialize(*scene, &overrides))
				{
					Sector& s = m_sectors.emplace_back();
					s.node = container->spawnRootNode(
						Transform(glm::vec3(0.0f, m_seaLevel, 0.0f), 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
					s.container = oc::move(container);
					glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
					for (const glm::vec3& p : positions)
					{
						mn = glm::min(mn, p);
						mx = glm::max(mx, p);
					}
					s.localCenter = (mn + mx) * 0.5f;
					s.halfXZ = glm::vec2(mx.x - mn.x, mx.z - mn.z) * 0.5f;
					s.horizonBand = emittingHorizonBand;
					s.radius = glm::length((mx - mn) * 0.5f) + 8.0f; // headroom for wave/swash displacement
					s.spatialEntry = SpatialEntry(Globals::spatialIndex.registerEntry(
						glm::dvec3(s.localCenter) + glm::dvec3(0.0, m_seaLevel, 0.0), s.radius, 0ull,
						SpatialLayer_Terrain, false));
				}
			}
			positions.clear();
			normals.clear();
			texCoords.clear();
			indices.clear();
		};

		// Vertices + quads for cell range [i0,i1) x [j0,j1) of ring r (vertex grid is the +1 superset).
		const auto emitBlock = [&](int r, int i0, int i1, int j0, int j1) {
			const float cell = c0 * float(1 << r);
			const float outerH = cell * float(N) * 0.5f; // this ring's coverage half-extent
			const uint32 base = (uint32)positions.size();
			const int w = i1 - i0 + 1;
			for (int j = j0; j <= j1; ++j)
			{
				for (int i = i0; i <= i1; ++i)
				{
					const float x = float(i - N / 2) * cell;
					const float z = float(j - N / 2) * cell;
					const float cheb = glm::max(std::fabs(x), std::fabs(z));
					const float morph = glm::clamp((cheb - MORPH_BAND_START * outerH) / ((1.0f - MORPH_BAND_START) * outerH), 0.0f, 1.0f);
					positions.push_back(glm::vec3(x, 0.0f, z));
					normals.push_back(glm::vec3(0.0f, 1.0f, 0.0f));
					texCoords.push_back(glm::vec3(cell, morph, 0.0f));
				}
			}
			for (int j = j0; j < j1; ++j)
			{
				for (int i = i0; i < i1; ++i)
				{
					const uint32 a = base + (uint32)((j - j0) * w + (i - i0));
					const uint32 b = a + 1;
					const uint32 c = a + (uint32)w;
					const uint32 d = c + 1;
					indices.push_back(a); indices.push_back(c); indices.push_back(b);
					indices.push_back(b); indices.push_back(c); indices.push_back(d);
				}
			}
		};

		// Ring 0: one sector (always around the camera, never worth splitting).
		emitBlock(0, 0, N, 0, N);
		emitSector();
		// Outer rings: the hole is exactly the central [N/4, 3N/4) cell block (the previous ring's
		// coverage), leaving 4 corner blocks + 4 edge strips.
		const int q = N / 4, q3 = 3 * N / 4;
		for (int r = 1; r < rings; ++r)
		{
			emitBlock(r, 0, q, 0, q);    emitSector(); // corners
			emitBlock(r, q3, N, 0, q);   emitSector();
			emitBlock(r, 0, q, q3, N);   emitSector();
			emitBlock(r, q3, N, q3, N);  emitSector();
			emitBlock(r, q, q3, 0, q);   emitSector(); // edge strips
			emitBlock(r, q, q3, q3, N);  emitSector();
			emitBlock(r, 0, q, q, q3);   emitSector();
			emitBlock(r, q3, N, q, q3);  emitSector();
		}

		// Horizon band: a coarse quad ring from the outermost ring's edge to the camera far plane, so
		// the sea reaches the horizon in every view direction instead of ending at the ring reach. Its
		// inner edge sits on the last ring's fully-morphed lattice (2x its cell, which the CDLOD morph
		// collapses the edge onto — morph hits exactly 1 there) with the matching texcoord cell size, so
		// the seam is watertight and mip-continuous by the same construction the rings use. Outer verts
		// carry a proportionally scaled cell size: they band-limit to the coarsest mips (near-flat), which
		// is what sub-pixel waves at that distance resolve to anyway; shading stays per-pixel regardless.
		// Chebyshev half-extent = far plane covers every Euclidean far-plane point (diagonals clip first).
		// One sector per side (corner vertices duplicated between sides — identical, watertight).
		// The cell size is stored NEGATED: that flags the band to the vertex shaders, which take abs()
		// for the mip/morph math and skip the land cull. The cull deletes a vertex once its +-3-cell
		// footprint is buried, which assumes no co-triangle vertex lies further away — true for the
		// rings, FALSE here (a band triangle spans from the ring edge out to the far plane), so one
		// buried inner vertex would take a kilometre-wide slice of the horizon with it.
		if (m_horizonBand && m_lastFar > 0.0f)
		{
			const float lastCell = c0 * float(1 << (rings - 1));
			const float outerH = lastCell * float(N) * 0.5f;
			if (m_lastFar > outerH * 1.01f)
			{
				const float step = 2.0f * lastCell;
				const float scale = m_lastFar / outerH;
				const int M = N / 2; // segments per side on the morphed lattice
				emittingHorizonBand = true;
				const glm::vec2 corners[5] = {
					{ -outerH, -outerH }, { outerH, -outerH }, { outerH, outerH }, { -outerH, outerH }, { -outerH, -outerH } };
				for (int side = 0; side < 4; ++side)
				{
					const glm::vec2 a = corners[side];
					const glm::vec2 d = (corners[side + 1] - a) / float(M);
					for (int k = 0; k <= M; ++k)
					{
						const glm::vec2 p = a + d * float(k);
						positions.push_back(glm::vec3(p.x, 0.0f, p.y));
						normals.push_back(glm::vec3(0.0f, 1.0f, 0.0f));
						texCoords.push_back(glm::vec3(-step, 0.0f, 0.0f)); // negative = horizon band
					}
					for (int k = 0; k <= M; ++k)
					{
						const glm::vec2 p = (a + d * float(k)) * scale;
						positions.push_back(glm::vec3(p.x, 0.0f, p.y));
						normals.push_back(glm::vec3(0.0f, 1.0f, 0.0f));
						texCoords.push_back(glm::vec3(-step * scale, 0.0f, 0.0f)); // negative = horizon band
					}
					for (uint32 k = 0; k < (uint32)M; ++k)
					{
						const uint32 in0 = k, in1 = k + 1;
						const uint32 out0 = (uint32)(M + 1) + k, out1 = out0 + 1;
						indices.push_back(in0); indices.push_back(in1); indices.push_back(out0);
						indices.push_back(in1); indices.push_back(out1); indices.push_back(out0);
					}
					emitSector();
				}
			}
		}
	}

	// Baked flow -> simulation wind. The FFT field travels along the wind its SPECTRUM was built with, and
	// the spectrum regenerates every frame — so turning that wind is the one continuous way to make the
	// whole sea (every cascade, chop, foam) genuinely travel toward the local shore. The per-pixel
	// alternative — rotating each texel's sample position by its own flow angle (oceanFlowRotation) — is
	// disabled in the shader: the rotation pivots on the world origin, so the sea creased along every
	// 8-bit angle contour, worse with distance.
	// Here the baked shore directions around the camera vote (ocean texels carrying a direction; land and
	// undirected open sea abstain) and the sim wind slews toward their mean at a bounded rate, morphing
	// the spectrum smoothly through the turn. The baked field itself eases back to the base wind offshore
	// (FlowField::oceanFade), so far-from-land votes already agree with the tweak angle and leaving the
	// coast hands back to it by construction.
	float OceanGenerator::steeredWindAngle(const Camera& camera)
	{
		if (!m_windSteerEnabled || m_windSteerRate <= 0.0f)
		{
			m_windSteerSynced = false; // re-adopt the base wind when steering comes back on
			return m_windAngle;
		}
		if (!m_windSteerSynced)
		{
			m_steeredWindAngle = m_windAngle;
			m_windSteerSynced = true;
		}
		glm::vec2 sum(0.0f);
		if (m_terrainData && m_terrainData->ranges.x > 0.0f && m_windSteerRange > 0.0f)
		{
			// Votes from the near cascade of the streamer's baked terrain-data map. The flow direction
			// rides bits 8-15 of the bit-cast packed climate channel (HeightMapBaker's data-map layout).
			const int32 res = (int32)m_terrainData->res;
			const float texel = m_terrainData->ranges.x / (float)res;
			const int32 radius = (int32)(m_windSteerRange / texel);
			const int32 step = glm::max(radius / 16, 1); // <= 33x33 taps of the CPU copy
			const glm::vec2 rel = glm::vec2(camera.position.x, camera.position.z) - m_terrainData->center;
			const int32 cx = (int32)std::floor(rel.x / texel) + res / 2;
			const int32 cy = (int32)std::floor(rel.y / texel) + res / 2;
			for (int32 y = glm::max(cy - radius, 0); y <= glm::min(cy + radius, res - 1); y += step)
				for (int32 x = glm::max(cx - radius, 0); x <= glm::min(cx + radius, res - 1); x += step)
				{
					const float* t = &m_terrainData->texels[((size_t)y * res + x) * 4];
					const uint32 enc = (std::bit_cast<uint32>(t[2]) >> 8) & 255u;
					if (enc == 0u || t[0] >= t[1]) // undirected or dry ground: abstains
						continue;
					const float a = (float)(enc - 1u) * (6.283185307f / 254.0f);
					sum += glm::vec2(std::cos(a), std::sin(a));
				}
		}
		// Needs a net vote worth at least one texel: a lone sliver of coast at the range's edge may turn
		// the sea, a wash of mutually cancelling directions may not. Votes are the direction the water
		// should TRAVEL; the sim's dominant waves travel AGAINST its wind vector (see swellTravelAngle),
		// so the wind target points the opposite way — offshore votes (faded to the travel heading) then
		// negate right back to the base tweak.
		const float target = glm::dot(sum, sum) > 1.0f ? std::atan2(-sum.y, -sum.x) : m_windAngle;
		float d = target - m_steeredWindAngle;
		d -= std::floor(d * (1.0f / 6.283185307f) + 0.5f) * 6.283185307f; // shortest arc
		const float maxStep = glm::radians(m_windSteerRate) * (float)glm::min(Globals::time.getDeltaSec(), 0.1);
		m_steeredWindAngle += glm::clamp(d, -maxStep, maxStep);
		return m_steeredWindAngle;
	}

	void OceanGenerator::update(Renderer& renderer, const Camera& camera,
	                           oc::shared_ptr<const BakedTerrainData> terrainData, float seaLevel)
	{
		ProfileScope profileScope("Ocean", EProfileCategory::Procedural);
		// ONE sea level, owned by the terrain (see the header). Adopted here every frame rather than
		// tweaked separately: this used to be its own slider, and the two silently forked — the water plane
		// moved while the terrain kept reporting its water at the old datum, which does not merely look
		// wrong. The swash gate fades on |baked water level - sea level|, so a metre of disagreement turned
		// the swash off planet-wide.
		m_seaLevel = seaLevel;
		// Adopt the streamer's active bake (the GPU passes read the same one): buoyancy and wind steering
		// sample this snapshot until the next update.
		m_terrainData = oc::move(terrainData);

		// A new bake rebuilds the dry-sector block grid: per-block MAX of (water level - height) over the
		// COARSEST cascade (largest coverage; its max is at least as wet as any finer view of the same
		// ground). One ~quarter-ms pass per shipped bake — every ~range/4 of camera travel.
		if (m_terrainData.get() != m_dryGridSource)
		{
			m_dryGridSource = m_terrainData.get();
			m_dryGridValid = false;
			if (m_terrainData && m_terrainData->cascades > 0 && m_terrainData->res > 0)
			{
				const uint32 res = m_terrainData->res;
				const uint32 farCascade = m_terrainData->cascades - 1;
				const float* texels = m_terrainData->texels.data() + (size_t)farCascade * res * res * 4;
				m_blockMaxDepth.fill(-FLT_MAX);
				const uint32 blockTexels = (res + DRY_BLOCKS - 1) / DRY_BLOCKS;
				for (uint32 j = 0; j < res; ++j)
				{
					float* row = &m_blockMaxDepth[(size_t)(j / blockTexels) * DRY_BLOCKS];
					for (uint32 i = 0; i < res; ++i)
					{
						const float* t = &texels[((size_t)j * res + i) * 4];
						float& blockMax = row[i / blockTexels];
						blockMax = glm::max(blockMax, t[1] - t[0]); // water level - terrain height
					}
				}
				m_dryGridCenter = m_terrainData->center;
				m_dryGridRange = m_terrainData->ranges[(int)glm::min(farCascade, 1u)];
				m_dryGridValid = m_dryGridRange > 1.0f;
			}
		}

		// Push the spectrum/shading params every frame; `enabled` also gates the GPU FFT simulation.
		const float windAngle = steeredWindAngle(camera); // base wind, turned toward the local shore flow
		OceanParams params;
		params.enabled = m_enabled;
		params.windDirection = glm::vec2(std::cos(windAngle), std::sin(windAngle));
		params.windSpeed = m_windSpeed;
		params.fetchKm = m_fetchKm;
		params.depth = m_depth;
		params.horizonLevelOffset = m_horizonLevelOffset;
		params.amplitude = m_amplitude;
		params.choppiness = m_choppiness;
		params.normalStrength = m_normalStrength;
		params.cascadeSizes = m_cascadeSizes;
		params.seaLevel = m_seaLevel;
		params.detailBias = m_detailBias;
		params.absorption = m_absorption;
		params.scatterColor = m_scatterColor;
		params.scatterStrength = m_scatterStrength;
		params.roughness = m_roughness;
		params.glintFilter = m_glintFilter;
		params.sssStrength = m_sssStrength;
		params.sssPower = m_sssPower;
		params.hitLighting = m_hitLighting;
		params.foamColor = m_foamColor;
		params.foamBias = m_foamBias;
		params.foamBreakAccel = m_foamBreakAccel;
		params.foamSoftness = m_foamSoftness;
		params.foamDecay = m_foamDecay;
		params.foamSpread = m_foamSpread;
		params.foamBoost = m_foamBoost;
		params.turbidity = m_turbidity;
		params.shoalScale = m_shoalScale;
		params.horizonDepth = m_horizonDepth;
		params.horizonDepthRange = m_horizonDepthRange;
		params.waveHeightLimit = m_waveHeightLimit;
		params.shoreFoamDepth = m_shoreFoamDepth;
		params.shoreFoamMax = m_shoreFoamMax;
		params.swashAmp = m_swashAmp;
		params.swashDrawdown = m_swashDrawdown;
		params.troughMargin = m_troughMargin;
		params.shoreFoamBias = m_shoreFoamBias;
		params.swashFlow = m_swashFlow;
		params.cullMargin = m_cullMargin;
		params.farCullError = m_farCullError;
		params.rtRefractionRange = m_rtRefractionRange;
		params.rtReflectionRange = m_rtReflectionRange;
		params.rtReflectionMaxRough = m_rtReflectionMaxRough;
		params.rtRayCutoffDist = m_rtRayCutoffDist;
		renderer.setOceanParams(params);

		if (!m_enabled)
		{
			renderer.setOceanWaveTrough(0.0f); // no waves: the underwater-fog boundary sits at the calm level
			return;
		}

		// The horizon band is sized to the camera far plane; a far change (rare — settings, VR) rebuilds.
		if (camera.far != m_lastFar)
		{
			m_lastFar = camera.far;
			m_gridDirty = true;
		}
		if (m_gridDirty || m_sectors.empty())
			rebuildGrid();
		if (m_sectors.empty())
			return;

		// Follow the camera, snapped to a multiple of the ring lattices so vertices re-land on the exact
		// same world positions (a clipmap's whole point: each world point keeps its sample position and
		// ring-fixed mip, so waves are rock-stable under camera motion). 8*cell aligns rings 0-2 perfectly;
		// coarser rings shift sub-texel, which is invisible against their band-limited content.
		const float snap = 8.0f * glm::max(m_ringCell, 0.01f);
		const float px = std::floor(camera.position.x / snap + 0.5f) * snap;
		const float pz = std::floor(camera.position.z / snap + 0.5f) * snap;
		const Transform xf(glm::vec3(px, m_seaLevel, pz), 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

		// Dry-sector test: skippable only when every block the footprint overlaps is buried deeper than
		// the same terms the vertex cull demands (margin + swash reach + far error + slack) — so a
		// skipped sector could never have contributed geometry. Out-of-range footprints count as wet.
		//
		// FENCED ON THE STREAMED MESH RADIUS, exactly like oceanVertexCulled: past the terrain mesh
		// nothing is drawn to hide the water, so the VS deliberately never culls out there and the sea
		// is what fills the horizon. A CPU test that skipped those sectors on baked "land" would delete
		// water the VS was going to draw — which is what left no horizon water behind unloaded terrain.
		// So a sector is only skippable when its WHOLE footprint sits under loaded mesh; anything
		// reaching past it renders, and the VS still culls the buried part per vertex. The horizon
		// band always reaches past the mesh, so it is never dropped here (and, if the streamer's ring
		// ever grew out to cover it, being under real mesh is exactly when dropping it is right).
		const float meshRadius = renderer.getTerrainMeshRadius();
		const glm::vec2 camXZ(camera.position.x, camera.position.z);
		const float wetNeed = glm::max(m_cullMargin, 0.0f) + swashReach() + glm::max(m_farCullError, 0.0f) + 2.0f;
		const auto sectorDry = [&](const Sector& s) {
			if (!m_drySectorCull || !m_dryGridValid || meshRadius <= 0.0f || s.horizonBand)
				return false; // the band is the horizon itself: never dropped for terrain
			const glm::vec2 center(px + s.localCenter.x, pz + s.localCenter.z);
			// Farthest footprint corner from the camera (the VS fences radially the same way).
			if (glm::length(glm::abs(center - camXZ) + s.halfXZ) >= meshRadius)
				return false;
			const float scale = float(DRY_BLOCKS) / m_dryGridRange;
			const glm::vec2 bMin = (center - s.halfXZ - m_dryGridCenter) * scale + float(DRY_BLOCKS) * 0.5f;
			const glm::vec2 bMax = (center + s.halfXZ - m_dryGridCenter) * scale + float(DRY_BLOCKS) * 0.5f;
			if (bMin.x < 0.0f || bMin.y < 0.0f || bMax.x >= float(DRY_BLOCKS) || bMax.y >= float(DRY_BLOCKS))
				return false; // reaches past the baked data: unknown terrain, keep it
			for (int y = (int)bMin.y; y <= (int)bMax.y; ++y)
				for (int x = (int)bMin.x; x <= (int)bMax.x; ++x)
					if (m_blockMaxDepth[(size_t)y * DRY_BLOCKS + x] > -wetNeed)
						return false;
			return true;
		};

		// Push the visible sectors, terrain-chunk style: re-center each sector's spatial entry on the
		// snapped node position, then gate on the dry test and the Main-pass stamp (Spatial/Culling >=
		// Cull). The ocean draws PASS_MAIN only, so a culled sector has nothing to push at all — and the
		// GPU per-instance frustum cull refines whatever the CPU gates let through.
		const SpatialCullingConfig& culling = Globals::spatialIndex.getCullingConfig();
		const bool gate = culling.mode >= int(ESpatialCullMode::Cull);
		for (Sector& s : m_sectors)
		{
			if (!s.node.isValid())
				continue;
			s.node.setTransform(xf);
			if (s.spatialEntry.isValid())
				Globals::spatialIndex.updateEntry(s.spatialEntry.handle(),
					glm::dvec3(px + s.localCenter.x, m_seaLevel + s.localCenter.y, pz + s.localCenter.z), s.radius);
			if (sectorDry(s))
				continue;
			if (gate && s.spatialEntry.isValid()
				&& !(Globals::spatialIndex.getPassMask(s.spatialEntry.handle()) & SpatialPassBit_Main))
				continue;
			renderer.renderNode(s.node, RendererVKLayout::PASS_MAIN);
		}

		// Refresh the CPU copy of the GPU displacement readback inside this frame's fence-safe window
		// (the slot's buffer is stable between beginFrame and present, and physics updates BEFORE
		// beginFrame — so buoyancy must query an owned copy, not the live buffer).
		uint32 tileRes = 0;
		const oc::span<const uint16> tile = renderer.getOceanDisplacementReadback(tileRes);
		m_dispTile.assign(tile.begin(), tile.end());
		m_dispTileRes = tileRes;

		estimateWaveTrough();
		renderer.setOceanWaveTrough(m_waveTrough); // sinks the underwater-fog boundary below live troughs
	}

	// FP16 -> FP32 (readback texels are RGBA16F). Fabian Giesen's half_to_float_fast: rebias the
	// exponent in float position; the multiply trick renormalizes subnormals.
	static float halfToFloat(uint16 h)
	{
		const uint32 shiftedExp = 0x7C00u << 13;
		uint32 o = (uint32)(h & 0x7FFFu) << 13;
		const uint32 exp = shiftedExp & o;
		o += (127u - 15u) << 23;
		if (exp == shiftedExp)
			o += (128u - 16u) << 23; // inf/nan stays inf/nan
		else if (exp == 0)
		{
			o += 1u << 23;
			o = std::bit_cast<uint32>(std::bit_cast<float>(o) - std::bit_cast<float>((113u << 23)));
		}
		o |= (uint32)(h & 0x8000u) << 16;
		return std::bit_cast<float>(o);
	}

	// (water depth, water surface level) at (x, z) from the terrain-data map's CPU copy (near cascade) —
	// the CPU mirror of the shaders' oceanSampleShoreData (depth = local water level - terrain height).
	// Outside the near cascade / without terrain data: open-ocean depth at sea level. One knowing
	// omission vs the shader: the far-cascade fallback — buoyancy queries only matter near the camera,
	// well inside the near cascade's range.
	glm::vec2 OceanGenerator::sampleShoreData(float x, float z) const
	{
		if (!m_terrainData || m_terrainData->texels.empty() || m_terrainData->ranges.x <= 1.0f)
			return glm::vec2(m_depth, m_seaLevel);
		const uint32 res = m_terrainData->res;
		const float u = (x - m_terrainData->center.x) / m_terrainData->ranges.x + 0.5f;
		const float v = (z - m_terrainData->center.y) / m_terrainData->ranges.x + 0.5f;
		if (u <= 0.0f || u >= 1.0f || v <= 0.0f || v >= 1.0f)
			return glm::vec2(m_depth, m_seaLevel);
		const float tx = glm::clamp(u * (float)res - 0.5f, 0.0f, (float)res - 1.001f);
		const float tz = glm::clamp(v * (float)res - 0.5f, 0.0f, (float)res - 1.001f);
		const uint32 x0 = (uint32)tx, z0 = (uint32)tz;
		const float fx = tx - (float)x0, fz = tz - (float)z0;
		const auto at = [&](uint32 i, uint32 j) { // (terrain height, water level) of the RGBA texel
			const float* t = &m_terrainData->texels[((size_t)j * res + i) * 4];
			return glm::vec2(t[0], t[1]);
		};
		const glm::vec2 hw = glm::mix(glm::mix(at(x0, z0), at(x0 + 1, z0), fx),
			glm::mix(at(x0, z0 + 1), at(x0 + 1, z0 + 1), fx), fz);
		return glm::vec2(hw.y - hw.x, hw.y);
	}

	// Deepest-possible current wave trough (m below the calm level) from the readback: the sum of each
	// cascade's layer minimum bounds any combined trough (cascades add; their minima rarely coincide, so
	// this is conservative — right for hiding fog under the surface). The minimum over the WHOLE tiling
	// patch is a sea-state statistic, near-stationary frame to frame, so re-scan sparsely.
	void OceanGenerator::estimateWaveTrough()
	{
		if (m_waveTroughCooldown-- > 0 || m_dispTileRes == 0)
			return;
		m_waveTroughCooldown = 15;
		float troughSum = 0.0f;
		for (uint32 c = 0; c < RendererVKLayout::OCEAN_CASCADES; ++c)
		{
			const size_t n = (size_t)m_dispTileRes * m_dispTileRes;
			const uint16* layer = m_dispTile.data() + (size_t)c * n * 4;
			float minH = 0.0f;
			for (size_t i = 0; i < n; ++i)
				minH = glm::min(minH, halfToFloat(layer[i * 4 + 1])); // texel.y = height displacement
			troughSum -= minH;
		}
		m_waveTrough = troughSum;
	}

	// Swash run-up reach (m). MIRRORS Renderer.cpp's UBO packing of u_oceanParams7.w — the conservative
	// max run-up height derived from the wave-trough estimate this class itself publishes. It sizes the
	// on-land band the shaders draw the tongue in, so it is also the band buoyancy must find water in.
	float OceanGenerator::swashReach() const
	{
		return glm::clamp(m_swashAmp, 0.0f, 4.0f) * (m_waveTrough + 0.25f);
	}

	// CPU mirror of oceanSwashWeight (ocean_wave.inc.glsl): how much of the RAW un-shoaled wave field
	// rides the surface at this water depth (negative = land height above the local level).
	float OceanGenerator::swashWeight(float depth, float waterLevel) const
	{
		const float amp = glm::clamp(m_swashAmp, 0.0f, 4.0f);
		if (amp <= 0.0f)
			return 0.0f;
		const float seaFade = 1.0f - glm::smoothstep(0.05f, 1.0f, std::fabs(waterLevel - m_seaLevel));
		if (seaFade <= 0.0f)
			return 0.0f; // landlocked water (lakes at altitude): no swell reaches it
		const float reach = glm::max(swashReach(), 0.01f);
		const float landFade = glm::clamp(1.0f + glm::min(depth, 0.0f) / reach, 0.0f, 1.0f);
		const float fadeIn = 1.0f - glm::smoothstep(0.0f,
			glm::max(2.0f * reach, glm::max(m_shoalScale, 0.0f) * glm::max(m_cascadeSizes.y, 1.0f)), depth);
		return amp * seaFade * landFade * fadeIn;
	}

	// CPU mirror of oceanSampleDisplacement (ocean_wave.inc.glsl) at an UNDISPLACED world XZ, bilinear-
	// wrapped over the readback tile: shoal-faded cascade sum, swash backflow, the raw
	// run-up residual, then the waterline floor — same order, same clamps, y relative to the LOCAL water
	// level like the shader's. The shader is what you SEE and this is what floats on it, so any change
	// there has to land here too; the divergence is invisible until a body sinks through a drawn wave.
	// Two knowing omissions: the ring-matched vertex mip (the readback is one fixed band limit — the
	// physics surface is the same waves minus the finest detail) and the flow rotation (disabled in the
	// shader; if OCEAN_FLOW_SAMPLE_ROTATION ever comes back it must come back here too).
	glm::vec3 OceanGenerator::sampleDisplacement(glm::vec2 worldXZ) const
	{
		const glm::vec2 shoreHW = sampleShoreData(worldXZ.x, worldXZ.y); // (depth, water level)
		const float depth = shoreHW.x;
		const float reach = swashReach();
		glm::vec3 disp(0.0f);
		float sw = 0.0f;
		// Buried deeper than the run-up band: every shoal fade and the swash weight are zero, so the
		// sampling below would displace nothing — skip to the floor clamp (bit-identical, no fetches).
		if (depth > -reach)
		{
			const uint32 res = m_dispTileRes;
			// Bilinear with wrap (the FFT patch tiles); texel centers at integer + 0.5.
			const auto sampleCascade = [&](uint32 c, float L) {
				const glm::vec2 t = glm::fract(worldXZ / L) * (float)res - 0.5f;
				const int x0 = (int)std::floor(t.x), z0 = (int)std::floor(t.y);
				const float fx = t.x - (float)x0, fz = t.y - (float)z0;
				const size_t layer = (size_t)c * res * res;
				const auto fetch = [&](int i, int j) {
					const uint32 xi = (uint32)(i + (int)res) % res, zi = (uint32)(j + (int)res) % res;
					const uint16* texel = &m_dispTile[(layer + (size_t)zi * res + xi) * 4];
					return glm::vec3(halfToFloat(texel[0]), halfToFloat(texel[1]), halfToFloat(texel[2])); // Dx, h, Dz
				};
				return glm::mix(glm::mix(fetch(x0, z0), fetch(x0 + 1, z0), fx),
					glm::mix(fetch(x0, z0 + 1), fetch(x0 + 1, z0 + 1), fx), fz);
			};
			const float shoal = glm::max(m_shoalScale, 0.0f);
			float rawY = 0.0f;
			glm::vec2 rawXZ(0.0f);
			for (uint32 c = 0; c < RendererVKLayout::OCEAN_CASCADES; ++c)
			{
				const float L = glm::max(m_cascadeSizes[c], 1.0f);
				const glm::vec3 d = sampleCascade(c, L);
				// oceanShoalFade. The shader's "Horizon depth" floor is knowingly omitted: it only
				// engages a kilometre out, and nothing floats there.
				disp += glm::vec3(d.x * m_choppiness, d.y, d.z * m_choppiness)
					* glm::smoothstep(0.0f, glm::max(shoal * L, 0.01f), depth);
				rawY += d.y;
				rawXZ += glm::vec2(d.x, d.z);
			}
			// Breaking limit, mirroring the shader: scale the shoaled sum so a wave never stands taller
			// than a fraction of the water it is in (the swash still rides the raw field).
			if (m_waveHeightLimit > 0.0f && depth > 0.0f)
			{
				const float cap = m_waveHeightLimit * depth;
				disp *= cap / (cap + std::fabs(disp.y));
			}
			sw = swashWeight(depth, shoreHW.y);
			// Backflow: the tongue slides seaward as the wave recedes, gated by its thickness above the
			// sand and soft-capped to ~the reach (a buried surface must not keep sliding).
			const float flowFade = glm::smoothstep(0.0f, 0.35f, rawY * sw + depth);
			glm::vec2 flowOff = rawXZ * (m_choppiness * glm::max(m_swashFlow, 0.0f) * sw * flowFade);
			const float flowCap = glm::clamp(0.5f * reach, 0.25f, 1.0f);
			flowOff *= flowCap / (flowCap + glm::length(flowOff));
			disp.x += flowOff.x;
			disp.z += flowOff.y;
			disp.y += rawY * sw;
		}
		// Waterline floor: the inner-rounded smooth max, tightening under an active swash.
		constexpr float eps = 0.05f;
		const float k = glm::mix(0.2f, 0.06f, glm::clamp(sw * 4.0f, 0.0f, 1.0f));
		float floorY = eps - glm::max(depth, 2.0f * eps);
		const float troughMargin = glm::max(m_troughMargin, 0.0f);
		if (troughMargin > 0.0f && depth > 0.0f)
			floorY += troughMargin * glm::smoothstep(0.0f, 2.0f * troughMargin, depth);
		// Drawdown sinks the receding surface UNDER the sand — which is exactly what beaches a floating
		// body as the wave leaves, so buoyancy wants it as much as the depth cut does.
		if (glm::clamp(m_swashAmp, 0.0f, 4.0f) > 0.0f && depth > 0.0f && m_swashDrawdown > 0.0f)
			floorY = glm::mix(floorY, -depth - glm::max(m_swashDrawdown, eps),
				1.0f - glm::smoothstep(0.0f, glm::max(reach, 0.01f), depth));
		const float hh = glm::max(k - std::fabs(disp.y - floorY), 0.0f) / k;
		disp.y = glm::max(disp.y, floorY) - hh * hh * (k * 0.25f);
		return disp;
	}

	float OceanGenerator::sampleWaterHeight(float x, float z) const
	{
		if (!m_enabled || m_dispTile.empty() || m_dispTileRes == 0)
			return -FLT_MAX;
		// Land beyond the run-up band: the swash weight and every shoal fade are zero there, so the
		// shaders draw no live water — the same gate the displacement uses, one shore fetch instead of
		// the whole inverse. Inside the band this DOES return water above the drawn shoreline: that is
		// the tongue, and a body in it floats until the drawdown floor lets it back down onto the sand.
		if (sampleShoreData(x, z).x <= -swashReach())
			return -FLT_MAX;
		// The maps store where the UNDISPLACED grid point ENDS UP; a fixed world column needs the
		// inverse. A few fixed-point iterations: find p whose displaced position lands on (x, z).
		const glm::vec2 query(x, z);
		glm::vec2 p = query;
		for (int i = 0; i < 2; ++i)
		{
			const glm::vec3 d = sampleDisplacement(p);
			p = query - glm::vec2(d.x, d.z);
		}
		// Waves ride the LOCAL water surface, read at the SOURCE point like the clipmap vertex does
		// (its base y is the water table under the undisplaced vertex, not under where it lands).
		return sampleShoreData(p.x, p.y).y + sampleDisplacement(p).y;
	}
}
