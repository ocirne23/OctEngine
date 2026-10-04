export module RendererVK:Settings;

import Core;
import Core.glm;

// Renderer configuration parameter blocks. Each exposes itself to the TweakPanel via registerTweaks();
// the Renderer owns one instance of each and feeds them into the per-frame UBO / push constants.

// Everything in the TweakPanel's "Sky" categories (Sky / Sky/Sun / Sky/Atmosphere / Sky/Clouds /
// Sky/Stars / Sky/Nebula / Sky/Moon).
export struct SkyParams
{
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f); // sky "up" axis; also the sky radiance light direction

    // Sun
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.635f, 0.763f, -0.122f));
    glm::vec3 sunColor = glm::vec3(0.9568f, 1.0f, 0.9214f);
    float sunIntensity = 3.0f;
    float sunAngularCos = 0.99998f;     // cos of the disc radius (1 = disc off)
    float sunGlow = 1.0f;               // sun halo strength (0 = none, ~0.5 subtle, 2 = heavy); HG forward lobe in sky.fs
    float sunRolloff = 1.25f;           // sky highlight roll-off: soft-clips the overexposed sun region so its gradient survives (0 = raw hard clip)
    float sunRolloffKnee = 0.75f;       // luminance where compression starts at full roll-off (lower = more range compressed)
    float sunRolloffHeadroom = 6.0f;    // brightness range the shoulder absorbs (higher = brighter values stay distinguishable)

    // Ambient + sky radiance (the non-sun lighting inputs)
    glm::vec3 ambientColor = glm::vec3(1.0f);   // flat non-physical minimum ambient
    float ambientIntensity = 0.003f;
    glm::vec3 skyRadianceColor = glm::vec3(0.55f, 0.65f, 1.0f); // directional sky radiance (moonlight/space light), along up
    float skyRadianceIntensity = 0.05f;

    // Ground plane of the sky sphere (below the horizon): albedo lit by sun + sky. Also tints the
    // ground-bounce fallback in skyRadiance() for downward GI/fog rays. Can stay 0: the GI probe gather
    // fills its range-bounded below-horizon misses from the mirrored sky instead (giMissRadiance), so
    // black ground no longer paints probe-spaced dark dots on sunlit floors.
    glm::vec3 groundColor = glm::vec3(0.9f, 0.9f, 1.0f);
    float groundIntensity = 0.0f;
    float groundHorizon = 0.25f;  // fraction of every hemisphere treated as sunlit terrain in the
                                  // out-of-GI-range fallback (skyGroundRadiance): up-facing surfaces see
                                  // ground near the horizon on rolling terrain, not a clean sky plane -
                                  // 0 = flat-world (up-facing surfaces get pure sky), raise to brighten
                                  // distant terrain toward what the probes see

    // Atmosphere scattering: multipliers on the Earth sea-level Rayleigh/Mie coefficients. These drive
    // both the visible sky and (through skyRadiance) the majority of the indirect sky lighting.
    float rayleighScatter = 1.0f;
    float mieScatter = 1.0f;
    float scatterBoost = 4.0f;          // in-scatter multiplier: how much sunlight the atmosphere scatters (more = more indirect light)
    float mieG = 0.76f;                 // Mie anisotropy (forward-scatter lobe)
    float rayleighHeight = 8500.0f;     // Rayleigh scale height (m): how fast air density falls off
    float mieHeight = 1200.0f;          // Mie scale height (m): how high the haze layer reaches
    float mieExtinction = 1.11f;        // Mie extinction/scattering ratio (> 1 = absorbing haze)
    float ozone = 4.0f;                 // ozone absorption strength (1 = Earth-like); absorbs green/yellow,
                                        // suppresses the green horizon band single scattering produces

    // Stars
    float starDensity = 0.63f;
    float starSize = 1.3f;              // base star core size multiplier
    float starSizeVar = 0.62f;          // 0 = uniform size, 1 = full per-star variation (skewed small)
    float starBrightness = 1.23f;
    float starColorVar = 0.85f;         // 0 = white, 1 = full cool/warm per-star tint

    // Nebula (milky-way band)
    float nebulaIntensity = 0.2f;       // band glow strength (0 = off)
    float nebulaScale = 5.7f;           // noise frequency
    float nebulaBandWidth = 0.1f;       // gaussian width of the band around its great circle
    float nebulaDust = 1.0f;            // dark dust lane strength inside the band
    glm::vec3 nebulaAxis = glm::normalize(glm::vec3(0.706f, -0.418f, 0.572f)); // band pole

    // Moon
    glm::vec3 moonDirection = glm::normalize(glm::vec3(0.728f, 0.659f, -0.190f)); // independent of the sun
    float moonSizeDeg = 6.0f;           // disc radius (degrees); real moon is ~0.26
    float moonBrightness = 0.3f;

    void registerTweaks();
};

// Volumetric clouds (CloudPipeline) - the TweakPanel's "Sky/Clouds" categories. All UBO-driven, so
// changes apply live. A spherical shell [bottom, top] above sea level (world Y 0) around a planet
// centre under the camera; the noise is world-anchored and moves with the wind.
export struct CloudParams
{
    bool  enabled = true;
    // Shape
    float bottom = 300.0f;             // shell bottom altitude (m)
    float top = 2000.0f;               // shell top altitude (m)
    float coverage = 0.75f;            // 0 = clear, 1 = overcast
    float coverageVariation = 1.75f;   // weather-map spread around the coverage (0 = uniform); fades in over coverage 0..0.25, so coverage 0 = clear
    float cloudType = 1.0f;            // 0 = stratus, 0.5 = cumulus, 1 = cumulonimbus
    float typeVariation = 1.0f;       // weather-map spread around the type
    // Per-column LIFT: whole clouds rise by up to this fraction of the shell height (0 = every base at the shell
    // bottom - flat, aligned bases); the clouds' own height shrinks to (1 - this). A field drifting over
    // kilometres, uncorrelated with the tower height.
    float baseVariation = 0.05f;
    // The vertical profile's shape (both layers), 0..1 each:
    float towerVariation = 0.55f;      // how far a cloud's top may drop below the layer top (0 = all reach it)
    float topRoundness = 0.1f;         // 0 = the plain taper (cones), 1 = flat-shouldered domes
    float baseSharpness = 0.26f;       // 0 = soft, wispy bases, 1 = flat bases
    float towerCoreLink = 1.0f;        // 0 = the top follows the tower field only (tilted ramps), 1 = the cloud's own coverage (its core rises)
    // SHELVES: stable layers (inversions) at fixed heights of the main layer where the clouds that reach them spread
    // out into flat tiers (stratocumulus cumulogenitus; at the top of a storm, the anvil).
    int   shelfCount = 1;              // 0..3
    float shelfStrength = 0.55f;       // how far a cloud spreads at a shelf
    float shelfThickness = 0.1f;       // half thickness (fraction of the layer height)
    float shelfSpacing = 0.2f;         // from one shelf to the next (fraction of the layer height); the stack is centred in the layer
    // The UPPER layer: an independent band with its own coverage and type (a stratiform / altocumulus deck over
    // the main layer's cumulus). The march shell covers both bands.
    bool  upperEnabled = true;
    float upperBottom = 6000.0f;       // m
    float upperTop = 7500.0f;          // m
    float upperCoverage = 0.6f;       // x the main layer's coverage
    float upperType = 0.1f;            // 0 = stratus (thin sheets), 0.5 = cumulus
    float upperHeightVariation = 0.2f; // per-column lift, fraction of the upper band (the column shrinks to 1 - this)
    float upperDensity = 0.1f;        // x the main layer's density ("Density (1/m)" scales both)
    float densityScale = 0.015f;       // extinction (1/m) at density 1
    float erosion = 0.66f;             // detail noise erosion of the base shapes
    float erosionCutoff = 0.15f;       // densities under this after the erosion are removed (the thin haze rest), the rest remapped to 0..1
    float curl = 150.0f;               // curl-noise distortion of the detail noise (m): wispy edges
    float weatherSizeKm = 20.0f;       // weather map period (km): the size of cloud clusters and gaps
    int   baseRepeats = 6;             // base noise tiles per weather tile (base period = weather / this)
    int   detailRepeats = 12;          // detail noise tiles per base tile
    float windSpeed = 10.0f;           // m/s
    float windAngleDeg = 30.0f;        // wind direction in XZ (degrees)
    float evolveSpeed = 0.5f;          // vertical drift of the detail noise (m/s): shapes change, not only move
    // Lighting
    float dropletSize = 20.0f;         // water droplet diameter (um) of the HG + Draine phase fit (5 .. 50)
    float forwardPeakLimit = 0.95f;    // cap on the fit's HG g (0.995 at 20 um: a ~0.3 degree diffraction lobe that
                                       // saturates to a sun-sized white blob behind thin cloud); 1 = uncapped
    float multiScatter = 0.8f;        // octave attenuation of the multiple-scattering approximation (0 = single scattering)
    float multiScatterStrength = 1.25f; // x the closed-form sum of ALL multiple-scattering octaves (non-physical above 1):
                                       // the sunlit side seen with the sun behind the viewer lives on it alone
    float ambient = 1.0f;              // sky ambient strength
    float groundAlbedo = 0.15f;      // ground bounce onto the cloud bottoms
    float groundLightDepth = 300.0f;  // m: how far the ground bounce reaches up into a cloud (exponential falloff over the height)
    float powder = 0.0f;              // dark-edge "powder" term strength (0 = off)
    float aerialStrength = 2.0f;      // x the air light added in front of the clouds (1 = physical; the dimming stays);
                                       // 0 = no aerial perspective at all (the clouds as lit: no haze, no dimming)
    // Shadows (the Beer shadow map)
    bool  shadows = true;
    float shadowStrength = 1.0f;       // 0 = the clouds cast no shadow on the scene
    float shadowNearKm = 1.0f;         // near cascade extent (km)
    float shadowFarKm = 40.0f;       // far cascade extent (km)
    int   shadowNearSteps = 16;        // map march steps per texel, near cascade
    int   shadowFarSteps = 32;         // map march steps per texel, far cascade
    float shadowFarSoftness = 1.5f;    // far cascade lookup jitter per pixel and frame, in texels (TAA-resolved penumbra)
    // PROGRESSIVE UPDATES: each frame a cascade renders 1 / split of its texels (interleaved), so the cost is
    // the same every frame. 0 = every texel every frame, 1 = 1/4 (2x2), 2 = 1/16 (4x4), 3 = 1/64 (8x8).
    // The far cascade at 1/64: a full cycle is 64 frames, and the wind moves the clouds ~11 m in that time
    // at 10 m/s - under a third of a far texel (~39 m at 40 km).
    int   shadowNearSplit = 1;
    int   shadowFarSplit = 3;
    bool  selfShadowFromMap = false;   // the clouds' own sun shadow from the map (else the light march)
    // Quality
    int   maxSteps = 360;             // view march step budget per pixel
    float maxDistanceKm = 45.0f;       // view march range (km)
    float nearStep = 90.0f;           // step length at the camera (m); a MAXIMUM: a ray with a shorter path through
                                       // the shell uses span / "Steps per ray", so it still takes its full step count
    float minStep = 3.0f;              // the floor of that per-ray near step (m): finer steps only re-read the same noise
    int   stepsPerRay = 180;          // view march steps over a ray's shell span: each ray solves its step growth to take
                                       // this many, so the quality through the layer holds whatever its top / bottom
    int   lightSteps = 4;              // sun march steps per dense sample
    float lightDistance = 8000.0f;     // MAX sun march reach (m): the reach is the way out of the sample's own layer
                                       // toward the sun, (layer top - altitude) / sun y, capped by this for a low sun
    float temporalBlend = 0.9f;        // history weight of the temporal accumulation
    float skyMapHistorySec = 1.0f;     // s: the sky-map clouds' temporal blend reaches 95 % of a change in this time (0 = no history)
    float giSkyHistorySec = 4.0f;      // s: the same for the GI layer (observers around the camera: each march a new one, so it needs more)
    float giSkyObserverRadius = 4000.0f; // m: the GI layer's observers are spread over a disc this wide around the camera
    float nearDetailRadius = 300.0f;   // extra high-frequency erosion within this camera distance (m)
    float detailDistanceKm = 12.0f;    // the detail erosion fades out over the last 20 % of this distance; no detail fetches past it
    bool  checkerboard = true;         // the march covers half the pixels per frame; the temporal pass fills the rest (CLOUD_CHECKERBOARD)
    int   debugMode = 0;               // 0 off, 1 step count, 2 density only, 3 history rejection

