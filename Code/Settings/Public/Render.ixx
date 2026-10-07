export module Settings.Render;

import Core;
import Core.glm;

// The renderer's settings (RendererVK reads them from Globals::settings; the UBO build feeds most of them to the
// shaders). Plain data: the registration (Private/Render.cpp) knows nothing of the renderer, which attaches its
// reactions (shader reloads, re-records, resizes) as Tweak::onChange listeners in Renderer::attachSettingsListeners.

// THE WIND ("Sky/Wind"): the one wind everything that moves with it reads - the weather particles (rain, snow), the
// tree and grass sway (wind.inc.glsl), the fog noise drift, the ocean (x "Ocean/Waves/Wind speed scale", blowing
// from the opposite heading: its spectrum convention) and the clouds (x "Sky/Clouds/Wind speed scale"). Rides the UBO
// as u_weather (live: these tweaks are Runtime under the "Sky" lock).
export struct WindParams
{
    float speed = 2.0f;         // m/s, the mean wind
    float angleDeg = 113.0f;    // the direction the wind blows TOWARDS, degrees from +X around +Y (113: the sea's former heading)
    float gustStrength = 5.0f;  // m/s, the amplitude of the 2D gust vector added to the mean (calm air flurries too)
    float gustSize = 50.0f;     // m, the gusts' feature size

    glm::vec2 direction() const // unit, XZ
    {
        const float a = glm::radians(angleDeg);
        return glm::vec2(std::cos(a), std::sin(a));
    }
};

// Everything in the TweakPanel's "Sky" categories (Sky / Sky/Sun / Sky/Atmosphere / Sky/Clouds /
// Sky/Stars / Sky/Nebula / Sky/Moon; Sky/Wind is WindParams).
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
    float starSize = 1.0f;              // base star core size multiplier
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
};

// Volumetric clouds (CloudPipeline) - the TweakPanel's "Sky/Clouds" categories. All UBO-driven, so
// changes apply live. A spherical shell [bottom, top] above sea level (world Y 0) around a planet
// centre under the camera; the noise is world-anchored and moves with the wind.
// The three bools (enabled, shadows, selfShadowFromMap), checkerboard, the debug mode and "powder above 0" are BAKED
// shader defines (CLOUDS, CLOUD_SHADOWS, CLOUD_SELF_SHADOW_MAP, CLOUD_CHECKERBOARD, CLOUD_DEBUG_MODE, CLOUD_POWDER -
// Shader.cpp's preamble): the Renderer's listener reloads the shaders when one of them changes.
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
    float windSpeedScale = 3.5f;       // x "Sky/Wind/Speed" (the layer's wind; its direction is the shared one)
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
    // A FLOOR under both layers' history weight, as the number of jittered marches it averages (an exponential average
    // of weight w holds (1 + w) / (1 - w)): a time-based weight averages fewer marches at a low frame rate - 1 s at
    // 30 fps held ~5, at 120 fps ~20 - and the ocean mirrored the noise as a per-frame flicker. 0 = time only.
    float skyMapMinSamples = 12.0f;
    float giSkyObserverRadius = 4000.0f; // m: the GI layer's observers are spread over a disc this wide around the camera
    float nearDetailRadius = 300.0f;   // extra high-frequency erosion within this camera distance (m)
    float detailDistanceKm = 12.0f;    // the detail erosion fades out over the last 20 % of this distance; no detail fetches past it
    bool  checkerboard = true;         // the march covers half the pixels per frame; the temporal pass fills the rest (CLOUD_CHECKERBOARD)
    int   debugMode = 0;               // 0 off, 1 step count, 2 density only, 3 history rejection
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
    // the static mesh pipeline through the Renderer's listener - no uniform, no per-pixel cost when off):
    // 0 off, 1 cascade index tint, 2 the cascade cross-fade band, 3 the raw sun visibility, 4 shadow-map
    // texel size heat. Cascade data only exists on the PCSS path (RT sun off).
    int debugMode = 0;
    float terrainMarchSpread = 0.02f;  // penumbra growth per metre along the ray. Deliberately far wider
                                // than the true sun disc (~0.005): the softness is what keeps the map's
                                // texels from resolving as stair-steps, and what keeps the doubling
                                // sample spacing self-consistent (this is a cone trace, not a point march).
};

