# RendererVK

> Library documentation for `Code/RendererVK`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Modern Vulkan renderer. Links Animation, File and Threading (+ vulkan, glslang PRIVATE). Nsight
Aftermath is NOT linked: `Util/Aftermath.ixx` (`RendererVK:Aftermath`) `LoadLibrary`s
`GFSDK_Aftermath_Lib.x64.dll` in `GpuCrashTracker::Initialize` and holds one function pointer per
entry point. **The DLL is optional** — when it is absent, App.exe runs with no GPU crash dumps and
the ShaderDatabase stays empty (`Aftermath::loaded()` gates every call).
**Minimum spec RTX 2000.**

* **Device Generated Commands for everything.**
* **Reversed-Z main view** (near = 1, far = 0). Shadow maps stay standard; fullscreen passes disable
  the depth test.
* `NUM_FRAMES_IN_FLIGHT` = 2.
* `Renderer::isInitialized()` is false in headless server mode — the global exists (static init_seg)
  but `initialize` never ran. **Anything that may run headless and would touch GPU or mapped state
  must gate on it.**

---

# Frame pacing

`Core.Time` owns all of it. `Time::registerTweaks()` plus
**`beginFrame(windowFocused, vr, vsync, displayRefreshHz, pumpWindow, waitFence)`** — ONE call at the
loop top does the fence wait, the frame-rate limit, the event-pump kick and the next frame's clock.

The fence goes through a function pointer main passes (`Renderer::waitFrameSlot`), **because Core
cannot see the renderer.** All the "Time" tweaks are Saved.

## The limit

Waits until the desired end — last frame start + 1/target, where target is `Max FPS` (0 = uncapped),
or `Inactive max FPS` (30) when unfocused. **Never in VR: `xrWaitFrame` owns pacing.**

It sleeps in 1 ms steps while more than `Busy-wait window (ms)` (2.5) remains — **real milliseconds
thanks to the window thread's `timeBeginPeriod(1)`** — then spins the last stretch.

## `Stable frame time` (on)

**When it waited, the frame's END is ATTRIBUTED as the desired end, not the clock after the wait.**
The OS's post-target lateness is charged to the NEXT frame as processing time, **which keeps the delta
flat instead of jittering with scheduler tails** (the CryEngine / Star Citizen "tail jitter" rule). An
already-late frame ends now.

## Uncapped + vsync

With `Max FPS` 0 and FIFO present, **every frame lands on a whole number of refresh periods BY
CONSTRUCTION — only the CPU's view of it jitters**, because the throttle lands either in the loop-top
fence or in the end-of-frame acquire.

> Loop-top intervals read like 4/8 ms around a 6 ms period, **which is why a tolerance-gated snap
> rejected most frames.**

So the start is QUANTIZED to the nearest whole number of periods (a dropped frame = 2), using
the display's reported refresh (`Window::getDisplayRefreshHz`) or the measured period EMA when the
platform reports none. **Rounding against the real clock every frame bounds the drift to half a
period.** Without vsync the intervals are arbitrary and the raw clock stands.

**A frame that rounds to ZERO periods (under half a period since the last start) keeps the raw
clock.** That is a present that did NOT throttle — an occluded window, a refresh-rate mismatch — and
snapping it forward would advance the attributed clock a whole period per frame, so it ran minutes
ahead of the wall clock; the limiter then slept the whole lead out the moment the window lost focus
(`Inactive max FPS`), which read as a deadlock. The limiter also never waits more than one period,
whatever the attributed start says.

## The pump kick

`requestPump()` fires `Input pump lead (ms)` (2.0) BEFORE the frame starts, so the window thread pumps
while main still waits.

* **CAPPED:** the end is exact, so the limiter kicks inside its own wait.
* **UNCAPPED:** the fence decides, so the kick targets **the EARLIEST plausible unblock** — the raw
  last start plus a decayed running MINIMUM of the raw interval. *Under FIFO the CPU unblocks early on
  alternate frames, and a mean-based prediction kicked too late on those.* The fence is waited
  (bounded) until a lead before that, then kicked, then waited out.

## VSync is a runtime tweak

`Time/VSync` (Saved) is registered by `Renderer::initialize` on `m_vsyncEnabled`. **Present mode FIFO
vs Immediate is swapchain creation state**, so `onChange` runs `recreateSwapchain()` (device idle +
re-init), guarded on `m_initialized` **because a saved or override value fires `onChange` at
registration, before the swapchain exists.**

`--no-vsync` is just `setOverride("Time/VSync=0")` — pinned for the run, never written back.

---

# The frame slot fence

`waitFrameSlot(timeoutNs = UINT64_MAX)` blocks until this frame slot's previous GPU submission has
retired. **It is the vsync/GPU throttle of the whole loop.**

* Returns true once signaled or already waited this frame; false only on a bounded timeout. **Timeout
  0 is a marker-free STATUS POLL** that still claims the slot when signaled. A device error recreates
  the swapchain and reports signaled. Idempotent within a frame.
* **Called FIRST THING in the loop, through `Time::beginFrame`** — before the "main loop" profiler
  scope opens and before input is sampled.
  > So the vsync/GPU stall shows as its own top-level "Fence wait" (the main-loop scope then measures
  > only real work) and **input → sim → present stays tight.** The wait used to sit inside
  > `beginFrame`, between input sampling and the sim: ~4 ms of stale input at 165 Hz.
* **NOTHING may write a slot's host-visible per-frame buffers before it** — renderNode instance
  memcpys, LOD redirects, sparse transforms. **Torn slot data was the one-frame LOD-flash bug.**
  `beginFrame` asserts it ran (and waits anyway); `present()` re-arms it when the slot advances.

---

# Begin frame as a job

