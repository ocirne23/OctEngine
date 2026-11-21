# Lighting, GI and shadows

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## The scene focus

`Renderer::setSceneFocus(worldPos)` / `clearSceneFocus()` set the world point every DISTANCE-BASED
QUALITY FALLOFF measures from; it reaches the shaders as `u_sceneFocus`. The game sets its PLAYER every windowed
frame, so the top-down camera hanging in empty sky shapes none of these. **Cleared, it is the GROUND under the
camera** (`defaultSceneFocus`): the camera's x / z at min(its height, the surface height there) - the App feeds
`setFocusGround` every frame before the begin-frame kick from `TerrainStreamer::surfaceHeightAt` (max(ground, water
level) of the terrain-data bake's CPU copy, near cascade else far; `clearFocusGround` without a bake = the camera
itself). So from high up the sun cascades (a real focus: spheres, not the camera-frustum slices), the GI window, RTAO
and the TLAS range stay on the terrain below, not in empty sky. **"Editor/Freeze scene focus"** holds the focus it had when switched on (captured in `beginFrame` next to
`m_cameraPos`), so the camera can fly away and look at the falloffs from outside.

**Anything new that fades or early-outs by distance measures from `u_sceneFocus`, never `u_viewPos`**
(view-ray geometry is the exception).

* **Sun cascades** (`computeSunCascades`). Focused: cascade c is the SPHERE of radius `splits[c + 1]`
  around the focus (nested), and `getSunCascade` (PCSS), `giSunShadow` (GI) and the debug overlay pick
  by distance to `u_sceneFocus`, so "Shadows/Max distance (m)" and the split lambda are metres from the
  point. Cleared: the classic camera-frustum slices.
* **GI grid shape** ("GI/Cascades", "Probes X/Y/Z (log2)", "Focus Y offset (m)") is `Globals::settings.gi.grid`
  (`GiGridConfig`, Settings.Render), injected into EVERY shader compile as `GI_NUM_CASCADES`, `GI_PROBE_DIM_X/Y/Z` and
  `GI_FOCUS_Y_OFFSET` (Shader.cpp `buildLayoutPreamble`), so the toroidal addressing stays constant-folded. Dims are
  powers of two (the `lc & (DIM-1)` mask) and every dim ≥ 4 keeps the probe count a multiple of 64 (the trace's sky
  workgroup). A change runs the Renderer's listener (`Renderer::attachSettingsListeners`, RendererSettings.cpp): GPU
  idle → `resizeGrid()` (SH buffer re-allocated, clear scheduled; the consumers rebind it at their next record) →
  `Renderer::reloadShaders()`. Every setting registers (main, `Settings::register*`) BEFORE the Renderer initializes,
  so a `--tweak` override is live for every shader and for the buffer `GIProbePipeline::initialize` allocates.
  **Every reload listener in `Renderer` returns while `!m_initialized`** (state the pipelines read at creation is
  handed over before that test): a Saved or override value can fire the listener before the device and the pipelines
  exist, and a reload there crashes. A positive Y offset lifts the grid centre so more probes sit above the ground
  than below.
* **`DescriptorSetUpdateInfo` holds small-buffer vectors** (`oc::small_vector<…, 2>`, CommandBuffer.ixx): the
  per-frame passes build these as temporaries with one info each; with `oc::vector` every entry was a heap allocation
  per record.
* **"Record GI" allocates nothing per frame.** `GIProbePipeline` keeps its `DescriptorSetUpdateInfo` lists as members
  (`buildUpdateScratch`, handles patched per record), and `AccelerationStructure::recordBuildSkinnedBlas` refills
  member build arrays. Keep it that way: a per-frame `oc::vector` temporary in that scope shows up as memory churn in
  the profiler.
* **GI clipmap + TLAS range.** `giCascadeOrigin(c, u_sceneFocus.xyz)` centres every probe cascade on the focus
  (sample, trace and debug sides alike), the trace's previous-window freshness test uses the focus of the last frame
  whose trace RAN (`m_giTracedFocus`, published as `u_gi_prevFocus`), and the TLAS
  instance range bound ("RT/TLAS Range", `u_rt_giTlasRange`) is measured from `u_sceneFocus` too, so the ray-traced
  set is the geometry around the player.
* **Whether GI runs in a frame is known only in `present()`** (no instances = no GI at all; RT or GI off; no TLAS
  yet), after the UBO build. So `buildUboRayTracing` consumes nothing: `noteGiFrameRan` (`recordCommandBuffers`, with
  the primary) moves the traced focus and ends the full-bake request on the frames whose cached GI secondary holds
  the trace (`PerFrameData::giTraceRecorded`). A request consumed on a frame with no GI is lost: probes that scrolled
  in are never replaced, and they hold the data of the probe that scrolled out.
* **A CLEARED probe buffer (start-up, a grid change) gets one WARM-UP frame** (`u_gi_skyOnly`, `m_giWarmup`: no trace
  ran since the clear). The trace projects the sky SH and the levels only, the bake copies the sky SH only (the images
  keep W = 0). The frame after it has no traced focus (`GI_NO_FOCUS`), so EVERY probe is fresh and replaced, and the
  bake is full. Two reasons: the misses read LAST frame's sky SH (black on the first traced frame), and a cleared probe
  that is not fresh waits for its wave's regular visit - minutes in the far field, far longer under a finer cascade -
  while every reader takes it for a live BLACK probe with sky visibility 0 (a black band at the outer cascades, black
  sky reflections on the water there).
* **RTAO** "Fade Start" / "Max Distance": the fade and the trace early-out in `rtao.cs.glsl`, and the forward pass's
  upsample-skip gate, all measure from `u_sceneFocus`. The ray-origin distance bias stays on the CAMERA distance — it
  compensates a depth-reconstruction error that lies along the view ray, and so do the upsample's depth weights.

`shadowParams()` / `setShadowParams()` expose the same `ShadowParams` block the "Shadows" tweaks edit, so a mode can
install a preset that stays live in the panel (the game's match ctor/dtor; the dtor keeps the current `debugMode`,
which is a baked define).

## GI probes

* **THE update interval of a wave is ONE product** (`giWaveUpdateInterval`, gi_probe.inc.glsl): "GI/Update Interval
  Mult" (the global factor — **NOT a frame count on its own**) x the priority factor x the covered factor, floored
  ONCE (so it moves in single frames) and at ONE frame: **a close block's priority factor < 1 cancels the multiplier,
  down to every frame.** The 1e6-frame ceiling is numeric safety for the uint conversion only. A wave traces every
  that many frames, interleaved per WORKGROUP (whole waves exit). Fresh (just scrolled-in) probes always trace.
* **"GI/Temporal Alpha" is the per-frame blend AT 60 FPS**: `getTraceParams0` compounds it over the WALL delta
  (`u_gi_temporalAlpha` = this frame's alpha; wall, not sim, so GI converges through a pause; clamped so a hitch cannot
  replace the history), which makes convergence — and the speed at which the blend's noise wanders — frame-rate
  independent. The per-visit alpha compounds that over the interval, `1 - (1 - a)^interval`, capped at
  `GI_VISIT_ALPHA_MAX` (0.15), so wall-time convergence holds until one visit would dominate the history; past the
  cap the far tiers converge slower instead of flickering. To pay for that, **a fresh probe's replace visit traces
  `GI_FRESH_RAY_MULT` (4) x the rays** — its one snapshot is the whole history until the next, rarer, visit.