// FOLIAGE cards (MATERIAL_FLAG_BILLBOARD - the tree billboards) - "Foliage ..." in the TweakPanel's "Trees" category.
// UBO-driven (u_foliage), so changes apply live.
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
    // TREE WIND ("Trees/Wind", tree_wind.inc.glsl): vertex-shader sway of the mesh trees, branch cards and billboards in the
    // shared wind (u_weather: "Sky/Wind"). Three layers: the TRUNK bend (every representation, from the
    // height alone), the BRANCH sway and the LEAF flutter (meshes only, from the bake's per-vertex payload).
    float windBend = 0.004f;        // the trunk's lean at the reference height, m per (m/s)^2 of wind (10 m/s: 0.4 m)
    float windRefHeight = 10.0f;    // m: the bend grows with (height / this)^2
    float windSway = 0.35f;         // the trunk's oscillation, x the lean
    float windSwayFrequency = 0.25f; // Hz
    float windBranch = 0.03f;       // a branch tip's sway, m per m/s of wind (x the tree's scale)
    float windBranchFrequency = 0.6f; // Hz
    float windLeaf = 0.04f;         // a leaf card tip's flutter (m)
    float windLeafFrequency = 3.0f; // Hz
    float windBranchFadeStart = 40.0f; // m: the branch sway fades out between these (before the mid tier's cards)
    float windBranchFadeEnd = 70.0f;
    float windLeafFadeStart = 20.0f;   // m: the leaf flutter fades out between these
    float windLeafFadeEnd = 40.0f;
    float windTrunkFadeEnd = 3000.0f;  // m: no wind at all beyond this (0 = no limit); a 100 m band fades it out
    float windBillboardWaves = 0.012f; // the whole-tree billboards' texture waves, a fraction of the card (0 = off)
    // "Trees/Debug view" - baked as TREE_DEBUG on the lit mesh fragments (a change reloads them; 0 = no define):
    // 1 = a colour per MATERIAL (each representation and fade copy has its own: a switch shows as a colour change),
    // 2 = a colour per MESH (the LOD level the cull picked: an LOD step shows), 3 = the distance FADE side (red = a
    // fade-out material, green = fade-in, white = none).
    int debugView = 0;
};

// PROCEDURAL GRASS ("Grass" tweaks; GrassPipeline, grass.inc.glsl). UBO-driven (u_grass*), so everything is live but
// "Blades per patch", which rebuilds the blade index buffer. Desktop only. Colours are picked as sRGB (the UBO build
// converts them).
export struct GrassParams
{
    static constexpr int MAX_BLADES = 1024; // = RendererVKLayout::GRASS_MAX_BLADES (static_asserted in Renderer.ixx)

    bool enabled = true;
    int bladesPerPatch = 1024;     // blades in a full patch (<= MAX_BLADES): the near density is this / patch size^2
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
    // The shared wind ("Sky/Wind", wind.inc.glsl: its direction, speed and gusts) bends the blades: the tip's push in
    // blade heights per m/s, plus a small-scale ripple (value noise moving with the mean wind).
    float windBend = 0.05f;        // blade heights per m/s
    float rippleBend = 0.08f;      // blade heights per m/s, x the ripple noise
    float rippleSize = 12.0f;      // m
    float swayFrequency = 0.6f;    // Hz
    // All wind movement eases out between these camera distances (m): small far blades moving read as grain.
    float windFadeStart = 100.0f;
    float windFadeEnd = 200.0f;
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
};