    // The three bools (enabled, shadows, selfShadowFromMap), the debug mode and "powder above 0" are BAKED shader
    // defines (CLOUDS, CLOUD_SHADOWS, CLOUD_SELF_SHADOW_MAP, CLOUD_DEBUG_MODE, CLOUD_POWDER - Shader.cpp's
    // preamble): onDefinesChanged reloads the shaders when one of them changes.
    void registerTweaks(const oc::function<void()>& onDefinesChanged);
};

// Sun shadow cascade distribution (raster path; RT sun shadows ignore these) - the TweakPanel's
// "Shadows" category. Consumed per frame by computeSunCascades, so changes apply live.
export struct ShadowParams
{
    float maxDistance = 2500.0f; // farthest distance receiving sun shadows (m), measured from the shadow
                                // focus (Renderer::setSceneFocus - the player in game mode; the camera when
                                // unset). Lower = every cascade covers less ground = sharper shadows
                                // everywhere, at range cost.
    float splitLambda = 0.80f;  // cascade split scheme: 0 = uniform splits, 1 = logarithmic (resolution
                                // bunches up near the camera)
    float casterPad = 4000.0f;   // how far up-sun a caster may sit above a cascade and still be captured
                                // (m). Must exceed the tallest shadow-casting feature (mountains!); too
                                // small clips casters out of the depth map and their shadows pop away.
    float depthBias = 0.001f;    // sun cascade depth bias
    float normalBias = 1.0f;     // sun cascade normal bias (texels)

    // Long-range terrain sun shadows. Past maxDistance (and past the RT path's TLAS range) both shadow
    // sources run out of data, so distant ground goes uniformly lit - a hard terminator across a mesh
    // ring that reaches ~33 km. The baked terrain height cascades still have data far beyond that, so
    // distant pixels cone-march them instead (terrainSunVisibility) and take the darker of the two. These
    // apply to BOTH shadow modes; only the terrain map has to be up.
    float terrainMarchStart = 1000.0f; // camera distance where the march fades in (m; 0 = off). Sits
                                // inside maxDistance on purpose: inside the overlap both terms see the
                                // same ridge, so the handover has nothing to show.
    float terrainMarchBias = 150.0f;   // first sample distance up the sun ray (m). THE self-shadow knob:
                                // the map's texels are 8 m near / 132 m far and carry none of the sub-
                                // texel relief that is actually rendered, so a surface point sits on a
                                // heightfield coarser than itself. Too small = acne on lit slopes.
    // Debug overlay, BAKED as the SHADOW_DEBUG define into the lit fragment variants (a change reloads
    // the static mesh pipeline through the Renderer's callback - no uniform, no per-pixel cost when off):
    // 0 off, 1 cascade index tint, 2 the cascade cross-fade band, 3 the raw sun visibility, 4 shadow-map
    // texel size heat. Cascade data only exists on the PCSS path (RT sun off).
    int debugMode = 0;
    float terrainMarchSpread = 0.02f;  // penumbra growth per metre along the ray. Deliberately far wider
                                // than the true sun disc (~0.005): the softness is what keeps the map's
                                // texels from resolving as stair-steps, and what keeps the doubling
                                // sample spacing self-consistent (this is a cone trace, not a point march).
    void registerTweaks(const oc::function<void()>& onReloadShaders); // debugMode is a baked define
};

