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
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.739f, 0.221f, -0.636f));
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
    glm::vec3 groundColor = glm::vec3(0.55f, 0.65f, 1.0f);
    float groundIntensity = 0.0f;
    float groundHorizon = 0.25f;  // fraction of every hemisphere treated as sunlit terrain in the
                                  // out-of-GI-range fallback (skyGroundRadiance): up-facing surfaces see
                                  // ground near the horizon on rolling terrain, not a clean sky plane —
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
    float ozone = 2.0f;                 // ozone absorption strength (1 = Earth-like); absorbs green/yellow,
                                        // suppresses the green horizon band single scattering produces

    // Clouds
    float cloudCoverage = 0.35f;
    float cloudHeight = 6300.0f;        // meters above the viewer (slab base)
    float cloudThickness = 86.0f;    // slab thickness sqrt(m)
    float cloudScale = 0.7f;            // multiplier on the base noise frequency
    float cloudWindSpeed = 3.00f;       // multiplier on the base wind drift
    float cloudWindAngle = 0.0f;       // radians
    float cloudSoftness = 0.26f;        // density smoothstep width (small = crisp edges)
    float cloudDensity = 1.58f;          // extinction strength (high = opaque cores)
    float cloudSharpness = 1.0f;       // density remap contrast (high = hard-edged shapes)
    float cloudBaseVar = 0.78f;          // per-column base/top height variation (0 = flat slab)
    float cloudShading = 2.00f;          // directional sun-shading strength

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

// Sun shadow cascade distribution (raster path; RT sun shadows ignore these) — the TweakPanel's
// "Shadows" category. Consumed per frame by computeSunCascades, so changes apply live.
export struct ShadowParams
{
    float maxDistance = 2000.0f; // farthest camera distance receiving sun shadows (m). Lower = every
                                // cascade covers less ground = sharper shadows everywhere, at range cost.
    float splitLambda = 0.95f;  // cascade split scheme: 0 = uniform splits, 1 = logarithmic (resolution
                                // bunches up near the camera)
    float casterPad = 4000.0f;   // how far up-sun a caster may sit above a cascade and still be captured
                                // (m). Must exceed the tallest shadow-casting feature (mountains!); too
                                // small clips casters out of the depth map and their shadows pop away.
    float depthBias = 0.001f;    // sun cascade depth bias
    float normalBias = 1.0f;     // sun cascade normal bias (texels)

    // Long-range terrain sun shadows. Past maxDistance (and past the RT path's TLAS range) both shadow
    // sources run out of data, so distant ground goes uniformly lit — a hard terminator across a mesh
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
    float terrainMarchSpread = 0.02f;  // penumbra growth per metre along the ray. Deliberately far wider
                                // than the true sun disc (~0.005): the softness is what keeps the map's
                                // texels from resolving as stair-steps, and what keeps the doubling
                                // sample spacing self-consistent (this is a cone trace, not a point march).
    void registerTweaks();
};