// THE ROCK MATERIAL (EPipelineIndex::LitRock, instanced_indirect_rock.fs.glsl; "Rocks/Material" tweaks). A rock has no
// textures of its own: it takes the terrain's BEDROCK material of the climate it stands in (the splat's rock entries,
// the same climate pick as a cliff there), projected in world space, then the terrain's snow, a cover of the climate's
// GROUND material on its up-facing faces, and a contact band at its foot. UBO-driven (u_rockParams*): live.
export struct RockParams
{
    float uvScale = 4.0f;             // x the terrain's "Rock uv scale": a boulder shows finer grain than a cliff face
    float coverAmount = 0.7f;         // the climate's ground material (moss, sand, litter) on up-facing faces; 0 = off
    float coverSlopeStart = 0.91f;    // normal.y where the cover begins ...
    float coverSlopeFull = 1.0f;      // ... and where it is full
    float coverPatchSize = 2.14f;     // m: the noise that breaks the cover into patches
    float contactHeight = 0.4f;       // m above the ground: the band at the rock's foot
    float contactBlend = 0.8f;        // the ground material's share at the very foot (it hides the cut line)
    float contactDarkening = 0.5f;    // ambient occlusion at the very foot
    float contactFadeDistance = 200.0f; // m: no band past it (the ground height comes from the terrain-data map,
                                        // too coarse far out)
    float cavityAo = 1.0f;            // the baked per-vertex cavity (Procedural RockGenerator) on the ambient; 0 = off
    float cavityCover = 0.5f;         // the ground cover's reach into the crevices (x "Ground cover" x the cavity)
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
    float overlap = 128.0f;        // the billboards draw to startDistance + this; the volume fades in over it (m)
    uint32 angularRes = 3000;      // texels around (cell = r x 2 pi / this).
    uint32 radialRes = 1500;       // texels from start to end (cell = r x ln(end / start) / this)
    uint32 slices = 16;            // height slices
    float height = 22.0f;         // m above the column's tree floor the volume covers
    float densityScale = 0.4f;    // x the baked extinction
    float blobShrink = 0.4f;    // 1/m off the baked extinction before the scale: blobs shrink toward their cores
    float stepScale = 1.5f;       // march step, x the cell size
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
    int pixelSkip = 1;        // 0 = off, 1 = 1 of 2 (checkerboard), 2 = 1 of 4
    bool temporalPath() const { return temporalBlend > 0.0f || halfRes; }
    float rebakeDistance = 64.0f; // the camera moves this far from the bake centre -> re-bake
    // BAKE AHEAD: the bake centres on where the camera will be at the swap (its smoothed velocity x the last bake's real
    // duration), and the re-bake test uses that point too - the lag cancels at a steady speed, at any frame rate.
    bool bakeAhead = true;
    int floorSmoothing = 2;       // the tree floor's tent blur radius, in columns (tree_volume_floor_smooth.cs); 0 = off
    // The world tree records' chunks within this of the bake centre (m) expand into their trees and bushes and splat in
    // detail; beyond, each record adds its mass per column (TreeVolumePipeline RecordSource).
    float recordDetail = 4000.0f;
    // A bake runs over about this many frames (the record splat's three passes spread evenly, plus a few steps of their
    // own); the march reads the previous bake until the last one. 1 = as fast as it goes.
    int bakeFrames = 8;
    // THE HAND-OVER: a finished bake CROSS-FADES into the shown one (a dithered per-ray pick, averaged by the temporal
    // pass / TAA) over this many seconds of real time - no GPU work, so a time, not frames: the same at any frame rate
    // (0 = at once; the swap then still takes the copy's frames).
    float swapTime = 0.5f;
    // ROCKS in the volume (Procedural's world rocks, R5): a rock is a SOLID - its extinction (1/m, before "Far density"
    // and the blob shrink) is this at any size, over its occupancy. A rebake setting.
    float rockExtinction = 4.0f;
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
    // (The noise drifts with the shared wind, "Sky/Wind".)
    float temporalBlend = 0.8f;    // history blend weight (jittered Z integration)
    bool  lightShadows = true;     // shadow ray per froxel per grid light (expensive)
    int   sunRays = 1;             // sun shadow rays per froxel (RT sun mode); main perf knob
    float sunSoftness = 0.02f;     // shadow ray cone half-angle (rad); softens + decorrelates the rays
    bool  spatialFilter = true;    // 3x3 tent on the scatter grid in the integrate pass
    bool  giAmbient = true;        // GI probe ambient (off = analytic sky only, cheaper)
};

// Exposure + tonemapping, applied in the composite pass (the HDR -> display mapping) - the
// TweakPanel's "Post" category. Exposure, tonemapper and auto exposure are baked into the composite push
// constants: the Renderer's listener re-records.
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
};

// The GI toggle is baked into the cached GI command buffer (re-record); the master, "RT Sun" and "RT Lights" into
// the lit fragment shaders (reload + re-record) - the Renderer's listeners.
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
};

