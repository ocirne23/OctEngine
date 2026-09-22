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

`setFrameView(camera, viewportRect)` publishes the frame's view and gives the spatial cull its
frustum BEFORE `beginFrame`:

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
    → GI → Volumetric fog
    → Scene opaque                         (WRITES the scene depth, then parks it read-only)
    → RTAO                                 (reads this frame's depth; NEXT frame's forward pass reads the result)
    → Force intervals → Force union march  (own render passes in the primary around cached draw secondaries, half-res, gated on the force enable; see Force)
    → Scene forward → TAA → Eye adaptation
  Composite + UI
```

> ### THERE IS NO DEPTH PREPASS / G-BUFFER
>
> Measured: the prepass cost about as much as the whole forward pass (both are geometry-bound) and
> its early-Z saved the forward pass ~10 %. It is gone, with its normal target. **`SceneColor` owns THE
> scene depth**, and every screen-space reader samples that one image: RTAO, the force march, decals,
> particles, fog apply, TAA — and, as "last frame's depth" out of the OTHER frame slot, the forward
> pass's AO reprojection, the AO temporal pass and the particle collision.
>
> * **The depth has exactly TWO layouts:** `DEPTH_STENCIL_ATTACHMENT` while the opaque stages write
>   it, and `SCENE_DEPTH_SAMPLED_LAYOUT` (= `DEPTH_STENCIL_READ_ONLY`, SceneColor.ixx) the rest of the
>   time — the layout of EVERY sampling descriptor and of the layered stages' read-only depth
>   attachment, which is why those stages may sample the depth they test against. One barrier per
>   frame and eye (`recordSceneDepthToSampled`); the first stage clears from `UNDEFINED`. A new depth
>   reader uses that layout constant and runs after "Scene opaque".
> * **Nothing reads a normal target.** Normals come from depth (`normalFromDepth`, see RTAO below).
> * **TAA's ocean flag is the scene colour's ALPHA:** `ocean.fs.glsl` writes 0, every other opaque
>   surface its material alpha (> 0), and TAA reads `alpha < 0.004` on non-sky pixels. So nothing
>   layered over the opaque scene may write alpha: `GraphicsPipelineLayout::colorWriteAlpha = false`
>   on decals, debug lines, force shells / union / upsample, particles and fog apply, and
>   `GraphicsPipeline` masks alpha on every BLENDED variant (a transparent mesh keeps the alpha of
>   the opaque surface behind it). **A new pipeline that draws into scene colour after the opaque
>   stages sets `colorWriteAlpha = false`.**
> * **The Sky variant does not write depth** (`depthWrite = false`): a sky pixel's depth stays at the
>   cleared far plane, which is how every reader tells "sky". (The prepass did this by skipping the
>   sky sphere; `MATERIAL_FLAG_SKY` and `MATERIAL_FLAG_GIZMO_UI` are gone with it.)

The scene renders as **one render-pass INSTANCE per enabled stage** (separate secondaries, explicit
attachment barriers between instances — `sceneInstanceBarrier`, RendererRecord.cpp), in two groups:

```
Scene opaque   (depth WRITTEN):            Static meshes → GI probe debug
Scene forward  (depth READ-ONLY + sampled): Decals → Debug lines → Force shells → Force union blend → Particles → Fog apply
```

`SceneColor::getStageRenderPass(first, last, depthReadOnly)`: the first instance clears, the last
hands colour to TAA, and the depth is either written or a read-only attachment. All variants are
compatible with the base pass (never begun; pipelines and secondaries are built against it) since only
load/store ops and layouts differ (**the deps are verbatim — they are part of compatibility**), and
one framebuffer serves them all. **The GI probe debug impostors write depth** (`gl_FragDepth`, they
sort among themselves), so they run in the opaque group; the AO trace and the decals then see them as
geometry (debug only). **VR** records the same split inline per eye: static meshes → barrier → AO →
one layered instance.

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
  lookup and unshaded; the flat-colour modes are shaded off the sphere normal. **The impostors
  need depth WRITES to sort among themselves** (draw order cannot: a far fine-cascade sphere would
  overdraw a near coarse one), so the stage runs in the "Scene opaque" group, before the depth turns
  read-only (see Frame order). The `giWaveUpdateInterval` function is shared with the debug view's **"Update priority"** colour mode
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
  = the irradiance volume's full-bake flag; `giVisMoments` in gi_probe.inc.glsl, shared with the debug view): w "Vis Mean Scale" (1.2)
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
  as UNCLAMPED irradiance (`giSampleCascade` returns `giEvalSHLinear` against `giIrradianceBasis(n)`)
  and are clamped once — identical to blending the SH coefficients, because everything before the
  `max(0)` is linear in them, but 3 live floats per cascade instead of 12 (it was the forward shader's
  register peak). The outermost
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
  `SKY_MAP_LAYER_*`); the forward set binds it at **20** (binding **21** is the GI irradiance volume, and
  the texture array is **22**, still the set's highest binding for the variable count). Anything that
  would call `skyRadiance` or `atmosphereScatterCheap` per pixel samples the map instead.
* **THE GI IRRADIANCE VOLUME** ("GI/Irradiance volume", on by default; "GI/Volume voxels per probe" 1–2,
  default 2 — both in `g_giGrid`; they reload every shader but keep the probe buffer, see below). Per
  frame, right after the trace (inside the GI toggle), `GIProbePipeline::recordVolumeBake`
  (`gi_volume_bake.cs.glsl`) bakes every cascade into 3D images: the probe lattice refined `volumeRes`
  times per axis, stored TOROIDALLY like the probes (slot = fine coord & (dims − 1)), so the lookup is
  REPEAT addressing + hardware trilinear. A voxel = the visibility-weighted blend of its 8 probes AT THE
  VOXEL CENTRE (trilinear × backface-dead fade × Chebyshev). **16 B per voxel, 4 images per cascade**
  (`RendererVKLayout::GI_VOLUME_FORMATS`): L0 × W in B10G11R11_UFLOAT (PREMULTIPLIED by the summed weight,
  so a pixel's trilinear fetch is a weight-correct blend and a dead voxel adds nothing instead of black; a
  small float keeps the HDR range at ~1.5 % steps), the 9 L1 terms as RATIOS to L0 scaled by 1/√3 (for a
  non-negative radiance every ratio is within ±√3) in 2 × RGBA8_SNORM + the 9th in RG16F next to W (fp16,
  because L0 = fetch / W needs W's precision when it is small). Low precision is safe here because the
  volume is never accumulated — every bake writes a finished value from the fp32 probe history. The price:
  the ratios filter unweighted, so where L0 changes fast the direction is slightly off, and a dead voxel's
  zero ratio pulls its neighbours' L1 a little toward 0. `createVolume` asserts storage + linear-filter
  support for the formats; B10G11R11 / RG16 storage need `shaderStorageImageExtendedFormats`, which
  `Device` enables with every other supported core feature. `evalProbeVolumeCoverage` (gi_probe.inc.glsl — same
  cascade walk, fade, coverage and dead fall-through as `evalProbeSHCoverage`) reads 4 filtered fetches per
  cascade instead of 8 probes × 6 vec4 loads.
  **The price: the half-Lambert probe-direction weight needs the surface normal and is NOT baked, and the
  Chebyshev test runs from the voxel centre — more leaking through geometry thinner than a voxel** (the
  per-pixel normal bias of the sample point is kept).
  **EVERY probe consumer switches with the toggle** (`GI_VOLUME` define; the probe code stays for the off
  path): the lit core, the ocean and the terrain's mirror hits (static-mesh set binding 21), decals (5),
  particles (10, vertex), fog scatter (12) and fog apply (5, sky only), and the TRACE's multi-bounce lookup at
  gather hits (13 — the wave stamps are 14, its texture array moved to 15), which reads LAST frame's bake with the baked Chebyshev
  instead of `giEvalBounce`. **The sky SH (the out-of-field fallback)** is copied by the bake every frame,
  before the partial early-out, into one extra 3×1×1 RGBA16F image at the fixed slot `GI_VOLUME_SKY_IMAGE`;
  in volume mode `giEvalSkySH` `texelFetch`es it. So in volume mode no consumer reads the probe buffer —
  only the trace, the bake and the probe debug view do. (The sky SH is GPU-made by the trace, so the UBO
  could only carry it through a CPU port of the sky or a readback; the sky map could hold it too, but every
  correct consumer multiplies it by the GI strength, so being valid with GI off buys nothing.) Every
  consumer binds `GiVolumeDescriptors` (GIProbePipeline.ixx): one sampler3D array of
  `GI_VOLUME_MAX_IMAGES` (8 cascades × 4 + the sky), partially bound, written with `fillUpdates` only while
  the volume exists — so no set layout changes with the grid. Images are GENERAL for life, cleared to zero
  at creation (W = 0 = no data). Default grid: 4 × 64³ voxels, ~16 MB. The bake's barriers cover
  fragment, vertex and compute readers.
  **Every GI consumer multiplies its GI term by `u_aoParams.y`** (GI strength, 0 with GI or RT off, where
  the probes and the sky SH are stale) — decals and particles did not, and kept stale GI with GI off.
  **The bake is PARTIAL, on the trace's own decision:** on a wave's regular visit, trace lane 0 writes
  `u_frameIndex + 1` into `GI.waveStamps` (one uint per wave = per trace workgroup, trace binding 14, bake
  binding 6). A voxel re-bakes only when a wave under its 8-probe stencil carries this frame's stamp
  (`gi_waveStamp[giWaveWorkgroup(...)]`) or one of those probes is fresh (`giProbeFresh` on the stencil's two
  corners — the trace's own test; a scrolled-in voxel always has a fresh probe); every other voxel keeps its
  value. The bake never evaluates `giWaveUpdateInterval` itself, so it cannot drift from the schedule. The
  stamp is written before the trace's dead-probe skip, so dead waves re-bake more often than they trace
  (harmless). **A new trace skip condition before the stamp must keep the stamp exact; a fresh-probe change
  goes through `giProbeFresh`.** `u_giVisParams.y` = one full bake: `takeVisibilityParams` sets it after
  `createVolume` and when a Chebyshev knob moves (the baked value depends on them), held until a frame with
  RT + GI on. **The two volume tweaks reload through their own callback** (`resizeVolume` + reload): they
  never re-allocate the probe buffer, so the traced history survives a volume toggle.
  **The cascade walks test the FADE only** (`evalProbeSHCoverage` / `evalProbeVolumeCoverage`): fade > 0
  implies the stencil fits, the coarser cascade of a blend always fits, and past the outermost box
  (coverage 0) nothing is sampled, because every caller then uses only its sky fallback. `giEvalBounce`
  keeps the fit test on purpose (no fade band) but skips the outermost cascade at coverage 0.
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
* **THE FORWARD PASS READS LAST FRAME'S AO.** `sampleAOBilateral` (instanced_indirect_lit.inc.glsl)
  reprojects the fragment in clip space (`prevScreenUVClip` off `gl_FragCoord`), samples the PREVIOUS
  slot's AO image (binding 13) and weights the 2x2 taps by their world distance to the fragment,
  reconstructed from the PREVIOUS slot's depth (binding 12, `u_prevDepth`, `u_prevInvMvp`, last frame's
  jitter). No input comes from this frame, so the trace has NO ordering constraint against the forward
  pass (this is what let the depth prepass go). Static geometry is exact under camera motion; a moving
  object trails one frame, inside the temporal pass's own lag. No valid tap (disocclusion, off-screen) =
  `(0, 0, 0, 1)`: no occlusion, no bent normal.
* **THERE IS NO NORMAL CONSUMER LEFT BUT TAA's OCEAN FLAG.** RTAO, its spatial blur, the decals and the
  particle collision derive a GEOMETRIC normal from depth: `normalFromDepth` (shared.inc.glsl) — per
  axis the neighbour with the closer depth, built on `viewRelFromDepth`, the CAMERA-RELATIVE
  reconstruction (`worldPosFromDepth` rounds at the scale of the world coordinate, which is a pixel
  footprint near the camera: fine for a position, noise for a difference of neighbours). The blur's
  crease edge-stop is the tap's distance off the centre's tangent plane (sigma = one AO texel's world
  footprint), not a per-tap normal compare. **A ZERO bent normal in the AO image means "none"** (past
  the max distance, fully occluded, background): the lit shader then keeps its own shading normal, so
  the depth-derived facets never reach the GI lookup.

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
  an instance's LOD chain from `MeshLodRegistry::getGroupIdxForMesh` (instances reference the LOD0 mesh), the
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
| `Objects/` | Thin Vulkan wrappers: Device, SwapChain, Buffer, ComputePipeline / GraphicsPipeline, AccelerationStructure, SceneColor (colour + THE scene depth), ShadowMap, GpuProfiler, BakedWorldMap, Texture, Shader, **VrEyeTargets** (the two per-eye LDR composite targets, re-created with the swapchain), ... |
| `Pipeline/` | One class per pass or feature: StaticMeshGraphics, GIProbe, RTAO, TAA, VolumetricFog, EyeAdaptation, Composite, Skinning, DebugLine, Particle, Decal, ForceField, OceanSimulation, TerrainWetness, LightGrid, IndirectCull, ShadowCull, ShadowMapGraphics. **Each registers its own tweaks.** |
| `Data/` | The GPU-resident scene, carved out of the Renderer. Streaming and managers: MeshDataManager, TextureManager, TextureStreamer, MeshStreamer, StagingManager, ShaderDatabase, GpuCrashTracker (Aftermath, runtime-loaded, optional). Plus the four registries the Renderer owns and delegates to — each takes its frame-wide effects as callbacks (`onGpuIdle` before a buffer is re-created, `onInvalidate` to re-record) and knows nothing about the device or the pipelines: |
| | **`InstanceStream`** — THE per-frame push surface: the six mapped buffers `renderNode` writes into (transforms, pass masks, LOD bias, mesh instances, first instances, mesh count), one set per frame slot, plus the lock-free monotonic instance claim, the transform slot free list and the two capacity growths. **A claim past the capacity is never rolled back** — see the header. |
| | **`SharedTable<T>`** — an append-only device-local scene table with slot recycling and a CPU mirror: the mesh infos, the materials and the mesh instance offsets are three instances of it. Growth doubles, re-uploads the mirror and re-records. |
| | **`SkinnedMeshRegistry`** — the skinning jobs + their parallel skinned-BLAS builds, the bone palette store, the per-container sources, and the spawn BUNDLES (parked in place on death, reused wholesale by the next spawn of the same container). |
| | **`MeshLodRegistry`** — the LOD chains, the per-mesh chain mapping, the three GPU selection buffers and `IndexRangeFreeList`. The Renderer keeps only the RT-alias half of `addMeshLodGroup`, because aliases are AccelerationStructure state. |
| | **`FrameSubmission`** — what the entity pass ADDS rather than draws: lights, fog volumes, decals, each a lock-free bump claim whose overflow is simply dropped. It also owns the light grid's per-slot GPU scratch and **its between-frames job** (`buildLightGrid` at the kick, `applyLightGridGrowth` at the join), because the table entries are 4x the pipeline's grid capacity — the CPU claim table IS the GPU table — so both grow together. |
| | **`TerrainResources`** — what Procedural pushes in: the world params, the splat material set + its climate boxes, `TerrainTexTweaks` / `TerrainWetTweaks`, the CPU-baked height/water map every terrain-aware pass samples, and the fixed-tick **wetness state machine** (`advanceWetness`). `buildUboTerrain` stays in the Renderer and reads it. |
| | **`ParticleState`** — the GPU particle system's CPU side: the emitter slot table (KILL flag drains through the sim before a slot recycles), this frame's spawn requests, the `"Particles"` tweaks, the weather volume's rain box (latched by present for the NEXT frame's begin-frame job) and the one-shot pool reset. |
| | **`ForceFieldState`** — params, the emitter + point-query slot registries (STABLE indices across the ~2-frame readback latency), this frame's bake chunk set, the shell-cull build, and **its between-frames job** (`buildGrid` / `applyGridGrowth`), mirroring `FrameSubmission`'s. |
| | **`BindlessTextures`** — the two counts that are not the same (the fixed LAYOUT CAP baked into every pipeline layout vs the LIVE descriptor count the variable-count sets are allocated with), the streamer's pending slot writes, and the deferred free queue. The six consumers are reached through two callbacks the Renderer wires in `initBindlessTextures`. |
| | **`RayTracingScene`** — owns `AccelerationStructure` plus the CPU bookkeeping around it: the per-MeshInfo vertex counts (BLAS maxVertex) and skinned-output flags, the TLAS instance capacity, and **the one-time build watermark**. A static BLAS builds once, so `takeBuildList` scans forward from the watermark — which is exactly why a RE-STREAMED mesh and a RECYCLED slot below it must be queued explicitly, and why a skinned output region is never in the list at all. |
| `Layout.ixx` | `RendererVKLayout` — every GPU struct and `MAX_*` cap. **Must stay in sync with `shared.inc.glsl` / `ubo.inc.glsl`.** |
| `Settings.ixx` | The tweak-backed param structs — the ones the outside pushes in (`OceanParams`, `ForceFieldParams`) and the ones the Renderer registers itself and folds into the UBO (`SkyParams`, `FogParams`, `ParticleParams`, `OceanSprayParams`, LOD, RT, ...). **Deliberately does not import `:Layout`** — the one place both are visible static_asserts that `ForceFieldParams::teamColors` covers `MAX_FORCE_TEAMS`. |
| `Util/` | `VK`, `DDS`, `LightingUtils`, `glslang`, `stb_image`, `GridClaim` (the light/force hash grid's CPU side) and `SlotTable` (`RecycledSlotTable<T>` — the deferred-recycle slot table every emitter/query registry uses). |
| `Renderer.ixx` | The whole class. One interface, **four implementation units** below — they all say `module RendererVK;` and are one class, so a member may move between them freely. |
| `Renderer.cpp` | Construction and the frame loop. **`kickGridBuilds` / `joinGridBuilds`** are the between-frames window: two jobs on one counter (the light grid merge and the force compaction + grid build), each now a one-line call into the object that owns that state, with the rare exact-fit growth applied at the join. Construction: `initialize()` as five phases (`registerTweaks` → `initDeviceAndSwapchain` → `initPipelines` → `initPerFrameResources` → `initSharedBuffers`), then `waitFrameSlot` → `beginFrame` (+ its job kick/join) → `present`. |
| `RendererUbo.cpp` | **The frame UBO**: `buildFrameUbo` and the `buildUbo*` helpers that split it by subject (views, sky, sun shadow, rain occlusion, fog, ocean, force, terrain). Pure CPU math over the param blocks and the `Data/` registries — it records nothing and touches no device object. Runs wherever `beginFrame` runs, and `m_ubo` persists across frames (the view build reprojects from last frame's mvps). |
| `RendererScene.cpp` | The scene the outside owns, in two halves. **Residency**: container add/remove, `renderNode` (the push), the spawn-path entry points, the bindless descriptor upkeep, and the cross-cutting work a capacity growth needs (`onUniqueMeshCapacityGrown`). **Submission** (bottom of the file, mostly called from jobs): lights / fog volumes / decals and the emitter + query registries, each a thin delegate into the `Data/` object that owns the contract. |
| `RendererRecord.cpp` | Command-buffer recording: one `record*()` per pass, plus the two primaries (desktop / VR). **`buildSceneStages()` is THE scene stage table** — name, gate, cached secondary and per-eye inline recorder for every stage inside the scene-colour pass. `recordSceneSecondaries`, `recordPrimaryDesktop` and `recordPrimaryVR` all drive off it, so a stage is added, re-ordered or re-gated in ONE place; a null `recordInline` means desktop only (the debug overlays). |
| `OpenXRSession.ixx` | VR (`Globals::openXR`, implements `IVrSession`). |

## Debug names and labels (Nsight / RenderDoc)

`VK_EXT_debug_utils` is enabled in EVERY build when the loader offers it (`Instance::isDebugUtilsEnabled`),
not only with validation — the validation messenger stays validation-only.

* **`Globals::device.setDebugName(handle, name)`** names any Vulkan-Hpp handle (the object type comes from
  `T::objectType`). Name an object where it is created (the call needs external sync on the object); the
  string is copied, so an `oc::format` temporary is fine. A no-op without the extension.
* **KEEP NAMES SHORT.** `setDebugName` caps a name at 63 characters and keeps the TAIL (a path keeps its
  file name). Put no defines or other decoration into a name.
* **Named automatically:** shader modules (the file path), pipelines + their layouts / set layouts /
  caches (`Shader::debugName`: the file name only; a graphics pipeline takes its FRAGMENT shader, or the
  vertex shader when it has none, extra variants add `#i`), every `Allocator` image and buffer that passes
  a name, textures (their file path). Variants of one file with different defines share a name.