* **A wave is a compact 4x4x4 probe BLOCK** (the id -> lattice mapping in the trace's `main`; storage is addressed by
  lattice coord, so the mapping is free) — a per-wave exit only saves time when the whole wave exits, and a block is
  what is spatially coherent. **The blocks are WORLD-aligned (`lc >> 2`, `giWaveMin`), NOT window-relative** — the id
  enumerates toroidal SLOT space, whose 4x4x4 blocks are world blocks because every dim is a multiple of 4.
  Window-relative blocks re-partition every probe each time the window scrolls a cell: a probe being walked AWAY from
  could land in a nearer block and speed UP. The one block per axis that straddles the wrap seam has its halves on
  opposite window faces; `giWaveMin` folds it so all 64 lanes agree.
* **Update priority** (`giWavePriority`, a float factor with NO bounds): `(focusDist / priorityDist) ^ falloff /
  viewBoost`.
  * **"GI/Priority Falloff"** is how hard the rate drops with distance: 1 = interval proportional to distance, 2 = to
    its square (the far field all but stops), 0.5 = gentle, 0 = no distance term. The curve pivots on
    `priorityDist`, so CLOSER than it a higher falloff is a FASTER rate.
  * `focusDist` is measured from **`u_sceneFocus`**, not the camera, **to the block's bounding sphere: centre distance
    MINUS the block radius** (3.6 spacings: 7 m in cascade 0, 58 m in cascade 3, the same radius the frustum test
    uses). So a coarse block counts as nearer, and updates faster, than a fine block at the same centre distance.
  * **"GI/Priority Distance (m)"** = the NOMINAL-RATE distance of a block OUT of view: a block there has factor 1 and
    traces at exactly the interval multiplier, whatever the falloff.
  * **`viewBoost` = "GI/Priority Frustum Weight" (>= 1) for a block IN `u_frustumPlanes`** — its interval is divided
    by it — **fading to 1 over `priorityDist` metres outside the frustum** (a block just off screen keeps most of it).
    The weight acts on the VISIBLE blocks on purpose: the debug colours can show it, and it is the big lever in first
    person, where most of the near cascades is out of view.
  * Defaults: 8 m / falloff 1.5 / weight 5, interval mult 16, 17 rays, temporal alpha 0.025. In view the interval is
    `16 x (d / 8)^1.5 / 5` frames — every frame within ~3.7 m, 3 at 8 m, 12 at 20 m, 50 at 50 m; out of view, 5x that.
    The values ride `u_rt_giIntervalMult` / `giPriorityDist` / `giPriorityFalloff` / `giPriorityFrustumWeight`.
* **Covered waves (hollow cascades):** the priority distance is in CELLS, so the centre of every coarse cascade — the
  1/8 under the next finer window, which no lookup reads — would have that cascade's highest rate. `giWaveCovered`
  (gi_probe.inc.glsl) is true when the block plus one spacing of trilinear support sits inside the finer cascade's
  fade == 1 box (minus half a finer cell for the normal-bias difference); `giWaveUpdateInterval` then multiplies by
  `GI_COVERED_INTERVAL` (16). **Not a skip:** the probes stay warm for the all-dead fall-through and for the moment
  the finer window scrolls off them. The test reads `GI_CASCADE_FADE_BAND` (0.1 of the narrowest dim, shared with
  `evalProbeCoverage`) — **widen the band and fewer blocks are covered; the two cannot drift apart because both read
  the define.**
* **The cross-cascade fade is `giCascadeFade`:** continuous in the sample point, measured from the UNSNAPPED focus as
  a box of `DIM/2 - 2` cells (the snapped window always holds that box, so fade > 0 implies the stencil fits),
  smoothstepped. A fade from the integer cell index or the snapped window steps along the seam on every scroll. Past
  the box a finer cascade is NOT sampled. The two cascades blend as UNCLAMPED irradiance (`giSampleCascade` returns
  `giEvalSHLinear` against `giIrradianceBasis(n)`) and are clamped once — identical to blending the SH coefficients,
  because everything before the `max(0)` is linear in them, but 3 live floats per cascade instead of 12. The outermost
  cascade's coverage fade (shading and `giEvalBounce`) uses the same function with a 0.2 band.