// FOLIAGE cards (MATERIAL_FLAG_BILLBOARD - the tree billboards) - "Foliage ..." in the TweakPanel's "Trees" category.
// UBO-driven (u_foliageParams / u_foliageParams2), so changes apply live.
export struct FoliageParams
{
    float crownNormal = 0.25f; // blend of the shading normal toward a CROWN normal - the view ray's hit on a
                                // sphere around the card's centre (0 = the baked normals only; 1 at the axis
                                // regardless). The same for both crossed cards, so the shading no longer splits
                                // at their crossing axis (each card's bake shades its own view's side).
    float shadowLength = 3.5f;  // the sun shadow fades in over this many metres between the receiver and its
                                // blockers (0 = hard, as any surface): light reaches into a crown, so its own
                                // crossed cards darken it gradually instead of with a hard edge at the crossing
                                // axis. Blockers far up-sun (another tree, terrain) still shadow fully.
    float interiorShadow = 0.5f; // darkening of the leaves deep INSIDE the crown, by the billboards' BAKED interior
                                // (from the piece's own crown field: any crown shape), on the sun and the ambient
                                // alike (0 = off) - a fully lit crown otherwise looked flat.
    float interiorStart = 0.25f; // the baked interior (0 = the crown's surface, 1 = about its core depth) where the
    float interiorEnd = 0.56f;  // darkening starts / reaches full strength (smoothstep between)
    // On the SUN (direct + transmitted) the interior fades out by this x |V.L|: from the front the leaves seen through
    // the gaps are lit through those gaps, from the back the self-shadow below darkens the whole crown. The ambient keeps
    // it (AO). 0 = the interior on the sun from every side.
    float interiorViewFade = 1.0f;
    // THE CROWN SELF-SHADOW (billboards): the sun x exp(-this x the chord from the view ray's entry on the crown
    // ellipsoid along the sun, in lateral radii) - a crown seen toward the sun is dark as a whole, the far side of a
    // side-lit one darker. 0 = off.
    float selfShadow = 3.0f;
    float transmissionSelfShadow = 0.3f; // the share of that self-shadow the TRANSMITTED sun takes (it scatters forward
                                         // through the leaves: at 1 a crown seen toward the sun hid the transmission)
    float interiorTopCardScale = 0.5f; // on a whole tree's HORIZONTAL card the interior term ^ this: > 1 darker,
                                // < 1 lighter (an exponent - a strength multiplier saturated at strength 1)
    // EDGE-ON fade: a card fades out (dithered) as |N.V| falls from edgeFadeEnd to edgeFadeStart - a grazing
    // card smears its texture into streaks. Near the crossing axis both thresholds are x edgeFadeCentreScale
    // (back to x1 at half the crown radius): < 1 keeps the centre, > 1 fades it sooner.
    float edgeFadeStart = 0.1f;
    float edgeFadeEnd = 0.6f;
    float edgeFadeCentreScale = 1.33f;
    // TRANSMISSION (MATERIAL_FLAG_LEAF: leaf clusters + billboards): the sun through the leaves - a diffuse back
    // term saturate(-N.L) plus a forward glow saturate(V.-L)^focus x glow, both x the leaf colour x strength.
    // Its sun visibility leans on the shadow by transmissionShadow only (0 = ignore it): a leaf seen from the
    // shaded side sits in its own crown's shadow, which would leave it no glow. The interior term still applies.
    float transmission = 0.6f;
    float transmissionFocus = 64.0f;
    float transmissionGlow = 1.5f;
    float transmissionShadow = 0.95f;
    // LEAVES (MATERIAL_FLAG_LEAF) seen nearly edge-on: their shading normal is bent toward the viewer until N.V reaches
    // this (0 = off). The cards' normals are bent (NormalBend), so N.V can approach 0 while N.L > 0: the sun's GGX at
    // grazing (Fresnel -> 1, the Smith term growing) lit a single leaf near the sun hundreds of times brighter than
    // its diffuse - a blinding spot with bloom.
    float minNoV = 0.25f;
    float edgeFadeTopCardScale = 1.0f; // x both thresholds on a whole tree's HORIZONTAL card (on top of the centre
                                // scale): > 1 hands the top-down view to the vertical cards sooner
    // A tree casts into sun cascade c only while its distance from the cascades' centre (the scene focus) minus its
    // radius stays within that cascade's split + this (m): far trees out of the near cascades. The margin keeps the
    // long shadows of trees up-sun, which fall into a nearer cascade's range (a 20 m tree at a 17 degree sun: ~65 m).
    float shadowCascadeMargin = 64.0f;
    // A tree farther than this (m) from the scene focus is not in the TLAS (no GI / RT shadow / RTAO / reflection hits;
    // 0 = only the general "RT/TLAS Range"): every tree there is an overlapping box every ray has to traverse.
    float rtRange = 500.0f;
    // "Trees/Debug view" - baked as TREE_DEBUG on the lit mesh fragments (a change reloads them; 0 = no define):
    // 1 = a colour per MATERIAL (each representation and fade copy has its own: a switch shows as a colour change),
    // 2 = a colour per MESH (the LOD level the cull picked: an LOD step shows), 3 = the distance FADE side (red = a
    // fade-out material, green = fade-in, white = none).
    int debugView = 0;

    void registerTweaks(const oc::function<void()>& onDebugViewChanged);
};

// PROCEDURAL GRASS ("Grass" tweaks; GrassPipeline, grass.inc.glsl). UBO-driven (u_grass*), so everything is live but
// "Blades per patch", which rebuilds the blade index buffer. Desktop only. Colours are picked as sRGB (the UBO build
// converts them).
export struct GrassParams
{
    bool enabled = true;
    int bladesPerPatch = 1024;     // blades in a full patch (<= GRASS_MAX_BLADES): the near density is this / patch size^2
    float patchSize = 2.0f;        // m; ideally a divisor of the terrain chunk size
    float range = 145.0f;          // m: no blade past it
    float rangeFade = 45.0f;       // m: the blades thin out to none over the last metres of the range
    float bladeHeight = 1.0f;      // m
    float heightVariation = 0.6f;  // 0..1: the shortest blade is (1 - this) x the height
    float bladeWidth = 0.15f;      // m at the root
    float rootSink = 0.05f;        // m below the mesh (+ half the tessellated relief, so no root floats over a hollow)
    float thinStart = 20.0f;       // m: all blades inside it; past it (start / d)^exponent of them
    float thinExponent = 1.0f;
    float widthCompensation = 0.5f; // the kept blades get (1 / kept fraction)^this wider: 1 = the same coverage
    float maxWidthScale = 4.0f;
    float lod1Distance = 10.0f;    // m: 8 segments per blade inside it, 4 to LOD 2, 2 to LOD 3, then 1 (a triangle)
    float lod2Distance = 30.0f;
    float lod3Distance = 60.0f;
    float lodMorphBand = 0.3f;    // the blades geomorph into the next LOD over this fraction of its distance (0 = pop)
    float minPixelWidth = 1.0f;    // px: a blade is never narrower (fights the far shimmer)
    float groundBlendDistance = 80.0f; // m at which the normal has blended "Ground normal blend" of the way to the ground's
    float groundBlend = 0.7f;
    float windAngleDeg = 30.0f;
    float windBend = 0.15f;        // the steady push on the tip, in blade heights
    float gustBend = 0.35f;
    float gustSize = 12.0f;        // m
    float gustSpeed = 5.0f;        // m/s
    float swayFrequency = 0.6f;    // Hz
    // All wind movement eases out between these camera distances (m): small far blades moving read as grain.
    float windFadeStart = 15.0f;
    float windFadeEnd = 25.0f;
    float curvature = 0.4f;        // the blades' own lean, in blade heights (random direction)
    float clumpSize = 5.0f;        // m: the size of the clumps and bare spots
    float patchiness = 0.6f;       // 0 = even cover, 1 = clumps with bare ground between
    float bareFraction = 0.25f;    // about this share of the ground is the bare / thin part of the clump pattern
    float growBand = 0.7f;         // a blade shrinks into the ground over this fraction of the kept range (no pops)
    float sizeByCover = 1.0f;      // 0..1: where the cover fades (climate, slope, water), the blades also get shorter and
                                   // narrower by the cover - not only fewer (sparse long stalks otherwise)
    // WHERE it grows is the terrain textures' logic (grass_cull.cs.glsl): the ground layer's coverage (no rock, beach,
    // snow) x the grass amount of its climate-picked textures (Procedural's TERRAIN_TEX_SOURCES .grass).
    // The look (sRGB).
    glm::vec3 rootColor = glm::vec3(0.20f, 0.27f, 0.08f);
    glm::vec3 tipColor = glm::vec3(0.45f, 0.53f, 0.20f);
    glm::vec3 dryColor = glm::vec3(0.58f, 0.52f, 0.30f);
    float colorVariation = 0.2f;
    // COLD: the albedo darkens by up to coldDarkening as the ground's temperature falls from warmTemperature to
    // coldTemperature (C, at the ground's height).
    float coldDarkening = 0.7f;
    float coldTemperature = 0.0f;
    float warmTemperature = 20.0f;
    float dryAmount = 0.2f;       // share of the ground with dry, straw-coloured blades (in patches)
    float roughness = 0.5f;
    float rootOcclusion = 0.8f;
    float shadowBias = 0.5f;       // m: the blade's sun shadow lookup moves this far toward the sun at the root (none at
                                   // the tip) - the sunk root sits under the ground's shadow-map surface otherwise
    // THE CANOPY's self-shadowing (grass.inc.glsl): the grass layer as a volume of blades - the sun reaching a point
    // below its top is exp(-extinction x the path along the sun), extinction = this x blades per m^2 x the mean blade
    // width x the local density. On the blades and on the ground under them. 0 = off.
    float canopyShadow = 0.10f;
    // SUN FLECKS: the light through the canopy in bright and dark spots of about this size (m), at this contrast (0 =
    // an even gray), faded out from half the fade distance to it (m).
    float fleckSize = 0.08f;
    float fleckContrast = 0.7f;
    float fleckFadeDistance = 40.0f;
    float fleckStretch = 1.0f;     // the flecks stretch along the sun into streaks: x 1 / tan(sun elevation) (0 = round)
    // THE NEAR GRASS CASCADE: real blade shadows around the camera - an extra sun shadow-map layer (2048^2) over a box
    // of +-nearShadowRange metres (texel = 2 x range / 2048), the blades only, on the blades and the ground under them.
    // It blends into the canopy over the box's outer part. nearShadowBias: the receiver's shift toward the sun (m).
    bool nearShadows = true;
    float nearShadowRange = 8.0f;
    float nearShadowBias = 0.15f;
    float nearShadowStrength = 0.7f; // 0..1: how dark the near blade shadows get (match it to the canopy at the hand-over)
    float transmission = 0.9f; // the sun through a blade from behind
    float roundness = 0.1f;        // the normal's tilt toward the blade's edges

    void registerTweaks(const oc::function<void()>& onBladesChanged);
};

