export module Procedural:OceanGenerator;

import Core;
import Core.glm;
import Core.Camera;
import Core.Transform;

import RendererVK;
import Spatial;
import Settings;

import :TerrainSampler;
import :HeightMapBaker;

export namespace Procedural
{
	// Render-only procedural ocean, the CPU side of the FFT/Tessendorf water: maintains a camera-following
	// GEOMETRY CLIPMAP - concentric square rings, each with a FIXED world-space cell size that doubles per
	// ring. Because a ring's cell size is constant, every world position inside it samples the displacement
	// maps at a FIXED mip regardless of camera distance - wave shapes no longer morph with camera motion
	// (the failure mode of the previous radially-graded grid, whose per-vertex mip followed distance). Ring
	// transitions use a CDLOD-style vertex morph baked per vertex (texcoord = ring cell size + morph
	// weight): over each ring's outer band, odd vertices collapse onto the next ring's coarser lattice and
	// the sampled mip blends +1, so the boundary matches the next ring exactly - seamless by construction.
	// The clipmap is split into SECTORS, terrain-chunk style - ring 0 whole, each outer ring as 8
	// rectangular blocks around its hole, the horizon band as its 4 sides - each a container/node with
	// its own SpatialIndex entry, so the CPU visibility gate and the GPU per-instance frustum cull drop
	// off-screen water instead of vertex-shading the whole multi-km disc every frame. All sectors share
	// one transform, snapped to a lattice multiple so vertices re-land on the same world positions as
	// the camera moves; sector borders duplicate identical vertices, so splitting can't open seams.
	//
	// Everything else runs on the GPU: OceanSimulationPipeline simulates the wave spectrum + IFFT into
	// displacement/gradient maps each frame, the Ocean pipeline variant displaces this mesh by them and
	// shades the surface (RT refraction, Beer-Lambert). All spectrum/shading parameters are Tweak-backed
	// and pushed via Renderer::setOceanParams (fully live); the mesh rebuilds only when ring params change.
	class OceanGenerator
	{
	public:
		OceanGenerator() = default;
		~OceanGenerator();
		OceanGenerator(const OceanGenerator&) = delete;
		OceanGenerator& operator=(const OceanGenerator&) = delete;

		void initialize();                                     // attaches the "Ocean" listeners + declares the push sources
		// Per-frame: push params, rebuild / re-center the clipmap, set each sector node's pass mask (the
		// dry test) (after beginFrame). The draw is NOT here: TerrainStreamer::render calls render(),
		// and its job's one walk of the Spatial visible hand-over pushes the visible sector nodes along
		// with its chunks. terrainData = the streamer's active baked
		// terrain-data map (TerrainStreamer::activeTerrainData(), nullptr = no terrain). The GPU passes
		// read the SAME bake (the fog terrain cascades) for water depth/level - shoaling, surf, swash,
		// the land cull - and this CPU copy feeds buoyancy and wind steering, so the drawn water and the
		// simulated water agree by construction.
		// `seaLevel` is the world datum, and it comes from the TERRAIN (TerrainStreamer::seaLevel) rather
		// than being a tweak here. It cannot be two values: the terrain generator builds its heights around
		// it (V3's elevations are relative to it, which is why moving it regenerates chunks), the ocean
		// floats its surface on it, and the swash gate compares the two - so a fork between them switches
		// the swash off everywhere rather than just looking off.
		void update(Renderer& renderer, const Camera& camera,
		            oc::shared_ptr<const BakedTerrainData> terrainData = nullptr, float seaLevel = 0.0f);
		// Kicks this frame's render job (after update; a no-op when it had nothing to draw - disabled,
		// no grid). The visible sectors come through TerrainStreamer's walk of the Spatial hand-over
		// (their nodes ARE the userData); this job pushes every sector itself only when culled = false
		// (culling Off). Either way each push uses the node's own pass mask (the dry test).
		void render(bool culled);
		// Joins the job render() kicked (the sector renderNode pushes, the displacement readback copy,
		// the wave-extent re-scan and the camera water-surface sample) and applies its three renderer
		// stores. main.cpp calls it right before Renderer::present; update(), rebuildGrid() and the dtor
		// join it too before they touch m_sectors. TerrainStreamer's render job also pushes sector nodes
		// (from the hand-over): main.cpp joins it before present too, so it is done before any of those.
		void joinRender();

		// Water surface world Y at (x, z), CPU-evaluated from the GPU displacement readback (a full mirror
		// of the clipmap vertex shader: the raw cascade sum times the shore's surface weight, plus the
		// swash backflow; ~2 frames of latency - invisible for physics). Returns -FLT_MAX where there is
		// no water: ocean disabled, readback not primed, or land beyond the swash run-up band per the
		// shore bake. Inside that band it returns the live tongue surface, which SINKS BELOW the terrain
		// as the wave recedes - bodies beach themselves on their own that way, so callers want a plain
		// surface-vs-point test, not a separate dry check. This is the buoyancy height field the App
		// wires into PhysicsWorld::setWaterSurface; keys 8/9's cubes bob in the swell through it.
		float sampleWaterHeight(float x, float z) const;