* **The probe debug view draws SPHERE IMPOSTORS in every mode** (a camera-facing quad, 6 verts; the fragment shader
  intersects the sphere and writes the hit's `gl_FragDepth`), so both debug bindings carry the fragment stage. The
  irradiance mode evaluates the SH per pixel along the true normal, scaled by "GI/Strength" (`u_rt_giStrength`) like
  the scene's lookup and unshaded; the flat-colour modes are shaded off the sphere normal. **The impostors need depth
  WRITES to sort among themselves** (draw order cannot: a far fine-cascade sphere would overdraw a near coarse one),
  so the stage runs in the "Scene opaque" group, before the depth turns read-only (see Frame order
  ([CONTEXT.md](CONTEXT.md#frame-order))). "GI/Debug probe colour" (key O cycles):
  * **"Update priority"** shares `giWaveUpdateInterval` and shows the wave's ACTUAL interval in frames: MAGENTA = every
    frame (a hue the ramp never makes; white would be ambiguous, the ramp's yellow can clip to it through exposure /
    bloom), then a LOG ramp blue (2 frames) -> green (~22) -> yellow (~76) -> red (256 or more), dead probes dimmed.
  * **"Relocation / backface"** shows the misc vec4: red = the lookup's backface-dead fade, blue = the relocation
    offset over its clamp, yellow = escaped on its last visit (the stored fraction pinned to exactly DEAD_MAX) — a probe
    that keeps returning to yellow is re-escaping, and jumps at its visit rate.
  * **"Visibility"** is per pixel like irradiance: for the sphere normal n it rebuilds what `giSampleCascade`'s
    Chebyshev test sees for a surface in direction n from the probe (same mean scale, cap and variance floor): grey =
    mean distance over the cap (black = occluder at the probe, white = open), orange tint (multiplied) = deviation
    above the variance floor (edge softness, linear to cap / 2), magenta = no depth data yet, dead probes dimmed.
* **The Chebyshev test has THREE knobs, one job each** (`u_rt_giVis*`; `giVisMoments` in gi_probe.inc.glsl, shared
  with the debug view):
  * "GI/Vis Mean Scale" (1.2) moves the occlusion THRESHOLD — it scales the DEPTH, so the second moment scales by k²
    and the variance stays consistent (**scaling the mean alone makes `mean2 - (k·mean)²` negative nearly everywhere**,
    and the variance is always the floor); a wall at m reads as k·m, so (k - 1)·m behind it leaks.
  * "GI/Vis Variance Floor" (0.35 spacings) is the MINIMUM edge softness (covers the L1 mean's 0.15..0.4-spacing error
    toward a wall; under ~0.25 the ray jitter moves the edge); the measured variance widens it sideways past a wall,
    where the L1 mean is 1.4..1.75x too short.
  * "GI/Vis Weight Floor" (0.01) is the leak level and the all-occluded fallback.
  * The exponent is the fixed `GI_VIS_CHEB_POWER` (2): near the threshold the weight is ~ 1 - p(Δ/σ)², so a power
    tweak only rescales the floor; an additive mean bias was removed too (same effect as the scale or the floor).
  * **"No depth data" is tested on the DC coefficient (`d2sh.x`), never on the reconstructed mean2** — that rings to
    <= 0 toward a close wall.
* **Temporal stability** (the blend averages the last ~1/alpha visits, so per-visit noise reads as a slow DRIFT, and
  ray count only buys sqrt(N)): the per-visit shift of the ray lattice is an **R2 sequence over the wave's visit
  number** (integer fixed point; the per-probe hash only decorrelates neighbours), so the blended shifts are stratified
  instead of white; and a **visit clamp with change detection** on the irradiance blend. Each probe keeps its visits'
  DC luminance history in `GI_SUN_V4.zw`: a slow second moment (plain alpha) gives sigma around the stored luminance
  (floored at `GI_SIGMA_MIN_REL`), a fast mean (`GI_FAST_MEAN_ALPHA`) follows the recent visits. A regular visit
  outside stored ± `GI_CLAMP_SIGMA` sigma is SCALED onto the bound (all SH coefficients and the sun part: direction and
  sun fraction kept), both ways. A REAL change moves the fast mean, noise does not: the fast mean `GI_CHANGE_SIGMA` off
  and the previous one `GI_CHANGE_CONFIRM_SIGMA` off on the same side (two visits, so one spike cannot trigger it)
  blends that visit unclamped at `GI_CHANGE_ALPHA`. The moments are never clamped, so sigma widens while a change
  passes; a replace restarts them with `GI_SIGMA_INIT_REL` as sigma. Boosted visits (relocation, just escaped) skip
  both, and a CLEARED (all-zero) slot is replaced like a fresh one.
* **SUN / SKY RESCALE (dawn, dusk): the probes store NORMALIZED light.** A visit's sun part (`GI_SUN_V4.x`: the direct
  sun at hits, the sunlit ground on misses, the sun share of the bounce) and sky part (the high fp16 half of `GI_SUN_V4.y`: the rest of the
  misses + the sky-radiance light) are DC luminances divided by the visit's levels — `giSunLevel()` (luma of
  `u_sunTransmittance × u_sunColor × u_sunVisible`) and `giSkyLevel(sky SH c0)` (its DC minus the sunlit-ground DC) —
  and the SH is scaled to match. Everything after that in the trace (the visit clamp, the change detection, the blend)
  sees the normalized visit, so a global dimming is neither clamped nor a "change". `giRescale` is the inverse at given
  levels: **the volume bake writes ABSOLUTE light at the REFERENCE levels** (`GI_SKY_REF_V4`), and the probe-loop
  lookups (`giSampleCascade`, `giEvalBounce`, the debug view) rescale at this frame's (`GI_SKY_LEVELS_V4`). Both live
  in the sky slot after the sky SH (`GI_SKY_SLOT_STRIDE`), written by the trace's sky workgroup. The reference moves,
  with a full bake, when this frame's levels are more than **"GI/Light rescale step"** (default 0.02) of their sum away,
  so every voxel is always baked at one set of levels (a partial bake leaves no seam). So the BRIGHTNESS follows the
  sun / sky at every distance at once; the colour and direction change still waits for each probe's updates.
  * **The volume has no sky channel, so the bounce's non-sun part stays absolute** ("rest", with the grid lights): a
    lookup cannot split the sky's bounce from a lamp's, and rescaling a lamp's bounce by the sky level would flash it
    at dawn. Only that second-order term lags as before.
  * The levels the trace normalizes with are this frame's sun and the sky SH its misses read (last frame's copy in
    volume mode); the one-frame difference is far below the step.
  * **A part normalizes only against a level that can hold it** (level > `GI_LEVEL_MIN` and part <= `GI_PART_MAX` (4) x
    level); otherwise it stays absolute in the rest. The volume's sky SH copy is ZERO before the first bake while the
    sky-radiance light is not: a part divided by a near-zero level made white GI at start-up that the far probes kept
    for minutes.
* **Sky visibility** (the low fp16 half of `GI_SUN_V4.y`, packed with the sky part by `giPackSkyVis` — only bit casts
  on that float, never arithmetic): the cosine-weighted share of the upper hemisphere (about `u_skyUp`) whose rays
  miss, blended at the plain alpha, baked × W into the volume tail's `.a`. `giSkyVisibility(pos, n)` reads it (one
  tail fetch, the next cascade where the first is dead, open sky past the field; 1 without the volume). It occludes
  the sky reflections the diffuse GI does not carry: the terrain's wet sky reflection and the film overlay's sky mirror
  + blurred sky share (a puddle under a roof mirrors no sky), and the ocean's top side: its blurred sky share and the
  mirror's sky fallback - a traced mirror HIT is geometry and keeps its weight. (Not on the view from below the
  surface.) **Keep it register-neutral:** the trace derives the miss terms per miss in `traceMiss` (one accumulator
  across the ray-query loop, the hemisphere total taken as its expectation N / 4) - holding the sun direction, up and
  the sums across the loop cost the trace 96 -> 128 registers; the terrain folds its lookup into `skyReflW` before
  the lit core.
* **Dead-probe skipping:** a probe whose stored backface fraction is past `GI_BACKFACE_DEAD_MAX` (under the terrain,
  inside a wall — the lookup rejects it anyway) traces only every `GI_DEAD_INTERVAL` (8) regular visits, enough for
  the escape relocation and the wake-up; an escape visit pins the stored fraction to exactly DEAD_MAX so the next
  visit is not skipped. **The stored fraction is the EMBEDDED fraction:** the backface-hit fraction × the enclosure, a
  smoothstep of the backface share of the hits (front hits counted within the depth cap) over
  `GI_ENCLOSED_SHARE_MIN` 0.6 .. `_MAX` 0.85. Inside a closed solid every hit is a backface (share ≈ 1); among
  double-sided geometry — a tree canopy's leaf diamonds and opaque-traced cards — about half are, and the raw fraction
  relocated canopy probes on every visit and marked them dead. The escape, the dead skip/rejection and the
  just-escaped flush all read the weighted value.
* **Direct light at a gather hit is ray-traced-shadowed for EVERY light:** the sun through `g_sunShadowOverride`, the
  grid lights through `giLightIrradianceShadowed` (lighting.inc.glsl, compiled in by the trace's
  `GI_LIGHT_RT_SHADOWS`; one ray to the light's centre, only past `GI_LIGHT_SHADOW_MIN`, and only while
  `u_rt_lightShadows` is on). Unshadowed, a lamp lights every hit in its range through walls — a leak the probe
  visibility test cannot see. The trace includes rt_shadow.inc.glsl BEFORE lighting.inc.glsl for this.
* **Every ray-hit vertex fetch** (the trace, RTAO, `rt_shadow.inc.glsl`, the ocean's and the terrain's mirror hits)
  reads `MeshVertex in_vertices[]` from `mesh_vertex.inc.glsl` (plain std430; the skinning pass uses it too).
  **`MeshVertex` is 3 × vec4 — `positionU`, `normalV`, `tangent` — with the texCoord in the two `.w`**, so every member
  is 16 B-aligned (one wide load each) and position stays at offset 0 for the BLAS build. The vertex input binds
  locations 0 / 1 as vec4s (a shader without uv declares vec3 there). The terrain is not in the TLAS: the trace stops at
  the terrain ray march's hit ("THE TERRAIN RAY MARCH" in [Terrain.md](Terrain.md)).
* **Miss rays AND the virtual sky probe sample THE SKY MAP** instead of marching the atmosphere, so the out-of-field
  fallback matches the misses by construction. **Gather hits use `giEvalBounce`** (gi_probe.inc.glsl, write side;
  probe path only): the cheap multi-bounce lookup — no Chebyshev, no cross-cascade blend — because the result is
  temporally blended. It picks its cascade with the shading lookup's `giStartCascade` (not the tracing probe's own
  cascade, whose covered coarse probes are nearly stale) and has the same one-cascade dead fall-through.