// Volumetric fog (froxel grid; see VolumetricFogPipeline) — the TweakPanel's "Fog" categories.
// All UBO-driven, so changes apply live.
export struct FogParams
{
    bool  enabled = true;
    float density = 0.010f;        // global extinction at the height base (1/m)
    float heightBase = 0.0f;       // world height where the global fog is densest
    float heightFalloff = 0.33f;   // exponential density falloff above the base (1/m)
    float terrainFollow = 1.0f;   // fraction of the local terrain height added to the height base (needs a
                                   // terrain height map, see Renderer::setFogTerrainHeightMap): 0 = flat fog,
                                   // 1 = fog hugs the terrain at constant depth; in between it reaches higher
                                   // on mountainsides but still clears the peaks
    glm::vec3 albedo = glm::vec3(1.0f, 1.0f, 1.0f);
    float albedoIntensity = 1.0f;  // > 1 is a non-physical gain (emissive-ish fog)
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
    int   farFieldSteps = 4;       // ground samples along the far segment. Each sub-segment between them is
                                   // solved exactly, so this sets how finely terrain-follow tracks the
                                   // ground, and low counts smooth it rather than adding noise
    float slicePower = 1.0f;       // froxel Z distribution exponent: 1 = plain exponential slices, < 1
                                   // shifts Z resolution from near to far (worth ~0.7-0.85 at long ranges)
    float terrainShadowDist = 512.0f; // froxels beyond this distance sun-shadow by marching the terrain
                                   // height map instead of TLAS rays / cascade taps (both run out of data
                                   // at distance — TLAS is range-bounded); needs the terrain fog map
    float regionStrength = 1.0f;   // how much the baked regional fog fields (terrain data map channel B:
                                   // thickness + height-falloff mul, Procedural's sampleFogThickness /
                                   // sampleFogHeightFalloff) modulate the height fog: 0 = uniform fog,
                                   // 1 = fully region-driven
    float shaftBoost = 10.0f;        // non-physical gain on the underwater sun in-scatter (fog only, not
                                    // surfaces): water scatters little at fog densities, so physically
                                    // correct shafts are faint — this makes them readable. 1 = physical.
    float causticStrength = 1.5f;   // underwater caustic focus (fold-Jacobian light pattern on submerged
                                    // surfaces + fog shafts; underwater_light.inc.glsl): 0 = off, > 1
                                    // exaggerated. Beer-Lambert depth absorption is separate (Ocean/Absorption).
    float causticDepthFade = 0.25f; // caustic contrast decay with depth (1/m) — approximates defocus;
                                    // higher = the pattern washes out closer to the surface
    float causticShoreFade = 1.0f;  // caustic contrast ramps in over this much water depth (m), so the
                                    // pattern dissolves at the terrain-waterline intersection; 0 = off
    float underwaterDensity = 0.7f; // multiplier on the global density at/below the LOCAL water surface
                                    // (terrain data map water level; always-on murk, immune to regional
                                    // thickness): thick murk under thin morning haze, or 0 to disable
    float underwaterOffset = -1.0f;  // raises/lowers the underwater fog boundary relative to the local
                                    // water surface (m); the live wave displacement rides on top
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

// Exposure + tonemapping, applied in the composite pass (the HDR -> display mapping) — the
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

    void registerTweaks();
};

export struct RTAOParams
{
    bool  enabled = true;
    int   rays = 6; //1;
    float radius = 1.0f;
    float power = 1.5f;
    float intensity = 1.0f;
    float fadeStart = 30.0f;   // distance from camera (world units) where AO begins to fade out
    float maxDistance = 60.0f; // distance at which AO is fully gone; 0 disables the falloff
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

// Mesh LOD chains (authored "LodN_*" meshes and/or meshopt-generated) — the TweakPanel's "LOD" category.
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
                                // (shallow values like 35 visibly mute the long swell — by design)
    float horizonLevelOffset = -0.5f; // vertical shift (m, usually negative) of the HORIZON BAND only.
                                // The band is exempt from the land cull (its triangles are far larger
                                // than the cull's footprint bound), so it draws over distant terrain;
                                // sinking it a little keeps its crests under near-sea-level ground.
    float amplitude   = 1.0f;   // artistic scale on the spectrum amplitude (1 = physical)
    float choppiness  = 1.1f;   // horizontal displacement lambda (0 = heightfield only, higher = sharper crests)
    float normalStrength = 1.0f; // artistic scale on the shading slopes
    glm::vec3 cascadeSizes = glm::vec3(1536.0f, 188.0f, 25.0f); // FFT patch sizes (m); each TILES with its
                                // own size, so the largest sets how often the sea repeats — keep it many
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
    // Crest subsurface scattering (Sea of Thieves-style): back-lit wave crests glow the scatter color,
    // scaled by height above the calm water line. Power shapes the toward-the-sun view lobe.
    float sssStrength      = 0.65f;  // per meter of crest height; 0 disables (and the extra shadow rays with it)
    float sssPower         = 1.0f;
    bool  hitLighting      = false; // evaluate the scene's grid lights at refraction/reflection ray hits
                                    // (OCEAN_HIT_LIGHTS shader variant; toggling reloads the pipeline)
    // Foam & turbulence. ONE instant-foam response (oceanInstantFoam) both draws the per-pixel crest
    // foam and injects the accumulated TURBULENCE field (the churn energy breaking leaves behind).
    // Turbulence then drives the wake look: it relaxes the fold threshold (aged foam paints itself along
    // the LIVE geometry's convergence lines) and makes the water milky + rough (entrained bubbles).
    glm::vec3 foamColor    = glm::vec3(0.88f, 0.92f, 0.94f);
    float foamBias         = 0.6f;  // fold threshold: Jacobian below this is folding (foaming)
    float foamBreakAccel   = 0.25f; // breaking threshold (Longuet-Higgins): downward crest acceleration
                                    // above this fraction of g is breaking — what makes LARGE waves foam
    float foamSoftness     = 0.5f;  // edge width of both thresholds (small = crisp crest lines)
    float foamDecay        = 0.985f; // turbulence retention per frame (wake persistence)
    float foamSpread       = 1.2f;  // turbulence diffusion per frame: the wake spreads as it lives (also
                                    // keeps the stored field free of texel structure)
    float foamBoost        = 0.6f;  // how much turbulence relaxes the fold threshold: the aged-foam
                                    // amount in the wake (0 = only actively breaking crests foam)
    float turbidity        = 0.4f;  // entrained-bubble strength: milky brightening + extra roughness of
                                    // turbulent water (the wake stays visible after the foam thins)