* **Named by the caller — the parameter is REQUIRED so the compiler finds every site:**
  `CommandBuffer::initialize(level, name)`, `DescriptorSet::initialize(layout, name, count)`,
  `ShadowMap::initialize(name, ...)`, `IndirectCommandsLayout::initialize(name, ...)`,
  `IndirectExecutionSet::initialize(pipeline, name)`. Views, samplers, framebuffers, render passes,
  fences and semaphores are named inline after their `create*`. **A new object gets a name the same way.**
* **Command labels = the `GpuProfiler` scopes.** `beginScope` / `endScope` also emit
  `vkCmdBegin/EndDebugUtilsLabelEXT` (`Device::beginDebugLabel`), BEFORE their early-outs, so the pair
  holds even when timestamps are off. A new profiled pass is therefore a Nsight marker range for free.
* **Nsight 2026.3.1 GPU Trace: turn OFF "multi-pass metrics"** in the capture settings. With it on,
  opening a capture that holds these labels crashes Nsight (heap corruption in its "Per Shader Warp
  Occupancy" code) for every labelled range that does not start at the beginning of the primary. The
  label structure is valid; the object names and the shader debug info were each ruled out as the
  trigger.
* Not named: the OpenXR session's own command pool and clear buffer (raw C API, no `:Device` import).

## Pipeline statistics (register counts)

`VK_KHR_pipeline_executable_properties` (optional, enabled when offered). **"Renderer/Log pipeline
stats"** (not Saved, off): pipelines created while it is on carry `CAPTURE_STATISTICS`, and
`Device::logPipelineStatistics` writes one tab-separated line per stage - `<debug name> <VS|FS|CS>
Register Count=.. Binary Size=.. Local Memory Size=.. ...` - to the log AND to
`Assets/Local/pipeline_stats.txt` (appended since startup; F5 re-creates the pipelines and appends a new
block). Unattended: `App.exe --quit-after 20 --tweak "Renderer/Log pipeline stats=1"`, then read the file.

* **Register Count sets the occupancy**; a non-zero **Local Memory Size** is spilled registers or
  dynamically indexed local arrays (both go through L1TEX). Integers print their low 32 bits: the NVIDIA
  driver reports Local Memory Size as 2^36 + bytes.
* Measure **RelWithDebInfo** for real numbers: Debug adds `SHADER_STATS` to the cull shaders.
* Compute and graphics pipelines (every variant) report under their debug name; ImGui's do not.

## Half floats (fp16) - measured on the RTX 4090, 2026-09-21/22

The shading of the lit, terrain and ocean fragment shaders is **fp16 math in whole blocks**; positions, UVs,
depths, ray origins / directions, ray-query and hit-attribute data stay fp32. Measure with the pipeline
stats (registers x 4 + local memory per thread); list the fp16 <-> fp32 conversions with
**`Tools/fp16conv.ps1 <shader>.glsl`** (every OpFConvert by source line - glslang never warns about an
implicit `float16_t -> float` widening). Totals (regs/local, B/thread), start -> now:
lit 96/32 (416) -> 64/32 (288), terrain 96/80 (464) -> 80/32 (352), ocean 80/48 (368) -> 80/16 (336).

* **The rules that came out of it:**
  * A half that is only STORED (math still fp32) gains nothing: it is not packed, it takes a full 32-bit
    register, and the conversions add temporaries. Only whole blocks of fp16 MATH pay.
  * A value must not be live in BOTH precisions across a peak (a ray query, PCSS, the light loop): convert
    once, before it, and keep only the half copy (e.g. `computeLitColor` takes the surface half).
  * **Fold before the peak.** The ocean top side and the terrain film are LINEAR in their traced radiances:
    `final = body * bodyWeight + mirror * mirrorWeight + C`, so the glint, SSS, turbidity, the blur's sky
    share and the foam fold into one half `C` BEFORE the traces (see both shaders).
  * **One light loop per shader** (large + cell lights in one loop): every `doLightShadowed` call inlines
    all light types plus the shadow ray query. Merging the ocean's and the film's double loops cut 36-40 KB
    of code each.
  * The scene colour target is RGBA16F, so radiance may be half where it is bounded; a light's radiance is
    not, so it is clamped to 65504 as it lands in a half accumulator.
  * The driver moves its register/spill split by +-16 B on neutral changes (e.g. the order of two
    independent blocks) - a 16 B difference needs a bisect before it means anything.
  * **Packing halves by hand does NOT work on this driver:** it folds `packFloat2x16` / `unpackFloat2x16`
    pairs away - packing the lit core's 13 surface halves across the light loop gave a BYTE-IDENTICAL
    binary. Each live half costs a full 32-bit register, so fewer live VALUES is the only lever.
  * **fp32 stays, for a certain reason, where:** positions, UVs, depths and ray data (precision); absolute
    heights (a 1 m band at 500 m altitude is 1 half step); a distance squared (overflows half past 256 m);
    E[x^2] - E[x]^2 (catastrophic cancellation - the LEAN variance); a value that underflows half's normal
    range (0.02^4, the water's base alpha^2); hash inputs (`fract` of large products); the GI cascade walk
    (measured +16 B in half).
* **The direct-light BRDF** (`punctual_lights.inc.glsl`, the `*H` copies beside the fp32 functions): N, V,
  L, H, the dots, GGX D + visibility, Fresnel and the colour factors; each light's factor widens once, at
  the multiply with its fp32 radiance. GGX D is Filament's fp16 form: `1 - NoH^2` as `|N x H|^2` (no
  cancellation near the peak) and the square after the divide (alpha^4 never forms), clamped to 65504.
  **Alpha must be >= 0.01** (the lit FS, the splat and the film clamp it).
* **The lit core:** `computeLitColor(worldPos, V, f16vec3 N, f16vec3 albedo, float16_t alpha, metalness,
  AO)`, a half accumulator, half AO / bent normal; the light loop puts each light's result into half BEFORE
  its shadow ray (`lightShadowVisibility`), so no 32-bit light result is live across the query. `g_sunVisSurface` (one half scalar; the film rebuilds the
  surface sun radiance with `sunSurfaceRadiance()`) replaced two fp32 vec3 globals that were live across the
  whole light loop.
* **The terrain splat** (`terrain_splat.inc.glsl`) is half: samples, coverages, blend weights, normals.
  The climate match and the crag fBm hash stay fp32.
* **GI:** `GI_PROBE_HALF` (defined by the lit core and the ocean) adds the half interface
  `giIndirectOverPiH(worldPos, f16vec3 n)` and `giEvalSkySHH`. **The cascade walk inside stays fp32:** a half
  walk (half fetch results, half SH basis, half cascade blend - every combination tried) cost the lit FS
  16 B/thread against the fp32 walk converted once. The reflection fog's blend is half on `giEvalSkySHH`.
* **The ocean spectrum / FFT images are RGBA16F** (`SPECTRUM_FORMAT`): 512^2 x 9 layers streamed ~6x per
  frame, memory-bound; Ocean sim 0.216 -> 0.179 ms. The butterflies stay fp32 in shared memory.
* **Where the peaks are now:** the terrain's ground FS is 72/16 since the film moved to the TERRAIN
  OVERLAY pass (64/16 itself; it was 80/32 inside the ground's shader). The film's scene mirror ray is
  disabled (`TERRAIN_FILM_RT_MIRROR`). The ocean's seabed splat costs 16 B. The film surface (wave taps,
  slopes, Jacobians, foam) and the terrain's wetness block are half math.
* **Tried and dropped: the film's lights in the lit core's loop** (one loop, one shadow ray per light for
  both lobes; code 184 -> 145 KB): 80/64. The film surface must then be resolved BEFORE that loop and its
  values stay live across the shadow ray query; packing them did nothing (the driver folds it), and a
  variant with the film surface after the loop (lobe on the unrippled base normal) measured 80/144 - the
  scheduler's placement of the film's independent texture taps is not controllable from GLSL. Same
  register count as the film's own loop, so no occupancy gain, and the spill cost lands on EVERY terrain
  pixel while the saving is only on filmed ones. NVIDIA returns no internal representations (no SASS) through
  `VK_KHR_pipeline_executable_properties`, so bisecting is the only way to locate a peak.
* **Tried and dropped:** a compact half LightInfo (the compiler loads light fields at use); a D16 shadow map
  (geometry-bound, D32 is compressed); the film's lights before its mirror (N / V then live across the
  lights' own ray-query peak: 368 vs 352 B). **Not worth it:** the GI probe buffer (the fp32 temporal
  HISTORY - fp16 would round small blend steps away), BakedWorldMap (heights), vertex attributes.

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
* **`setTerrainSplatMaterials`** registers the textures AND each material's climate box in one call,
  once, when the background DDS bake finishes. The box (`TerrainSplatMaterial::climate`) arrives with
  temperature already in t01 but **precipitation still in mm/yr: its divisor is the live tweak
  "Terrain/V3/Precip for full humidity"**, which rides `TerrainTexTweaks::precipFullMm` (pushed every
  frame), and `buildUboTerrain` normalizes it per frame — so that tweak reaches the shader without a
  texture re-upload. Materials lay out
  `[numGround][numRock][beach?][snow?]` as ONE contiguous range (climate boxes index the same slots),
  but **that is slot order, not draw order: the shader composites ground → beach → rock → snow**, so
  rock covers the beach. **Beach and snow are OVERLAYS, not materials the climate blend can pick** —
  beach paints over the waterline whatever the climate, and snow paints over everything else, ground
  AND rock. Per material: diffuse (sRGB), normal (linear; BC5 sets `MATERIAL_FLAG_BC5_NORMAL`), ARM
  (linear; R = AO, G = roughness, B = metalness, stored in `metalRoughnessTexIdx`). The full contract is
  on the definition in `Renderer.cpp`.
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
  variant (GPU idle + shader reload).
  **The surface-water film is the TERRAIN OVERLAY pass** (`EPipelineIndex::TerrainOverlay`, variant 11):
  the terrain chunks overlapping the wetness clipmap drawn a SECOND time over the ground - the main cull
  (`instanced_indirect.cs.glsl`) emits it for `TerrainLit` instances into the mesh's otherwise unused
  TRANSPARENT sequence (after every opaque draw, same instance list, count raised with `atomicMax` so every
  overlapping instance is inside the drawn range). The terrain VS/FS compiled with `TERRAIN_OVERLAY_PASS`:
  the VS lifts the rasterized surface 2 cm along its normal (no depth fight; the FS still shades the ground
  point), depth test on / write off, `early_fragment_tests`, uncovered pixels discard, and the FS composites
  DUAL-SOURCE (`PipelineVariant::dualSourceBlend`: out = K + ground * factor per channel, the film being
  linear in the ground colour) - it never reads the scene colour. Both passes read the same mask
  (`terrainWetMask`). The overlay resolves its own sun visibility: ONE hard shadow tap (one ray with the RT
  sun), as the ocean. Why a separate pass: the film set the terrain FS's register allocation for every
  pixel (80/32 regs/local -> ground 72/16, overlay 64/16), and Nsight showed the Static meshes pixel warps
  launch-stalled on register allocation ~75% of the range. **The overlay is meant to grow** (snow,
  deformation, other surface layers): any layer that is a K + factor composite fits as is; deformation
  would need displaced geometry and depth writes, which it does not do.
  **The film reflects the SKY only** (the baked mirror sky, fogged). Its scene mirror ray (`terrainFilmMirror`, the ocean's ray under
  the ocean's gates) is DISABLED behind `TERRAIN_FILM_RT_MIRROR` (a `constexpr false` in
  StaticMeshGraphicsPipeline): a ray query sets the terrain's register allocation for every pixel, filmed
  or not, and Nsight showed the Static meshes pixel warps launch-stalled on register allocation 72% of the
  range. **The film's base normal is the LEVEL water plane, not the ground normal** (sun, sky and lights
  match the ocean on sloped ground); it eases to the ground normal as the view flattens onto
  the plane (`V.y` 0.35 -> 0.05), because a hard switch drew a line at eye height. The overlay runs the
  film in two stages (`terrainFilmSurface`, `terrainFilmShade` -> K + ground factor) with its OWN grid-light
  walk (specular only, full light shapes); the ground's wet gloss is off under it. (Merging the film's lights
  into the lit core's loop - one shadow ray per light for both - was tried and dropped, see Half floats.) **Inland**
  (above the swash run-up, where the FFT depth weight is 0) the film takes wind ripples: the finest
  cascade keeps a slope weight of its own there ("Terrain/Wetness/Wind ripple strength",
  `u_terrainWetParams7.z`) - the film loop's existing taps, no extra fetch. The film also carries the
  ocean's sub-band DETAIL slope (the `oceanDetailSlope` math inlined, "Ocean/Shading/Detail *", one
  extra tap), its crest foam (`oceanInstantFoam` inlined, shore-gated) and its own normal knob
  ("Surface water normal scale", `u_terrainWetParams7.w`, x the ocean's normal strength); no foam inland, and the amplitude follows the ocean wind through the spectrum.
  **Every scene ray is gated on its VISIBLE weight (2%)**, resolved before it is traced: the ocean's
  refraction on `(1 - F)(1 - milk)(1 - foam)`, its mirror on `F (1 - reflBlur)(1 - foam)`, the film's
  shadowed light walk on coverage x `(1 - foam)`, the
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
* **The Ocean variant is BACK-FACE CULLED.** The clipmap carries every triangle in both windings (see
  Sectors in [`Code/Procedural/CONTEXT.md`](../Procedural/CONTEXT.md)), so the underside draws from
  under water and the scene depth holds the nearest face from either side. Do not switch it to cull
  none: every wave crossing along an underwater ray would be rasterized, and shaded until a nearer
  one lands (there is no prepass early-Z to reject them). The underside shading (Snell's window, TIR, the "Underside transmission" tweak in
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
* **Compile in ONE `parse()` with the includer** (`Shader::GLSLtoSPV`), never `preprocess()` + a
  re-parse of its output. The SPIR-V carries full NonSemantic debug info (the `-gVS` equivalent), and
  one pass embeds the original text of the file and of every include at their true lines. The old
  re-parse embedded the flattened text, and its `#line N 0` include epilogues kept the last include's
  file name, so every function after an `#include` (`main` too) pointed into that include — Nsight
  reports that as "incorrect function definition locations ... glslangValidator 15.0.0".
  `Local/Shaders/*.spv` is the dump of each compile (`spirv-dis` it to check the debug info).
* **Per-pixel work hoisted to the UBO / per pixel:** `u_sunTransmittance` is the CPU mirror of
  `atmosTransmittanceToLight(0, sun, up)` (`buildUboSky`; keep the constants in sync with
  atmosphere.inc.glsl) — the lit sun term never runs the Chapman function per pixel; `u_sunDirection` is
  normalized on the CPU, so shaders use it raw; the PCSS Vogel disk rotates its compile-time tap angles
  by ONE per-pixel `(cos, sin)` (`rotSC`) instead of a sincos per tap (keep the tap loops fully
  unrolled: 4-tap batches with a run-time-indexed offset table measured 93 registers against 83); `u_cascadeSunSizeTexels` holds
  the per-cascade PCSS penumbra scale (`buildUboSunShadow`, from the matrices' bottom-row scalars);
  divide-by-PI is `* INV_PI`; the AO bilateral weights use `exp2` of the squared distance.
* **The GI probe buffer is `vec4[]`** (every includer declares it so; layout table at the top of
  gi_probe.inc.glsl): a probe is 6 wide loads — SH in 3, depth moments in 2, backface + relocation
  offset packed in 1 — not 24 scalar ones; the write side packs the same way. The Chebyshev weight
  exponent is the fixed `GI_VIS_CHEB_POWER` define (2, gi_probe.inc.glsl; no tweak), unrolled to
  multiplies.
* **`shared.inc.glsl` / `ubo.inc.glsl` structs must stay in sync with `Private/Layout.ixx`.**
* **Only `EPipelineIndex::LitMasked` discards** (the lit fragment compiled with `ALPHA_MASK`). A
  `discard` anywhere in a pipeline's shader costs it early depth WRITES, and with no prepass that is
  every opaque pixel's overdraw. `ObjectContainer` sends a Mask material that resolved to `LitOpaque`
  (after the `.oc` overrides) to `LitMasked`. Keep `discard` out of `LitOpaque`.
* **The RT shadow toggles are BAKED into the lit-core fragments** (lit, masked, transparent, terrain):
  `LIT_RT_SUN_SHADOW` / `LIT_RT_LIGHT_SHADOWS`, always defined 0/1 from `RTParams::effectiveSunShadow()`
  / `effectiveLightShadows()` (master AND toggle — the same expression as the UBO flags). The lit
  core `#error`s without them. Why: register allocation covers every compiled path, so the PCSS
  search and the RT sun loop in one shader cost the occupancy of the larger one. "RT/Enable RT",
  "RT Sun" and "RT Lights" reload the pipeline; the setter runs BEFORE the idle test, because a Saved
  value fires at registration and `initialize()` must build with it. The ocean, fog and GI still read
  the uniforms. `computeLitColor` evaluates the sun FIRST, so no AO/GI value is live across the shadow
  search.
* **The default static-mesh VS packs its interpolants into 4 locations** (was 6): `posU` (xyz + uv.x),
  `normalV` (xyz + uv.y), `tangent` (xyz + bitangent sign), flat `meshIdxMaterialIdx` at 3. The
  fragment shader rebuilds the bitangent. Every FS paired with that VS (lit, unlit, the gizmos, sky)
  uses this layout; the terrain and ocean VS have their own.
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
    same counter; the claim table + touch array live in `Util/GridClaim.ixx`, shared by both).
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