// FAR TREES as a marched volume (TreeVolumePipeline, "Trees/Far ..." tweaks): the GPU tree sets' trees baked into
// a camera-centred POLAR volume (angle x log radius, startDistance .. endDistance: cells grow with the distance) and
// marched there - beyond the billboards, kilometres out. The volume geometry re-bakes on change; the resolutions
// recreate the images.
export struct FarTreeParams
{
    bool enabled = true;
    float startDistance = 600.0f; // the volume / march starts here (m; x the camera height, Renderer::farTreesStart)
    float endDistance = 12000.0f;  // and ends here (m)
    float overlap = 64.0f;         // the billboards draw to startDistance + this; the volume fades in over it (m)
    uint32 angularRes = 3000;      // texels around (cell = r x 2 pi / this).
    uint32 radialRes = 1500;       // texels from start to end (cell = r x ln(end / start) / this)
    uint32 slices = 15;            // height slices
    float height = 22.0f;         // m above the column's tree floor the volume covers
    float densityScale = 0.25f;   // x the baked extinction
    float blobShrink = 0.4f;    // 1/m off the baked extinction before the scale: blobs shrink toward their cores
    float stepScale = 2.0f;       // march step, x the cell size
    uint32 maxSteps = 600;
    float ambient = 1.0f;        // x the real sky's irradiance on the canopy (GI's sky map, hemisphere mean; 1 = physical)
    float sunScale = 1.0f;        // the direct sun's factor (a leaf's mean cosine toward the sun)
    float selfShadow = 1.0f;     // x the sun taps' optical depth: how dark the crowns' insides / shaded sides get
    float normalStrength = 0.5f;  // 0..1: the sun term toward max(N.L, 0), N from the density gradient (3 more taps)
    float groundDarkening = 1.0f; // how much darker the sky light is at the ground than at the volume's top
    float interiorShadow = 1.0f;// darkening of a blob's CORE: exp(-this x the mean extinction of 6 taps around x
                                  // their distance), on the sun and the sky alike (0 = off; the taps cost only then)
    float interiorRadius = 0.0f;  // the taps' distance, x the cell size
    float forwardScatter = -0.25f; // Henyey-Greenstein g of the sun term (> 0: backlit crowns glow)
    float albedoScale = 1.0f;    // x the leaf colour
    float saturationScale = 0.85f; // the leaf colour's saturation: 0 = grey (its luminance), 1 = as baked, > 1 more vivid
    float temporalBlend = 0.0f;  // the history's weight in cloud_temporal's TREE_TEMPORAL pass (0 = off: no pass, no images)
    // The march at HALF resolution (each 2x2 block's centre, to its farthest surface; a depth-aware upsample after the
    // temporal pass - half res runs the temporal pass, at weight 0 it only reconstructs) and / or PIXEL SKIPPING: per
    // frame only 1 of 2 pixels (the checkerboard) or 1 of 4 (one per 2x2 block) is marched, the rest copied from a
    // persistent image of each pixel's latest march (tree_volume_march.cs.glsl). Under the temporal pass the pass
    // reconstructs instead, and 1 of 4 runs as 1 of 2 (its checkerboard).
    bool halfRes = false;
    int pixelSkip = 1;            // 0 = off, 1 = 1 of 2 (checkerboard), 2 = 1 of 4
    bool temporalPath() const { return temporalBlend > 0.0f || halfRes; }
    float rebakeDistance = 64.0f; // the camera moves this far from the bake centre -> re-bake
    int floorSmoothing = 2;       // the tree floor's tent blur radius, in columns (tree_volume_floor_smooth.cs); 0 = off
    // The world tree records' chunks within this of the bake centre (m) expand into their trees and bushes and splat in
    // detail; beyond, each record adds its mass per column (TreeVolumePipeline RecordSource).
    float recordDetail = 4000.0f;
    // A bake runs over about this many frames (the record splat's three passes spread evenly, plus a few steps of their
    // own); the march reads the previous bake until the last one. 1 = as fast as it goes.
    int bakeFrames = 12;
    void registerTweaks();
};

// Volumetric fog (froxel grid; see VolumetricFogPipeline) - the TweakPanel's "Fog" categories.
// All UBO-driven, so changes apply live.
export struct FogParams
{
    bool  enabled = true;
    float density = 0.010f;        // global extinction at the height base (1/m)
    float heightBase = 0.0f;       // world height where the global fog is densest
    float heightFalloff = 0.25f;   // exponential density falloff above the base (1/m)
    float terrainFollow = 1.0f;   // fraction of the local terrain height added to the height base (needs a
                                   // terrain height map, see Renderer::setFogTerrainHeightMap): 0 = flat fog,
                                   // 1 = fog hugs the terrain at constant depth; in between it reaches higher
                                   // on mountainsides but still clears the peaks
    glm::vec3 albedo = glm::vec3(0.839f, 0.961f, 1.0f); // #D6F5FF
    float albedoIntensity = 1.0f;  // > 1 is a non-physical gain (emissive-ish fog)
    // Non-physical gain on the SUN in-scatter only (froxels + far field): sunlit fog - the light shafts - brightens,
    // shadowed fog (ambient only) and the extinction do not. Strong god rays through thin fog, without fogging up
    // the world.
    float sunScatter = 1.0f;
    // SHAFT HAZE: a thin medium for the god rays alone - sunlit in-scatter only (x "Sun scatter"), no extinction, no
    // ambient - reaching up to the clouds (its own scale height from the fog's height base). The fog itself is a
    // height fog, nearly gone a few tens of metres up, so its shafts needed a cranked base density. 0 = off.
    float shaftHazeDensity = 0.001f;   // 1/m
    float shaftHazeHeight = 200.0f;   // m: the scale height (density / e per this much height)
    // AERIAL PERSPECTIVE: the sky's own Rayleigh + Mie atmosphere between the camera and the scene (the blue haze on
    // distant terrain), baked per frame into a frustum LUT (aerial_lut.cs.glsl) and laid behind the fog by the apply.
    // Strength scales the air's density along the view ray only (1 = the sky's atmosphere; the sun light stays as the
    // sky has it); 0 = off.
    float aerialStrength = 1.0f;
    float aerialMaxDistanceKm = 40.0f; // the LUT's depth: past it the scene takes the value at this distance
    float anisotropy = 0.15f;      // HG phase g (0 = isotropic, ->1 = forward scattering)
    float range = 1024.0f;         // froxel grid far distance (m). With the far field on, this is a
                                   // near-field quality knob rather than a view distance: shortening it
                                   // concentrates the same 128 slices where the medium actually has
                                   // structure (local lights, fog volumes, noise)
    bool  farField = true;         // extend the fog past `range` analytically instead of with more slices
                                   // (vol_apply's volFarField); unbounded, so the horizon fully fogs
    float farFieldDensity = 1.0f;  // far-field deviations from the near field's own fog. At 1/1 the two are
    float farFieldThickness = 1.0f; // one continuous medium; near fog is usually authored far thicker than
                                   // anything readable over tens of km, hence the knobs. Thickness scales
                                   // heightFalloff's scale height (> 1 = thicker at range)
    float farFieldMaxDistanceKm = 40.0f; // the far field integrates up to this distance from the camera (0 = unbounded).
                                   // Past it a cloud and the scene behind it see the SAME fog, so the fog apply
                                   // evaluates it once for both (vol_apply.fs.glsl)
    int   farFieldSteps = 4;      // ground samples along the far segment. Each sub-segment between them is
                                   // solved exactly, so this sets how finely terrain-follow tracks the
                                   // ground, and low counts smooth it rather than adding noise
    float slicePower = 1.0f;       // froxel Z distribution exponent: 1 = plain exponential slices, < 1
                                   // shifts Z resolution from near to far (worth ~0.7-0.85 at long ranges)
    float terrainShadowDist = 512.0f; // froxels beyond this distance sun-shadow by marching the terrain
                                   // height map instead of TLAS rays / cascade taps (both run out of data
                                   // at distance - TLAS is range-bounded); needs the terrain fog map
    float regionStrength = 1.0f;   // how much the baked regional fog fields (terrain data map channel B:
                                   // thickness + height-falloff mul, Procedural's sampleFogThickness /
                                   // sampleFogHeightFalloff) modulate the height fog: 0 = uniform fog,
                                   // 1 = fully region-driven
    float shaftBoost = 10.0f;        // non-physical gain on the underwater sun in-scatter (fog only, not
                                    // surfaces): water scatters little at fog densities, so physically
                                    // correct shafts are faint - this makes them readable. 1 = physical.
    float causticStrength = 1.5f;   // underwater caustic focus (fold-Jacobian light pattern on submerged
                                    // surfaces + fog shafts; underwater_light.inc.glsl): 0 = off, > 1
                                    // exaggerated. Beer-Lambert depth absorption is separate (Ocean/Absorption).
    float causticDepthFade = 0.25f; // caustic contrast decay with depth (1/m) - approximates defocus;
                                    // higher = the pattern washes out closer to the surface
    float causticShoreFade = 1.0f;  // caustic contrast ramps in over this much water depth (m), so the
                                    // pattern dissolves at the terrain-waterline intersection; 0 = off
    float underwaterDensity = 1.0f; // multiplier on the global density at/below the LOCAL water surface
                                    // (terrain data map water level; always-on murk, immune to regional
                                    // thickness): thick murk under thin morning haze, or 0 to disable
    float underwaterWaveOffset = 0.25f; // lowers the underwater fog boundary (the local water surface, the live
                                    // wave displacement on top) by this x the deepest live wave trough (m per
                                    // m): the fog's coarse froxels miss steep waves' troughs, and the murk
                                    // peeked through them - more so the higher the sea
    float noiseScale = 0.08f;      // density noise frequency (1/m)
    float noiseStrength = 0.5f;    // 0 = uniform fog, 1 = fully modulated (dusty wisps)
    float windSpeed = 1.5f;        // noise drift (m/s)
    float temporalBlend = 0.8f;    // history blend weight (jittered Z integration)
    bool  lightShadows = true;     // shadow ray per froxel per grid light (expensive)
    int   sunRays = 1;             // sun shadow rays per froxel (RT sun mode); main perf knob
    float sunSoftness = 0.02f;     // shadow ray cone half-angle (rad); softens + decorrelates the rays
    bool  spatialFilter = true;    // 3x3 tent on the scatter grid in the integrate pass
    bool  giAmbient = true;        // GI probe ambient (off = analytic sky only, cheaper)

