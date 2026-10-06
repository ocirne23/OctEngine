export module Settings.Ocean;

import Core;
import Core.glm;

// "Ocean*": Procedural's OceanGenerator, the CPU side of the FFT/Tessendorf water. NOT the renderer's OceanParams: that
// is the SCALED set OceanGenerator::pushOceanParams builds from these every frame.
// "Ocean/World scale" is the ocean's metersPerPixel: every metre-valued setting below is a MODEL metre, scaled ONCE in
// pushOceanParams (lengths x s, wind speed x sqrt(s), the spectrum clock x sqrt(s)).
export struct OceanSettings
{
	// --- Clipmap geometry ("World scale", ring cell / resolution / count and the horizon band rebuild the mesh) ---
	bool  enabled = false;
	float worldScale = 1.0f;   // 1 = the model sea
	// Reach = ringCell * res/2 * 2^(rings-1), and every ring costs the same vertex count whatever its cell size - so buy
	// near-field detail by trading cell size for ring COUNT, not by biasing the mip.
	float ringCell = 0.25f;    // ring 0 cell size (m); doubles per ring
	int   ringRes = 256;       // cells per axis per ring (ring 0 is a full grid, outer rings are annuli)
	int   rings = 8;           // ring count (defaults: 128 m fine region, ~4 km reach)
	bool  horizonBand = true;  // one coarse quad band past the outermost ring, out to the camera far plane
	float horizonLevelOffset = -0.5f; // vertical shift (m) of the band only; negative sinks it under distant
	                                  // near-sea-level terrain (it is cull-exempt)
	// Bias on the ring-matched displacement mip (negative = sample finer than the ring's Nyquist).
	float detailBias = 0.3f;

	// --- Spectrum (TMA/JONSWAP + finite-depth dispersion) + shading; all live via setOceanParams ---
	float windSpeedScale = 2.5f; // x "Sky/Wind/Speed" = the U10 (m/s): the main sea-state knob
	float fetchKm = 300.0f;      // wind fetch (km)
	float depth = 100.0f;        // ocean depth (m): finite-depth dispersion + TMA attenuation
	// Flow -> wind steering: near a coast the SIM wind turns toward the baked flow so the waves roll toward the shore.
	bool  windSteerEnabled = true;
	float windSteerRate = 10.0f;   // deg/s the simulation wind may turn (spectrum morphs through it)
	float windSteerRange = 400.0f; // m around the camera whose baked shore directions vote
	float amplitude = 1.0f;        // artistic scale on the spectrum (1 = physical)
	float choppiness = 1.25f;      // horizontal displacement lambda
	float normalStrength = 1.0f;
	// FFT patch sizes (m). Each cascade TILES with its own size, so the largest one sets how often the sea visibly
	// repeats. Scaled as a SET; the ratios (8.17, 7.52) stay non-rational so the three tilings never re-align.
	glm::vec3 cascadeSizes = glm::vec3(1536.0f, 188.0f, 25.0f);

	glm::vec3 absorption = glm::vec3(85.0f / 255.0f, 14.0f / 255.0f, 20.0f / 255.0f); // Beer-Lambert extinction (1/m)
	glm::vec3 scatterColor = glm::vec3(0.047f, 0.1f, 0.15f);
	float scatterStrength = 1.0f;
	float roughness = 0.07f;
	float glintFilter = 1.0f;      // scale on the roughness-widening variance (spec AA + LEAN)
	// Slope variance below the FINEST cascade's Nyquist (the capillary band). NOT world scaled: dimensionless.
	float microRoughness = 0.01f;
	float crestSlopeLimit = 0.0f;  // the shading slope's soft limit, s /= 1 + k * |s|; 0 = off
	// Sub-band detail: the finest cascade's gradients re-sampled at a fraction of its patch size in a rotated domain,
	// added to the SHADING slope only.
	float detailStrength = 0.66f;
	float detailScale = 0.33f;     // fraction of the finest cascade's patch size
	float detailFadeDist = 60.0f;  // MODEL metres
	float detailRotation = 0.9f;   // radians
	float sssStrength = 0.75f;     // crest SSS: back-lit crests glow the scatter color, per meter of height
	float sssPower = 1.0f;         // crest SSS toward-the-sun view lobe exponent
	float undersideTransmission = 1.0f; // sky through Snell's window from below (1 = Fresnel; less = more internal reflection)
	bool  hitLighting = false;     // grid lights at refraction/reflection ray hits (pipeline reload on toggle)
	// Foam: one instant-foam response draws the crest foam AND injects the world-space foam field.
	glm::vec3 foamColor = glm::vec3(0.88f, 0.92f, 0.94f);
	float foamBias = 0.9f;         // fold threshold (Jacobian below this foams)
	float foamBreakAccel = 0.4f;   // breaking threshold (downward crest accel, g units)
	float foamSoftness = 1.0f;     // edge width of both thresholds
	float foamWindFull = 15.0f;    // model U10 (m/s) from which the surf band has its full width (off in a calm)
	float bubbleDepth = 0.5f;      // m under the surface (model metre): deeper = darker, more turquoise
	float bubbleBrightness = 1.25f; // the bubble cloud's albedo, x foam color
	float bubbleBlur = 2.0f;       // model m: the bubble cloud reads the foam field this blurred
	float foamFlatten = 0.5f;      // 0..1: the foam's Lambert normal eased toward up
	// The world-space foam field: the foam amount sticks to the water it formed on and drifts downwind.
	float foamSurfaceDecay = 0.999f;   // foam amount retention per frame
	float foamSurfaceStrength = 1.0f;  // display scale on the stuck foam (0 = crest foam only)
	float foamTexel = 0.4f;        // level 0 texel (model m); x 4 per level, 512^2 texels each
	float foamThreshold = 0.5f;    // stuck foam density (amount / Jacobian) where it turns on
	float foamEdge = 0.33f;        // that threshold's half-width (smaller = crisper foam)
	float foamFineWaves = 0.5f;    // 0..1: the finest cascade's share in the Jacobian the foam reads
	float foamDetail = 1.25f;      // scale on the sub-band detail slope in the foam's lighting normal
	float foamDrift = 2.0f;        // % of the wind speed: the surface drift along the swell's travel