* The probe buffer is **NOT `coherent`** in the trace (each invocation writes only its own probe; stale cross-probe
  reads are by design). **TLAS exclusions are INACTIVE instances** (reference 0, `gi_tlas_instances.cs.glsl`), which
  the build skips entirely, not mask-0 nodes.
* **THE GI RECORD IS SPLIT IN TWO.** `recordGlobalIllumPrep` is the PER-FRAME secondary (GPU scope "BLAS builds") —
  the work whose content changes frame to frame: one-shot static BLAS builds, compaction copies, the skinned BLAS
  rebuild, **the TLAS-instance write and the TLAS build over this frame's LIVE instance count**, the one-time probe
  clear. `recordGlobalIllum` is a CACHED secondary (recorded with the scene secondaries, scope "TLAS + probe trace"):
  the sky map, the probe trace and the volume bake. **Everything per-frame in it rides the UBO** — `u_rt` / `u_gi` (the
  trace tweaks, last frame's focus, the TLAS range), `u_frameIndex`, `u_sceneFocus` — so neither shader has push
  constants.
  * **Why the TLAS is per frame:** a build's primitive count is a recorded CPU value, so a cached build would have to
    cover the instance buffer's whole CAPACITY. The live count is final before the record (`present()` sets
    `m_giTlasDemand`, `giTlasLiveCount()`, before `recordCommandBuffers`), and an indirect build is no option (NVIDIA
    offers no indirect acceleration-structure build). The build always runs, at count 0 too (an empty TLAS): a skipped
    one would keep records that may reference freed BLASes.
  * **`u_present_giTlasNumInstances` is a `UboPresent` value** (`giTlasLiveCount()`, written in `present()`), never a
    beginFrame UBO value: the live count is not known there, and a 0 count gives an empty TLAS and no ray hits
    anywhere (RT shadows, RTAO, GI, ocean rays) with no validation error.
  * `AccelerationStructure::ensureTlasCapacity` (CPU, at the top of `recordCommandBuffers`) sizes each slot's TLAS to
    that capacity and invalidates on a handle change, so the GI/RTAO/fog secondaries and the forward set's TLAS
    descriptor all re-record together. Invalidation also comes from the instance-capacity growth and from the
    "RT/Enable RT" + "GI/Enable GI" tweaks (baked in). The trace set's texture array is filled at set allocation
    (`fillTextureDescriptors`) and kept current per slot by the streamer's pending-write path — the record never
    rewrites it.