    void registerTweaks();
};

// Exposure + tonemapping, applied in the composite pass (the HDR -> display mapping) - the
// TweakPanel's "Post" category. Baked into the composite push constants, so changes re-record.
export struct PostParams
{
    float exposureEV = 0.0f; // exposure in stops; manual exposure, or exposure compensation in auto mode
    int   tonemapper = 3;    // 0 = off (legacy raw clip), 1 = Reinhard, 2 = ACES, 3 = AgX
    bool  autoExposure = true; // eye adaptation: drive exposure from scene luminance
    float adaptTau = 3.0f;   // adaptation time constant (s); larger = slower eye
    float adaptKey = 0.25f;  // target middle-grey luminance
    float adaptMinLogLum = -3.14f; // histogram log2-luminance range
    float adaptMaxLogLum = 4.0f;
    float adaptMinEV = -6.0f; // auto-exposure clamp (stops)
    float adaptMaxEV = 0.0f;

    // onReRecord is invoked when a tweak that's baked into the composite push constants changes, so the
    // command buffers can be re-recorded.
    void registerTweaks(const oc::function<void()>& onReRecord);
};

export struct RTParams
{
    bool enabled = true;        // master: builds BLAS/TLAS and drives RTAO + all ray-traced shadows.
                                // Off -> no acceleration structures built at all (sun falls back to PCSS
                                // cascades, lights unshadowed, RTAO + GI off). Diagnostic A/B switch.
    bool giEnabled = true;      // GI probe clear/trace + probe indirect contribution (needs enabled)
    bool rtSunShadow = false;   // sun shadows from TLAS ray queries instead of PCSS cascades (A/B tweak)
    int  sunShadowRays = 5;     // RT sun shadow rays per pixel
    bool rtLightShadows = true; // ray-traced shadows for punctual/area/tube lights
    bool rtSkyRadiance = true;  // ray-traced sky visibility for the sky radiance light (GI probe trace)
    int  blasLodLevel = 0;      // LOD level whose geometry backs a chain's single shared BLAS (clamped per
                                // chain; rays don't need per-level fidelity). Applied when containers load.
    bool blasCompaction = true; // copy-compact static BLASes after build (~30-50% of their memory back);
                                // applies to BLASes built after a change
    // onReRecord: the GI toggle is baked into the cached GI command buffer. onReloadLitShaders (also
    // re-records): the master, "RT Sun" and "RT Lights" are baked into the lit fragment shaders.
    void registerTweaks(const oc::function<void()>& onReRecord, const oc::function<void()>& onReloadLitShaders);
    bool effectiveSunShadow() const { return enabled && rtSunShadow; }
    bool effectiveLightShadows() const { return enabled && rtLightShadows; }
};

// The clustered light grid's distance LOD (LightGridComputePipeline::build, on the CPU): each
// occupied GRID_SIZE^3 grid is split into cells of `cellSize` world units, picked per grid from its
// view distance:
//   level    = floor(pow(max(dist - lodStart, 0) / lodStep, lodPower))
//   cellSize = clamp(minCell << level, minCell, maxCell)
// so cells start at minCell (1 = a cell per world unit, the full GRID_SIZE^3 resolution) and double
// every "step" of the curve; lodPower 0.5 gives the old sqrt ramp, 1 a linear one. minCell ==
// maxCell pins one resolution everywhere. Read every frame: a change takes effect at once.
export struct LightGridParams
{
    float lodStart = 0.0f;  // m: no coarsening inside this distance
    float lodStep = 16.0f;  // m: the curve's unit distance (one doubling at lodPower 1)
    float lodPower = 0.5f;  // curve exponent on (dist - start) / step
    int   minCellLog2 = 0;  // finest cell size = 2^n world units (0 = 1 m: GRID_SIZE cells per axis)
    int   maxCellLog2 = 1;  // coarsest cell size = 2^n world units (5 = 32 = one cell per grid)
    // A light spanning more cells than this inside ONE grid becomes that grid's LARGE light (one
    // entry, evaluated by every pixel in the grid) instead of a per-cell candidate: it bounds the
    // candidate list every cell of a full-res grid loops, and the header entry is one uint16.
    int   cellBudget = 1024;
    // Forward-pass lighting debug overlay (computeLitColor, instanced_indirect_lit.inc.glsl), BAKED as
    // the LIGHT_GRID_DEBUG define on the lit fragment variants (a change reloads the static mesh
    // pipeline; every debug branch folds away at 0): 0 off, 1 light grid cells (random colour per
    // grid), 2 per-cell light count heat (green -> red at the cell cap, magenta = the cell's hash
    // lookup missed), 3 light ranges (blue per covering light).
    int   debugMode = 0;

    // onReloadLitShaders: debugMode (the static mesh pipeline's lit fragments). The LOD params need
    // no callback: the CPU build reads them every frame.
    void registerTweaks(const oc::function<void()>& onReloadLitShaders);
};

export struct RTAOParams
{
    bool  enabled = true;
    int   rays = 6; //1;
    float radius = 1.0f;
    float power = 1.5f;
    float intensity = 1.0f;
    float fadeStart = 100.0f;  // distance (m) from the SCENE FOCUS (Renderer::setSceneFocus - the player in game
                               // mode; the camera when unset) where AO begins to fade out
    float maxDistance = 120.0f; // focus distance at which AO is fully gone (trace early-out); 0 disables the falloff
    float normalBias = 0.02f;    // constant ray-origin offset along the surface normal (m)
    float distanceBias = 0.001f; // ray-origin offset toward the camera per meter of view distance: absorbs
                                 // the depth-reconstruction error (grows with distance, lies along the view
                                 // ray) that caused self-hit banding on distant sloped terrain
    float maxHistory = 0.77; //0.99f;
    int   blurRadius = 2; //0;
    bool  alphaTest = false; // ray-test alpha-masked geometry (vegetation) instead of treating it as solid

    void registerTweaks(const oc::function<void()>& onReRecord, const oc::function<void()>& onReloadShaders);
};

export struct TAAParams
{
    bool  taaEnabled = true;
    float taaFeedback = 0.9f;
    // History weight cap on OCEAN pixels: waves animate but the reprojection is camera-only (no motion
    // vectors), so full-weight history blurs the specular sparkle away. Lower = crisper, shimmerier water.
    float taaOceanFeedback = 0.2f;

    void registerTweaks(const oc::function<void()>& onReRecord);
};

// DLSS Super Resolution through Streamline ("Post/DLSS" tweaks; RendererVK:Streamline, DlssPipeline). Any mode
// but Off REPLACES TAA (desktop only). The mode sets the render resolution: a change re-creates the render-size
// targets (onResize), so the mode tweak waits for the GPU.
export struct DlssParams
{
    int mode = 2;           // Streamline::DlssMode: Off, DLAA, Quality (default), Balanced, Performance, Ultra Performance
    // 0 = the DLSS default for the mode, else J / K / L / M. Default M: DLAA's own default (K) smeared the trees'
    // alpha-tested foliage under camera rotation, L / M do not, and M is closer to K's cost (sl_dlss.h).
    int preset = 4;
    bool mipBias = true;    // negative texture LOD bias by the render scale (log2(render / output))
    // DLSS's current-colour bias on ocean pixels (no motion vectors on the waves): 1 = no history there. The
    // default matches TAA's "Ocean feedback" 0.2 history weight.
    float oceanBias = 0.8f;
    // Streamline's verbose log (Assets/Local/Streamline/); SL reads it at slInit, so it applies at the next start.
    bool verboseLog = false;

    void registerTweaks(const oc::function<void()>& onResize, const oc::function<void()>& onReRecord);
};

// Motion blur ("Post/Motion blur" tweaks; MotionBlurPipeline). The blur is the motion over the EXPOSURE, a
// shutter fraction of the frame: physically, a higher frame rate blurs less.
export struct MotionBlurParams
{
    bool  enabled = true;
    float shutter = 0.5f;     // exposure / frame time (0.5 = a 180 degree shutter; > 1 exaggerates)
    float maxRadius = 24.0f;  // px (the blur is at most twice this); capped at RendererVKLayout::MOTION_BLUR_TILE
    float cameraScale = 1.0f; // the camera's share of the blur (0 = moving objects only)
    int   samples = 12;       // gather samples per blurred pixel