	// --- Shore interaction (driven by the terrain streamer's baked terrain-data map) ---
	float shoalScale = 0.005f;     // the shore's APPROACH BAND depth, x the mid cascade's patch size
	// Past horizonDepthRange the waves assume at least horizonDepth of water, whatever the baked map says (distant
	// depth errors all run SHALLOW). Only the seabed moves, never the surface. 0 range = off.
	float horizonDepth = 2.0f;
	float horizonDepthRange = 3000.0f;
	float shoreFoamDepth = 8.0f;   // surf band: water-column height (m) that churns white; 0 = off
	float shoreFoamMax = 1.0f;     // surf band opacity cap: keeps the refracted bottom visible through the foam
	float swashAmp = 0.5f;         // swash run-up: the fraction of the raw wave height that runs up the beach (0 = hard cutoff)
	float shoreFoamBias = 0.1f;    // surf fold-threshold shift: negative = sparser/more transparent surf
	float swashFlow = 0.5f;        // backflow: horizontal chop on the tongue (recede flows seaward; 0 = off)
	float cullMargin = 1.0f;       // VS land cull: footprint buried deeper than this = triangle discarded (0 = off)
	float farCullError = 4.0f;     // land cull from the FAR terrain cascade: flat error allowance (m); 0 = near-only
	bool  drySectorCull = true;    // whole-sector skip where the baked terrain buries a sector's entire footprint

	// --- Ray tracing budget (ocean.fs.glsl per-pixel refraction/reflection rays; all live) ---
	float rtRefractionRange = 30.0f;    // max refracted-ray length (m): underwater visibility of traced geometry
	int   debugMode = 0;                // OCEAN_DEBUG_MODE shader variant (pipeline reload on change)
	bool  rtReflections = true;         // OCEAN_RT_REFLECTIONS shader variant (pipeline reload on toggle)
	float rtReflectionRange = 3000.0f;  // max mirror-ray length (m): how distant scenery still reflects
	float rtReflectionMaxRough = 0.25f; // roughness above which the mirror ray is skipped (blurred-sky fallback)
	float rtReflectionFog = 0.2f;       // fog on mirror rays: 1 = the reflected source's own fog, 0 = off
	float rtRayCutoffDist = 0.0f;       // camera distance (m) beyond which NO rays trace; 0 = unlimited
};

// THE world-scale conversion (Froude similarity): every MODEL-metre setting in WORLD metres. The one place it is
// applied - OceanGenerator::pushOceanParams and the renderer's lockable ocean values both read it. Lengths x s,
// per-metre densities / s, the spectrum clock x sqrt(s); dimensionless ratios pass through untouched.
export struct OceanWorldScaled
{
	float s;                  // max(World scale, 0.001)
	glm::vec3 cascadeSizes;
	glm::vec3 absorption;     // 1/m
	float fetchKm;
	float depth;
	float horizonLevelOffset;
	float horizonDepth;
	float horizonDepthRange;
	float detailFadeDist;
	float sssStrength;        // per metre of crest height
	float bubbleDepth;
	float bubbleBlur;
	float foamTexel;
	float cullMargin;
	float farCullError;
	float rtRefractionRange;
	float rtReflectionRange;
	float rtRayCutoffDist;
	float timeScale;          // sqrt(s): Froude periods are x sqrt(s)
};

export inline OceanWorldScaled oceanWorldScaled(const OceanSettings& o)
{
	const float s = glm::max(o.worldScale, 0.001f);
	return OceanWorldScaled{
		.s = s,
		.cascadeSizes = o.cascadeSizes * s,
		.absorption = o.absorption / s,
		.fetchKm = o.fetchKm * s,
		.depth = o.depth * s,
		.horizonLevelOffset = o.horizonLevelOffset * s,
		.horizonDepth = o.horizonDepth * s,
		.horizonDepthRange = o.horizonDepthRange * s,
		.detailFadeDist = o.detailFadeDist * s,
		.sssStrength = o.sssStrength / s,
		.bubbleDepth = o.bubbleDepth * s,
		.bubbleBlur = o.bubbleBlur * s,
		.foamTexel = o.foamTexel * s,
		.cullMargin = o.cullMargin * s,
		.farCullError = o.farCullError * s,
		.rtRefractionRange = o.rtRefractionRange * s,
		.rtReflectionRange = o.rtReflectionRange * s,
		.rtRayCutoffDist = o.rtRayCutoffDist * s,
		.timeScale = std::sqrt(s),
	};
}

export namespace Settings
{
	void registerOcean(OceanSettings& s);
}