* **THE SKY MAP** (`gi_sky_map.cs.glsl`, owned by `GIProbePipeline`, `recordSkyMap` at the top of the cached
  `recordGlobalIllum` — ahead of the RT toggle — with its own read→write→read barriers): a 256×128 RGBA16F lat-long
  array of 4 layers (`SKY_MAP_LAYERS`), GENERAL for life. Mapping + layer ids live in atmosphere.inc.glsl (`skyMapUV`
  / `skyMapDir`, `SKY_MAP_LAYER_*`, `SKY_MAP_*_TEXEL`):
  * layer 0 = `skyRadiance` WITH the volumetric clouds, held only in a `SKY_MAP_GI_WIDTH x HEIGHT` (64×32) corner -
    the GI miss rays and the sky SH. **Read it with `skyMapGISample`, never `textureLod` / `skyMapUV`** (the corner
    wraps in U by hand);
  * layer 1 = `mirrorSkyRadiance` WITH the clouds, full size (the ocean's and the terrain wet film's reflection-ray
    sky, 12-step march + saturation); clouds composited as `inScatter + sky * transmittance` (CloudPipeline's sky
    clouds, see "Volumetric clouds" ([Clouds.md](Clouds.md)));
  * layer 2 = per-frame CONSTANTS in single texels (`texelFetch`): `SKY_MAP_CLOUD_AMBIENT_TEXEL` (`skyRadiance`
    WITHOUT clouds - the clouds' own ambient, so they never light themselves through their image),
    `SKY_MAP_TREE_SKY_TEXEL` (the far trees' sky light), `SKY_MAP_GI_ZENITH_TEXEL` (the GI-off sky ambient);
  * layer 3 = the CLEAR mirror sky, the bake's own cache: each frame re-marches one texel of every 2x2 block and
    composites the clouds over the cached rest.
  **A single zenith texel is NOT an ambient once clouds are on:** it is the one cloud straight over the camera. The
  OCEAN therefore takes its sky light (body in-scatter, whitewater, underside) as the sky SH's hemisphere average
  `giEvalSkySH(up) / pi`, and the roughness blur's sky share as `giEvalSkySHH(normalize(R + up)) / pi` (the
  reflection's side of the sky; NOT centred on R: a grazing reflection's lobe would be half below the horizon, where the
  SH holds the dark ground); the zenith texel is only the GI-off fallback (`u_rt_giStrength` 0, stale SH). The terrain
  WET FILM does the same (`terrain_film.fs.glsl`). The forward set binds the map at **20** (binding **21** is the GI
  irradiance volume, **22** the cloud shadow map, **23** the near grass cascade, **24** the faction map, and the texture
  array is **25**: it must stay the set's highest binding, for the variable count - a new binding takes its number and
  moves it up, in `buildPipelineLayout`, `updateTextureDescriptor`, `record()` and the six `u_textures` declarations). **Anything that would call `skyRadiance` or `atmosphereScatterCheap` per pixel samples the map
  instead.**
* **THE GI IRRADIANCE VOLUME** ("GI/Irradiance volume", on by default; "GI/Volume voxels per probe" 1–2, default 2 —
  both in `Globals::settings.gi.grid`). Per frame, right after the trace (inside the GI toggle),
  `GIProbePipeline::recordVolumeBake` (`gi_volume_bake.cs.glsl`) bakes every cascade into 3D images: the probe lattice
  refined `volumeRes` times per axis, stored TOROIDALLY like the probes (slot = fine coord & (dims − 1)), so the lookup
  is REPEAT addressing + hardware trilinear. A voxel = the visibility-weighted blend of its 8 probes AT THE VOXEL
  CENTRE (trilinear × backface-dead fade × Chebyshev).
  * **20 B per voxel, 4 images per cascade** (`RendererVKLayout::GI_VOLUME_FORMATS`): L0 × W in B10G11R11_UFLOAT
    (PREMULTIPLIED by the summed weight, so a pixel's trilinear fetch is a weight-correct blend and a dead voxel adds
    nothing instead of black), the 9 L1 terms as RATIOS to L0 scaled by 1/√3 (for a non-negative radiance every ratio
    is within ±√3) in 2 × RGBA8_SNORM + the 9th in RGBA16F next to W (fp16, because L0 = fetch / W needs W's precision
    when it is small) and the sun fraction × W (the cloud dimming, see "GI under cloud shadows" ([Clouds.md](Clouds.md))).
    Low precision is safe because the volume is never accumulated — every bake writes a finished value from the fp32
    probe history. The price: the ratios filter unweighted, so where L0 changes fast the direction is slightly off,
    and a dead voxel's zero ratio pulls its neighbours' L1 a little toward 0. Default grid: 8 × 32³ voxels, ~5 MB.
  * `createVolume` asserts storage + linear-filter support for the formats; B10G11R11 storage needs
    `shaderStorageImageExtendedFormats`, which `Device` enables with every other supported core feature.
  * **The price of the volume: the half-Lambert probe-direction weight needs the surface normal and is NOT baked, and
    the Chebyshev test runs from the voxel centre — more leaking through geometry thinner than a voxel** (the
    per-pixel normal bias of the sample point is kept).
  * **EVERY probe consumer switches with the toggle** (`GI_VOLUME` define; the probe code stays for the off path;
    `GI_VOLUME_TEXTURES_NAME` makes `evalProbeCoverage` read the volume through `giLookupCascade`, 4 filtered fetches
    per cascade instead of 8 probes × 7 vec4 loads): the lit core, the ocean and the terrain's mirror hits (static-mesh
    set binding 21), decals (5), particles (10, vertex), fog scatter (12) and fog apply (5, sky only), and the TRACE's
    multi-bounce lookup at gather hits (13 — the wave stamps are 14, its texture array 15), which reads LAST frame's
    bake with the baked Chebyshev instead of `giEvalBounce`.
  * **The sky SH (the out-of-field fallback)** is copied by the bake every frame, before the partial early-out, into
    one extra 3×1×1 RGBA16F image at the fixed slot `GI_VOLUME_SKY_IMAGE`; in volume mode `giEvalSkySH` `texelFetch`es
    it. So in volume mode no consumer reads the probe buffer — only the trace, the bake and the probe debug view do.
  * Every consumer binds `GiVolumeDescriptors` (GIProbePipeline.ixx): one sampler3D array of `GI_VOLUME_MAX_IMAGES`
    (8 cascades × 4 + the sky), partially bound, written with `fillUpdates` only while the volume exists — so no set
    layout changes with the grid. Images are GENERAL for life, cleared to zero at creation (W = 0 = no data). The
    bake's barriers cover fragment, vertex and compute readers.
  * **Every GI consumer multiplies its GI term by `u_rt_giStrength`** (GI strength, 0 with GI or RT off, where the
    probes and the sky SH are stale).
  * **The bake is PARTIAL, on the trace's own decision:** on a wave's regular visit, trace lane 0 writes
    `u_frameIndex + 1` into `GI.waveStamps` (one uint per wave = per trace workgroup, trace binding 14, bake binding 6).
    A voxel re-bakes only when a wave under its 8-probe stencil carries this frame's stamp
    (`gi_waveStamp[giWaveWorkgroup(...)]`) or one of those probes is fresh (`giProbeFresh` on the stencil's two
    corners — the trace's own test); every other voxel keeps its value. The bake never evaluates
    `giWaveUpdateInterval` itself, so it cannot drift from the schedule. The stamp is written before the trace's
    dead-probe skip, so dead waves re-bake more often than they trace (harmless). **A new trace skip condition before
    the stamp must keep the stamp exact; a fresh-probe change goes through `giProbeFresh`.** `u_gi_fullBake` = one full
    bake: `fullBakeDue` asks for it after `createVolume` and when a Chebyshev knob moves (the baked value depends
    on them), held until a frame whose bake ran with it (`markFullBakeDone`, from `noteGiFrameRan`). **The trace forces a full bake too** (`GI_SKY_REF_V4.z`) when it moves
    the sun / sky rescale's reference levels (below).
  * **The two volume tweaks reload through their own listener** (`resizeVolume` + reload): they never re-allocate
    the probe buffer, so the traced history survives a volume toggle.
* **ONE shading lookup, `evalProbeCoverage`**, for the probe and the volume path alike (`giLookupCascade` picks the
  per-cascade sampler at compile time). **`giIrradiance(worldPos, n)`** wraps it with the sky SH fade — the lit core
  (`giIndirectOverPiH`), decals, particles and the fog scatter all call that; only the trace (scales by coverage, no
  sky) calls `evalProbeCoverage` itself. **It has NO cascade walk**: `giStartCascade` computes c from the Chebyshev
  focus distance (a normal-free bound, the bias taken at its per-axis maximum, so at most one cascade early — then
  c + 1, which always holds the point for probe dims >= 16), and returns the biased point and its fade (> 0 implies
  the stencil fits). **At most TWO cascades are sampled: c, and c + 1** as either the blend partner in c's band or the
  fall-through when c is all dead; a dead c does NOT blend on from c + 1 into c + 2. Dead in both, or past the
  outermost box: coverage 0, and every caller uses only its sky fallback. `giEvalBounce` uses the same pick and
  fall-through, with no blend.

## RTAO and normals

* **THE FORWARD PASS READS LAST FRAME'S AO.** `sampleAOBilateral` (instanced_indirect_lit.inc.glsl) reprojects the
  fragment in clip space (`fragPrevClip` off `gl_FragCoord`: the camera through `u_reprojClip` plus the point's own
  motion, `MOTION_WORLD_DELTA` - see "Motion vectors" ([PostProcess.md](PostProcess.md))), samples the PREVIOUS slot's
  AO image (binding 13) and weights the 2x2 taps by their world distance to where the point WAS, reconstructed from
  the PREVIOUS slot's depth (binding 12, `u_prevDepth`, `u_prevInvMvp`, last frame's jitter). No input comes from this
  frame, so the trace has NO ordering constraint against the forward pass (this is what allows no depth prepass).
  Static geometry is exact under camera motion, and a moving object reads its own AO from where it was (the terrain
  has no `MOTION_WORLD_DELTA`: it compiles the camera-only path). No valid tap (disocclusion, off-screen) =
  `(0, 0, 0, 1)`: no occlusion, no bent normal. **All eight fetches (4 depth + 4 AO, `textureLod`, AO held half) are
  issued first, then the weights** - one texture-latency wait instead of each AO tap sitting behind its own depth
  test.
* **THERE IS NO NORMAL CONSUMER LEFT BUT TAA's OCEAN FLAG.** RTAO, its spatial blur, the decals and the particle
  collision derive a GEOMETRIC normal from depth: `normalFromDepth` (shared.inc.glsl) — per axis the neighbour with the
  closer depth, built on `viewRelFromDepth`, the CAMERA-RELATIVE reconstruction (`worldPosFromDepth` rounds at the
  scale of the world coordinate, which is a pixel footprint near the camera: fine for a position, noise for a
  difference of neighbours). The blur's crease edge-stop is the tap's distance off the centre's tangent plane (sigma =
  one AO texel's world footprint), not a per-tap normal compare. **A ZERO bent normal in the AO image means "none"**
  (past the max distance, fully occluded, background): the lit shader then keeps its own shading normal, so the
  depth-derived facets never reach the GI lookup.
* **GRASS, CLUTTER AND TREE LEAVES ARE HOLES IN THE AO.** They are in the depth image but not in the TLAS, and they read no RTAO
  (`LIT_NO_RTAO`; the tree FS, which also draws the bark, skips the read per fragment: `LIT_RTAO_SKIP` - [Scene.md](Scene.md)
  "RTAO AND THE TREES"). A trace from their depth is a near-random normal a few cm above the ground, its history fails under
  the wind, and the ground between the blades upsamples it: grain at range, worse at a lower DLSS render resolution.
  So their fragments write the NO-AO flag into the motion target (`motionVectorNoAo`, see "Motion vectors"
  ([PostProcess.md](PostProcess.md))); the trace reads it (binding 1) and writes a HOLE (a = -1) without rays, the
  temporal pass keeps the hole and rejects hole history taps, and the spatial pass fills a hole like the background
  (Gaussian over the traced texels around, `(0, 0, 0, 1)` with none), and skips holes in the blur. The final image
  holds no hole.

## The sun cycle ("Sky/Sun cycle" tweaks)