    void registerTweaks(const oc::function<void()>& onReRecord);
};

// Bloom ("Post/Bloom" tweaks; BloomPipeline). With a threshold (> 0) only the light above it, in EXPOSED units,
// goes into the blur, which is then ADDED: dark and mid tones never blur. Threshold 0 = the physical, energy-
// conserving mix(scene, blur, intensity), where every pixel spreads a share (keep the intensity small then).
export struct BloomParams
{
    bool  enabled = true;
    float intensity = 0.3f;  // the blur's weight (additive with a threshold, the mix share without)
    float threshold = 1.0f;  // exposed brightness where the glow starts (1 = display white); 0 = no threshold
    float knee = 0.5f;       // the soft ramp's half width around the threshold (exposed units)
    float radius = 0.75f;    // level weights 2^(k (2 radius - 1)): 0.5 = equal, higher = wider glow, less haze
    int   levels = 6;        // mip levels of the chain (each doubles the reach); BloomPipeline::MAX_LEVELS at most

    void registerTweaks(const oc::function<void()>& onReRecord);
};

// Mesh LOD chains (authored "LodN_*" meshes and/or meshopt-generated) - the TweakPanel's "LOD" category.
// Selection runs per instance at renderNode time; generate/generateLevels/minIndices are read at
// ObjectContainer load, so they only affect containers loaded after a change.
export struct MeshLodParams
{
    bool  enabled = true;         // per-instance LOD selection (off = everything renders LOD0)
    float maxErrorPixels = 0.33f;  // screen-space error budget: coarsest level whose geometric deviation projects below this is used (generated chains carry per-level meshopt errors)
    float fullResPixels = 256.0f; // FALLBACK metric for chains without error data (authored LodN_): projected diameter (px) above which LOD0 is used; each halving drops one level
    int   bias = 0;               // coarseness bias: doubles the error budget per step (fallback: levels added)
    float hysteresis = 0.25f;     // switch dead-band: fraction of the error budget (fallback: fraction of a level) a change must overshoot before switching
    int   forceLod = -1;          // >= 0: clamp every LOD instance to this level (debug)
    bool  generate = true;        // meshopt-generate chains for static meshes without authored LODs
    int   generateLevels = 4;     // max generated levels beyond LOD0
    float generateReduction = 0.5f; // index-count factor per generated level (0.25 = quarter the triangles)
    int   minIndices = 32;        // don't generate for meshes below this index count

    void registerTweaks();
};

// The TweakPanel's "Particles" category: the GPU particle sim's own knobs plus the weather inputs the
// Renderer folds into the per-frame UBO (cameraVelocity.w, weatherWind0/1/2, rainOcclusion*). All live -
// the particle stage's primary CB re-records every frame, so none of them needs a re-record callback.
export struct ParticleParams
{
    bool  enabled = true;         // the whole GPU particle chain (sim + draw stage)
    bool  collision = true;       // depth-buffer collision in the sim
    float timeScale = 1.0f;       // multiplier on the sim delta
    bool  logStats = false;       // prints GPU alive/dead counts ~once a second

    // Rain occlusion: a top-down depth map of the weather volume, so roofs shelter what is under them.
    bool  rainOcclusion = false;      // off by default: the map costs a cull + depth pass per frame
    float rainOcclusionCasterPad = 100.0f; // how far above the box a roof still shelters (m)
    float rainOcclusionTolerance = 0.25f;  // depth below the surface before a drop counts as sheltered (m)

    float streakCameraBlur = 0.15f; // fraction of the camera velocity the weather streaks subtract
    float anisotropy = 0.33f;       // the lit particles' scattering phase g (0 = isotropic, forward < 1)

    // Weather wind for the volumes. A storm: speed 15, gust strength 8, sheet contrast 0.7, sheet drift 6.
    float windSpeed = 1.0f;         // m/s
    float windAngleDeg = 0.0f;      // direction the wind blows TOWARDS, degrees from +X around +Y
    float windGustStrength = 5.0f;  // m/s, amplitude of the 2D gust vector added to the mean (calm air flurries too)
    float windGustSize = 50.0f;     // m, the gust field's feature size
    float windSheetContrast = 0.5f; // [0,1] alpha density bands sweeping through
    float windSheetSize = 50.0f;    // m
    float windSheetDrift = 5.0f;    // m/s the fields travel along the wind direction on top of half the wind speed

    void registerTweaks();
};

// The TweakPanel's "Ocean" spray knobs - the ocean spray step (ocean_spray.cs.glsl) is the particle GPU
// spawn path's first producer, so these ride the UBO (oceanSpray0/1/2) rather than OceanParams, which
// Procedural::OceanGenerator overwrites wholesale every frame. MODEL units, like the ocean tweaks: the UBO
// build applies OceanParams::worldScale.
export struct OceanSprayParams
{
    float rate = 20.0f;       // spawns per m^2 per s at full breaking
    float radius = 60.0f;     // m, the producer grid's half extent around the scene focus
    float threshold = 0.003f; // instant-foam value where spray starts
    float kick = 0.0f;        // m/s upward
    float speed = 10.0f;      // m/s along the wind
    float forward = 3.0f;     // m, spawn lead ahead of the crest along its travel (negative = behind)
    float height = 0.0f;      // m, spawn offset above the surface (negative = below)

    void registerTweaks();
};

// FFT/Tessendorf ocean: spectrum inputs for the GPU simulation (OceanSimulationPipeline) + water shading.
// The Renderer feeds these into the per-frame UBO (ocean* fields), which drives BOTH the compute simulation
// (the TMA spectrum is re-evaluated every frame, so all of it is live) and the surface shading. The grid
// geometry + Tweaks live in Procedural::OceanGenerator, which builds one of these each frame and hands it to
// Renderer::setOceanParams.
export struct OceanParams
{
    bool enabled = false;       // gates the per-frame FFT simulation + ocean draw

    // Spectrum (TMA = JONSWAP x Kitaigorodskii finite-depth attenuation, Hasselmann directional spreading;
    // Horvath 2015). Dispersion is finite-depth: w^2 = g k tanh(k D).
    glm::vec2 windDirection = glm::normalize(glm::vec2(0.8f, 0.35f)); // dominant wave travel direction (XZ)
    float windSpeed   = 10.5f;  // U10 wind speed (m/s): the main sea-state knob
    float fetchKm     = 300.0f; // fetch (km): distance the wind has blown over; longer = bigger swell
    float depth       = 100.0f; // ocean depth D (m): finite-depth dispersion + TMA shallow-water attenuation
                                // (shallow values like 35 visibly mute the long swell - by design)
    float horizonLevelOffset = -0.5f; // vertical shift (m, usually negative) of the HORIZON BAND only.
                                // The band is exempt from the land cull (its triangles are far larger
                                // than the cull's footprint bound), so it draws over distant terrain;
                                // sinking it a little keeps its crests under near-sea-level ground.
    float amplitude   = 1.0f;   // artistic scale on the spectrum amplitude (1 = physical)
    float choppiness  = 1.25f;   // horizontal displacement lambda (0 = heightfield only, higher = sharper crests)
    float normalStrength = 1.0f; // artistic scale on the shading slopes
    glm::vec3 cascadeSizes = glm::vec3(1536.0f, 188.0f, 25.0f); // FFT patch sizes (m); each TILES with its
                                // own size, so the largest sets how often the sea repeats - keep it many
                                // times the peak wavelength. Non-rational ratios keep the three from
                                // re-aligning; scale them as a SET (the band split ties cascade c+1's
                                // range to L_c, so growing one alone just moves the repetition down)
    float seaLevel    = 0.0f;   // world Y of the calm water plane
    float detailBias  = 0.0f;   // bias on the ring-matched vertex displacement mip (negative = finer;
                                // the clipmap rings carry their cell size per vertex, so 0 is motion-stable)