		// True when sampleWaterHeight can return water AT ALL (enabled + displacement readback
		// primed) - the App wires this as buoyancy's global gate, so a disabled ocean costs the
		// PhysicsComponents nothing.
		bool hasWater() const { return m_settings.enabled &&!m_dispTile.empty() && m_dispTileRes != 0; }

		// The heading the swell actually TRAVELS in open water (radians, XZ) - the terrain streamer's baked
		// flow field eases back to this offshore so the encoded directions meet the wind-driven open sea
		// without a turn. NOTE the sim's convention: the spectrum's dominant term is h0(k) e^{i(k.x + wt)},
		// which moves AGAINST the wind-direction vector, so travel = baseWindAngle + pi = THE wind's ("Sky/Wind")
		// direction (steeredWindAngle converts back when it feeds the sim). Derived from the BASE wind,
		// deliberately not the steered value: feeding that into the bake would re-bake both maps every frame the
		// wind turns (bake -> steering -> bake feedback).
		float swellTravelAngle() const { return baseWindAngle() + 3.14159265f; }

	private:
		float baseWindAngle() const; // THE wind's heading in the sim's convention (the direction it blows FROM)
		float windSpeed() const;     // THE wind's speed x "Wind speed scale" (the MODEL U10)
		// Turns the SIMULATION wind toward the baked shore flow around the camera - how the waves actually
		// travel inland at the coast. See the .cpp.
		float steeredWindAngle(const Camera& camera);
		// Fills OceanParams from the settings (oceanWorldScaled) and the live wind / sea / camera, and pushes it
		// (Renderer::setOceanParams: the simulation and the live UBO values); `enabled` rides along and gates the GPU
		// FFT + the ocean draw, so the disabled transition pushes exactly once through the same path.
		void pushOceanParams(Renderer& renderer, const Camera& camera);
		void rebuildGrid();
		glm::vec2 sampleShoreData(float x, float z) const;          // (water depth, water level) from the terrain-data CPU copy
		float swashReach() const;                                   // run-up band height; mirrors the UBO's estimate
		float swashWeight(float depth, float waterLevel) const;     // mirrors oceanSwashWeight
		float swashBase(float depth, float waterLevel) const;       // mirrors oceanSwashBase (no depth fade-in)
		glm::vec3 sampleDisplacement(glm::vec2 worldXZ) const;      // CPU mirror of oceanSampleDisplacement
		void estimateWaveExtents(); // sparse re-scan of the readback for the current trough / crest / chop reach
		// Worst-case distance a clipmap vertex travels from its authored lattice position. BOTH culls
		// need it: the mesh the culls were built from is the UNDISPLACED lattice, and the vertex shader
		// then moves every vertex by the wave height AND - the part that surprises - by the CHOPPY
		// horizontal displacement, which grows with the "Choppiness" tweak. Without it a high
		// choppiness drops sectors whose crests are still on screen, opening gaps at the screen edges.
		float displacementExtent() const;

		// The "Ocean*" tweaks (Settings.Ocean): clipmap geometry (a change rebuilds the mesh), spectrum, shading, foam,
		// shore interaction and the ray tracing budget.
		const OceanSettings& m_settings = Globals::settings.ocean;
		float m_seaLevel = 0.0f;   // mirrors the terrain's datum; set every update(), never tweaked here
		// "World scale": UNIFORM, the ocean's counterpart of the terrain's "Meters per pixel" (mpp / 30):
		// every tweak is authored in MODEL metres and the sea is drawn at model x scale. The
		// spectrum is scaled by Froude similarity (lengths x s, wind speed x sqrt(s), gravity untouched),
		// which is the one scaling of the JONSWAP/TMA inputs under which wavelengths AND wave heights both
		// come out x s - so the scaled sea is a shrunk copy of the model sea. Froude periods would be
		// x sqrt(s) (a miniature races), so the spectrum clock runs at sqrt(s) (OceanParams::timeScale) and
		// the periods stay the model sea's. Applied ONCE, in pushOceanParams: the shaders and the CPU
		// buoyancy mirror both read the scaled set (m_params), so neither can disagree with the other.
		OceanParams m_params;      // the SCALED param set last pushed to the renderer; the CPU mirror reads it
		// "Horizon band": one coarse quad band appended past the outermost ring, stretching its edge lattice out to
		// the camera far plane. Its inner edge sits on the last ring's fully-morphed (2x cell) lattice at the
		// matching mip, so the seam is watertight by the same CDLOD construction the rings use.
		float m_lastFar = 0.0f;        // camera far plane the current grid was built for (change = rebuild)

		// Flow -> wind steering (steeredWindAngle): near a coast the SIM wind turns toward the baked flow
		// so the waves roll toward the local shore; away from any it returns to baseWindAngle.
		float m_steeredWindAngle = 0.0f;  // follows baseWindAngle/the flow at the slew rate
		bool  m_windSteerSynced = false;  // adopt baseWindAngle on first use instead of turning in from 0

		// The streamer's active baked terrain-data map (adopted each update()); the shared_ptr keeps this
		// snapshot alive across the frame even while the streamer ships a replacement bake - buoyancy
		// queries (physics, before the next update) and wind steering read it.
		oc::shared_ptr<const BakedTerrainData> m_terrainData;

