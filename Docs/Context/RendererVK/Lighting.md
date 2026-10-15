# Lighting, GI and shadows

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## The scene focus

`Renderer::setSceneFocus(worldPos)` / `clearSceneFocus()` set the world point every DISTANCE-BASED
QUALITY FALLOFF measures from; it reaches the shaders as `u_sceneFocus` (the camera position when
cleared). The game sets its PLAYER every windowed frame, so the top-down camera hanging in empty sky shapes none of
these. **"Editor/Freeze scene focus"** holds the focus it had when switched on (captured in `beginFrame` next to
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
  (sample, trace and debug sides alike), the trace's previous-window freshness test uses last frame's focus
  (`m_giPrevFocusPos`, advanced in `buildUbo` only while GI traces, published as `u_gi_prevFocus`), and the TLAS
  instance range bound ("RT/TLAS Range", `u_rt_giTlasRange`) is measured from `u_sceneFocus` too, so the ray-traced
  set is the geometry around the player.
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
* **Sky visibility** (`GI_SUN_V4.y`): the cosine-weighted share of the upper hemisphere (about `u_skyUp`) whose rays
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
  locations 0 / 1 as vec4s (a shader without uv declares vec3 there); the shadow pass binds `normalV.w` alone at
  location 3.
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
  irradiance volume, **22** the cloud shadow map, and the texture array is **23**, still the set's highest binding for
  the variable count). **Anything that would call `skyRadiance` or `atmosphereScatterCheap` per pixel samples the map
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
    and a dead voxel's zero ratio pulls its neighbours' L1 a little toward 0. Default grid: 4 × 64³ voxels, ~21 MB.
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
    bake: `takeVisibilityParams` sets it after `createVolume` and when a Chebyshev knob moves (the baked value depends
    on them), held until a frame with RT + GI on.
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

## Long-range sun shadows

"Shadows/Terrain march start (m) / bias (m) / spread". Past the PCSS "Max distance (m)" and "RT/TLAS Range", distant
pixels cone-march the baked terrain height cascades (`terrainSunVisibility` in `terrain_height.inc.glsl`) and take
`min` with the shadow term.

**The march fades in inside cascade range, so the handover is seamless.** Steps double from "Terrain march bias",
which is also the self-shadow bias. **Deterministic — no jitter, no temporal integration.**

## Sun shadow draw cost

The cascades are ONE MULTIVIEW pass over ONE caster list (the cull packs each caster's cascade mask; the VS makes the
triangles of the other cascades degenerate). The pass is nearly all TERRAIN, and it is VERTEX-bound, not
raster-bound: with every terrain triangle made degenerate it still cost ~70% of its time (sandbox, 1440p).

* **Tried and REVERTED: one draw list + one single-view pass per cascade.** Slower (the shadow draw and the cull both
  went up): on this hardware the multiview views SHARE the vertex work, so per-cascade lists only multiplied it.
* No effect either: a smaller "Shadows/Caster pad (m)", dropping terrain chunks from the coarser cascades where a
  finer one covers them (the masks change, the vertex work does not), and column-strip chunk indices for better
  post-transform reuse (vertex reuse is not the limit).
* What is left: fewer terrain vertices into the pass (a coarser shadow LOD - a visual trade), or caching the static
  casters' cascades.

## Aerial perspective ("Fog/Aerial perspective" tweaks)

The blue haze on distant geometry: the SKY'S OWN Rayleigh + Mie + ozone atmosphere (atmosphere.inc.glsl) between the
camera and the scene. **Do not tint the height fog for it**: the fog is grey (one albedo, a scalar extinction) and sits
on the ground, so tinting it turns the near valley mist blue and still never matches the sky behind the mountains.

* **The LUT** (`aerial_lut.cs.glsl`, pass 3 of `VolumetricFogPipeline::record`, so it exists only while the fog
  records - fog on AND RT on): `AERIAL_LUT_X x Y x Z` (64 x 36 x 32, Layout.ixx) RGBA16F per frame slot over the
  CENTRE view's frustum. ONE WORKGROUP PER COLUMN, one lane per slice (`AERIAL_SUBSTEPS` 2 steps each), and every
  slice written: rgb = the in-scatter (sun + the sky-radiance light), a = the view ray's MEAN transmittance (the
  apply's blend is scalar, so the scene behind dims grey; the blue is all in-scatter). Slice z holds the value at its
  far edge, `t = maxDist * ((z + 1) / Z)^2`. **A slice is a PREFIX SUM**: every step's contribution is independent
  (the view optical depth is closed-form, `atmosRayOD`), so each lane marches its own slice and a
  `subgroupInclusiveAdd` adds the slices in front. **The workgroup IS one subgroup: `AERIAL_LUT_Z` = 32 = the warp -
  keep it there.** (One thread per column marching every step was latency-bound: too few threads on the whole GPU.)
* **Matched to sky.fs.glsl:** observer at the camera altitude along `u_skyUp`, the scatter boost, the eclipse
  saturation curve on the sun part. A level ray far out goes to the horizon sky's own colour. Each step's sun is
  cloud-shadowed (`cloudSunTransmittanceBilinear`; past the far cascade, the layer's mean). No jitter: no temporal
  pass averages the LUT, and a texel covers many pixels.
* **Apply** (`vol_apply.fs.glsl` `aerialTo` / `withAerial`, binding 11): the air goes BEHIND the fog (`fog + fog.a x
  air`), on the scene and the far-tree layer. **Not on the sky** (it holds the whole ray) **and not on the clouds**
  (their march has its own `cloudAerialScatter`; with `sameFog` the cloud folds the scene's fog BEFORE the air is
  added).
* **"Strength"** (`u_fog_aerialStrength`, default 1, min 0.05: always on) scales the air density along the VIEW ray
  only (the sun still reaches each point through the unscaled atmosphere, as in the sky). **"Max distance (km)"**
  (`u_fog_aerialMaxDistance`, default 40): the LUT's depth; past it the scene keeps the last slice.
* Not covered: multiple scattering (the shadowed haze is too dark, as in the sky), the mirror rays' reflection fog,
  the fog-off path (the Cloud apply / Far trees apply stages carry no air).