    // Optics: extinction drives Beer-Lambert absorption of the ray-traced refraction (1/m, Jerlov-ish
    // coastal water); scatter is the in-scattered radiance albedo (the water's "color" in deep water).
    glm::vec3 absorption   = glm::vec3(0.42f, 0.085f, 0.04f);
    glm::vec3 scatterColor = glm::vec3(0.012f, 0.08f, 0.085f);
    float scatterStrength  = 1.0f;
    float roughness        = 0.07f; // perceptual micro-roughness (widens the sun glint)
    float glintFilter      = 0.5f;  // 1 = full variance widening, 0 = none (raw sharp GGX)
    // Slope variance of the waves BELOW the finest cascade's Nyquist - the capillary band the FFT cannot
    // represent at any distance. The LEAN term only returns variance the MIP CHAIN filtered away, and it
    // is exactly 0 at mip 0, so near the camera the roughness used to collapse onto its 0.02 clamp and
    // the water mirrored the sky (plastic). Added to alpha^2 in the same form as LEAN (alpha^2 = 2 sigma^2)
    // and deliberately NOT scaled by glintFilter: this band is missing from the spectrum, not filtered
    // out of it. Dimensionless, so the world scale leaves it alone.
    float microRoughness   = 0.008f;
    // Rational soft limit on the shading slope, s /= 1 + k * |s| (ocean_wave.inc.glsl). It keeps the
    // near-fold division from exploding into dark creases, but it compresses exactly the steep crest
    // faces, which reads as blobby. 0 = no limit (sharpest crests, creases possible at folds).
    float crestSlopeLimit  = 0.0f;
    // Sub-band detail (oceanDetailSlope): the finest cascade's gradient field re-sampled at
    // detailScale x its patch size, in a domain rotated by detailRotation, added to the SHADING slope
    // only. Wave statistics at a shorter wavelength than the FFT band holds, for one fetch - no extra
    // memory, no extra FFT, and the displacement (so geometry, prepass and the CPU buoyancy mirror) is
    // untouched. Faded out past detailFadeDist because this band is absent from the LEAN moments.
    float detailStrength   = 0.35f; // 0 = off
    float detailScale      = 0.18f; // fraction of the finest cascade's patch size (smaller = finer)
    float detailFadeDist   = 60.0f; // m from the camera (a world metre: scaled); 0 = never fade
    float detailRotation   = 0.9f;  // radians; keeps the borrowed field off the parent's crest lines
    // Crest subsurface scattering (Sea of Thieves-style): back-lit wave crests glow the scatter color,
    // scaled by height above the calm water line. Power shapes the toward-the-sun view lobe.
    float sssStrength      = 0.75f;  // per meter of crest height; 0 disables (and the extra shadow rays with it)
    float sssPower         = 1.0f;
    float undersideTransmission = 1.0f; // scale on the sky seen through Snell's window from below (1 = Fresnel
                                        // transmission; less = more internal reflection, a darker ceiling)
    bool  hitLighting      = false; // evaluate the scene's grid lights at refraction/reflection ray hits
                                    // (OCEAN_HIT_LIGHTS shader variant; toggling reloads the pipeline)
    bool  rtReflections    = true;  // ray-traced mirror of the scene on the top side (OCEAN_RT_REFLECTIONS
                                    // shader variant; off = sky only, no mirror ray compiled in)
    int   debugMode        = 0;     // OCEAN_DEBUG_MODE shader variant (mode list: ocean.fs.glsl); 0 = off
    // Foam. ONE instant-foam response (oceanInstantFoam) both draws the per-pixel crest foam and injects
    // the world-space FOAM FIELD (the foam amount breaking leaves stuck to the water): white foam above
    // foamThreshold, the bubble cloud (and its roughness) from the same amount below it.
    glm::vec3 foamColor    = glm::vec3(0.88f, 0.92f, 0.94f);
    float foamBias         = 0.6f;  // fold threshold: Jacobian below this is folding (foaming)
    float foamBreakAccel   = 0.25f; // breaking threshold (Longuet-Higgins): downward crest acceleration
                                    // above this fraction of g is breaking - what makes LARGE waves foam
    float foamSoftness     = 0.5f;  // edge width of both thresholds (small = crisp crest lines)
    float bubbleDepth      = 3.0f;  // m under the surface: the bubble cloud's water absorbs the red both
                                    // ways, so deeper = darker and more turquoise (a world metre: scaled)
    float bubbleBrightness = 1.0f;  // the cloud's albedo, x foam color
    float bubbleBlur       = 4.0f;  // m (world): the cloud reads the foam field this blurred (a diffuse volume)    // The world-space foam field (ocean_foam.cs.glsl): SURFACE FOAM sticks to the water it formed on (the
    // rest lattice, so it rides the orbits and stays behind as the crest moves on) and drifts downwind.
    float foamSurfaceDecay    = 0.995f; // foam amount retention per frame
    float foamSurfaceStrength = 1.0f;   // display scale on the stuck foam (0 = only crest foam, as before)
    float foamTexel        = 0.5f;  // level 0 texel (m, world); level l = x 4^l, 512^2 texels per level
    // The stuck foam's coverage (oceanStuckFoamCoverage): a threshold on its density amount / Jacobian.
    float foamThreshold    = 0.5f;  // density where the foam turns on
    float foamEdge         = 0.06f; // threshold half-width: smaller = crisper foam edges
    float foamFineWaves    = 0.25f; // 0..1: the finest cascade's share in the Jacobian the foam reads (lower =
                                    // steadier foam shapes; the film's crest + shore foam too)
    float foamDetail       = 1.5f;  // scale on the sub-band detail slope in the foam's lighting normal (the
                                    // large waves' slope is eased by foamFlatten instead)
    float foamDriftSpeed   = 0.3f;  // m/s (world) along the swell's travel: the wind drift of the surface
    float foamFlatten      = 0.6f;  // 0..1: the foam's Lambert normal eased toward up (bent crests stop
                                    // going dark at grazing sun angles)
    bool  cameraUnderwater = false; // per frame, from the CPU mirror: the camera is below the live surface.
                                    // Gates the shader's underside path - a back face seen from above is a fold

    // Shore interaction: the baked terrain-data cascades (Renderer::setFogTerrainHeightMap, baked by the
    // terrain streamer) give the water its depth - open water eases to the swash amplitude across an
    // approach band at the shore (oceanSurfaceWeight, ocean_wave.inc.glsl), the swash tongue runs up the
    // beach and flows back, and a surf/foam band forms where the water column vanishes at the waterline.
    float shoalScale     = 0.005f; // approach band depth as a fraction of the mid cascade's patch size
                                // (floored at two swash reaches; scaled down with the cascade sizes)
    // Horizon depth: past horizonDepthRange the waves assume AT LEAST horizonDepth of water, whatever
    // the baked map says. Every distant depth error runs shallow - coarse texels average shore slopes
    // into the water, the generator reports depth exactly 0 for samples it could not resolve, and the
    // vertical scale compresses real shelves - and shallow is the ruinous direction: it fades the waves
    // out AND (via fade^2) the LEAN variance, leaving a mirror that reflects the sky exactly like wind 0.
    // Only the assumed seabed moves, never the surface, so it cannot put water over land; the land cull
    // still reads the raw map. 0 range = off.
    float horizonDepth      = 30.0f;
    float horizonDepthRange = 1500.0f;
    // Rate of the spectrum's clock relative to the frame clock (1 = real time). OceanGenerator sets
    // sqrt(world scale): its Froude-scaled inputs give a shrunk sea whose periods are x sqrt(s), and this
    // slows the evolution back to the model sea's periods so the miniature does not race. Only the
    // e^{iwt} evolution reads it - the breaking-crest acceleration stays in the spectrum's own time, so
    // the foam criterion (a fraction of g) keeps the model sea's look.
    float timeScale = 1.0f;
    // "Ocean/World scale" itself (s; 1 = the model sea). Nothing above re-applies it - they arrive in world
    // metres already. The spray and the ocean-bound fog metres read it: the model sea at model periods,
    // shrunk by s, so their lengths, speeds and accelerations all ride s.
    float worldScale = 1.0f;
    float shoreFoamDepth = 8.0f;  // water-column height (m) below which the waterline churns white; 0 = off
    float shoreFoamMax   = 0.75f; // surf band opacity cap: shore foam coverage never exceeds this, so the
                                  // refracted bottom stays visible through the lace (whitecaps unaffected)
    float swashAmp       = 0.5f;  // swash run-up: scale on the un-shoaled wave height riding through the
                                  // waterline and up the beach (waves crash and flow over; 0 = hard cutoff)
    float shoreFoamBias  = -0.33f;  // shifts the surf fold threshold: negative = sparser lace / more
                                  // transparent shore waves, positive = denser churn
    float swashFlow      = 0.33f;  // backflow: scale on the raw horizontal chop riding the swash weight -
                                  // the tongue visibly flows back seaward as the wave recedes (0 = off)
    float cullMargin     = 1.0f;  // land cull: clipmap triangles whose whole footprint is buried deeper
                                  // than this under the local water level are VS-culled (0 = off)
    float farCullError = 4.0f;    // land cull from the FAR terrain cascade (beyond the near cascade's
                                  // ~860 m): flat burial error allowance in METERS, covering how far
                                  // the far mesh LODs stray from the bake. Deliberately NOT scaled by
                                  // the far texel - that left everything under tens of meters of
                                  // terrain alive (a visible band of buried water past the near
                                  // handover). Narrow rivers the coarse point-sampled bake cannot
                                  // resolve may lose triangles out there (speed over accuracy);
                                  // 0 = never cull from far data