export struct TAAParams
{
    bool  taaEnabled = true;
    float taaFeedback = 0.9f;
    // History weight cap on OCEAN pixels: waves animate but the reprojection is camera-only (no motion
    // vectors), so full-weight history blurs the specular sparkle away. Lower = crisper, shimmerier water.
    float taaOceanFeedback = 0.2f;
};

// DLSS Super Resolution through Streamline ("Post/DLSS" tweaks; RendererVK:Streamline, DlssPipeline). Any mode
// but Off REPLACES TAA (desktop only). The mode sets the render resolution: a change re-creates the render-size
// targets (the Renderer's listener), so the mode tweak waits for the GPU.
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
};

// The TweakPanel's "Particles" category: the GPU particle sim's own knobs plus the weather inputs the
// Renderer folds into the per-frame UBO (u_particles, u_weather's rain occlusion). All live -
// the particle stage's primary CB re-records every frame, so none of them needs a listener.
export struct ParticleParams
{
    bool  enabled = true;         // the whole GPU particle chain (sim + draw stage)
    bool  collision = true;       // depth-buffer collision in the sim
    float timeScale = 1.0f;       // multiplier on the sim delta
    bool  logStats = false;       // prints GPU alive/dead counts ~once a second

    // Rain occlusion: a top-down depth map of the weather volume, so roofs shelter what is under them.
    bool  rainOcclusion = true;       // only while a rain / snow volume asks for it; one downward ray per texel (needs RT)
    float rainOcclusionCasterPad = 100.0f; // how far above the box a roof still shelters (m)
    float rainOcclusionTolerance = 0.25f;  // depth below the surface before a drop counts as sheltered (m)
    float rainOcclusionFoliageBlock = 0.7f; // the rain one foliage layer (alpha-masked card) stops, 0..1

    float streakCameraBlur = 0.15f; // fraction of the camera velocity the weather streaks subtract
    float anisotropy = 0.33f;       // the lit particles' scattering phase g (0 = isotropic, forward < 1)

    // The volumes blow in the shared wind ("Sky/Wind"); these are the rain's own sheets on top. A storm: wind speed 15,
    // gust strength 8, sheet contrast 0.7, sheet drift 6.
    float windSheetContrast = 0.5f; // [0,1] alpha density bands sweeping through
    float windSheetSize = 50.0f;    // m
    float windSheetDrift = 5.0f;    // m/s the fields travel along the wind direction on top of half the wind speed
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
};

// Forcefield bubbles (Force library / ForceFieldPipeline / force_*.glsl). The Force library registers
// these tweaks (TweakPanel "Force" categories, Settings::registerForce) and hands the struct to
// Renderer::setForceFieldParams every frame, like OceanParams; the values are UBO-driven, so all of them are live.
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
    // in Renderer.ixx - Settings cannot import RendererVK's :Layout).
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

// Diffuse GI probe clipmap shape (RendererVK GIProbePipeline; see "GI" in RendererVK's Layout.ixx). The GI_* sizing
// values are injected into EVERY shader compile (Shader.cpp buildLayoutPreamble) as #defines, so a change makes the
// Renderer's listener wait for the GPU, re-allocate the SH buffer (or only the volume) and reload every shader.
export struct GiGridConfig
{
    int numCascades = 4;                    // nested clipmap levels (1..8)
    int dimLog2X = 5, dimLog2Y = 5, dimLog2Z = 5; // probes per axis per cascade as log2 (2..6 = 4..64): power of two for the toroidal mask
    float focusOffsetY = 2.0f;              // metres added to the scene focus before centring the grids (> 0 = more probes above the ground than below)
    // The IRRADIANCE VOLUME (GIProbePipeline::recordVolumeBake, gi_volume_bake.cs.glsl): per frame, the probe
    // field is baked into one set of 3D textures per cascade, visibility-weighted at every voxel centre, and
    // the forward lit shaders read it with hardware trilinear filtering instead of looping over 8 probes.
    // volumeRes = voxels per probe spacing per axis (1 or 2, a power of two for the toroidal mask).
    bool volume = true;
    int  volumeRes = 2;