    // Shore interaction: the baked terrain-data cascades (Renderer::setFogTerrainHeightMap, baked by the
    // terrain streamer) drive fake shoaling — each cascade's displacement fades out below depth =
    // shoalScale * its patch size, so long swell dies offshore while chop runs almost to the beach and
    // waves never poke through land — plus a surf/foam band where the water column vanishes at the
    // waterline.
    float shoalScale     = 0.005f; // shoaling depth as a fraction of each cascade's patch size (scaled
                                // down with the cascade sizes above to hold the absolute depths)
    // Horizon depth: past horizonDepthRange the waves assume AT LEAST horizonDepth of water, whatever
    // the baked map says. Every distant depth error runs shallow — coarse texels average shore slopes
    // into the water, the generator reports depth exactly 0 for samples it could not resolve, and the
    // vertical scale compresses real shelves — and shallow is the ruinous direction: it fades the waves
    // out AND (via fade^2) the LEAN variance, leaving a mirror that reflects the sky exactly like wind 0.
    // Only the assumed seabed moves, never the surface, so it cannot put water over land; the land cull
    // still reads the raw map. 0 range = off.
    float horizonDepth      = 30.0f;
    float horizonDepthRange = 1500.0f;
    // Breaking limit: max wave height as a fraction of the local depth (real waves break near H/d ~ 0.78,
    // crest ~ 0.39 d). The per-cascade shoal fade cannot express this — it keys on each band's WAVELENGTH,
    // so with cascade sizes 8x apart any fade depth that suits the swell leaves the mid band at full
    // amplitude in a metre of water. Scales the shoaled sum rather than clipping crests. 0 = unbounded.
    float waveHeightLimit = 0.5f;
    float shoreFoamDepth = 8.0f;  // water-column height (m) below which the waterline churns white; 0 = off
    float shoreFoamMax   = 0.75f; // surf band opacity cap: shore foam coverage never exceeds this, so the
                                  // refracted bottom stays visible through the lace (whitecaps unaffected)
    float swashAmp       = 0.5f;  // swash run-up: scale on the un-shoaled wave height riding through the
                                  // waterline and up the beach (waves crash and flow over; 0 = hard cutoff)
    float swashDrawdown  = 1.0f;  // m below the seabed a receding swash surface sinks: deeper = steeper,
                                  // cleaner cut against the sand (shallow grazes sawtooth the retreat edge)
    float shoreFoamBias  = -0.33f;  // shifts the surf fold threshold: negative = sparser lace / more
                                  // transparent shore waves, positive = denser churn
    // How far above the seabed the wave TROUGH is held (m). The clamp that does this had a hard-coded 5 cm
    // margin, which is far under the decimetre-scale disagreement between the baked depth map it measures
    // against and the LOD'd terrain mesh you actually see — so a trough legally clear of the map's seabed
    // still sank under the real ground and let it poke through the surface. Raise until that stops.
    // Tapered in with depth (see the shader): the margin cannot exceed the water it is lifting the surface
    // within, and lifting it at the waterline would drag the water's edge seaward.
    float troughMargin = 0.15f;
    float swashFlow      = 0.33f;  // backflow: scale on the raw horizontal chop riding the swash weight —
                                  // the tongue visibly flows back seaward as the wave recedes (0 = off)
    float cullMargin     = 1.0f;  // land cull: clipmap triangles whose whole footprint is buried deeper
                                  // than this under the local water level are VS-culled (0 = off)
    float farCullError = 4.0f;    // land cull from the FAR terrain cascade (beyond the near cascade's
                                  // ~860 m): flat burial error allowance in METERS, covering how far
                                  // the far mesh LODs stray from the bake. Deliberately NOT scaled by
                                  // the far texel — that left everything under tens of meters of
                                  // terrain alive (a visible band of buried water past the near
                                  // handover). Narrow rivers the coarse point-sampled bake cannot
                                  // resolve may lose triangles out there (speed over accuracy);
                                  // 0 = never cull from far data