    // Ray tracing budget (ocean.fs.glsl traces the scene TLAS per pixel for refraction + reflection).
    float rtRefractionRange = 35.0f;  // max refracted-ray length (m): how far underwater geometry stays
                                       // visible through the surface (the ~99% Beer-Lambert extinction
                                       // bound still applies on top, so clear water is the case this caps)
    float rtReflectionRange = 3000.0f; // max mirror-ray length (m): how distant scenery still reflects
    float rtReflectionMaxRough = 0.25f; // filtered roughness above which the mirror ray is skipped and the
                                        // blurred sky stands in (a wide lobe can't be one mirror sample)
    float rtReflectionFog = 0.2f; // fog on mirror rays (ocean + terrain film): 1 = the reflected source's own fog, 0 = off
    float rtRayCutoffDist = 0.0f; // camera distance (m) beyond which NO scene rays are traced: refraction
                                   // falls back to the analytic baked-terrain bottom (the same path RT
                                   // misses take), reflections to the atmosphere. 0 = unlimited
};

// Forcefield bubbles (Force library / ForceFieldPipeline / force_*.glsl). The Force library owns
// these tweaks (TweakPanel "Force" categories) and hands the struct to Renderer::setForceFieldParams
// every frame, like OceanParams; the values are UBO-driven, so all of them are live.
export struct ForceFieldParams
{
    bool enabled = true;             // gates the shell draw + force compute passes
    // LIVE team count (2..MAX_FORCE_TEAMS), a GAME-MODE setting (ForceSystem::setNumTeams - co-op
    // runs 2), not a tweak: changing it recompiles the force shaders (NUM_FORCE_TEAMS define) and
    // remakes the team-sized bake volume/buffers, so per-sample cost and memory fit the mode
    // instead of always paying for 8 teams. MAX_FORCE_TEAMS stays the CAP: the UBO color array
    // size and the "outside every bubble" sentinel.
    uint32 numTeams = 8;
    // HALF-RES union march (rebuild-class toggle, the useGrid pattern): the march + interval
    // targets run at swapchain/2 and a depth-aware upsample blends into scene color; OFF = the
    // march draws directly into scene color at full res and NO march target exists.
    bool unionHalfRes = true;
    // Static per-pixel march-phase jitter (rebuild-class: compiled out of force_union.fs when
    // off): turns step banding into spatial noise so larger "Union step (m)" stays presentable.
    bool unionJitter = false;
    float isoThreshold = 0.15f;      // field strength where an uncontested bubble surface sits
    int marchSteps = 10;             // ray-march steps through a shell proxy's ray interval
    bool useGrid = true;             // hash-grid candidate gathering; off = brute-force scan of every
                                     // emitter per evaluation (small scenes / A-B correctness check).
                                     // Toggling rebuilds the force pipelines (FORCE_GRID define)
    float forceGain = 5.0f;          // scale on the read-back per-emitter applied forces
    float shellAlpha = 0.5f;         // base shell opacity (rim-weighted in the shader)
    float interiorAlpha = 0.0f;      // shell opacity floor when seen from INSIDE the bubble (the rim
                                     // math reads near-zero head-on from within; this keeps the dome visible)
    float backfaceAlpha = 1.0f;      // visibility of the far/inner shell surface seen from OUTSIDE,
                                     // composited behind the front surface (0 = single-surface shell)
    float rimPower = 3.0f;           // fresnel rim exponent
    float rimIntensity = 1.5f;       // rim emissive gain
    float contactGlowIntensity = 0.33f; // seam glow where opposing bubbles press (equal-field zone)
    float contactGlowWidth = 0.15f;    // opposing/own field ratio band that reads as contact
    float contactWallAlpha = 0.5f;     // visibility of the interior equilibrium WALL between two
                                       // pressed opposing bubbles (0 = only the outer seam glows)
    bool logTierDebug = false;         // DEBUG: log each drawable's reach/visible radius/tier once a second
    bool densityView = false;          // DEBUG: draw bubbles as a heatmap of the strongest field along
                                       // the view ray instead of the shell (tip power/merging readout);
                                       // white contour marks the iso threshold
    float densityRange = 2.0f;         // field value that maps to the heatmap's white end
    float junctionSmoothing = 0.5f;    // smooth-max width (fraction of iso) rounding the crease where
                                       // shells meet the wall: continuous normals kill the junction
                                       // jaggies; 0 = hard crease. Queries use the same function, so
                                       // the gameplay inside-test always matches the drawn surface
    float geoGlowDistance = 0.5f;      // glow band where the shell intersects scene geometry (m)
    // SHELL CULLING/LOD (desktop; VR skips the cull - the center frustum is the wrong eye's):
    float minShellPixels = 3.0f;       // a shell whose projected proxy radius is under this skips
                                       // the DRAW entirely (its field/readbacks stay live); 0 = off
    float shellFullResPixels = 160.0f; // projected radius at/above which the march runs the full
                                       // "March steps"; smaller shells taper linearly (floor 8)
    float sampledShellRadius = 8.0f;   // emitters whose VISIBLE bubble radius (forceEmitterVisibleRadius,
                                       // not authored reach) is >= this march the BAKED shell volume
                                       // (two trilinear taps/sample) instead of the analytic
                                       // candidate loop - hits/normals/shading stay analytic.
                                       // 0 = tier off. Small bubbles stay analytic: the fixed-size
                                       // volume's resolution cannot resolve them
    float shellVolumeViewMargin = 10.0f; // the sampled-tier volume's fit is CLIPPED to the camera's
                                       // ground-band view footprint plus this margin (m), so its
                                       // fixed texel grid follows the zoom, not the spread of every
                                       // large bubble; 0 = the unbounded union (old behaviour)
    // UNION MARCH (desktop): the ANALYTIC tier renders as ONE march per pixel - the small proxies
    // only rasterize their ray intervals (MIN-blend), a fullscreen pass marches the per-pixel
    // union once. Kills the overdraw term where small bubbles stack. Off = per-proxy marches (A/B).
    bool unionMarch = true;
    float unionStepSize = 1.5f;        // world metres per union-march step
    int unionMaxSteps = 8;            // hard cap on union steps per pixel
    float patternScale = 0.6f;       // animated surface pattern frequency (1/m)
    float patternSpeed = 0.3f;       // pattern scroll speed
    float patternIntensity = 0.5f;
    // Per-team shell colors (linear rgb); 8 = RendererVKLayout::MAX_FORCE_TEAMS (static_asserted
    // where both are visible - Settings deliberately doesn't import :Layout).
    glm::vec3 teamColors[8] = {
        { 0.20f, 0.55f, 1.00f }, // 0 blue
        { 1.00f, 0.30f, 0.15f }, // 1 red
        { 0.25f, 1.00f, 0.35f }, // 2 green
        { 0.70f, 0.30f, 1.00f }, // 3 purple
        { 1.00f, 0.85f, 0.20f }, // 4 yellow
        { 1.00f, 0.25f, 0.70f }, // 5 magenta
        { 0.20f, 1.00f, 0.90f }, // 6 teal
        { 0.90f, 0.95f, 1.00f }, // 7 white
    };
};

export struct Stats
{
    uint32 numLights;
    uint32 maxLights;

    uint32 numMeshInstances;
    uint32 maxMeshInstances;

    uint32 numInstanceOffsets;
    uint32 maxInstanceOffsets;

    uint32 numMeshTypes;
    uint32 maxMeshTypes;

    uint32 numMaterials;
    uint32 maxMaterials;

    uint32 numRenderNodes;
    uint32 maxRenderNodes;

    uint32 numTextures;
    uint32 maxTextures;

    uint64 vertexDataUsedBytes;
    uint64 maxVertexDataBytes;

    uint64 indexDataUsedBytes;
    uint64 maxIndexDataBytes;

    uint32 numObjectContainers;

    uint32 numLightGrids;
    uint32 maxLightGrids;

    uint64 lightGridMemUsageBytes;
    uint64 maxLightGridMemUsageBytes;

    // Total GPU memory tracked by VMA across all heaps.
    uint64 gpuMemoryUsedBytes;     // bytes our live allocations occupy
    uint64 gpuMemoryReservedBytes; // bytes VMA has reserved in blocks (>= used)
    uint64 gpuMemoryBudgetBytes;   // device-local budget available to the process

    // Texture mip streaming (see TextureStreamer).
    uint64 textureBudgetBytes;
    uint64 textureResidentBytes;   // live allocations of streamable textures
    uint64 texturePinnedBytes;     // unstreamable textures, always fully resident
    uint64 textureDesiredBytes;    // what the priority pass wants resident
    uint64 textureTailBytes;       // always-resident mip tails (part of resident)
    uint32 numStreamableTextures;
    uint32 numStreamOpsInFlight;

    // Static BLAS memory (see AccelerationStructure; excludes the per-frame skinned BLASes).
    uint64 blasBytes;
    uint64 blasCompactionSavedBytes; // cumulative bytes reclaimed by copy-compaction

    // Mesh data streaming (see MeshStreamer).
    uint64 meshBudgetBytes;
    uint64 meshStreamableBytes; // registered mesh sets, resident or not
    uint64 meshResidentBytes;
    uint64 meshColdBytes;       // resident but unseen long enough to be eviction candidates
    uint32 numMeshSets;
    uint32 numEvictedMeshSets;

    // Mesh LOD (see MeshLodParams; selection runs in the GPU cull, counters read back a few frames late).
    uint32 numMeshLodGroups;
    uint32 lodInstanceCounts[5];   // VISIBLE LOD instances per selected level (MAX_MESH_LODS)
};