`kickBeginFrameJob(camera, viewportRect)` / `joinBeginFrameJob()` — camera and rect copied into
members (the job outlives the caller's stack), ONE High `"Begin frame job"`.

* main.cpp kicks BEFORE `physics.update`, next to "Spatial cull", and joins after the physics step,
  `audio.update` and the nav publish ([main.cpp:757-782](../App/main.cpp#L757)).
* **Legal because the window is renderer-QUIESCENT**: tweak `onChange` is flushed at the top of the
  frame, force and terrain param setters run after `world.update`, and the streamers' Vulkan work
  happens in `present` on main. Also because `StagingManager` is thread-safe with every graphics-queue
  call behind Device's queue mutex.
* **`beginFrame` is the SERIAL-OWNER of the render chain — ONE call in flight.** Nothing may touch
  renderer frame state between kick and join, and its outputs — counter resets, `m_cameraPos`,
  `m_mipPixelScale`, `m_centerViewProj`, `m_sunCascadeViewProj`, the returned frustum — **are valid
  only after the join.**
* **VR:** the kick only STORES and DEFERS; `joinBeginFrameJob` then runs `beginFrame` synchronously on
  main, **because `xrWaitFrame` owns VR pacing and would pin a worker.** The OpenXR poll, beginFrame
  and head pose all live inside it.

## The cull view

`getCullView(camera, viewportRect)` gives the spatial cull its frustum BEFORE `beginFrame`:

* **Desktop** — `computeCullFrustum` builds **the exact frustum `beginFrame` will build**, because the
  camera and viewport are final by then and **TAA jitter is never baked into the mvp.** It is
  bit-identical by construction: both go through `computeCenterViewProj`, **the ONE place the centre
  view-projection is built.** It applies the viewport rect (idempotent with `beginFrame`'s own apply)
  and publishes `m_centerViewProj` early. Asserts `!VR`.
* **VR** — LAST frame's head view, since the head pose only exists after `xrWaitFrame` inside
  `beginFrame`. **One frame of cull latency, absorbed by the culling margin and near-ball slack**, and
  the first VR frame's view is invalid so culling is skipped that frame.

---

# Frame order

The **primary command buffer**, assembled in `present()`. Desktop:

```
GPU Frame
  Skinning → Ocean sim (+ its spray step: particle spawn requests) → Indirect cull → Light grid → Force compute
    → Rain occlusion cull → Rain occlusion draw  (only while a weather volume requested the map; see Particle)
    → Particle sim → Terrain wetness
    → Shadow cull → Shadow draw            (both skipped under RT sun shadow)
    → G-buffer → GI → RTAO → Volumetric fog
    → Force intervals → Force union march  (own render passes in the primary around cached draw secondaries, half-res, gated on the force enable; see Force)
    → Scene forward → TAA → Eye adaptation
  Composite + UI
```

**Inside "Scene forward"**, as separate secondaries with explicit attachment barriers between
instances ([Renderer.cpp:3519](Private/Renderer.cpp#L3519)):

```
Static meshes → Decals → GI probe debug → Debug lines
  → Force shells → Force union blend → Particles → Fog apply
```

Each enabled stage becomes **one render-pass INSTANCE**: the first clears and stores depth, middles
load/store, and the last hands colour to TAA. They are compatible with the original pass since only
load/store ops and layouts differ (**the deps are verbatim — they are part of compatibility**). A
single-stage frame uses the original pass, and **VR eye passes stay unsplit**.

**Line width.** Every pipeline that rasterizes lines — the debug-line pass (line topology) and the
wireframe variants (`eLine` polygon mode) — is created with `lineWidth = LINE_WIDTH` (3 px, a
constant in `Objects/GraphicsPipeline.cpp`). This needs the core `wideLines` device feature, enabled
in `Objects/Device.cpp`; it is NOT an extension.

> The split exists so `GpuProfiler` can time each stage: timestamps must be written **outside** render
> passes, because multiview would replicate them and a SECONDARY_COMMAND_BUFFERS subpass admits no
> other primary commands. See Profiling in [`Code/Core/CONTEXT.md`](../Core/CONTEXT.md).

## Recording vs submitting

`recordCommandBuffers()` records the CACHED SECONDARIES, **only on invalidation frames**
(`setHaveToRecordCommandBuffers`). The primary is re-recorded every frame, **so enable toggles take
effect immediately.**

* **Indirect dispatches never re-record**: emitter, query and spawn changes for force and particles,
  and the skinning dispatch (CPU-written dims per frame), so instances spawned later run without a
  re-record.
* **Bindless descriptor writes** for streamed textures happen first, before anything records against
  them — safe because `acquireNextImage` waited this slot's fence and the arrays are
  UPDATE_AFTER_BIND.
* The baked terrain-data cascades, the RTAO view and the TLAS are all UPDATE_AFTER_BIND too, rewritten
  only when this slot re-records or the underlying handle changed.

**Call `setHaveToRecordCommandBuffers()` after invalidating changes.**

---

# Features

* **Lights** culled through a world-space hash table into distance-scaled grids. `MAX_LIGHTS` is
  `USHRT_MAX - 1`.
* **GI** through scrolling clipmap radiance cascades (ray-traced probes).
* **RTAO**, **TAA**, **PCSS cascades**, **volumetric fog**, **GPU compute skinning**.
* **"RT/RT Sun" replaces the PCSS cascades entirely** — the shadow cull and draw are skipped.

## The scene focus

`Renderer::setSceneFocus(worldPos)` / `clearSceneFocus()` set the world point every DISTANCE-BASED
QUALITY FALLOFF measures from; it reaches the shaders as `u_sceneFocus` (the camera position when
cleared, so the sandbox behaves as before). The game sets its PLAYER every windowed frame, so the
top-down camera hanging in empty sky shapes none of these:

* **Sun cascades** (`computeSunCascades`). Focused: cascade c is the SPHERE of radius `splits[c + 1]`
  around the focus (nested), and `getSunCascade` (PCSS), `giSunShadow` (GI) and the debug overlay pick
  by distance to `u_sceneFocus`, so "Shadows/Max distance" and the split lambda are metres from the
  point. Cleared: the classic camera-frustum slices.
* **GI grid shape** ("GI" tweaks Cascades, Probes X/Y/Z (log2), Focus Y offset) is
  `RendererVKLayout::g_giGrid`, injected into EVERY shader compile as `GI_NUM_CASCADES`,
  `GI_PROBE_DIM_X/Y/Z` and `GI_FOCUS_Y_OFFSET` (Shader.cpp `buildLayoutPreamble`), so the toroidal
  addressing stays constant-folded. Dims are powers of two (the `lc & (DIM-1)` mask) and every dim ≥ 4
  keeps the probe count a multiple of 64 (the trace's sky workgroup). A change runs
  `GIProbePipeline::registerGridTweaks`'s callback: GPU idle → `resizeGrid()` (SH buffer re-allocated,
  clear scheduled; the consumers rebind it at their next record) → `Renderer::reloadShaders()`. A
  positive Y offset lifts the grid centre so more probes sit above the ground than below.
* **`DescriptorSetUpdateInfo` holds small-buffer vectors** (`oc::small_vector<…, 2>`,
  CommandBuffer.ixx): the per-frame passes build these as temporaries with one info each, and with
  `oc::vector` every entry was a heap allocation per record — the bulk of "Record primary"'s churn.
* **GI trace cost levers** (gi_probe_trace.cs.glsl): **THE update interval of a wave is ONE product**
  (`giWaveUpdateInterval`, gi_probe.inc.glsl): `"GI/Update Interval Mult"` (the global factor — **NOT a
  frame count on its own**) x the priority factor, floored ONCE, so it moves in single frames, and
  floored at ONE frame: **a close block's priority factor < 1 cancels the multiplier, down to every
  frame.** A wave traces every that many frames, interleaved per WORKGROUP (whole waves exit); the
  **`"GI/Temporal Alpha"` is the per-frame blend AT 60 FPS**: `getTraceParams0` compounds it over the
  WALL delta (`u_giTrace0.y` = this frame's alpha; wall, not sim, so GI converges through a pause;
  clamped so a hitch cannot replace the history), which makes convergence — and the speed at which
  the blend's noise wanders — frame-rate independent (uncorrected, 0.01 was a 1.7 s time constant at
  60 fps and 0.4 s at 240). The per-visit alpha compounds that over the interval,
  `1 - (1 - a)^interval`, capped at `GI_VISIT_ALPHA_MAX` (0.15), so wall-time convergence holds
  until one visit would dominate the history; fresh (just
  scrolled-in) probes always trace. **A wave is a compact 4x4x4 probe BLOCK** (the id -> lattice
  mapping in the trace's `main`; storage is addressed by lattice coord, so the mapping is free) —
  every per-wave exit only saves time when the whole wave exits, and a block is what is spatially
  coherent. **The blocks are WORLD-aligned (`lc >> 2`, `giWaveMin`), NOT window-relative** — the id
  enumerates toroidal SLOT space, whose 4x4x4 blocks are world blocks because every dim is a multiple
  of 4. Window-relative blocks re-partitioned every probe each time the window scrolled a cell: a
  probe being walked AWAY from could land in a block whose centre was nearer and speed UP, and the
  nearest block centre swung 0.5 .. 1.5 spacings within one cell of movement (14 .. 41 m in cascade
  3). The one block per axis that straddles the wrap seam has its halves on opposite window faces
  (both ~half a window from the focus); `giWaveMin` folds it so all 64 lanes agree. **Update priority** (`giWavePriority`, a float factor with NO bounds — the old
  "Priority Max Mult" cap is gone; the 1e6-frame ceiling in `giWaveUpdateInterval` is numeric safety
  for the uint conversion only): `(focusDist / priorityDist) ^ falloff / viewBoost`.
  **`"GI/Priority Falloff"`** is how hard the rate drops with distance: 1 = interval proportional to
  distance, 2 = to its square (the far field all but stops), 0.5 = gentle, 0 = no distance term. The
  curve pivots on `priorityDist`, so CLOSER than it a higher falloff is a FASTER rate.
  `focusDist` is measured from **`u_sceneFocus`**, not the camera (the dominant term; the cascades
  centre on it, and in first person it is the camera) **to the block CENTRE — the block radius is
  used by the frustum test only**: it is 3.6 spacings (7 m in cascade 0, 58 m in cascade 3), so
  subtracting it gave one world distance a different priority per cascade.
  `"GI/Priority Distance (m)"` = the NOMINAL-RATE distance of a block OUT of view: a block there has
  factor 1 and traces at exactly the interval multiplier, whatever the falloff.
  **`viewBoost` = `"GI/Priority Frustum Weight"` (>= 1) for a block IN `u_frustumPlanes`
  — its interval is divided by it — fading to 1 over `priorityDist` metres outside the
  frustum** (a block just off screen keeps most of it). The weight acts on the VISIBLE blocks on
  purpose: it is what the debug colours can show, and it is the big lever in first person, where 80%
  of the near cascades is out of view. Defaults (10 m / falloff 3 / weight 5.0, interval mult 16, 31
  rays, temporal alpha 0.025) are a STEEP curve that spends the rays near the focus: in view the
  interval is `16 x (d / 10)^3 / 5` frames — every frame within ~8.5 m, 3 at 10 m, 25 at 20 m, 400
  at 50 m; out of view, 5x that. Past the alpha cap the far tiers converge slower instead of flickering; to pay for
  that, **a fresh probe's replace
  visit traces `GI_FRESH_RAY_MULT` (4) x the rays** — its one snapshot is the whole history until the
  next, now rarer, visit. The three values ride the UBO's former GI pad floats. The function
  lives in gi_probe.inc.glsl. **The probe debug view draws SPHERE IMPOSTORS in every mode** (a
  camera-facing quad, 6 verts; the fragment shader intersects the sphere and writes the hit's
  `gl_FragDepth`), so both debug bindings carry the fragment stage. The irradiance mode evaluates the
  SH per pixel along the true normal, scaled by "GI/Strength" (`u_aoParams.y`) like the scene's
  lookup and unshaded; the flat-colour modes are shaded off the sphere normal. **While the debug view
  is on, depth-prepass reuse is forced OFF:** reuse binds the scene depth READ-ONLY, and the
  impostors need depth WRITES to sort among themselves (draw order cannot: a far fine-cascade sphere
  would overdraw a near coarse one). `m_depthPrepassReuse` is the EFFECTIVE state every consumer
  reads = the "Spatial/Depth prepass reuse" tweak (`m_depthPrepassReuseWanted`) AND debug off;
  `applyDepthPrepassReuse()` moves it next to `checkFrameCapacities` at the top of `beginFrame` (GPU
  idle, swap the scene pipelines' depth write, re-record), so the P key and the tweaks only flip
  flags. **GizmoUI under reuse:** the forward variant cannot write its near depth, so the G-buffer
  prepass VS stamps the SAME near depth for `MATERIAL_FLAG_GIZMO_UI` materials (set from the `.oc`
  `PipelineIdx GizmoUI` override; the expression must stay identical to `FORCE_NEAR_DEPTH` in
  instanced_indirect.vs.glsl) — otherwise geometry drawn after the gizmo covers it. The function is shared with the debug view's **"Update priority"** colour mode
  ("GI/Debug probe colour", key O cycles): it shows the wave's ACTUAL interval in frames: MAGENTA = every frame (the maximum rate; a hue the
  ramp never makes — white was ambiguous, the ramp's yellow can clip to it through exposure/bloom),
  then a LOG ramp (each doubling an equal step) blue (2 frames) -> green (~22) -> yellow (~76)
  -> red (256 or more), dead probes dimmed. The **"Relocation /
  backface"** mode shows the misc vec4: red = the lookup's backface-dead fade, blue = the relocation
  offset over its clamp, yellow = escaped on its last visit (the stored fraction pinned to exactly
  DEAD_MAX) — a probe that keeps returning to yellow is re-escaping, and jumps at its visit rate.
  The **"Visibility"** mode (4) is per pixel like irradiance: for the sphere normal n it rebuilds what
  `giSampleCascade`'s Chebyshev test sees for a surface in direction n from the probe (same mean
  scale, cap and variance floor): grey = mean distance over the cap (black = occluder at the probe,
  white = open), orange tint (multiplied, so black stays black) = deviation above the variance floor
  (edge softness, linear to cap / 2), magenta = no depth data yet,
  dead probes dimmed. **The Chebyshev test has THREE knobs, one job each** (`u_giVisParams`, y
  unused; `giVisMoments` in gi_probe.inc.glsl, shared with the debug view): w "Vis Mean Scale" (1.2)
  moves the occlusion THRESHOLD — it scales the DEPTH, so the second moment scales by k² and the
  variance stays consistent (**the old code scaled the mean only: `mean2 - (k·mean)²` was negative
  nearly everywhere, the variance was always the floor and the stored second moment did nothing**);
  a wall at m reads as k·m, so (k - 1)·m behind it leaks. x "Vis Variance Floor" (0.35 spacings)
  is the MINIMUM edge softness (covers the L1 mean's 0.15..0.4-spacing error toward a wall; under
  ~0.25 the ray jitter moves the edge); the measured variance widens it sideways past a wall, where
  the L1 mean is 1.4..1.75x too short. z "Vis Weight Floor" (0.01) is the leak level and the
  all-occluded fallback. **Removed as redundant:** the "Vis Cheb Power" tweak (fixed define, 2:
  near the threshold the weight is ~ 1 - p(Δ/σ)², so the power only rescales the floor, and the
  weight floor cuts the tail it shapes) and the additive "Vis Mean Bias" (same effect as the scale
  or the floor). **"No depth data" is tested on the DC coefficient (`d2sh.x`), never on the reconstructed
  mean2** — that rings to <= 0 toward a close wall, and the old test switched the occlusion off there.
  **Temporal stability** (the blend is an average over the last ~1/alpha visits, so per-visit noise
  reads as a slow DRIFT, and ray count only buys sqrt(N)): the per-visit shift of the ray lattice is
  an **R2 sequence over the wave's visit number** (integer fixed point; the per-probe hash only
  decorrelates neighbours), so the blended shifts are stratified instead of white; and a **step
  limiter** on the irradiance blend — a visit brighter than `(1 + GI_STEP_LIMIT_REL)` x the stored DC
  luminance (only brightening can be an outlier) has its step shrunk to the bound's, never below
  `GI_STEP_LIMIT_MIN` of the asked step, which bounds the switch-on lag. Boosted visits (relocation,
  just escaped) skip it, and a CLEARED (all-zero) slot is replaced like a fresh one.
  **Dead-probe skipping:** a probe whose stored backface fraction is past
  `GI_BACKFACE_DEAD_MAX` (under the terrain, inside a wall — the lookup rejects it anyway) traces only
  every `GI_DEAD_INTERVAL` (8) regular visits, enough for the escape relocation and the wake-up; an
  escape visit pins the stored fraction to exactly DEAD_MAX so the next visit is not skipped.
  **Covered waves (hollow cascades):** the priority distance is in CELLS, so the centre of every
  coarse cascade — the 1/8 that lies under the next finer window, which no lookup reads — had that
  cascade's highest rate. `giWaveCovered` (gi_probe.inc.glsl) is true when the block plus one spacing
  of trilinear support sits inside the finer cascade's fade == 1 box (minus half a finer cell for
  the normal-bias difference); `giWaveUpdateInterval` then multiplies by `GI_COVERED_INTERVAL` (16).
  **The cross-cascade fade is `giCascadeFade`:** continuous in the sample point (the old one came from
  the integer cell index — a staircase, two steps over the band), measured from the UNSNAPPED focus
  as a box of `DIM/2 - 2` cells (the old band was tied to the snapped window and jumped a whole cell
  along the seam on every scroll; the snapped window always holds that box, so fade > 0 implies the
  stencil fits), smoothstepped. Past the box a finer cascade is NOT sampled. The two cascades blend
  as SH COEFFICIENTS (`giSampleCascade` returns them) and are evaluated once. The outermost
  cascade's coverage fade (shading and `giEvalBounce`) uses the same function with a 0.2 band.
  **Not a skip:** the probes stay warm for the all-dead fall-through and for the moment the finer
  window scrolls off them. The test reads `GI_CASCADE_FADE_BAND` (0.05 of the narrowest dim, shared
  with `evalProbeSHCoverage`) — **widen the band and fewer blocks are covered; the two cannot drift
  apart because both read the define.** **Direct light at a gather hit is ray-traced-shadowed for
  EVERY light:** the sun through `g_sunShadowOverride`, the grid lights through
  `giLightIrradianceShadowed` (lighting.inc.glsl, compiled in by the trace's `GI_LIGHT_RT_SHADOWS`;
  one ray to the light's centre, only past `GI_LIGHT_SHADOW_MIN`, and only while `u_rtLightShadows`
  is on). Unshadowed, a lamp lit every hit in its range through walls — a leak the probe visibility
  test cannot see. The trace includes rt_shadow.inc.glsl BEFORE lighting.inc.glsl for this. **Miss
  rays AND the virtual sky probe sample THE SKY MAP** instead of marching the
  atmosphere, so the out-of-field fallback matches the misses by construction. **Gather hits use
  `giEvalBounce`** (gi_probe.inc.glsl, write side): the cheap multi-bounce lookup — no Chebyshev, no
  cross-cascade fade — because the result is temporally blended. Its walk starts at the FINEST
  cascade, like the shading lookup: starting at the tracing probe's own cascade read the covered
  coarse probes, which are now nearly stale. The probe buffer is **NOT `coherent`** in the trace (each invocation writes only its own
  probe; stale cross-probe reads are by design). The light and force grids have no GPU insert pass
  any more (CPU-built, see the light grid section). **TLAS exclusions are INACTIVE
  instances** (reference 0, `gi_tlas_instances.cs.glsl`), which the build skips entirely, not
  mask-0 nodes.
* **THE GI RECORD IS SPLIT IN TWO.** `recordGlobalIllumPrep` is the PER-FRAME secondary — the work whose
  content changes frame to frame: one-shot static BLAS builds, compaction copies, the skinned BLAS
  rebuild, the one-time probe clear — and is empty on most frames. `recordGlobalIllum` is a CACHED
  secondary (recorded with the scene secondaries): the sky map, the TLAS-instance write, the TLAS build
  and the probe trace. **Everything per-frame in it rides the UBO** — `u_giTlasNumInstances`,
  `u_giTrace0/1` (the trace tweaks, last frame's focus, the TLAS range), `u_frameIndex`, `u_sceneFocus`
  — so neither shader has push constants. The instance dispatch and the TLAS build cover the instance
  buffer's whole CAPACITY (`m_maxGiTlasInstances`); the shader writes every slot past the live count
  INACTIVE. **`u_giTlasNumInstances` is patched in `present()`, not written by the beginFrame UBO
  build** — that build runs right after `m_meshInstanceCounter = 0`, so a count taken there is always 0:
  every slot inactive, an empty TLAS, and no ray hits anywhere (RT shadows, RTAO, GI, ocean rays) with
  no validation error. `AccelerationStructure::ensureTlasCapacity` (CPU, at the top of `recordCommandBuffers`)
  sizes each slot's TLAS to that capacity and invalidates on a handle change, so the GI/RTAO/fog
  secondaries and the forward set's TLAS descriptor all re-record together. Invalidation also comes
  from the instance-capacity growth and from the `RT/Enable RT` + `GI/Enable GI` tweaks (baked in).
  The trace set's texture array is filled at set allocation (`fillTextureDescriptors`) and kept current
  per slot by the streamer's pending-write path — the record never rewrites it.
* **THE SKY MAP** (`gi_sky_map.cs.glsl`, owned by `GIProbePipeline`, `recordSkyMap` at the top of
  the cached `recordGlobalIllum` — ahead of the RT toggle — with its own read→write→read barriers):
  a 256×128 RGBA16F lat-long 2-layer array, GENERAL for life. Layer 0 = `skyRadiance` (GI miss rays,
  the forward pass's per-frame-constant `skyRadiance(up)` ambient), layer 1 = `mirrorSkyRadiance`
  (atmosphere.inc.glsl: the ocean's and the terrain wet film's reflection-ray sky, 12-step march +
  saturation). Mapping + layer ids live in atmosphere.inc.glsl (`skyMapUV` / `skyMapDir`,
  `SKY_MAP_LAYER_*`); the forward set binds it at **20** (the texture array moved to **21**, still the
  set's highest binding for the variable count). Anything that would call `skyRadiance` or
  `atmosphereScatterCheap` per pixel samples the map instead.
* **"Record GI" allocates nothing per frame.** `GIProbePipeline` keeps its `DescriptorSetUpdateInfo`
  lists as members (`buildUpdateScratch`, handles patched per record), and
  `AccelerationStructure::recordBuildSkinnedBlas` refills member build arrays. Keep it that way: a
  per-frame `oc::vector` temporary in that scope shows up as memory churn in the profiler.
* **GI clipmap + TLAS range.** `giCascadeOrigin(c, u_sceneFocus.xyz)` centres every probe cascade on the
  focus (sample, trace and debug sides alike), the trace's previous-window freshness test uses last
  frame's focus (`m_giPrevFocusPos`, advanced in `buildUbo` only while GI traces, published as
  `u_giTrace1.xyz`), and the TLAS instance range bound (`RT/TLAS Range`, `u_giTrace1.w`) is measured
  from `u_sceneFocus` too, so the ray-traced set is the geometry around the player.
* **RTAO** "Fade Start" / "Max Distance": the fade and the trace early-out in `rtao.cs.glsl`, and the
  forward pass's upsample-skip gate, all measure from `u_sceneFocus`. The ray-origin distance bias
  stays on the CAMERA distance — it compensates a depth-reconstruction error that lies along the view
  ray, and so do the upsample's depth weights.

**Anything new that fades or early-outs by distance measures from `u_sceneFocus`, never `u_viewPos`**
(view-ray geometry is the exception).

`shadowParams()` / `setShadowParams()` expose the same `ShadowParams` block the "Shadows" tweaks
edit, so a mode can install a preset that stays live in the panel (the game's match ctor/dtor).

## Long-range sun shadows

"Shadows/Terrain march *". Past the PCSS `Max distance` and `RT/TLAS Range`, distant pixels
cone-march the baked terrain height cascades (`terrainSunVisibility` in `terrain_height.inc.glsl`) and
take `min` with the shadow term.

**The march fades in inside cascade range, so the handover is seamless.** Steps double from
`Terrain march bias`, which is also the self-shadow bias. **Deterministic — no jitter, no temporal
integration.**

## TAA off bypasses the pass completely

The secondary CB is not recorded, the dispatch and its GPU scope are skipped, and eye adaptation and
composite sample this frame's SceneColor directly (`resolvedLayout` SHADER_READ_ONLY instead of TAA's
GENERAL storage image; **the scene-colour barrier gains the fragment stage**, since the composite then
reads it).

> It used to only zero the feedback, so a disabled TAA still ran a full-screen resolve that copied —
> **a "TAA" scope in the profiler for a pass doing nothing.**

The tweak carries `onReRecord`, so toggling rebuilds the descriptors either way.

## Colour

Everything upstream of composite is **linear HDR**. `EyeAdaptationPipeline` (log-luminance histogram
auto-exposure) feeds `CompositePipeline` (HDR → display into the swapchain before ImGui; tonemap
off / Reinhard / ACES / AgX).

---

# Per-frame instance flow

**`renderNode(node, passMask)` is LOCK-FREE**, callable from any job between `beginFrame` and
`present`: atomic cursor claims, monotonic.

> On an overflow frame the tail that no longer fits **drops for one frame**, `m_instanceOverflowStart`
> CAS-min records it, `present()` clamps, and capacity regrows at the next `beginFrame`.
> **NEVER roll a bump cursor back.**

`addLightInfo` / `addFogVolume` / `addPointLight` / `addAreaLight` / `addSpotLight` / `addDecal` are
lock-free too. **`addDebugLine` is `PerWorker`-staged**; `present()` hands the `PerWorker` to
`DebugLinePipeline::upload`, which copies each worker list straight into the slot's mapped vertex
buffer (one memcpy per list, no merged CPU copy) and clears it.

**Pass masks** `PASS_MAIN` / `PASS_SHADOW` / `PASS_GI` (Layout.ixx — the same bits Spatial uses). Main
cull, shadow cull and the GI TLAS writer each early-out on their bit; TLAS also range-bounds by
`RT/TLAS Range`.

**GPU stats atomics are debug-build only:** `buildLayoutPreamble` defines `SHADER_STATS` under
`#ifndef NDEBUG`, and the main cull's per-level LOD pick counter (`out_lodStats`) is written only under
it. The buffer stays bound in every build; the readout (the UI's "picks L0-L4") reads zeros in
RelWithDebInfo / Release. Put any new per-instance stats atomic behind the same define.

**Transforms upload SPARSELY, inside `renderNode`:** write only through `RenderNode::setTransform`,
which is change-detected into the node's own dirty bits (one per frame in flight). `renderNode` copies
the transform into its slot's mapped buffer when that slot's bit is set, so there is no dirty list and
no merge in `present()`, and a node that moves while it is not pushed uploads nothing until its next
push. **A mutable `getTransform` would bypass the tracking.** One owner per node: `setTransform` and
`renderNode` of the same node must not run concurrently. `growRenderNodeCapacity` bumps
`m_renderNodeBufferGeneration`; a node with an older generation counts as all-dirty at its next push.

`IndexRangeFreeList` recycles the contiguous slot ranges freed by destroyed ObjectContainers — mesh
infos, materials, instance offsets, skinning jobs. Ranges stay sorted and coalesced, and **allocation
is BEST-FIT so small requests do not shred the large holes.**

---

# Scene objects

`ObjectContainer` wraps one loaded `ISceneData`.

* `spawnNodeForIdx()` returns a move-only RAII `RenderNode`; `spawnSkinnedNode()` the GPU-skinned
  variant, where AnimatorComponent feeds the bone palette through `allocateSkinningPalette` /
  `setSkinningPalette`.
* **`RenderNode` is ONE CACHE LINE** (`static_assert`): transform slot, skinned bundle handle, LOD
  state base, the upload-state byte, local bounds and the mesh-instance vector. Everything else the
  push needs is derived from renderer tables: per-mesh instance counts from the instances themselves,
  an instance's LOD chain from `m_meshToLodGroup[meshIdx]` (instances reference the LOD0 mesh), the
  skinning palette from the bundle (`setSkinningPalette(node, palette)`). Do not add per-node side
  vectors.
* **Destroying a RenderNode recycles without GPU sync**: transform slots are free-listed, skinned
  bundles parked in place per container, and rebased sub-node offsets shared.
* `~ObjectContainer` frees ALL renderer resources (`Renderer::removeObjectContainer`).
* The container keeps its own copy of the source `Skeleton`, **so animators can retarget against it at
  spawn.**

## Per-entity tint

`createSolidColorMaterial(color)` mints a material whose diffuse is a 1×1 solid-colour texture,
**cached per quantized colour so repeat calls are free.** Pass the index to
`RenderNode::setMaterialOverride`. Main thread.

---

# Streaming

## Texture mips (`TextureStreamer`, "Texture Streaming" tweaks)

DDS textures load **tail-only** (mips at or below `Tail max dim`, default 128 px) and stream against a
VRAM budget by camera priority (`Mip bias`, `Texel ratio`).

**Residency changes recreate the image and rewrite the slot in all 7 bindless texture arrays** —
UPDATE_AFTER_BIND, applied per frame slot after the fence wait. Disk reads run on a worker thread, and
**old images retire after `NUM_FRAMES_IN_FLIGHT`.**

Rate limits: `Max ops in flight`, `Max MB/frame`, plus `Demote hysteresis frames` and `Decay frames`
so a briefly-unseen texture is not immediately demoted. `GPU mip copies` moves the mip promotion onto
the GPU.

**Non-DDS, procedural and embedded textures are pinned at full res.**

Both streamers' `update()` allocate nothing per frame once warm: the completion queue is swapped into
a KEPT twin deque (a fresh deque per frame allocated even when empty), and the solver's grant heap,
retention order, promotion order and eviction candidates are kept member scratch.

## Mesh data (`MeshStreamer`, "Mesh Streaming" tweaks)

Cooked scenes register mesh sets — source mesh, LOD levels and `.vsc` byte ranges (see
`MeshStreamSource` in [`Code/File/CONTEXT.md`](../File/CONTEXT.md)). Vertex and index mega-buffers are
`BitRangeAllocator` free lists in `MeshDataManager`.

Over `Mesh budget (MB)`, least-recently-referenced sets unseen for `Mesh cold frames` **evict**:

* MeshInfos get `indexCount 0`, **so DGC, shadow and TLAS become no-ops with NO re-record**, and
  static BLASes persist as geometry snapshots.
* Ranges free after `NUM_FRAMES_IN_FLIGHT`.
* Re-reference re-streams from the `.vsc` on a worker (`Mesh max ops`, `Mesh max MB/frame`), and **the
  LOD selector renders the nearest resident level meanwhile.**

**Only cooked static meshes stream.** With Spatial culling everything visible stays referenced, so only
despawned containers and `Col_` proxies go cold.

---

# Mesh LODs ("LOD" tweaks)

`MAX_MESH_LODS` is 5.

* **`LodN_<name>` meshes and nodes form authored chains**; level > 0 is never instanced, like `Col_`.
* Static meshes without authored LODs get **meshopt-generated index-only chains, pre-baked by the scene
  cache** (`Generate LODs`, `Generated levels`, `Generated reduction`, `Min indices`).
* Skinned meshes get bind-pose-generated chains too (per-instance `MeshLodGroups`, recycled with the
  bundle). **Skinned output MeshInfos are excluded from static BLAS builds and compaction.**

## Selection is per-instance ON THE GPU

`mesh_lod.inc.glsl` in the main and shadow cull; params live through the UBO.

**Instances push LOD0 and the cull redirects the draw.** The CPU sizes every chain member's bucket to
the chain's full count, **hysteresis lives in per-instance GPU slots (benign races)**, shadow cull
selects stateless 4× / +2 coarser, and GI needs no selection at all (one chain BLAS).

**The metric is screen-space error**: the meshopt simplify error of each level, in mesh-local units,
projected to pixels against `Max error (px)`. **Authored chains carry no error data**, so those fall
back to projected diameter vs `Full-res pixels (Authored Lods)`.

Also: `Bias`, `Hysteresis`, `Force LOD` (−1 = off).

`renderNode` keeps **every level of a referenced chain warm** (`noteUse`, once per frame per chain),
and the cull falls back to the nearest resident level during a re-stream.

## RT

**A LOD chain shares ONE BLAS at `RT/BLAS LOD level`** (an alias table; the aliased RT meshIdx rides
the TLAS instance `sbtOffset` for hit-shader fetches, and materials stay per-instance). Static BLASes
**copy-compact after build** (`RT/BLAS compaction`, ~30–50 % back).

---

# Layout under `Private/`

| Directory | Contents |
|---|---|
| `Objects/` | Thin Vulkan wrappers: Device, SwapChain, Buffer, ComputePipeline / GraphicsPipeline, AccelerationStructure, GBuffer, SceneColor, ShadowMap, GpuProfiler, BakedWorldMap, Texture, Shader, ... |
| `Pipeline/` | One class per pass or feature: StaticMeshGraphics, GBuffer, GIProbe, RTAO, TAA, VolumetricFog, EyeAdaptation, Composite, Skinning, DebugLine, Particle, Decal, ForceField, OceanSimulation, TerrainWetness, LightGrid, IndirectCull, ShadowCull, ShadowMapGraphics. **Each registers its own tweaks.** |
| `Data/` | MeshDataManager, TextureManager, TextureStreamer, MeshStreamer, StagingManager, ShaderDatabase, GpuCrashTracker (Aftermath, runtime-loaded, optional). |
| `Layout.ixx` | `RendererVKLayout` — every GPU struct and `MAX_*` cap. **Must stay in sync with `shared.inc.glsl` / `ubo.inc.glsl`.** |
| `Settings.ixx` | The tweak-backed param structs the outside pushes in (`SkyParams`, `FogParams`, `OceanParams`, `ForceFieldParams`, LOD, RT, ...). **Deliberately does not import `:Layout`** — the one place both are visible static_asserts that `ForceFieldParams::teamColors` covers `MAX_FORCE_TEAMS`. |
| `Renderer.ixx` / `.cpp` | Orchestrates everything; per-pass `record*()` methods. |
| `OpenXRSession.ixx` | VR (`Globals::openXR`, implements `IVrSession`). |

## The graphics-queue mutex

**`StagingManager` is THREAD-SAFE** — one internal mutex over the ring and region lists, `upload*`
callable from jobs — and **its ring-overflow path submits to the graphics queue.**

> That is why **EVERY graphics-queue call in the engine goes through Device's queue mutex**: submits
> through `CommandBuffer::submitGraphics`, presents through `SwapChain::present`, idles through
> `Device::graphicsQueueWaitIdle()`. **Never call `getGraphicsQueue().submit()` or `waitIdle()` raw.**
> Lock order: staging mutex → queue mutex.

Known unlocked queue holders, both main-thread-only by construction: the ImGui backend (texture-upload
submits in the post-join window) and OpenXR (`xrEndFrame`; VR stays synchronous on main).

`update()` fence-waits under the staging mutex, so concurrent uploads block for that long — **keep bulk
uploads off latency-critical threads.**

## VR

Two-phase init around Vulkan device creation; stereo is a 2-layer multiview swapchain. **In VR the
editor viewport sub-rect is ignored** (VR renders full-extent).

`beginFrame(camera, viewportRect)` applies the rect — **`setViewportRect` is private, so rect changes
only happen at frame boundaries** — and builds the projection from its aspect.

---

# Terrain and ocean integration

Both push params in every frame; the renderer owns none of the tweaks.

* **`setTerrainParams(meshRadius, seaLevel, lapseRate)`.** `meshRadius` is the radius inside which
  streamed chunks are guaranteed resident — **the fence for the ocean's land cull, and CPU-side ocean
  culling must fence on the SAME value its vertex shaders do**, or it deletes water the VS would have
  drawn (`getTerrainMeshRadius()`). `lapseRate` is the other half of the temperature reconstruction:
  the map bakes the SEA-LEVEL baseline and the shaders multiply this by the height THEY shade
  (`terrainTemperatureAt`) — **ONE value for the world, so no consumer can disagree with another.**
* **`setTerrainSplatMaterials` / `setTerrainSplatClimate`** are deliberately split: **textures are heavy
  and change ~never; the climate boxes are two dozen floats and track live tweaks**, so a tweak change
  must reach the shader without dragging a texture re-upload behind it. Materials lay out
  `[numGround][numRock][beach?][snow?]`, and **beach and snow are OVERLAYS, not materials the climate
  blend can pick** — beach paints over the waterline whatever the climate, and snow paints over
  everything else, ground AND rock.
* **`setTerrainTextureParams`** carries the shaping tweaks. Notable reasoning baked into the defaults:
  the rock slope thresholds read as angles (0.30 = 45°, 0.55 = 63°) because **soil genuinely stops
  holding around 45°, which is also about the steepest the diffusion model's 30 m/px field reaches**;
  snow **sheds EARLIER than rock appears** (0.18 ≈ 35° vs 0.30 ≈ 45°), so a steepening slope loses its
  snow first and only then goes to bedrock; and the snow temperature band sits well below 0 °C because
  **mean annual temperature below freezing is NOT permanent snow — Siberia averages −10 °C and is
  forest.** The crag wander exists because V3's macro altitude is nearly flat across one mountain, so
  **the raw crag test traces an elevation contour right across a range.**
* **`setOceanParams`** — flipping `hitLighting` (`OCEAN_HIT_LIGHTS`) or `rtReflections`
  (`OCEAN_RT_REFLECTIONS`, the scene mirror ray; "Ocean/RT/Reflections") or `debugMode`
  (`OCEAN_DEBUG_MODE`, "Ocean/Debug mode"; the mode legend is at the top of `ocean.fs.glsl`) rebuilds the ocean fragment
  variant (GPU idle + shader reload). `OCEAN_RT_REFLECTIONS` also goes onto the TERRAIN fragment
  variant: the surface-water film (`terrainWaterFilm`) traces the same mirror ray under the same gates
  (`terrainFilmMirror`; a hit takes its material's diffuse texture, no splat at the hit; TERRAIN hits
  fade out with the local ground slope - a level mirror on a slope would mirror the hill it lies on).
  **The film's base normal is the LEVEL water plane, not the ground normal** (sun, sky, lights and the
  mirror ray match the ocean on sloped ground); it eases to the ground normal as the view flattens onto
  the plane (`V.y` 0.35 -> 0.05), because a hard switch drew a line at eye height. The film runs the
  ocean's grid-light walk (specular only) and the ground's wet gloss is off under it. **Inland**
  (above the swash run-up, where the FFT depth weight is 0) the film takes wind ripples: the finest
  cascade keeps a slope weight of its own there ("Terrain/Wetness/Wind ripple strength",
  `u_terrainWetParams7.z`) - the film loop's existing taps, no extra fetch. The film also carries the
  ocean's sub-band DETAIL slope (the `oceanDetailSlope` math inlined, "Ocean/Shading/Detail *", one
  extra tap), its crest foam (`oceanInstantFoam` inlined, shore-gated) and its own normal knob
  ("Surface water normal scale", `u_terrainWetParams7.w`, x the ocean's normal strength); no foam inland, and the amplitude follows the ocean wind through the spectrum.
  **Every scene ray is gated on its VISIBLE weight (2%)**, resolved before it is traced: the ocean's
  refraction on `(1 - F)(1 - milk)(1 - foam)`, its mirror on `F (1 - reflBlur)(1 - foam)`, the film's
  mirror on the same x its coverage, the film's shadowed light walk on coverage x `(1 - foam)`, the
  underside's mirror (+ its sun shadow ray) on `1 - window transmission`, and an underside pixel the
  water path has absorbed to 0.1% traces nothing.
  **The UNDERSIDE traces the scene above the water** through Snell's window (the refracted ray into the
  air; same define, "Reflection range" and cutoff as the mirror ray, gated on the window's transmission
  > 2%); a miss keeps the sky + sun glitter. Hit and sky are both fogged over the LITERAL path (`applyRayFog` /
  `applyRayFogSky`):
  the mirror rule below needs a directly visible source, which an underwater viewer does not have.
  **The underside wears the surface FOAM too**: the foam terms are resolved before the side split (the
  turbulence fetch takes screen derivatives), a foam patch is laid over the window + mirror as a backlit
  diffuse sheet (half the top side's whitewater light), and it scales both underside rays' weights.
  **Mirror rays are fogged, hits AND the reflected sky** (`reflection_fog.inc.glsl`, ocean + film).
  **THE RULE: a reflection carries the fog its SOURCE carries when seen directly** -
  `tau(camera -> what the ray shows) - tau(camera -> the water point)`, the second term being what the
  screen-space fog pass lays over the water pixel afterwards. NOT the literal path from the water point:
  height fog is densest at its base (sea level over water, with terrain follow), so a grazing mirror ray
  stays in the densest metre while the camera's ray to the same shore runs a camera height above it
  (~7x thinner at the defaults) - the literal path buried every reflection and needed a fudge factor no
  single value of which fit two fog settings. Closed form (`volAnalyticOpticalDepthLinear`), near medium,
  capped at the fog range, no fetch: terrain follow is approximated from the origin's and the hit's own
  height above sea level (half of it - a point's height overshoots the macro altitude). No regional
  climate, no noise, unshadowed; lit like the far field (HG sun + the GI sky probe).
  "Ocean/RT/Reflection fog" (`u_oceanParams8.x`): 1 = the source's fog, 0 = off.
  The mirror ray's "Reflection max rough" gate reads the roughness
  WITHOUT "Micro roughness" — that term is a constant floor, so inside the gate it switched the mirror
  off on every pixel. `setOceanWaveTrough` sizes the waterline band the fog scatter samples for the underwater fog
  boundary.
* **The Ocean variant is BACK-FACE CULLED, like the prepass.** The clipmap carries every triangle in
  both windings (see Sectors in [`Code/Procedural/CONTEXT.md`](../Procedural/CONTEXT.md)), so the
  underside draws from under water and the prepass holds the nearest face from either side. Do not
  switch it to cull none: the depth-read-only forward pass would shade every wave crossing along an
  underwater ray. The underside shading (Snell's window, TIR, the "Underside transmission" tweak in
  `u_oceanParams10.z`) lives in `ocean.fs.glsl`.
* **`setTerrainWetParams`** — the terrain WETNESS clipmap (`TerrainWetnessPipeline`,
  `terrain_wetness.cs.glsl` / `.inc.glsl`): ONE persistent R16F image, `TERRAIN_WET_RES`² texels of
  `texelSize` m, stored **toroidally around the scene focus** exactly like the GI probe clipmap (slot =
  lattice & (RES−1); the CPU packs this frame's and last frame's window origin into
  `u_terrainWetParams0`, and a texel whose coord was outside last frame's window starts dry). The pass
  runs after the particle sim: decay `exp(−dt/dryTime)` (sharpened on warm ground from the map's own
  climate), then **ground under the LIVE ocean surface is set to 1** — the same calm-depth + swash
  residual predicate the lit core uses for underwater sunlight, so the wet tongue is where the water
  was drawn, and permanently submerged seabed stays 1 until a drawdown exposes it — plus a uniform
  rain term. GENERAL for life, two layers ping/ponged (the diffusion tent reads neighbours). The
  TERRAIN fragment shader (binding 18, UPDATE_AFTER_BIND) manual-bilinears it (never across the wrap
  seam), ORs in the current wave's own footprint at mesh resolution, and scales albedo / lerps
  roughness. **Disabled = the pass is skipped and the presence flag is 0**; re-enabling parks the previous
  origin out of range so nothing stale shows. Rain from weather, particle hits and script splats are
  the planned injection sources.
  **The pass runs on a FIXED TICK** ("Terrain/Wetness/Update rate (Hz)", default 20), not per frame:
  the UBO build accumulates the sim delta and ticks when it reaches the interval, integrating the whole
  accumulated delta at once; the primary executes the pass on tick frames only (`m_terrainWetTick`).
  **Why:** the image is R16F, whose step is ~0.0005 at wetness 0.5 — a per-frame change at 165 fps is
  below half a step and ROUNDS AWAY in `imageStore`, so rain and drying stalled and wetness depended on
  the framerate. A tick writes the ping/pong layer the last tick did not (`m_terrainWetLayer`, NOT the
  frame-slot parity any more); between ticks the UBO keeps naming that layer and the last tick's window
  origin, so the reader stays consistent with the data. The origin therefore scrolls per tick.
* **The particle GPU SPAWN PATH + ocean spray.** `ParticlePipeline` keeps one shared spawn-request
  buffer (`MAX_PARTICLE_GPU_SPAWNS` × `ParticleSpawnRequestGpu`) and a request counter in its counters
  block; a producer compute pass that runs BEFORE "Particle sim" binds both (`getCountersBuffer` /
  `getSpawnRequestBuffer`) and appends through `particle_spawn.inc.glsl` (the contract is in
  Particle/CONTEXT.md). The first producer is `ocean_spray.cs.glsl`, step 6 of
  `OceanSimulationPipeline::record` (after the mip chain; the maps' final barrier includes compute):
  an `OCEAN_SPRAY_GRID`² world grid around `u_sceneFocus` ("Ocean/Spray radius"), per cell the fold
  Jacobian + crest acceleration at explicit LOD (compute has no derivatives) through the water shader's
  own `oceanInstantFoam`, land skipped through the shore data (binding 2, the fog terrain map,
  UPDATE_AFTER_BIND + refreshed per frame like the wetness pass), and a hashed dice at "Spray rate" ×
  cell area × dt × breaking. The emitter slot arrives through `setOceanSprayEmitter` (the Particle
  system's `Effects/ocean_spray.pfx` instance) in `u_oceanSpray0.x`; `UINT32_MAX` switches the step off
  in-shader, so the cached CB records once. Tweaks `Ocean/Spray rate / radius / threshold / kick /
  speed / forward offset / height offset`.
* **`setRainOcclusionVolume`** — the RAIN OCCLUSION MAP for the weather particle volumes
  (`PARTICLE_FLAG_OCCLUDE`, see Particle): ONE top-down orthographic D32 view
  (`RAIN_OCCLUSION_RESOLUTION`², a single-layer `ShadowMap` per frame slot) rendered by SECOND
  INSTANCES of the shadow cull + depth pipelines in their `RAIN_OCCLUSION` shader variant (one view,
  `u_rainOcclusionViewProj`, a plain matrix - no packed bottom row - instead of the cascades; the same
  `PASS_SHADOW` casters and alpha-mask discard). It runs right after the indirect cull and BEFORE the
  particle sim, so the sim samples THIS frame's map (binding 10, the map's non-comparison sampler, border
  1 = open sky). The particle system requests the box every frame (main thread after the begin-frame
  join, the scene-focus pattern); `present` latches it for the NEXT frame's `buildUboRainOcclusion`
  (eye `Rain occlusion pad` m above the box top, footprint padded 25 % for the one-frame lag), and the
  primary executes the pass only when the UBO it was built with says so (`u_rainOcclusionParams.x`), so
  the pass and the sim's test never disagree. Skipped entirely (no request or "Particles/Rain
  occlusion" off - **the default**) = the params are zero and the sim's shelter test is off. Tweaks
  under `Particles/*`: Rain occlusion (off by default), Rain occlusion pad, Rain occlusion bias (the
  depth below the surface that counts as sheltered).

---

# Shaders

GLSL in `Assets/Shaders/`, **compiled at runtime with glslang — shader edits need no rebuild**, and F5
calls `reloadShaders()`.

* `*.inc.glsl` are includes.
* **Per-pixel work hoisted to the UBO / per pixel:** `u_sunTransmittance` is the CPU mirror of
  `atmosTransmittanceToLight(0, sun, up)` (`buildUboSky`; keep the constants in sync with
  atmosphere.inc.glsl) — the lit sun term never runs the Chapman function per pixel; `u_sunDirection` is
  normalized on the CPU, so shaders use it raw; the PCSS Vogel disk rotates its compile-time tap angles
  by ONE per-pixel `(cos, sin)` (`rotSC`) instead of a sincos per tap; `u_cascadeSunSizeTexels` holds
  the per-cascade PCSS penumbra scale (`buildUboSunShadow`, from the matrices' bottom-row scalars);
  divide-by-PI is `* INV_PI`; the AO bilateral weights use `exp2` of the squared distance.
* **The GI probe buffer is `vec4[]`** (every includer declares it so; layout table at the top of
  gi_probe.inc.glsl): a probe is 6 wide loads — SH in 3, depth moments in 2, backface + relocation
  offset packed in 1 — not 24 scalar ones; the write side packs the same way. The Chebyshev weight
  exponent is the fixed `GI_VIS_CHEB_POWER` define (2, gi_probe.inc.glsl; no tweak), unrolled to
  multiplies.
* **`shared.inc.glsl` / `ubo.inc.glsl` structs must stay in sync with `Private/Layout.ixx`.**
* **The light grid build is split CPU / GPU, and the CPU part is NOT on the main thread**
  (`LightGridComputePipeline`):
  * **Inline in `addLightInfo`, on the adding thread (`addLight`):** the light's per-type bounds
    (point/spot sphere, spot cone sector, area front-half box, tube capsule) → its cull record;
    every 32^3 grid the box covers is claimed in a lock-free CPU hash table (the same
    `getPositionHash` as the shaders, bit for bit; bump-allocated slots, CAS on the table entry,
    a slot that loses the race for a grid stays DEAD with `cellSize 0`), the grid's distance-LOD
    cell size is picked at the claim, the grid's cell/large count is bumped (`atomic_ref`), and
    one (slot, light) touch is appended to a bounded array (one `fetch_add` per light for its
    whole block). A light with `reach > 16 m` or spanning more than **"Per-cell budget (cells)"**
    (1024) cells inside a grid is that grid's LARGE light (`MAX_LARGE_LIGHTS_PER_GRID` 14, one
    entry evaluated by every pixel of the grid). A light whose block or grid claim does not fit
    goes to an overflow list; the touch array grows for the next frame.
  * **The merge job (`build`, `Renderer::kickGridBuilds` from the App loop right after the
    force update — the frame's last light source; `present()` kicks as a fallback):** re-walks the
    overflow lights serially (reading the mapped, write-combined light buffer — a non-goal path),
    prefix-sums the per-slot counts into per-grid list ranges, data offsets and workgroup counts,
    scatters the touches (claim order, a thread race — it only matters past the per-cell cap, a
    non-goal), lays out the workgroups, and — when the capacity fits — `upload`s.
    O(grids + touches).
  * **`joinGridBuilds`** (present, before the staging update) is the ONLY main-thread part: the
    wait, plus the rare exact-fit growth. The `Demand` counts FAILED claims too, so
    `growLightGridBuffers` (GPU idle, table / data / host buffers recreated, re-record) fits an
    overflowed burst; the upload then runs on the main thread for that frame. No frame drops a
    light except an overflow light that ALSO meets an exhausted grid capacity in the re-walk.
    The force grid follows the same contract (`ForceFieldPipeline::buildGrid`, its own job on the
    same counter; the claim table + touch array live in `Pipeline/GridClaim.ixx`, shared by both).
  * **`upload`** writes the grid jobs, the workgroup list (one per 64 cells of a grid), the light
    list, the hash table (header `{numGrids, gridDataUints, tableSize}` + slots — the readers'
    probe loop is unchanged) and the indirect dispatch.
  * **THE CLAIM TABLE IS THE GPU TABLE** (light and force alike): same hash, same linear probing,
    no deletions, SAME SIZE — so the light upload is one sequential pass writing
    `claim[i] == EMPTY ? EMPTY : dataOffset[claim[i]]` (grid data sizes vary with the LOD), and
    the force upload is a plain `memcpy`: its records are fixed-size, one per slot, so the table
    keeps SLOT indices and `forceFindCell` does `slot * FORCE_CELL_UINTS`. No hashing, no probing,
    no read of the mapped memory. That is why the table entry count is tied to the claim capacity
    (`getTableEntries()` = 4 x grid capacity; the renderer's `m_lightTableEntries` follows it on
    growth; the force pipeline's claim capacity is `m_tableEntries / 4`), and why
    `GridClaim::resize` REHASHES the live slots: a growth lands between a build and its upload.
  * **A light added after the kick is missed for that frame.** New light sources go before
    `kickGridBuilds` in the App loop.
  * **GPU (`light_grid.cs.glsl`, GATHER):** one thread per cell loops its grid's candidate list
    (box test + the range sphere for point/spot) and writes the cell's count + packed ids in one
    go. No atomics, no spin, no clear: every header and cell of every grid is written. Counts hold
    the TRUE candidate count (the debug heat view shows saturation); readers clamp.
    `light_grid.inc.glsl` is read-only API now; the table wrappers need `TABLE_SIZE_NAME` +
    `GRID_TABLE_NAME`, the gather shader does without them.
  * **The distance LOD** is `LightGridParams` under "Graphics/LOD/Light grid", read by the CPU
    build every frame (no reload): `level = floor(pow(max(dist - start, 0) / step, power))`,
    `cellSize = clamp(minCell << level, minCell, maxCell)` in world units per cell. Min cell 0
    (log2) = the full GRID_SIZE cells per axis; min == max pins one resolution everywhere; the
    defaults (0 m, 16 m, 0.5, 0, 2) follow the old sqrt ramp but stop at 4 m cells.
  * The three grid constants (`GRID_SIZE`, the two caps) and the header layout are mirrored in
    `LightGridComputePipeline.cpp`: **change both.**
* **Debug overlays are `#define`-driven, NEVER uniforms** — a debug switch rebuilds its pipeline
  (GPU idle + reload, the wireframe pattern), so the release shader carries no debug branch at all.
  The three today: `SHADOW_DEBUG` and `LIGHT_GRID_DEBUG` (below, both on the lit fragments) and
  `FORCE_DENSITY_VIEW` ("Force/Debug/Density view", `ForceFieldPipeline::setDensityView` through
  `setForceFieldParams`'s rebuild branch). Keep new ones on the same pattern.
* **"Shadows/Debug mode"** (`ShadowParams::debugMode`) is BAKED as the `SHADOW_DEBUG` define into the
  lit and terrain fragment variants (`StaticMeshGraphicsPipeline::setShadowDebugMode`; the tweak callback
  in `Renderer` does the GPU-idle + reload, the wireframe pattern), so the release shader carries none of
  it. `shadowDebugOverlay` in `shadows.inc.glsl`: 1 cascade index tint, 2 the cross-fade band (white),
  3 the raw sun visibility, 4 shadow-map texel size heat (green 5 cm → red 1 m). PCSS path only.
  `~GameMatch`'s shadow-preset restore leaves this field alone for the same reason.
* **"Graphics/LOD/Light grid/Debug Mode"** (`LightGridParams::debugMode`) is BAKED as the
  `LIGHT_GRID_DEBUG` define on the same lit fragments (its own reload callback; at 0 every debug branch
  folds away) and overlays the forward pass's `computeLitColor` (`instanced_indirect_lit.inc.glsl`):
  1 light grid cells (random colour per grid), 2 per-cell light count heat (green → red at the cell
  cap, magenta = the point's grid is missing from the hash table), 3 light ranges (a step of blue
  per covering light). Not saved. The sun cascade view lives in the baked "Shadows/Debug mode" above.
* SPIR-V plus source dumps land in `Assets/Local/` for Aftermath crash analysis.