On by default, starting at 5 h (before a sunrise); "Paused" holds the time (the direction still follows a dragged time /
latitude). While "Enabled", `Renderer::advanceSunCycle` (main, in `kickBeginFrameJob`, before the begin-frame job
reads the sky) advances "Time of day (h)" by the SIM delta x "Speed (x real time)" (1 = real time, 60 = a day in 24
minutes; the pause holds the sun), wraps it at 24 h and WRITES the "Sun Direction" tweak every frame (`SkyParams::cycleSunDirection`, in Settings so the game reads the same sun through `SkyParams::sunHeight`:
the declination from "Day of year", the hour angle 15 degrees per hour from noon, "Latitude (deg)"; east +X, up +Y,
north -Z) - a hand edit of the direction is overwritten while it runs, a dragged time / latitude shows at once even at
speed 0. **Sky Radiance** follows the day: `skyRadianceNow` scales the UBO's `sky_radiance` by a smooth step over the
sun's height (x "Night sky radiance" with the sun down, x 1 from ~15 degrees up); the "Sky Radiance" tweak keeps its
value. The cycle tweaks are Runtime, and they are `sky_radiance`'s sources: that entry stays a LIVE uniform (never
baked under the "Sky" lock).

## Beam lights (`BeamLight`, Light.ixx)