		// CPU copy of the GPU displacement readback tile, refreshed every update() inside the frame's
		// fence-safe window - sampleWaterHeight can then run at ANY point in the frame (physics updates
		// before beginFrame) without racing the GPU rewriting the slot's readback buffer.
		oc::vector<uint16> m_dispTile;                  // RGBA16F texels, res^2 per cascade
		uint32 m_dispTileRes = 0;
		float m_waveTrough = 0.0f;      // deepest current trough below the calm level (m; see estimateWaveExtents)
		float m_waveCrest = 0.0f;       // highest current crest above it (m)
		float m_waveHoriz = 0.0f;       // largest RAW horizontal displacement per axis (m, BEFORE choppiness:
		                                // the maps store raw Dx/Dz, so the live chop tweak multiplies this
		                                // without needing a re-scan)
		int   m_waveTroughCooldown = 0; // sparse re-scan counter (the patch extremes are near-stationary)

		bool m_gridDirty = true;
		bool m_disabledIdle = false; // disabled AND cleared: update() is a branch and a return

		// One clipmap sector per draw (see the class comment). Registered in the SpatialIndex like
		// terrain chunks (SpatialLayer_Terrain, no spawn guard); unlike chunks they MOVE - updateEntry
		// re-centers them on the snapped node position every frame, so they stay in the dynamic tier.
		// The SpatialIndex hands &node over as userData (| SpatialTerrainTag_Ocean), so a grid's
		// sectors never move in memory: registered only after the grid is complete, and retired whole
		// (m_retiredGrids) instead of freed while a hand-over list may still name them.
		struct Sector
		{
			RenderMesh mesh;           // declared first -> destroyed AFTER the node that draws it
			RenderNode node;           // pass mask PASS_MAIN, or 0 while the dry test drops it (update)
			SpatialEntry spatialEntry;
			glm::vec3 localCenter = glm::vec3(0.0f); // mesh-local bounds center (the node snap adds on top)
			glm::vec2 halfXZ = glm::vec2(0.0f);      // mesh-local XZ half extents (dry-sector test footprint)
			bool horizonBand = false;                // never under-terrain-culled (see rebuildGrid)
			float baseRadius = 0.0f; // bounds sphere of the UNDISPLACED lattice - what the mesh actually is
			                     // The registered radius is this plus displacementExtent(), refreshed every
			                     // frame: the XZ half-diagonal used to absorb the displacement incidentally,
			                     // which held until "Choppiness" pushed vertices further sideways than the
			                     // spare diagonal, and sectors started dropping with their crests on screen.
		};
		oc::vector<Sector> m_sectors;
		bool sectorDry(const Sector& s, float px, float pz, glm::vec2 camXZ, float meshRadius, float wetNeed) const;
		// A replaced / released grid: nodes, meshes and culling entries released at once, the vector (so
		// every Sector's address) kept until the Spatial collect generation moves past `generation`.
		struct RetiredGrid { uint32 generation; oc::vector<Sector> sectors; };
		oc::vector<RetiredGrid> m_retiredGrids;
		void retireGrid();
		uint16 m_material = UINT16_MAX;  // shared by every sector (created at the first grid)

		// The render job (see render / joinRender). Its outputs (the wave extents, m_dispTile,
		// m_cameraSurfaceY) are stored to the renderer in joinRender.
		JobCounter          m_renderCounter;
		glm::vec3           m_cameraPos = glm::vec3(0.0f); // update's camera: the job samples the surface under it
		bool                m_renderPending = false; // a kicked job's stores still owed to the renderer
		bool                m_renderReady = false;   // update() ran with a grid: render() may kick
		float               m_cameraSurfaceY = -FLT_MAX;
		// Last re-centering of the sector entries (update): re-run only when one of these changes.
		float m_lastPx = FLT_MAX, m_lastPz = FLT_MAX, m_lastSeaLevel = FLT_MAX, m_lastPad = -1.0f;

		// Dry-sector cull: sectors whose whole footprint is buried under land per the baked terrain data
		// are skipped before anything reaches the GPU (a camera deep inland then renders no water at all).
		// Conservative by construction - a per-block MAX of (water level - height) over the COARSEST
		// cascade, rebuilt once per adopted bake, and a sector only skips when every block it overlaps is
		// dry beyond the same burial terms the vertex cull demands; footprints leaving the baked range
		// always count as wet (unknown terrain = open-ocean fallback in the shaders). "Dry sector cull" toggles it.
		static constexpr uint32 DRY_BLOCKS = 32;
		oc::array<float, DRY_BLOCKS * DRY_BLOCKS> m_blockMaxDepth{};
		bool  m_dryGridValid = false;
		const BakedTerrainData* m_dryGridSource = nullptr; // bake identity the block grid was built from
		glm::vec2 m_dryGridCenter = glm::vec2(0.0f);
		float m_dryGridRange = 0.0f;
	};
}

export namespace Globals
{
OC_INIT_SEG(OC_SEG_PROCEDURAL)
	Procedural::OceanGenerator ocean;
}