    uint32 dimX() const { return 1u << dimLog2X; }
    uint32 dimY() const { return 1u << dimLog2Y; }
    uint32 dimZ() const { return 1u << dimLog2Z; }
    uint32 probesPerCascade() const { return dimX() * dimY() * dimZ(); } // a multiple of 64 (every dim >= 4): the trace's sky workgroup relies on it
    uint32 probesTotal() const { return (uint32)numCascades * probesPerCascade(); }
    uint32 traceThreads() const { return probesTotal() + 64; }            // one invocation per probe + one workgroup projecting the sky SH
    uint32 volumeDimX() const { return dimX() * (uint32)volumeRes; }
    uint32 volumeDimY() const { return dimY() * (uint32)volumeRes; }
    uint32 volumeDimZ() const { return dimZ() * (uint32)volumeRes; }
};

// The "GI" tweaks (GIProbePipeline). The trace knobs and the visibility knobs ride the UBO (u_rt_gi*); the debug
// colour mode and radius are push constants of the CACHED debug secondary (a change re-records).
export struct GiSettings
{
    GiGridConfig grid;

    int raysPerProbe = 17;           // gather rays per probe per visit
    float updateIntervalMult = 16.0f; // global factor of a wave's update interval: frames = max(1, this x the priority factor),
                                     // so it is the interval AT "GI/Priority Distance" and close blocks cancel it (fresh probes always trace)
    float temporalAlpha = 0.025f;    // blend toward freshly traced irradiance per frame AT 60 FPS (rescaled by the wall delta, see getTraceParams0)
    float maxRayDist = 8.0f;         // gather ray max distance (world units)
    float strength = 1.0f;           // multiplier on the sampled probe irradiance at shading time
    float tlasRange = 4096.0f;       // TLAS instance range bound around the camera (origin distance) - "RT/TLAS Range"

    // Update priority (gi_probe.inc.glsl giWavePriority, one factor of giWaveUpdateInterval): a wave's interval is multiplied by
    // (focus distance / priorityDist) ^ falloff / viewBoost - NO bounds: a close wave's factor < 1 cancels the
    // interval multiplier, and the far field has no cap. viewBoost = frustumWeight for a wave IN the view
    // frustum (its interval divides by it), fading to 1 over priorityDist metres outside it.
    // Defaults (first-person scene, focus = camera, interval mult 16, falloff 3, weight 5): in view the
    // interval is 16 x (d / 10)^3 / 5 frames - every frame within ~8.5 m, 3 at 10 m, 25 at 20 m, 400 at
    // 50 m; out of view, 5x that. A steep curve: all the rays go to what is near the focus.
    float priorityDist = 8.0f;          // focus distance (m) of the nominal rate (factor 1) for a wave OUT of view; the falloff curve pivots here
    float priorityFalloff = 1.5f;       // exponent on (distance / priorityDist): 1 = linear, 2 = quadratic (far field all but stops), 0.5 = gentle, 0 = no distance term
    float priorityFrustumWeight = 5.0f; // a wave IN the view frustum has its interval divided by this (1 = the frustum is ignored)

    // SH-L1 depth visibility (Chebyshev) lookup tuning. Three knobs, each with its own job: the mean scale
    // moves the occlusion THRESHOLD, the variance floor is the MINIMUM edge softness (the measured variance
    // widens it where the depth really spreads - sideways past a wall), the weight floor is the leak level /
    // the all-occluded fallback. The exponent is fixed (GI_VIS_CHEB_POWER = 2 in gi_probe.inc.glsl): near the
    // threshold it only rescales the floor (weight ~ 1 - p (delta / sigma)^2), and the weight floor cuts the
    // tail it shapes. An additive mean bias was tried and removed: the same effect as the scale or the floor.
    float visVarianceFloor = 0.35f;  // min std-dev as a fraction of the cascade's probe spacing: covers the L1 mean's error
                                     // toward a wall (0.15 .. 0.4 spacings); below ~0.25 the ray jitter moves the edge (flicker)
    float visWeightFloor = 0.01f;    // occluded probes keep this much weight (0 = hard cutoff, noisy when all 8 are occluded)
    float visMeanScale = 1.2f;       // scales the reconstructed depth (mean AND, by its square, the second moment, so the
                                     // variance stays consistent) before the Chebyshev test: > 1 widens each probe's visible
                                     // footprint. A wall at distance m reads as scale x m, so points up to (scale - 1) x m
                                     // BEHIND it keep full weight: the leak depth