    // Ray tracing budget (ocean.fs.glsl traces the scene TLAS per pixel for refraction + reflection).
    float rtRefractionRange = 10.0f;  // max refracted-ray length (m): how far underwater geometry stays
                                       // visible through the surface (the ~99% Beer-Lambert extinction
                                       // bound still applies on top, so clear water is the case this caps)
    float rtReflectionRange = 3000.0f; // max mirror-ray length (m): how distant scenery still reflects
    float rtReflectionMaxRough = 0.25f; // filtered roughness above which the mirror ray is skipped and the
                                        // blurred sky stands in (a wide lobe can't be one mirror sample)
    float rtRayCutoffDist = 0.0f;  // camera distance (m) beyond which NO scene rays are traced: refraction
                                   // falls back to the analytic baked-terrain bottom (the same path RT
                                   // misses take), reflections to the atmosphere. 0 = unlimited
};

// Forcefield bubbles (Force library / ForceFieldPipeline / force_*.glsl). The Force library owns
// these tweaks (TweakPanel "Force" categories) and hands the struct to Renderer::setForceFieldParams
// every frame, like OceanParams; the values are UBO-driven, so all of them are live.
export struct ForceFieldParams
{
    bool enabled = true;             // gates the shell draw + force compute passes
    float isoThreshold = 0.15f;      // field strength where an uncontested bubble surface sits
    int marchSteps = 10;             // ray-march steps through a shell proxy's ray interval
    float bigReachThreshold = 48.0f; // max directional reach (m) above which an emitter bypasses the
                                     // grid into the globally-scanned big-emitter list
    bool useGrid = true;             // hash-grid candidate gathering; off = brute-force scan of every
                                     // emitter per evaluation (small scenes / A-B correctness check).
                                     // Toggling rebuilds the force pipelines (FORCE_GRID define)
    float forceGain = 5.0f;          // scale on the read-back per-emitter applied forces
    float shellAlpha = 0.33f;        // base shell opacity (rim-weighted in the shader)
    float interiorAlpha = 0.0f;     // shell opacity floor when seen from INSIDE the bubble (the rim
                                     // math reads near-zero head-on from within; this keeps the dome visible)
    float backfaceAlpha = 1.0f;     // visibility of the far/inner shell surface seen from OUTSIDE,
                                     // composited behind the front surface (0 = single-surface shell)
    float rimPower = 3.0f;           // fresnel rim exponent
    float rimIntensity = 1.5f;       // rim emissive gain
    float contactGlowIntensity = 0.33f; // seam glow where opposing bubbles press (equal-field zone)
    float contactGlowWidth = 0.15f;    // opposing/own field ratio band that reads as contact
    float contactWallAlpha = 0.5f;     // visibility of the interior equilibrium WALL between two
                                       // pressed opposing bubbles (0 = only the outer seam glows)
    bool densityView = false;          // DEBUG: draw bubbles as a heatmap of the strongest field along
                                       // the view ray instead of the shell (tip power/merging readout);
                                       // white contour marks the iso threshold
    float densityRange = 2.0f;         // field value that maps to the heatmap's white end
    // GLOBAL AMBIENT FIELD: an analytic distance-based field term added to one team everywhere —
    // zero within safeRadius of the planar center, growing at `slope` per metre, capped at
    // maxStrength. Never drawn (no proxy; shading treats it as an invisible field), but it deforms
    // bubbles and feeds pressure/query readbacks like any field. slope <= 0 disables.
    uint32 ambientTeam = 1;
    glm::vec2 ambientCenter = glm::vec2(0.0f); // world XZ
    float ambientSafeRadius = 30.0f;
    float ambientSlope = 0.0f;         // field strength per metre beyond the safe radius
    float ambientMaxStrength = 2.0f;
    float junctionSmoothing = 0.5f;    // smooth-max width (fraction of iso) rounding the crease where
                                       // shells meet the wall: continuous normals kill the junction
                                       // jaggies; 0 = hard crease. Queries use the same function, so
                                       // the gameplay inside-test always matches the drawn surface
    float geoGlowDistance = 0.5f;      // glow band where the shell intersects scene geometry (m)
    float patternScale = 0.6f;       // animated surface pattern frequency (1/m)
    float patternSpeed = 0.3f;       // pattern scroll speed
    float patternIntensity = 0.5f;
    // Per-team shell colors (linear rgb); 8 = RendererVKLayout::MAX_FORCE_TEAMS (static_asserted
    // where both are visible — Settings deliberately doesn't import :Layout).
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