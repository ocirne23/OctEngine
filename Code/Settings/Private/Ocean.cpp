module Settings.Ocean;

import Core;
import Core.glm;
import Settings.Tweaks;

// "World scale", "Ring cell (m)", "Ring resolution", "Rings" and "Horizon band" rebuild the clipmap:
// OceanGenerator::initialize attaches that listener.
void Settings::registerOcean(OceanSettings& s)
{
	Tweak::boolean("Ocean", "Enabled", &s.enabled);
	// The ocean's "Meters per pixel": every metre-valued tweak in this panel is a MODEL metre, and the
	// sea is drawn at model x scale - wavelengths, heights, cascades, the shore depths, the clipmap
	// cells and the optical depths all shrink together, so the coastline keeps its look on a compressed
	// terrain (terrain mpp 3 = scale 0.1). The waves keep the model sea's PERIODS (the spectrum clock
	// slows by sqrt(scale) to undo the Froude speed-up), so the miniature moves like the model in
	// slow motion rather than racing. Rebuilds the grid (ring cell).
	Tweak::floatVar("Ocean", "World scale", &s.worldScale, 0.01f, 4.0f, 0.01f); // 1 = model scale
	Tweak::floatVar("Ocean", "Ring cell (m)", &s.ringCell, 0.02f, 2.0f, 0.005f);
	Tweak::intVar("Ocean", "Ring resolution", &s.ringRes, 64, 512, 4.0f);
	Tweak::intVar("Ocean", "Rings", &s.rings, 1, 10, 1.0f);
	// One coarse quad band from the outermost ring's edge to the camera far plane, so the sea always
	// reaches the horizon; its geometry is band-limited to the coarsest mips (near-flat), which is
	// exactly what sub-pixel waves at that distance resolve to anyway.
	Tweak::boolean("Ocean", "Horizon band", &s.horizonBand);
	// The band is exempt from the land cull (its triangles far exceed the cull's footprint bound),
	// so it draws over distant terrain: sink it a little and its crests stay under ground sitting
	// near sea level. Only the band moves, so large values leave a step at its inner seam.
	Tweak::floatVar("Ocean", "Horizon level offset (m)", &s.horizonLevelOffset, -20.0f, 5.0f, 0.1f);
	// Negative = displacement sampled finer than the ring's Nyquist (slight shimmer while moving);
	// with fixed-cell rings the default 0 is already motion-stable.
	Tweak::floatVar("Ocean", "Detail bias", &s.detailBias, -2.0f, 2.0f, 0.05f);

	// TMA/JONSWAP spectrum inputs (Horvath 2015); re-evaluated on the GPU every frame, so all live.
	// The sea's wind is THE wind ("Sky/Wind": its direction, its speed x this - the U10 the spectrum takes).
	Tweak::floatVar("Ocean/Waves", "Wind speed scale", &s.windSpeedScale, 0.0f, 100.0f, 0.1f);
	Tweak::floatVar("Ocean/Waves", "Fetch (km)", &s.fetchKm, 1.0f, 2000.0f, 1.0f);
	Tweak::floatVar("Ocean/Waves", "Depth (m)", &s.depth, 1.0f, 500.0f, 0.5f);
	// Flow -> wind steering: near a coast the SIMULATION wind turns toward the baked flow directions
	// (waves roll in toward the local shore); away from any shore it returns to the wind angle above.
	Tweak::boolean("Ocean/Waves", "Flow steers wind", &s.windSteerEnabled);
	Tweak::floatVar("Ocean/Waves", "Steer rate (deg/s)", &s.windSteerRate, 0.0f, 90.0f, 0.5f);
	Tweak::floatVar("Ocean/Waves", "Steer range (m)", &s.windSteerRange, 0.0f, 2000.0f, 10.0f);
	Tweak::floatVar("Ocean/Waves", "Amplitude scale", &s.amplitude, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Ocean/Waves", "Choppiness", &s.choppiness, 0.0f, 2.5f, 0.01f);
	Tweak::floatVar("Ocean/Waves", "Normal strength", &s.normalStrength, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Ocean/Waves", "Cascade 0 (m)", &s.cascadeSizes.x, 16.0f, 2000.0f, 1.0f);
	Tweak::floatVar("Ocean/Waves", "Cascade 1 (m)", &s.cascadeSizes.y, 4.0f, 500.0f, 0.5f);
	Tweak::floatVar("Ocean/Waves", "Cascade 2 (m)", &s.cascadeSizes.z, 1.0f, 100.0f, 0.1f);

	Tweak::color3("Ocean/Shading", "Absorption (1/m)", &s.absorption);
	Tweak::color3("Ocean/Shading", "Scatter color", &s.scatterColor);
	Tweak::floatVar("Ocean/Shading", "Scatter strength", &s.scatterStrength, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Ocean/Shading", "Roughness", &s.roughness, 0.02f, 0.5f, 0.001f);
	// Sharper sun glints: sharpness biases the shading-normal mips finer (some shimmer past ~1.5),
	// filtering scales the roughness-widening variance terms (0 = raw sharp GGX, 1 = fully filtered).
	Tweak::floatVar("Ocean/Shading", "Glint filtering", &s.glintFilter, 0.0f, 2.0f, 0.05f);
	// The capillary band below the finest cascade: slope variance the FFT can never carry, added to
	// the microfacet roughness at every distance. Without it the LEAN variance is 0 at mip 0 and the
	// near water falls onto the 0.02 roughness clamp - a sky mirror, the "plastic" look. Raise it for
	// a duller, wetter near field; 0 restores the mirror.
	Tweak::floatVar("Ocean/Shading", "Micro roughness", &s.microRoughness, 0.0f, 0.05f, 0.0005f);
	// The shading slope's fold-over soft limit. It also compresses the steep crest faces, so LOWER =
	// sharper crests (and creases at real folds), 0 = no limit at all.
	Tweak::floatVar("Ocean/Shading", "Crest slope limit", &s.crestSlopeLimit, 0.0f, 1.0f, 0.01f);
	// Sub-band detail: the finest cascade's own gradients re-sampled at "Detail scale" x its patch
	// size, in a domain rotated by "Detail rotation" so the borrowed field cannot line up with the
	// cascade it came from. Shading slope only - the geometry and the buoyancy mirror never see it.
	// It fades out past "Detail fade" because this band is absent from the LEAN moments, so what the
	// mips filter away would just disappear instead of turning into roughness.
	Tweak::floatVar("Ocean/Shading", "Detail strength", &s.detailStrength, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Ocean/Shading", "Detail scale", &s.detailScale, 0.02f, 1.0f, 0.01f);
	Tweak::floatVar("Ocean/Shading", "Detail fade (m)", &s.detailFadeDist, 0.0f, 500.0f, 5.0f);
	Tweak::floatVar("Ocean/Shading", "Detail rotation (rad)", &s.detailRotation, 0.0f, 3.14159265f, 0.01f);
	// Crest SSS (Sea of Thieves-style): sun shining through back-lit crests, scaled by wave height.
	Tweak::floatVar("Ocean/Shading", "SSS strength", &s.sssStrength, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Ocean/Shading", "SSS power", &s.sssPower, 1.0f, 16.0f, 0.1f);
	Tweak::floatVar("Ocean/Shading", "Underside transmission", &s.undersideTransmission, 0.0f, 1.0f, 0.01f); // sky through Snell's window from below; less = more internal reflection
	Tweak::boolean("Ocean/Shading", "Hit lighting", &s.hitLighting); // lights on geometry seen through/mirrored in the water
	Tweak::color3("Ocean/Shading", "Foam color", &s.foamColor);
	// One instant-foam response (thresholds + softness) draws the crest foam AND injects the foam field.
	Tweak::floatVar("Ocean/Foam", "Fold bias", &s.foamBias, 0.0f, 1.2f, 0.01f);
	Tweak::floatVar("Ocean/Foam", "Break accel (g)", &s.foamBreakAccel, 0.05f, 1.5f, 0.01f);
	Tweak::floatVar("Ocean/Foam", "Softness", &s.foamSoftness, 0.02f, 2.0f, 0.01f);
	// The model wind ("Ocean/Waves/Wind speed") from which the surf band has its full "Shore foam depth" (it
	// narrows to off in a calm).
	Tweak::floatVar("Ocean/Foam", "Foam wind full (m/s)", &s.foamWindFull, 0.0f, 60.0f, 0.1f);
	// The world-space foam field: ONE foam amount sticks to the water it formed on (it stays behind as the
	// crest moves on) and drifts downwind - white foam above "Foam threshold", the bubble cloud below it.
	Tweak::floatVar("Ocean/Foam", "Foam decay", &s.foamSurfaceDecay, 0.5f, 0.9995f, 0.0005f);
	Tweak::floatVar("Ocean/Foam", "Surface foam", &s.foamSurfaceStrength, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Ocean/Foam", "Foam texel (m)", &s.foamTexel, 0.1f, 4.0f, 0.05f);
	Tweak::floatVar("Ocean/Foam", "Foam drift (% wind)", &s.foamDrift, 0.0f, 10.0f, 0.1f);
	// The stuck foam's coverage: a threshold on its density over the live Jacobian (packs where the water
	// converges, tears where it stretches).
	Tweak::floatVar("Ocean/Foam", "Foam threshold", &s.foamThreshold, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Ocean/Foam", "Foam edge", &s.foamEdge, 0.0f, 0.5f, 0.005f);
	// The finest cascade's share in the Jacobian the foam reads: its short, fast waves reshape foam every frame.
	Tweak::floatVar("Ocean/Foam", "Foam fine waves", &s.foamFineWaves, 0.0f, 1.0f, 0.01f);
	// The foam's lighting normal: the sub-band detail slope at this scale ("Foam flatten" eases the large waves).
	Tweak::floatVar("Ocean/Foam", "Foam detail", &s.foamDetail, 0.0f, 4.0f, 0.01f);
	// The bubble cloud (ocean_bubbles.inc.glsl): the foam amount itself - how deep it floats and how bright it scatters.
	Tweak::floatVar("Ocean/Foam", "Bubble depth (m)", &s.bubbleDepth, 0.0f, 10.0f, 0.05f);
	Tweak::floatVar("Ocean/Foam", "Bubble brightness", &s.bubbleBrightness, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Ocean/Foam", "Bubble blur (m)", &s.bubbleBlur, 0.0f, 32.0f, 0.1f);
	// The foam's sun term on the wave normal eased toward up: bent crests stop going dark at grazing angles.
	Tweak::floatVar("Ocean/Foam", "Foam flatten", &s.foamFlatten, 0.0f, 1.0f, 0.01f);

	// Shore interaction: driven by the terrain streamer's baked terrain-data map (nothing baked here;
	// no data while terrain rendering is disabled - the ocean then behaves as open sea).
	// "Shoal depth scale" sizes the APPROACH BAND (x the mid cascade's patch size, floored at two
	// swash reaches): the depth over which open water eases to the swash amplitude. See
	// oceanSwashFadeIn / oceanSurfaceWeight in ocean_wave.inc.glsl.
	Tweak::floatVar("Ocean/Shore", "Shoal depth scale", &s.shoalScale, 0.0f, 0.1f, 0.001f);
	// Past "range" the waves assume at least "Horizon depth" of water whatever the map says (see the
	// header): distant depth readings all err shallow, and shallow reads as a dead mirror sea. Only
	// the assumed seabed moves - the surface stays put, so this cannot put water over land.
	Tweak::floatVar("Ocean/Shore", "Horizon depth (m)", &s.horizonDepth, 0.0f, 200.0f, 1.0f);
	Tweak::floatVar("Ocean/Shore", "Horizon depth range (m)", &s.horizonDepthRange, 0.0f, 8000.0f, 50.0f);
	Tweak::floatVar("Ocean/Shore", "Shore foam depth (m)", &s.shoreFoamDepth, 0.0f, 8.0f, 0.05f);
	Tweak::floatVar("Ocean/Shore", "Shore foam max", &s.shoreFoamMax, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Ocean/Shore", "Swash amplitude", &s.swashAmp, 0.0f, 2.0f, 0.01f);
	Tweak::floatVar("Ocean/Shore", "Shore foam bias", &s.shoreFoamBias, -1.0f, 1.0f, 0.01f);
	Tweak::floatVar("Ocean/Shore", "Swash backflow", &s.swashFlow, 0.0f, 3.0f, 0.01f);
	// Land cull: clipmap triangles buried deeper than this under the local water level (over their
	// whole footprint) are discarded in the vertex shaders - no displacement sampling, no raster.
	Tweak::floatVar("Ocean/Shore", "Cull margin (m)", &s.cullMargin, 0.0f, 4.0f, 0.05f);
	// Beyond the near terrain cascade (~860 m) the cull uses the FAR cascade with this flat burial
	// error allowance in meters (covers the far mesh LODs' drift off the bake). Narrow rivers the
	// coarse far bake cannot resolve may lose triangles out there - speed over accuracy; raise it
	// if that shows, 0 = never cull from far data.
	Tweak::floatVar("Ocean/Shore", "Far cull error (m)", &s.farCullError, 0.0f, 20.0f, 0.25f);
	// Whole-sector skip when the baked terrain buries a sector's entire footprint (see the header):
	// deep inland the ocean then costs nothing at all.
	Tweak::boolean("Ocean/Shore", "Dry sector cull", &s.drySectorCull);

	// Ray-tracing budget: the water shader traces the scene TLAS per pixel for refraction (seeing
	// geometry through the water) and reflection (scenery mirrored in it). Refraction range = how far
	// underwater stays visible (the ~99% Beer-Lambert extinction bound still applies on top, so this
	// caps the clear-water case); reflection rays skip above the roughness cutoff (a wide lobe cannot
	// be represented by one mirror sample - the blurred sky stands in); the ray cutoff distance stops
	// ALL rays past that camera distance (refraction falls back to the analytic baked-terrain bottom,
	// reflections to the atmosphere - the same paths misses already take), 0 = unlimited.
	Tweak::floatVar("Ocean/RT", "Refraction range (m)", &s.rtRefractionRange, 1.0f, 100.0f, 1.0f);
	// 1 depth, 2 swash, 3 surface weight, 5 shore foam band, 6 mirror ray (legend: ocean.fs.glsl)
	Tweak::intVar("Ocean", "Debug mode", &s.debugMode, 0, 6);
	Tweak::boolean("Ocean/RT", "Reflections", &s.rtReflections); // scene mirror ray (pipeline reload on toggle)
	Tweak::floatVar("Ocean/RT", "Reflection range (m)", &s.rtReflectionRange, 50.0f, 10000.0f, 50.0f);
	Tweak::floatVar("Ocean/RT", "Reflection max rough", &s.rtReflectionMaxRough, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Ocean/RT", "Reflection fog", &s.rtReflectionFog, 0.0f, 4.0f, 0.01f); // 1 = a reflection hazes like its source seen directly; also the terrain film's
	Tweak::floatVar("Ocean/RT", "Ray cutoff dist (m)", &s.rtRayCutoffDist, 0.0f, 10000.0f, 50.0f);
}