    // Debug probes (also driven by the testbed's P / O keys)
    bool  debugEnabled = false; // a per-frame stage flag (no re-record)
    int   debugMode = 0;        // 0 = irradiance, 1 = cascade/LOD, 2 = update priority, 3 = relocation / backface, 4 = visibility
    float debugRadius = 0.12f;  // cube half-extent as a fraction of sqrt(spacing)
};

// Texture mip streaming ("Texture Streaming"; RendererVK TextureStreamer). Read every frame.
export struct TextureStreamingSettings
{
    int  budgetMB = 512;
    bool enabled = true;
    int  tailMaxDim = 128;
    int  maxOpsInFlight = 4;
    float maxMBPerFrame = 24.0f;   // issued read volume per frame; keeps stream-ins well under the staging buffer
    bool gpuMipCopies = true;      // copy surviving mips old->new on the GPU (demotions skip the disk entirely)
    bool debugRewriteAllSlots = false;
    float mipBias = 0.0f;          // global quality knob: +1 = one mip level coarser everywhere
    float texelRatio = 1.0f;       // texels wanted per projected pixel (tiling textures want > 1)
    int  demoteHysteresisFrames = 60;
    int  decayFrames = 120;        // unseen for this long -> desire only the tail
};

// Mesh data streaming ("Mesh Streaming"; RendererVK MeshStreamer). Read every frame.
export struct MeshStreamingSettings
{
    int budgetMB = 256;
    int coldFrames = 240;          // frames a set must go unseen before it may evict
    int maxOpsInFlight = 8;        // concurrent re-stream reads
    int maxStreamMBPerFrame = 32;  // stream-in issue cap (staging pressure)
    bool enabled = true;
};

// The renderer's remaining single switches, each in its own TweakPanel category.
export struct RendererSettings
{
    bool vsync = true;               // "Time/VSync": FIFO present (Time's stable-dt snap relies on it); a change re-creates the swapchain
    bool logPipelineStats = false;   // "Renderer/Log pipeline stats": F5 re-creates the pipelines with it
    // "Renderer/Overlap compute": independent compute passes share their barriers so they run side by side (the primary
    // is re-recorded every frame: takes effect at once). Off: each pass on its own, with its own GPU timing scope.
    bool overlapCompute = true;
    bool wireframe = false;          // "Editor/Wireframe": baked polygon mode of the scene variants (reload + re-record)
    int  anisotropyLevel = 2;        // "Renderer/Textures/Anisotropy" index: 0 = off, else 2^level (2 = 4x); a change re-creates the scene sampler
    bool decals = true;              // "Decals/Enabled"
    bool terrainWetDiffusion = true; // "Terrain/Water/Diffusion on": the WET_DIFFUSION define of the wetness compute (reload + re-record)
};

// The registration, one function per struct. Run them in this order (the TweakPanel lists a category in
// registration order): Sky, Wind, Shadow, Foliage, FarTree, Rock, Grass, Fog, Clouds, RT, RTAO, TAA, Dlss,
// MotionBlur, Bloom, Post, MeshLod, LightGrid, Renderer, Gi, Particles, OceanSpray, MeshStreaming, TextureStreaming.
// ForceFieldParams is registered by the Force library (Settings::registerForce).
export namespace Settings
{
    void registerSky(SkyParams& s);
    void registerWind(WindParams& s);
    void registerShadow(ShadowParams& s);
    void registerFoliage(FoliageParams& s);
    void registerFarTree(FarTreeParams& s);
    void registerRock(RockParams& s);
    void registerGrass(GrassParams& s);
    void registerFog(FogParams& s);
    void registerClouds(CloudParams& s);
    void registerRT(RTParams& s);
    void registerRTAO(RTAOParams& s);
    void registerTAA(TAAParams& s);
    void registerDlss(DlssParams& s);
    void registerMotionBlur(MotionBlurParams& s);
    void registerBloom(BloomParams& s);
    void registerPost(PostParams& s);
    void registerMeshLod(MeshLodParams& s);
    void registerLightGrid(LightGridParams& s);
    void registerRenderer(RendererSettings& s);
    void registerGi(GiSettings& s);
    void registerParticles(ParticleParams& s);
    void registerOceanSpray(OceanSprayParams& s);
    void registerMeshStreaming(MeshStreamingSettings& s);
    void registerTextureStreaming(TextureStreamingSettings& s);
}