A straight line of light of any length: a lamp tube, or a column kilometres tall (the co-op Base's beacon). The record
is the former tube light's (`radius < 0`, `width` = the core radius, `direction` x the full length, `pos` = the CENTRE).

* **Surfaces** (`doBeamLight`, punctual_lights.inc.glsl): the closest point on the line drives the falloff over the
  light's range; the specular is the representative point.
* **The shadow is ONE ray at a FIXED point** (`beamLightVisibility`): the closest point on the line, moved along the
  axis by the distance to the line (the ray leaves at 45 degrees, over what stands at the beam's foot), clamped to the
  line. A point BELOW the beam's start measures from the start (`max(along, 0)`), so ground lower than a beacon still
  aims well up the beam and not into the building under it. No jitter, so no noise: a jittered point over a line this long never converges under TAA.
* **The glow in the air is closed form** (`beamGlow`, Sky/beam_glow.inc.glsl - the fog apply's `beamColumns` and the
  beam cards below both call it): along the view ray the
  squared distance to the line is a quadratic, and two profiles integrate exactly over it - a tight core
  `r^3 / (d^2 + r^2)^2` and a weak halo `r / (d^2 + r^2)` (`BEAM_HALO`) that ends at the light's range. **The halo's ends are ROUND:** its
  fade takes the distance to the beam as a capsule (from the point where the ray passes the beam to the SEGMENT), and
  its ray part reaches the range past each end plane; cut on the end planes it showed a flat bottom and top. Past an
  end the profile's own distance is the capsule's too (`qh`: + the distance past the end, squared) - with the line's
  distance alone the halo drew a thin dim beam going on past the tip. The core is
  cut to the beam's length, both to the scene depth; a cloud in front dims it by the cloud's transmittance; the fog and the
  far trees do not dim it. No march, no shadow ray. **Only with the fog on** (the cloud apply draws none).
* **THE TIP FADE** (`BeamLight`'s `tipFade`, m; it rides in the record's `rotation`, which a beam does not use;
  `u_present_beamFade[i].x`): the glow (core and halo) x a smoothstep over that length toward the end the direction
  points at, by where the view ray passes the beam - a short beam ends soft. 0 = a hard end (the default). The light
  on surfaces does not fade.
* **The frame's first `RendererState::MAX_BEAM_COLUMNS` (32, `BEAM_COLUMNS` in the shader: change both) beams** get a column: `addLightInfo` claims a slot
  (lock-free, reset in the begin-frame job), and they ride the UBO's PRESENT values (`u_present_beamCount` /
  `beamFoot` / `beamAxis` / `beamColor`) - lights are added after the begin-frame build.
* **The light grid** bins a beam over at most 2048 m of its height from its lower end (`MAX_GRIDS_PER_AXIS`): surfaces
  above that get no light from it. **A tall beam with a wide range does not belong in the grid:** the grid holds every
  64 m grid in the light's bound (the range on all sides), 1.7 MB each at 2 m cells (26 KB at 8 m), so the count grows
  with range^3 (a 300 m range = ~1000 grids). `Renderer::addBeamColumn` adds the glow in the air ALONE (a column
  slot, no light record), and `addLightInfo(light, false)` a beam with no column: the co-op beacon is a tall column
  plus a short surface beam at its foot. The fog froxels, GI and particles see a beam as a point at its centre.
* **A column costs EVERY pixel of the screen** (the fog apply walks all the frame's columns per pixel), so the count
  is small. **`Renderer::addBeamCard` draws the same glow as a BEAM CARD** (`BeamCardPipeline`, Sky/beam_card.*.glsl; no
  light record, no column slot): one camera-facing quad per beam around the capsule its glow reaches (the segment +
  the range on every side; each end offset in the plane that faces the camera there, so a beam seen end-on is a
  square around each end), whose fragment shader runs `beamGlow` for that one beam - cut at the scene depth and
  dimmed by a cloud in front as the columns are. **The cost is the card's pixels.** With the camera within 1.5 x the
  range of the segment the card is the whole render rect (one column's cost). Additive (ONE, ONE), alpha masked, no
  depth test. The scene stage "Beam cards" right after "Fog apply" (VR: per eye inline), gated on the frame's count;
  the cards are claimed lock-free like the columns (`m_beamCards`, reset in the begin-frame job, the first
  `BeamCardPipeline::MAX_CARDS` = 256), copied into a mapped per-slot buffer in `present` and drawn with one indirect
  call (6 vertices per card, the count in a mapped indirect buffer: the cached secondary never re-records for it).
  **Unlike the columns they draw with the fog off.** The game's light links use them (Game's Economy.md, "Drawn").

## Long-range sun shadows

"Shadows/Terrain march start (m) / bias (m) / spread". Past the PCSS "Max distance (m)" and "RT/TLAS Range", distant
pixels cone-march the baked terrain height cascades (`terrainSunVisibility` in `terrain_height.inc.glsl`) and take
`min` with the shadow term.

**The march fades in inside cascade range, so the handover is seamless.** Steps double from "Terrain march bias",
which is also the self-shadow bias. **Deterministic — no jitter, no temporal integration.**

## Sun shadow draw cost

The map: `SHADOW_MAP_RESOLUTION` (Layout.ixx) 4096² D32 x `NUM_SHADOW_CASCADES` (4) layers per frame slot - 256 MiB a
slot, 512 MiB for the two. The cascades are ONE MULTIVIEW pass over ONE caster list (the cull packs each caster's cascade mask; the VS makes the
triangles of the other cascades degenerate). The pass is nearly all TERRAIN, and it is VERTEX-bound, not
raster-bound: with every terrain triangle made degenerate it still cost ~70% of its time (sandbox, 1440p).

* **Tried and REVERTED: one draw list + one single-view pass per cascade.** Slower (the shadow draw and the cull both
  went up): on this hardware the multiview views SHARE the vertex work, so per-cascade lists only multiplied it.
* No effect either: a smaller "Shadows/Caster pad (m)", dropping terrain chunks from the coarser cascades where a
  finer one covers them (the masks change, the vertex work does not), and column-strip chunk indices for better
  post-transform reuse (vertex reuse is not the limit).
* **The terrain's shadow LOD** ("Shadows/Terrain half-res from (m)" 300, "Terrain quarter-res from (m)" 1000; the node's near
  side from the focus): the shadow cull gives a far node a coarser shared index range over the same vertices, every 2nd or
  every 4th (`terrainNodeIndices` stride, `setTerrainShadowIndices`, `u_terrain_shadowIndices`), so only those vertices run.
  By distance and not by the cascade mask: a near cascade's box reaches far up-sun and takes in distant nodes. A coarse
  caster differs from the surface it shades, so a value too low shows as false self-shadow on slopes.
* **ONLY THE CASTERS THE VIEW CAN SEE THE SHADOW OF** (`casterShadowInView`, instanced_indirect_shadow.cs.glsl; the
  far ring's trees likewise in tree_voxel_far.cs.glsl, THE FAR SHADOW CASTERS in [Scene.md](Scene.md)). The cascades lie
  all around the scene focus, but only what the view draws reads them: the lit pixels, the fog's froxels, the water.
  **The GI's sun is RAY TRACED** (`sunVisibility` in gi_probe_trace.cs.glsl), so no off-screen reader exists. A caster's
  shadow volume is its sphere swept along the light's travel for its REACH (`casterShadowReach`): down to the lowest
  ground a shadow can be seen on (`SHADOW_LOW_GROUND` 200 m below the sea level, or below the camera where that is
  lower), at most three times the cascades' range; a tree, which stands on the ground it shades, at most 6 x its radius
  x the sun's cotangent + 20 m. Outside one view-frustum plane with both ends of that sweep = culled. A view over a dense
  forest, 13:00: the shadow draw 1.08 -> 0.47 ms (far ring 0.36, the near trees and the terrain 0.35). Checked at 07:00
  and 18:18 against the uncut pass: the same picture.
* **In a forest the trees are the other half of the UNCUT pass** (1.10 ms; 0.49 ms with no trees). With
  EVERY caster made degenerate that pass still costs 0.56 ms: the vertex work again. The alpha test of the trees' shadow
  billboards is 0.13 ms, their raster 0.31 ms. The casters' vertex MATH is not the cost: the far casters' shader without
  the wind bend (a third of its code) took the same time. Bushes and rocks cost nothing here.
* What is left: caching the static casters' cascades.

## The atmosphere (`atmosphere.inc.glsl`, "Sky/Atmosphere" tweaks)

ONE model for the visible sky (`sky.fs.glsl`), the sky map (GI, reflections, the clouds' ambient), the aerial
perspective LUT and the air in front of the clouds: AIR (Rayleigh) and HAZE (Mie: a layer of "Haze height" scale
height + a background share), both exponential in height, so every optical depth is closed form (Chapman:
`atmosLightOpticalDepth`, `atmosRayOD`), plus an OZONE layer high up. Three lights reach a point of it: the sun, the
sky-radiance light (from straight up) and the multiple-scattering light.

* **THE HAZE SCATTERS IN TWO LOBES** (`atmosHazePhase`, a constant mode per march): haze is particles of two sizes,
  and one Henyey-Greenstein lobe cannot hold both. The WIDE lobe ("Haze forward scatter", `u_sky_mieG`: the fine
  particles) is the dull glow over a large part of the sky; the SHARP lobe ("Haze sun glow sharpness",
  `u_sky_hazeSharpG`, default 0.95: half its peak ~2 degrees from the sun) holds "Haze sun glow share"
  (`u_sky_hazeSharpShare`, 0.15) of the haze's scattering: a bright glow a few degrees round the sun, as bright as the
  haze is thick (x 4.7 at the sun, x 1.4 at 5 degrees at the defaults). Past ~20 degrees the two-lobe phase is the
  wide lobe x (1 - share).
  * `ATMOS_HAZE_SUN` (both lobes): the sky, the mirror layer, the aerial LUT (terrain toward the sun), the air in front
    of the clouds. The phase is per RAY, so it costs a march nothing per step.
  * `ATMOS_HAZE_SUN_WIDE` (all of the haze in the wide lobe, the same energy): `skyRadiance` - the low-res GI sky and
    every ambient. The sharp lobe is narrower than a GI texel (5.6 degrees): a sun that moves over the texel centres
    would make the sky light pulse.
  * `ATMOS_HAZE_DIFFUSE` (no lobe): the sky-radiance light (below).
  * **The sky's "Sun Glow" halo is another thing** (`sky.fs.glsl`): a glare under a degree wide at a fixed strength,
    in the visible sky only.
* **THE HAZE'S BACKGROUND** ("Haze background share", `u_sky_hazeBackground`, 0 .. 0.5): this share of the ground's
  haze density does not stay in the low layer; it thins with the AIR ("Air height"). Haze above the low layer: a whiter
  sky from altitude, a redder low sun (at 0.05 the haze straight up is x 1.3). **It has the air's scale height on
  purpose**: its density and its optical depth are a `mix` with the air's own (`atmosDensity`,
  `atmosLightOpticalDepth`), with no exponential and no Chapman evaluation of its own. A real haze top (an inversion)
  is still not covered.
* **THE OZONE IS A LAYER, not a share of the air** (`atmosOzonePath`, "Ozone": 1 = Earth-like): uniform, 15 - 35 km
  up, absorption only. Near the ground there is none, so a level ray gets it only on the LIGHT's way in; the layer's
  airmass toward a sun at the horizon is ~11.6 (the air's is ~38), and the light that reaches the high air after
  sunset goes through it twice. A layer has no Chapman form: its optical depth is a LENGTH through it, closed form for
  any point and direction (the ray's coordinate along itself against the two half chords).
  * **It is NOT in `atmosTau`.** A march adds it to the exponent of each sample's light (the light's way in + the
    view's way back to the origin) and to the transmittance it returns; the view transmittance its steps take from
    holds the air and the haze only, so (source / extinction) stays right.
  * The aerial LUT and `cloudAerialScatter` lie far under the layer: the lights' way in only, ONE length per column /
    per ray, outside the step (`atmosOzoneZenith` for the sky-radiance light). Their view transmittance has no ozone.

* **THE MARCH to the atmosphere's edge** (`atmosMarch`, for ONE light: it returns a radiance; `atmosphereScatter` =
  from the camera altitude, with the ground; `atmosphereScatterCheap` = from the ground observer, any direction - the
  mirror layer; `atmosphereScatterRising` = the same for a direction above the horizon, the least work - `skyRadiance`
  and the sky's ground). Two rules, both for the haze - a layer ~1 km thick on a ray 80 km (zenith) to 1000 km
  (horizon) long:
  * the step ENDS are quadratic, `tFar x (i / steps)^2`: the first samples lie inside the haze;
  * a step's light = (source / extinction) at its sample x **the transmittance the step takes away** (both
    transmittances closed form). A step of any optical thickness then scatters the right amount.

  12 steps (the sky, the mirror layer) are within 1 % of the integral in every direction, 4 (the GI layer) within ~3 %
  (~10 % near the sun), a thick haze included. **Tried and dropped: the midpoint rule over equal steps** - 20 % too dark
  at the horizon with 12 steps and 60 % with 4, and worse the thicker the haze.
  * **Its form is set by the register counts** (the comments in `atmosMarch`; the measured rules in "The atmosphere
    marches", [Shaders.md](Shaders.md)): ONE sum, the light colour and the "Saturation" curve applied in the step (the
    curve is linear, so per step = on the sum); the taken transmittance as its two halves (`sum += through x gain`,
    then the next transmittance, then `sum -= next x gain`), so the old transmittance is dead while the next one's
    optical depth is worked out.
* **"Humidity"** (0 .. 0.9, `SkyParams::humidityHazeScale`): the haze particles take up water - "Haze scattering" x
  `1 / (1 - humidity)^2` (0.5 = x 4, 0.7 = x 11, 0.8 = x 25, 0.9 = x 100). The base coefficient is very clean air (an
  optical depth of 0.005 straight up); x 11 is a clear day with a soft band at the horizon, x 25 a hazy one, x 100 a
  white murk. The water only scatters: `u_sky_mieExtinction` = 1 + ("Haze extinction" - 1) / the scale. CPU only
  (`skyBetaMie`, `skyMieExtinction`), so `u_sunTransmittance` and every shader see the same haze: a humid low sun is dim
  and red, far terrain and far clouds go into the haze.
* **THE MULTIPLE-SCATTERING LIGHT** (`atmosMultiScatter(h)`, "Multiple scattering" = its gain, 1 = physical): the
  radiance the air and the haze get from the rest of the sky and from the lit ground - every order past the first. A
  march adds it x the scattering at each sample: no phase (isotropic), no light transmittance. **It is what makes a
  thick haze a bright, white band:** with the sun's own light alone the horizon away from the sun gets DARKER the more
  haze there is (the extinction wins). At "Humidity" 0.7 it is about 60 % of the horizon's light, at 0.9 about 90 %.
  * **Computed on the CPU** (`RendererState::buildUboSky`, `skyMultiScatter`; after Hillaire 2020): the once-scattered
    light gathered over 32 directions x 12 steps of the same march, plus a diffuse ground (albedo 0.15), x
    `1 / (1 - the share that scatters again)`. Per unit light it depends only on the atmosphere and the light's
    height: the part for the light from straight up is computed when an atmosphere tweak changes, the sun's part again
    when the sun has moved ~0.02 degrees (~0.1 ms then; within 4 % of 512 directions x 64 steps).
  * **Two altitudes** (`ATMOS_MS_LOW` 500 m, `ATMOS_MS_HIGH` 8000 m, mirrored in `c_skyMultiScatterHeights`), linear
    between: `u_sky_multiScatterLow` / `High` (live). Each is ALREADY a radiance: x the sun (colour, eclipse) + x the
    Sky Radiance, x "Scatter Boost" x "Multiple scattering". **Add it ONCE per ray, with no light colour**:
    `atmosMarch`'s `multiScatter` argument is true for ONE of a ray's two marches. **The sky and the mirror layer put
    it on the SKY-LIGHT march** (4 steps, unsaturated, so it runs with a Sky Radiance of 0 too): on their saturated sun
    march it cost the sky FS 56 -> 64 registers. `skyRadiance` (no saturation) has it on the sun march.
  * **The LUT and the clouds add it in their own marches:** the LUT per substep; `cloudAerialScatter` with no step
    work - from the sky light's sums, the upward transmittance divided out at the segment's middle (within ~8 % of the
    integral at "Humidity" 0.7; see "Aerial perspective in front of the cloud" in [Clouds.md](Clouds.md)).
  * The LOW value's sun part takes the clouds' mean shadow (`mix(1, 0.3, coverage)`, x "Shadows/Strength"): the haze
    lies under them. The high one does not.
  * **It does not take "Saturation"** (only the sun's once-scattered light does, in the sky, the mirror layer and the
    LUT): it is the haze's white.
* **The sky-radiance light has NO forward lobe in the haze** (`ATMOS_HAZE_DIFFUSE` in every march): it stands for a
  diffuse glow, and with the sun's lobe a thick haze drew a bright spot round the zenith at night.
* **"Scatter Boost"** x all the scattered light (the multiple-scattering light too), not the transmittance; 1 =
  physical.
* **The CPU mirror** (`SkyAtmosphere`, `SkyRay` in RendererUbo.cpp: the same Chapman form, `atmosTau`, the haze's
  background mix, the ozone layer's path, the ray's cases) serves `u_sunTransmittance` and the multiple-scattering
  light. **A change of the GLSL model goes there too.**
* Not covered: the humidity of the place (the terrain's climate does not drive it), a haze top that is not
  exponential (an inversion), a haze that scatters by colour (the fine particles scatter blue: `u_sky_betaMie` is one
  number), the fog-off path's air. **The clouds' air march is 8 EQUAL steps** (the midpoint rule):
  in a very thick haze ("Humidity" above ~0.85) the air in front of far clouds is under-sampled - up to ~45 % too
  dark at 0.9, and noisy with the jitter. The "Aerial fade" hides most of it (those clouds fade into the sky).

## Aerial perspective ("Fog/Aerial perspective" tweaks)

The blue haze on distant geometry: the SKY'S OWN Rayleigh + Mie + ozone atmosphere (atmosphere.inc.glsl) between the
camera and the scene. **Do not tint the height fog for it**: the fog is grey (one albedo, a scalar extinction) and sits
on the ground, so tinting it turns the near valley mist blue and still never matches the sky behind the mountains.

* **The LUT** (`aerial_lut.cs.glsl`, pass 3 of `VolumetricFogPipeline::record`, so it exists only while the fog
  records - fog on AND RT on): `AERIAL_LUT_X x Y x Z` (64 x 36 x 32, Layout.ixx) RGBA16F per frame slot over the
  CENTRE view's frustum. ONE WORKGROUP PER COLUMN, one lane per slice (`AERIAL_SUBSTEPS` 2 steps each), and every
  slice written: rgb = the in-scatter (sun + the sky-radiance light + the multiple-scattering light), a = the view ray's MEAN transmittance (the
  apply's blend is scalar, so the scene behind dims grey; the blue is all in-scatter). Slice z holds the value at its
  far edge, `t = maxDist * ((z + 1) / Z)^2`. **A slice is a PREFIX SUM**: every step's contribution is independent
  (the view optical depth is closed-form, `atmosRayOD`), so each lane marches its own slice and a
  `subgroupInclusiveAdd` adds the slices in front. **The workgroup IS one subgroup: `AERIAL_LUT_Z` = 32 = the warp -
  keep it there.** (One thread per column marching every step was latency-bound: too few threads on the whole GPU.)
* **Matched to sky.fs.glsl:** observer at the camera altitude along `u_skyUp`, the scatter boost, the "Sky/Atmosphere/
  Saturation" curve on the sun part (`u_sky_saturation` x (2 - sun visible): more in an eclipse; the mirror sky has
  it too), the multiple-scattering light past that curve (see "The atmosphere" above). A level ray far out goes to the horizon sky's own colour. Each step's sun is
  cloud-shadowed (`cloudSunTransmittanceBilinear`; past the far cascade, the layer's mean). No jitter: no temporal
  pass averages the LUT, and a texel covers many pixels.
* **Apply** (`vol_apply.fs.glsl` `aerialTo` / `withAerial`, binding 11): the air goes BEHIND the fog (`fog + fog.a x
  air`), on the scene and the far-tree layer. **Not on the sky** (it holds the whole ray) **and not on the clouds**
  (their march has its own `cloudAerialScatter`, with the same sun + sky-light terms; with `sameFog` the cloud folds the scene's fog BEFORE the air is
  added).
* **"Strength"** (`u_fog_aerialStrength`, default 1, min 0.05: always on) scales the air density along the VIEW ray
  only (the sun still reaches each point through the unscaled atmosphere, as in the sky). **"Max distance (km)"**
  (`u_fog_aerialMaxDistance`, default 40): the LUT's depth; past it the scene keeps the last slice.
* Not covered: the mirror rays' reflection fog, the fog-off path (the Cloud apply / Far trees apply stages carry no
  air).
