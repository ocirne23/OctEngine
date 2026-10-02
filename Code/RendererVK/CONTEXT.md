# RendererVK

> Library documentation for `Code/RendererVK`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Modern Vulkan renderer. Links Animation, File and Threading (+ vulkan, glslang PRIVATE). Nsight
Aftermath is NOT linked: `Util/Aftermath.ixx` (`RendererVK:Aftermath`) `LoadLibrary`s
`GFSDK_Aftermath_Lib.x64.dll` in `GpuCrashTracker::Initialize` and holds one function pointer per
entry point. **The DLL is optional** — when it is absent, App.exe runs with no GPU crash dumps and
the ShaderDatabase stays empty (`Aftermath::loaded()` gates every call). **Streamline / DLSS is optional the
same way** (`Util/Streamline.ixx`, see "DLSS" below).
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
cannot see the renderer.**

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

`Time/VSync` is registered by `Renderer::initialize` on `m_vsyncEnabled`. **Present mode FIFO
vs Immediate is swapchain creation state**, so `onChange` runs `recreateSwapchain()` (device idle +
re-init), guarded on `m_initialized` **because a saved or override value fires `onChange` at
registration, before the swapchain exists.**

`--no-vsync` is just `setOverride("Time/VSync=0")` — pinned for the run.

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
  Skinning → Ocean sim (+ its spray step: particle spawn requests) → Indirect cull (+ the previous-transform copy, see "Motion vectors") → Light grid → Force compute
    → Rain occlusion cull → Rain occlusion draw  (only while a weather volume requested the map; see Particle)
    → Particle sim → Terrain wetness
    → Shadow cull → Shadow draw            (both skipped under RT sun shadow)
    → Cloud shadow                         (the Beer shadow map; only the cascades due this frame; see "Volumetric clouds")
    → Cloud sky                            (the clouds of the GI sky map, before GI bakes it)
    → GI → Volumetric fog
    → Scene opaque                         (WRITES the scene depth, then parks it read-only)
    → RTAO                                 (reads this frame's depth; NEXT frame's forward pass reads the result)
    → Cloud march                          (march + temporal, half res; reads this frame's depth; see "Volumetric clouds")
    → Far trees                            (the far-tree volume: bake when due + march, full res; see "Far-tree volume")
    → Force intervals → Force union march  (own render passes in the primary around cached draw secondaries, half-res, gated on the force enable; see Force)
    → Scene forward → TAA (+ motion blur velocity) | DLSS (mvec pass + the upscale, see "DLSS")
    → Motion blur (desktop, "Post/Motion blur": tile max only;
      the composite gathers) → Eye adaptation (+ bloom level 0)
    → Bloom (desktop, "Post/Bloom": the down/up chain; the composite mixes it in)
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
>   frame and eye (`recordSceneOpaqueToSampled`, which also moves the motion target to
>   SHADER_READ_ONLY); the first stage clears from `UNDEFINED`. A new depth reader uses that layout
>   constant and runs after "Scene opaque".
> * **The depth-writing stages have a SECOND colour attachment, the motion target** (see "Motion
>   vectors"). Their passes are a separate render-pass family (`getOpaqueRenderPass`,
>   `getOpaqueFramebuffer`; a stage variant with `depthReadOnly = false` is of it), so the layered
>   stages and their pipelines never see it. **A pipeline drawn in the opaque group sets
>   `GraphicsPipelineLayout::motionTarget`** (two blend states), and its secondary begins with
>   `beginScenePassSecondary(frameIdx, cb, true)`.
> * **Nothing reads a normal target.** Normals come from depth (`normalFromDepth`, see RTAO below).
> * **TAA's ocean flag is the scene colour's ALPHA:** `ocean.fs.glsl` writes 0, every other opaque
>   surface its material alpha (> 0), and TAA reads `alpha < 0.004` on non-sky pixels. So nothing
>   layered over the opaque scene may write alpha: `GraphicsPipelineLayout::colorWriteAlpha = false`
>   on decals, debug lines, force shells / union / upsample, particles, cloud apply and fog apply, and
>   `GraphicsPipeline` masks alpha on every BLENDED variant (a transparent mesh keeps the alpha of
>   the opaque surface behind it). The ONE exception is the Ocean variant (`dualSourceAlpha`): it blends
>   its edge over the ground, so it composites the flag itself (see "Terrain surface water"). **A new pipeline that draws into scene colour after the opaque
>   stages sets `colorWriteAlpha = false`.**
> * **The Sky variant does not write depth** (`depthWrite = false`): a sky pixel's depth stays at the
>   cleared far plane, which is how every reader tells "sky". (The prepass did this by skipping the
>   sky sphere; `MATERIAL_FLAG_SKY` and `MATERIAL_FLAG_GIZMO_UI` are gone with it.)

The scene renders as **one render-pass INSTANCE per enabled stage** (separate secondaries, explicit
attachment barriers between instances — `sceneInstanceBarrier`, RendererRecord.cpp), in two groups:

```
Scene opaque   (depth WRITTEN):            Static meshes → GI probe debug
Scene forward  (depth READ-ONLY + sampled): Decals → Debug lines → Force shells → Force union blend → Particles → Far trees apply → Cloud apply → Fog apply
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
* **RTAO**, **TAA**, **PCSS cascades**, **volumetric fog**, **volumetric clouds** (sandbox), **GPU compute skinning**,
  **motion vectors** (moving + skinned meshes; see "Motion vectors").
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
  clear scheduled; the consumers rebind it at their next record) → `Renderer::reloadShaders()`. The grid
  tweaks register in `Renderer::registerTweaks`, BEFORE any pipeline compiles, so a `--tweak` override is live
  for every shader and for the buffer `GIProbePipeline::initialize` allocates.
  **Every reload callback in `Renderer` returns while `!m_initialized`** (state the pipelines read at
  creation is handed over before that test): such a value fires the callback at registration, before the
  device and the pipelines exist, and a reload there crashed. A positive Y offset lifts the grid centre
  so more probes sit above the ground than below.
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
  centre on it, and in first person it is the camera) **to the block's bounding sphere: centre
  distance MINUS the block radius** (3.6 spacings: 7 m in cascade 0, 58 m in cascade 3, the same radius
  the frustum test uses). So one world distance has a different priority per cascade: a coarse block
  counts as nearer, and updates faster, than a fine block at the same centre distance.
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
  decorrelates neighbours), so the blended shifts are stratified instead of white; and a **visit clamp
  with change detection** on the irradiance blend (it replaced a brightening-only step limiter). Each
  probe keeps its visits' DC luminance history in `GI_SUN_V4.zw`: a slow second moment (plain alpha)
  gives sigma around the stored luminance (floored at `GI_SIGMA_MIN_REL`), a fast mean
  (`GI_FAST_MEAN_ALPHA`) follows the recent visits. A regular visit outside stored ± `GI_CLAMP_SIGMA`
  sigma is SCALED onto the bound (all SH coefficients and the sun part: direction and sun fraction kept),
  both ways. A REAL change moves the fast mean, noise does not: the fast mean `GI_CHANGE_SIGMA` off and
  the previous one `GI_CHANGE_CONFIRM_SIGMA` off on the same side (two visits, so one spike cannot
  trigger it) blends that visit unclamped at `GI_CHANGE_ALPHA`. The moments are never clamped, so
  sigma widens while a change passes; a replace restarts them with `GI_SIGMA_INIT_REL` as sigma.
  Boosted visits (relocation, just escaped) skip both, and a CLEARED (all-zero) slot is replaced like
  a fresh one.
  **Sky visibility** (`GI_SUN_V4.y`): the cosine-weighted share of the upper hemisphere (about
  `u_skyUp`) whose rays miss, blended at the plain alpha, baked × W into the volume tail's `.a`.
  `giSkyVisibility(pos, n)` reads it (one tail fetch, the next cascade where the first is dead, open sky
  past the field; 1 without the volume). It occludes the sky reflections the diffuse GI does not carry:
  the terrain's wet sky reflection and the film overlay's sky mirror + blurred sky share (a puddle under a
  roof mirrors no sky), and the ocean's top side: its blurred sky share (in C) and the mirror's sky
  fallback - a traced mirror HIT is geometry and keeps its weight. (Not on the view from below the surface.)
  **Registers (measured 2026-09-27): unchanged everywhere, but only in this form.** The first version cost
  the trace 96 -> 128 (the sun direction, the ground sun, up, the ray max and two sky sums held across the
  ray-query loop - now derived per miss in `traceMiss`, one accumulator left, the hemisphere total taken as
  its expectation N / 4) and the terrain +8 regs / +16 B (the lookup after the lit core - now folded into
  `skyReflW` before it; the film's lookup is first in `terrainFilmShade`). The ocean's lookup (before its
  traces, one half live across them) measured 80/16 -> 80/16.
  **Dead-probe skipping:** a probe whose stored backface fraction is past
  `GI_BACKFACE_DEAD_MAX` (under the terrain, inside a wall — the lookup rejects it anyway) traces only
  every `GI_DEAD_INTERVAL` (8) regular visits, enough for the escape relocation and the wake-up; an
  escape visit pins the stored fraction to exactly DEAD_MAX so the next visit is not skipped.
  **The stored fraction is the EMBEDDED fraction:** the backface-hit fraction × the enclosure, a smoothstep
  of the backface share of the hits (front hits counted within the depth cap) over `GI_ENCLOSED_SHARE_MIN`
  0.6 .. `_MAX` 0.85. Inside a closed solid every hit is a backface (share ≈ 1); among double-sided geometry
  — a tree canopy's leaf diamonds and opaque-traced cards — about half are. The raw fraction relocated canopy
  probes on every visit and marked them dead. The escape, the dead skip/rejection and the just-escaped flush
  all read the weighted value.
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
  window scrolls off them. The test reads `GI_CASCADE_FADE_BAND` (0.1 of the narrowest dim, shared
  with `evalProbeCoverage`) — **widen the band and fewer blocks are covered; the two cannot drift
  apart because both read the define.** **Direct light at a gather hit is ray-traced-shadowed for
  EVERY light:** the sun through `g_sunShadowOverride`, the grid lights through
  `giLightIrradianceShadowed` (lighting.inc.glsl, compiled in by the trace's `GI_LIGHT_RT_SHADOWS`;
  one ray to the light's centre, only past `GI_LIGHT_SHADOW_MIN`, and only while `u_rtLightShadows`
  is on). Unshadowed, a lamp lit every hit in its range through walls — a leak the probe visibility
  test cannot see. The trace includes rt_shadow.inc.glsl BEFORE lighting.inc.glsl for this. **Every
  ray-hit vertex fetch** (the trace, RTAO, `rt_shadow.inc.glsl`, the ocean's and the terrain's mirror hits)
  reads `MeshVertex in_vertices[]` from `mesh_vertex.inc.glsl` (plain std430; the skinning pass uses it
  too). **`MeshVertex` is 3 × vec4 — `positionU`, `normalV`, `tangent` — with the texCoord in the two `.w`**,
  so every member is 16 B-aligned (one wide load each) and position stays at offset 0 for the BLAS build.
  The vertex input binds locations 0 / 1 as vec4s (a shader without uv declares vec3 there); the shadow
  pass binds `normalV.w` alone at location 3. **Miss
  rays AND the virtual sky probe sample THE SKY MAP** instead of marching the
  atmosphere, so the out-of-field fallback matches the misses by construction. **Gather hits use
  `giEvalBounce`** (gi_probe.inc.glsl, write side; probe path only): the cheap multi-bounce lookup — no
  Chebyshev, no cross-cascade blend — because the result is temporally blended. It picks its cascade with
  the shading lookup's `giStartCascade` (the finest fade box, not the tracing probe's own cascade, whose
  covered coarse probes are nearly stale) and has the same one-cascade dead fall-through. The probe buffer is **NOT `coherent`** in the trace (each invocation writes only its own
  probe; stale cross-probe reads are by design). The light and force grids have no GPU insert pass
  any more (CPU-built, see the light grid section). **TLAS exclusions are INACTIVE
  instances** (reference 0, `gi_tlas_instances.cs.glsl`), which the build skips entirely, not
  mask-0 nodes.
* **THE GI RECORD IS SPLIT IN TWO.** `recordGlobalIllumPrep` is the PER-FRAME secondary (GPU scope
  "BLAS builds") — the work whose content changes frame to frame: one-shot static BLAS builds, compaction
  copies, the skinned BLAS rebuild, **the TLAS-instance write and the TLAS build over this frame's LIVE
  instance count**, the one-time probe clear. `recordGlobalIllum` is a CACHED secondary (recorded with the
  scene secondaries, scope "TLAS + probe trace"): the sky map and the probe trace. **Everything per-frame
  in it rides the UBO** — `u_giTrace0/1` (the trace tweaks, last frame's focus, the TLAS range),
  `u_frameIndex`, `u_sceneFocus` — so neither shader has push constants.
  **Why the TLAS is per frame:** a build's primitive count is a recorded CPU value, so the cached build had
  to cover the instance buffer's whole CAPACITY, which only ever doubles — every frame read and filtered
  every slot of the largest scene seen. The live count is final before the record (`present()` sets
  `m_ubo.giTlasNumInstances` before `recordCommandBuffers`), and an indirect build is no option
  (NVIDIA offers no `accelerationStructureIndirectBuild`). The build always runs, at count 0 too (an
  empty TLAS): a skipped one would keep records that may reference freed BLASes.
  **`u_giTlasNumInstances` is patched in `present()`, not written by the beginFrame UBO
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
  a 256×128 RGBA16F lat-long 3-layer array, GENERAL for life. Layer 0 = `skyRadiance` (GI miss rays,
  the forward pass's per-frame-constant `skyRadiance(up)` ambient), layer 1 = `mirrorSkyRadiance`
  (atmosphere.inc.glsl: the ocean's and the terrain wet film's reflection-ray sky, 12-step march +
  saturation), both WITH the volumetric clouds (CloudPipeline's sky clouds, composited as
  `inScatter + sky * transmittance`; see "Volumetric clouds"); layer 2 = `skyRadiance` CLEAR, the clouds'
  own ambient (so they never light themselves through their image).
  **A single zenith texel is NOT an ambient once clouds are on:** it is the one cloud straight over the camera.
  The OCEAN (2026-09-29) therefore takes its sky light (body in-scatter, whitewater, underside) as the sky SH's
  hemisphere average `giEvalSkySH(up) / pi`, and the roughness blur's sky share as
  `giEvalSkySHH(normalize(R + up)) / pi` (the reflection's side of the sky; NOT centred on R: a grazing
  reflection's lobe was then half below the horizon, where the SH holds the ground - black at the default ground
  intensity - and the rougher ocean went darker than the film); the zenith texel stays only as the GI-off fallback (`u_aoParams.y` 0,
  stale SH). The terrain WET FILM does the same (`terrain_film.fs.glsl`: its body in-scatter / whitewater and
  its blur share). The terrain's own zenith read is only the reflection fog's GI-off fallback (the fog uses
  the SH while GI is on), so it stays. Mapping + layer ids live in atmosphere.inc.glsl (`skyMapUV` / `skyMapDir`,
  `SKY_MAP_LAYER_*`); the forward set binds it at **20** (binding **21** is the GI irradiance volume, **22**
  the cloud shadow map, and the texture array is **23**, still the set's highest binding for the variable count). Anything that
  would call `skyRadiance` or `atmosphereScatterCheap` per pixel samples the map instead.
* **THE GI IRRADIANCE VOLUME** ("GI/Irradiance volume", on by default; "GI/Volume voxels per probe" 1–2,
  default 2 — both in `g_giGrid`; they reload every shader but keep the probe buffer, see below). Per
  frame, right after the trace (inside the GI toggle), `GIProbePipeline::recordVolumeBake`
  (`gi_volume_bake.cs.glsl`) bakes every cascade into 3D images: the probe lattice refined `volumeRes`
  times per axis, stored TOROIDALLY like the probes (slot = fine coord & (dims − 1)), so the lookup is
  REPEAT addressing + hardware trilinear. A voxel = the visibility-weighted blend of its 8 probes AT THE
  VOXEL CENTRE (trilinear × backface-dead fade × Chebyshev). **20 B per voxel, 4 images per cascade**
  (`RendererVKLayout::GI_VOLUME_FORMATS`): L0 × W in B10G11R11_UFLOAT (PREMULTIPLIED by the summed weight,
  so a pixel's trilinear fetch is a weight-correct blend and a dead voxel adds nothing instead of black; a
  small float keeps the HDR range at ~1.5 % steps), the 9 L1 terms as RATIOS to L0 scaled by 1/√3 (for a
  non-negative radiance every ratio is within ±√3) in 2 × RGBA8_SNORM + the 9th in RGBA16F next to W (fp16,
  because L0 = fetch / W needs W's precision when it is small) and the sun fraction × W (the cloud dimming,
  see "GI under cloud shadows"). Low precision is safe here because the
  volume is never accumulated — every bake writes a finished value from the fp32 probe history. The price:
  the ratios filter unweighted, so where L0 changes fast the direction is slightly off, and a dead voxel's
  zero ratio pulls its neighbours' L1 a little toward 0. `createVolume` asserts storage + linear-filter
  support for the formats; B10G11R11 storage needs `shaderStorageImageExtendedFormats`, which
  `Device` enables with every other supported core feature. `evalProbeCoverage` (gi_probe.inc.glsl) reads the
  volume through `giLookupCascade` whenever the includer defines `GI_VOLUME_TEXTURES_NAME` (every consumer does
  under `GI_VOLUME`): 4 filtered fetches per cascade instead of 8 probes × 7 vec4 loads.
  **The price: the half-Lambert probe-direction weight needs the surface normal and is NOT baked, and the
  Chebyshev test runs from the voxel centre — more leaking through geometry thinner than a voxel** (the
  per-pixel normal bias of the sample point is kept).
  **EVERY probe consumer switches with the toggle** (`GI_VOLUME` define; the probe code stays for the off
  path): the lit core, the ocean and the terrain's mirror hits (static-mesh set binding 21), decals (5),
  particles (10, vertex), fog scatter (12) and fog apply (5, sky only), and the TRACE's multi-bounce lookup at
  gather hits (13 — the wave stamps are 14, its texture array 15), which reads LAST frame's bake with the baked Chebyshev
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
  **ONE shading lookup, `evalProbeCoverage`**, for the probe and the volume path alike (`giLookupCascade`
  picks the per-cascade sampler at compile time). **`giIrradiance(worldPos, n)`** wraps it with the sky SH
  fade — the lit core (`giIndirectOverPiH`), decals, particles and the fog scatter all call that; only the
  trace (scales by coverage, no sky) calls `evalProbeCoverage` itself. **It has NO cascade walk**:
  `giStartCascade` computes c from the Chebyshev focus distance (a normal-free bound, the bias taken at its
  per-axis maximum, so at most one cascade early — then c + 1, which always holds the point for probe dims
  >= 16), and returns the biased point and its fade (> 0 implies the stencil fits). **At most TWO cascades
  are sampled: c, and c + 1** as either the blend partner in c's band or the fall-through when c is all
  dead; a dead c does NOT blend on from c + 1 into c + 2. Dead in both, or past the outermost box: coverage
  0, and every caller uses only its sky fallback. `giEvalBounce` uses the same pick and fall-through, with
  no blend.
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
  reprojects the fragment in clip space (`fragPrevClip` off `gl_FragCoord`: the camera through
  `u_reprojClip` plus the point's own motion, `MOTION_WORLD_DELTA` - see "Motion vectors"), samples the
  PREVIOUS slot's AO image (binding 13) and weights the 2x2 taps by their world distance to where the
  point WAS, reconstructed from the PREVIOUS slot's depth (binding 12, `u_prevDepth`, `u_prevInvMvp`, last
  frame's jitter). No input comes from this frame, so the trace has NO ordering constraint against the
  forward pass (this is what let the depth prepass go). Static geometry is exact under camera motion, and
  a moving object reads its own AO from where it was (the terrain has no `MOTION_WORLD_DELTA`: it compiles
  the camera-only path). No valid tap (disocclusion, off-screen) = `(0, 0, 0, 1)`: no occlusion, no bent
  normal. **All eight fetches (4 depth + 4 AO, `textureLod`, AO held half) are issued first, then the
  weights** - one texture-latency wait instead of each AO tap sitting behind its own depth test
  (2026-09-28, for the L1TEX long-scoreboard stall).
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

## Motion vectors

Per-pixel OBJECT motion, so the temporal passes follow moving meshes (skinned characters, moving nodes)
instead of only the camera.

* **The motion target** (`SceneColor`, `SCENE_MOTION_FORMAT` = RGBA16F, one per frame slot and eye): colour
  attachment 1 of the depth-writing stages only (see "Frame order"). Value (`shared.inc.glsl`): xy = this
  frame's minus last frame's UNJITTERED full-frame uv, z = the surface's hardware depth LAST frame (<= 0:
  behind last frame's camera), w = 1. **Cleared to 0: w = 0 = no object motion**, and the readers then keep
  the exact camera-only reprojection (`prevScreenUVClip`). So only a point that really moved stores a uv
  (fp16 rounding: sub-pixel below ~0.1 of the screen per frame); a still scene is bit-for-bit the old path.
  Readers: `prevScreenUVMotion(uvUnjit, depth, motion, valid)`; a disocclusion test compares last frame's
  depth against `worldPosFromDepthMat(prevUv, motion.z, u_prevInvMvp)` - where the point WAS - instead of
  this frame's position.
* **Who writes it** (`GraphicsPipelineLayout::motionTarget` + `writeMotion` / `PipelineVariant::writeMotion`;
  otherwise the second blend state is write-masked): the static-mesh variants that can draw moving meshes -
  LitOpaque, LitMasked, UnlitOpaque. Masked, and correct because static: terrain (both pipelines), sky,
  GI probe debug. Masked, keeping the opaque surface behind: the transparent variants, the terrain overlay,
  the gizmos. **The ocean cannot write it**: dual-source blending allows one fragment output location
  (`maxFragmentDualSrcAttachments`), so the waves keep TAA's ocean feedback cap, and an ocean pixel over a
  moving underwater object carries that object's motion (known limitation).
* **TWO DGC EXECUTION SETS.** A set needs ONE fragment output interface for every pipeline in it
  (VUID-vkUpdateIndirectExecutionSetPipelineEXT-initialPipeline-11147), and a dual-source blend may not
  write location 1. So the variants split in two families (`RendererVKLayout::PIPELINE_TRANSPARENT_MASK`,
  injected as a define): the TRANSPARENT family (LitTransparent, UnlitTransparent - compiled with
  `NO_MOTION_OUTPUT` - Ocean, TerrainOverlay) writes location 0 only; every other variant writes locations 0
  and 1 (sky and terrain write a masked 0). The cull routes a draw into the opaque or the transparent
  sequence by this FAMILY, not by its alpha mode, and `StaticMeshGraphicsPipeline` executes each sequence
  with its own set (`IndirectExecutionSet::initialize(pipeline, name, variantMask)`; the other slots stay
  unwritten). Before each execute the set's INITIAL pipeline must be bound (`getInitialVariant`: 0 for the
  opaque set, LitTransparent for the transparent one). **A new variant: pick its family in the mask, and give its fragment shader that family's
  outputs.**
* **The vertex side** (`instanced_indirect.vs.glsl` `prevWorldDelta`): the WORLD offset of the point from
  where it was last frame, interpolated to `motion_vector.inc.glsl`. A world offset, not a clip position:
  exactly 0 on a still instance (the fragment then writes 0), small, and computed as a DIFFERENCE of the two
  transforms' terms (the translations cancel first), so it keeps its precision far from the origin. The
  fragment adds the camera through `u_reprojClip` and the object through `u_prevMvp`'s linear part only.
* **The delta costs the lit FS 3 inputs (17 -> 20, measured 2026-09-28; registers unchanged at 72 / 16 B).**
  Tried and REVERTED: rebuilding the world position in the FS from `gl_FragCoord` + the depth to win them back
  (20 -> 17). An interpolant costs no register until it is read (it is re-interpolated where used), but the
  rebuilt position stayed live across the whole lit core: spill 16 -> 64 B (LitMasked 32 -> 64 B), Static
  meshes 1.548 -> 1.563 / 1.576 ms. Nsight (the same day): the pass is occupancy-bound - PS warp launch
  stalled on registers 34 %, TRAM 17 %, ISBE 16 % of cycles, 9 % of the pixel warp slots active.
* **Last frame's instance transform** (`instanced_indirect.cs.glsl` `prevInstanceTransform`, into the
  cull's `OutMeshInstance`, now 64 B): after the main cull the primary copies this slot's node transforms +
  pass masks into ONE device-local previous set (`InstanceStream::recordPrevCopy`; the other slot cannot
  serve - the CPU may write it while the GPU reads it), which next frame's cull reads (bindings 18/19). A
  pass mask carries its push frame above the PASS_* byte (`InstanceStream::stampedPassMask`, from
  `m_ubo.frameIndex`); a node not pushed LAST frame (off screen, or just spawned into a recycled transform
  slot) draws with no motion instead of a stale transform. prevScale 0 = the node did not move: the vertex
  shader then keeps the exact current transform (the previous quaternion is snorm16, moving nodes only).
* **Skinned meshes**: the output region is 2 x vertexCount (`spawnSkinnedNode`, freed the same way). The
  skin pass copies each position it is about to overwrite into the second half; `MeshInfo::prevVertexDelta`
  (the old unused `firstInstance`) = vertexCount, copied into every LOD level's info (they index the same
  region), 0 = not skinned. The lit vertex shader reads it from the vertex SSBO (binding 14, now also in the
  vertex stage). `SkinningJob::prevValid` = 0 on a job's first frame (fresh or re-acquired bundle: the
  region holds no last frame), set by `SkinnedMeshRegistry::markJobsUploaded` after the upload.
* **Readers**: TAA (the motion of the NEAREST surface in the 3x3, so a moving silhouette's history follows
  the object), the AO temporal pass, the forward pass's AO read (its own `MOTION_WORLD_DELTA`, no
  target read), the motion blur, and DLSS's motion vector pass (`dlss_mvec`). Not (yet): the cloud temporal
  pass, the particle collision.
* Cost: +8 B per pixel per slot and eye (~30 MB per slot at 1440p), +16 B per culled instance, one extra
  varying (vec3) on the lit/unlit vertex path.

## Long-range sun shadows

"Shadows/Terrain march *". Past the PCSS `Max distance` and `RT/TLAS Range`, distant pixels
cone-march the baked terrain height cascades (`terrainSunVisibility` in `terrain_height.inc.glsl`) and
take `min` with the shadow term.

**The march fades in inside cascade range, so the handover is seamless.** Steps double from
`Terrain march bias`, which is also the self-shadow bias. **Deterministic — no jitter, no temporal
integration.**

## Volumetric clouds (`CloudPipeline`, "Sky/Clouds" tweaks)

**A SANDBOX feature: full 3D clouds the camera flies through.** The game turns them off
(`setCloudsSuppressed`, GameMatch ctor/dtor) without touching the user's `Enabled` tweak. The sky
variant (`sky.fs.glsl`) draws NO clouds any more.

* **The shell:** altitudes `[Bottom, Top]` above world Y 0 on a sphere of `ATMOS_R_PLANET` whose centre
  lies straight under the camera (the terrain is flat; from altitude the deck still curves to the
  horizon). All cloud math is CAMERA-RELATIVE. **Precision:** `cloudAltitude` forms `(r² - R²) / (r + R)`
  and `cloudRaySphere` the stable quadratic, because float differences of two ~6.4e6 values are
  metres off.
* **TWO LAYERS** (2026-09-28): `cloudDensity` = the MAIN layer (its own band, "Bottom" / "Top",
  `u_cloudLayer0.xy`) + the optional UPPER layer ("Sky/Clouds/Upper layer": its own band, coverage, type and
  density scale, `u_cloudLayer0.zw` + `u_cloudLayer1`; default on, 6000-7500 m, type 0.1, density 0.1 = thin high
  sheets). Its "Coverage" and "Density" are MULTIPLIERS on the main layer's (2026-09-29; coverage default 0.6):
  the CPU sends `upperCoverage x coverage` (clamped 0..1) in
  `u_cloudLayer1.z`, and the density was always `x upperDensity` before "Density (1/m)". **Its own column
  LIFT** ("Upper layer/Height variation", default 0.2, `u_cloudLayer2.w`; 2026-09-29), the main layer's method:
  without it every sheet sat at the band's bottom, one altitude for the whole deck. Its field is the tower field
  NEGATED and offset at mip 2 (uncorrelated with the main lift and this layer's weather; still tiles). The upper layer's weather is the same map rotated -90° and offset (scale 1: still tiles), so its
  gaps do not follow the main layer's. The SHELL (`u_cloudShape0.xy`, what the march and the shadow map cover)
  is the union of the bands; the gap between them is crossed by the coarse empty-space steps. Why: one tall
  shell stretched every column's profile into a peak - a taller "Top" made the towers thinner, not the sky
  layered. Both layers share `cloudLayerShape` (the noise threshold + detail erosion). **The upper layer does
  not cost the main layer quality** (2026-09-30): the SHADOW MAP marches the MAIN band only (`cloudMainIntervals`,
  `cloudMainDensity`) - the upper layer casts no map shadow (thin, low-density sheets; its own lighting comes from
  the sun march) - and the view march SOLVES its step growth over the main band's span (see the March).
  **COVERAGE 0 = CLEAR** (`cloudColumnCoverage`, both layers; 2026-09-29): a column's coverage is the base
  coverage + (weather.r - 0.5) x "Coverage variation", the variation faded in over the first 0.25 of the base
  coverage (`CLOUD_VARIATION_RAMP`). With a constant spread the weather map's peaks stayed clouds at coverage 0;
  from 0.25 up it is exactly the old sum. The upper layer ramps on its own effective coverage (upper x main).
  **EROSION CUTOFF** ("Erosion cutoff", default 0.1, `u_cloudShape4.w`; 2026-09-29): the erosion's remap
  `(d - lo) / (1 - lo)` leaves a thin rest wherever the detail fBm (so `lo`) is low, and over kilometres of ray
  that rest read as haze in the open, eroded areas. `cloudLayerShape` ends with `(d - cut) / (1 - cut)`: the rest
  goes, the cores stay at 1. Monotonic, so the cheap shape still bounds the full one (the coarse test).
  **SHELVES** ("Shelf count" 0-3 (default 1), "Shelf strength", "Shelf thickness"; `u_cloudLayer2`; 2026-09-28): the
  main layer's LAYERED look - stable layers (inversions) at fixed heights of the layer ("Shelf spacing" between them,
  default 0.2, the stack CENTRED in the layer: the CPU sends the lowest, `0.5 - (count - 1) / 2 x spacing`, in
  `u_cloudLayer3.x` and the spacing in `.y`; 2026-09-29 - before, count evenly spaced at i / (count + 1)), where every
  cloud that reaches one spreads out sideways into a flat tier (stratocumulus cumulogenitus; the anvil at a storm's
  top). The profile is raised past 1 around each shelf height (layer-relative hf, so all clouds spread at the same
  altitude), lowering the coverage threshold there - only where the cloud already exists, so the tiers follow the
  coverage of the clouds under them. **Tried and removed:** a MIDDLE layer (the main model over a second band, first
  with the main settings, then with its own copy): independent decks whose coverage did not follow the clouds below.
* **PEAKS - the tower field was the cause** (2026-09-28): the weather map's tower height (alpha, per-column top)
  was generated at 16 cells x 4 octaves per tile against the coverage's 6, so every cloud held several top
  maxima and its top followed them - a range of peaks, higher with a taller "Top". It is now 6 cells x 2 octaves
  (`cloud_noise.cs.glsl`): about one smooth top per cloud. The profile shape is three 0..1 tweaks in
  `u_cloudShape5`: **"Tower variation"** (how far a top may drop below the layer top; 0.55 = the old fixed
  [0.45, 1]), **"Top roundness"** (the top ramp as a superellipse `(1 - s^p)^(1/p)`, p = 1 + 5 x roundness: the
  profile - which raises the coverage threshold - holds near 1 and falls only at the ramp's end, a dome instead
  of a cone), **"Base sharpness"** (shortens the bottom ramp toward a flat base). Tried first and replaced: caps
  on the ramps in metres - barely visible, and awkward to tune.
  **"Tower core link"** (`u_cloudShape5.w`, default 0.75; `cloudTowerSignal`): the broad tower field alone put
  most clouds on a slope of it, and their tops tilted alike - "ramps". The link ties the top to the cloud's OWN
  coverage (`coverage x (0.5 + tower)`): its core rises highest, its edges stay low, and the tower field scales
  that per cloud (some tower, some stay flat). 0 = the field-only tops.
* **The density model is `clouds.inc.glsl`**, shared by every cloud pass: weather map (coverage, type,
  density, tower height) × height profile per type (stratus → cumulus → cumulonimbus) × Perlin-Worley
  base, eroded by curl-distorted Worley detail, plus a near-camera octave inside `Near detail radius`.
  **The profile raises the coverage THRESHOLD** (only the strongest noise passes near the top and base, so
  tops round into domes); a profile that only scaled the density against one fixed threshold cut every
  cloud at the same height. The tower height stretches the profile per column over `[0.45, 1]` of the column.
  **Per-column LIFT** ("Base height variation" v, `u_cloudShape4.x`, default 0.03 - the user's pick; 2026-09-28): the WHOLE
  column rises by up to v of the shell - the profile and the base + detail noise with it (`noiseAlt`) - and its
  height shrinks to (1 - v), so each cloud keeps its own rounded base and only sits higher. The field is a
  second weather fetch: the tower field ROTATED 90° (uncorrelated with the tops; scale 1 still tiles with the
  weather period) at MIP 2, so the height drifts over kilometres. **Tried first and replaced:** a per-column
  BASE cut over the same profile (the field at twice the frequency, mip 0) - the cloud's low part became thin
  tendrils down to the shell bottom, a "peak" under every cloud. 0 = no lift (and no extra fetch).
* **The noise is world-anchored and tiles.** Base repeats `Base repeats` times per weather tile and detail
  `Detail repeats` times per base tile — integers, so `buildUboClouds` wraps (camera − wind) by the
  weather period ONCE, in double, and every texture stays continuous. The wind and the detail's vertical
  drift (`Evolve speed`) accumulate in double on the SIM clock. **A new noise lookup must use an integer
  multiple of these frequencies**, or it seams when the origin wraps.
* **The noise textures are generated on the GPU** (`cloud_noise.cs.glsl`) at initialize and on every
  shader reload, with a blit mip chain: base 128³, detail 32³, weather 512², curl 128², all RGBA8.
* **Three passes per eye:** the march (compute, half res, EVERY pixel EVERY frame — a fly-through has
  parallax at every depth, so a 1-in-16 update would smear), the temporal accumulation, and the apply.
  * **March** (`cloud_march.cs.glsl`, right after RTAO): up to the FARTHEST scene surface of the pixel's
    2x2 block; steps are `max(Near step, g * t)`, and **g is SOLVED PER RAY** so the schedule takes
    **"Steps per ray"** (default 300, capped at ~75 % of `Max steps`) over the ray's shell span `[tStart, tEnd]`
    (a fixed-point solve of `N(g) = max(0, n/g - tStart)/n + ln(tEnd / max(tStart, n/g))/g`): short steps near the
    camera, faster growth far away. (2026-09-29: it replaced the fixed "Step growth" g, with which a ray crossing
    the shell from below took `ln(top / bottom) / g` steps at any angle - the quality followed the layer's
    top / bottom RATIO, ~280 steps at 300-5000 m but ~70 at 1500-3000 m.) **The near step scales with the
    span:** `clamp(span / target, "Min step", "Near step")` (2026-09-30) - a fixed near step gave a SHORT span (up
    through a thin layer) fewer steps than the target; now it takes `target` uniform steps, and a long span keeps
    "Near step". "Min step (m)" (default 3, `u_cloudLayer3.z`) is the floor: finer steps only re-read the same noise.
    **With the upper layer on, the solve span is the MAIN band's** (`sStart` / `sEnd`, two more sphere tests): over
    the union shell the target was spread over the gap and the upper band, and the main layer lost steps. The
    upper layer takes its steps on top at the same growth; the gap uses the coarse empty-space steps; a ray that
    misses the main band solves over the shell. `tStart` / `tEnd` stay the shell's for the air term. The uniform floor (distance left / steps left) is
    only a safety net: as the rule it made a long ray's EVERY step long (400 m from the camera on, flat
    through the layer: one density sample per step, opaque at once - a grainy band at the camera's altitude).
    **Known issue, open:** with a small budget (200) a darker band remains on the LIT side of far clouds at
    the camera's altitude - lighting only ("Density only" is clean), gone with the sun march off, with
    "Max distance" 20 km, or with 600 steps. An in-cloud step cap of one mean free path did NOT remove it
    (tried and removed 2026-09-27). The default `Max steps` is 600: only long (flat) rays use the extra steps. **The march itself is `cloudRaymarch` (`cloud_raymarch.inc.glsl`), shared with
    the sky clouds.** **Empty-space skipping:** in clear air it takes 3x steps that test only the cheap
    shape (weather + base); the detail only erodes that shape, so a zero there is a zero of the full
    density. A hit backs up and marches finely; 4 empty fine steps return it to coarse. The sun march reads
    the detail noise on its first step only. **Past `Detail distance`** the detail erosion is off (its
    weight fades over the last 20 %; `cloudDensity` takes a detail WEIGHT) and the detail fetches are
    skipped. **The march ends at transmittance 0.02**, and below 0.3 its fine steps double. **The final
    transmittance is remapped `(T - 0.02) / 0.98`**, so the early-out level is fully opaque (2026-09-28): the
    composite passes scene x T, and the leftover 2 % let the SUN DISC (thousands of times the sky) shine through
    any cloud, however dense. **The aerial in-scatter in front of the cloud is in the clouds' shadow**: EACH of
    `cloudAerialScatter`'s 8 steps, placed at the ray's per-pixel / per-frame jitter (4 fixed points stamped the
    openings' lit air columns onto the clouds beside them as blue BLOBS; jittered, the temporal pass averages them
    into soft shafts), takes `cloudAirSunVis` (cloud_raymarch.inc.glsl; the function moved there from
    clouds.inc.glsl): the shadow map inside it, and PAST it the WEATHER MAP - the coverage of the column where the
    sun ray from the air point crosses the main layer's middle, `1 - smoothstep(0.35, 0.75, coverage)`. (2026-09-29:
    it was ONE visibility for the whole segment, the mean of two `cloudSunTransmittance` taps at 1/4 and 3/4 - and
    past the far cascade that function returns the sky-wide constant mean `mix(1, 0.3, coverage)`, so the 10-40 km of
    air toward far cloud bases lay half-lit under a dense deck: a BLUE Rayleigh SHEEN on the dark bases.) Its
    Mie forward peak otherwise drew a sun glow on top of dense cloud. Tried first and replaced: a ramp to the
    VIEW ray's transmittance within ~10 degrees of the sun - it cut the halo's centre only, a black hole in a
    ring. With cloud shadows off the air stays unshadowed.
  * **Checkerboard** (`CLOUD_CHECKERBOARD`, "Quality/Checkerboard", on): the march dispatch covers half the
    columns, each thread on this frame's parity (`marchPixel`) - full warps, half the rays. The parity flips
    every frame and so does the frame slot, so a slot's march image only ever holds ONE parity; the
    temporal pass reads only this frame's. A marched pixel clamps to its 4 diagonals; the others take the
    mean of their 4 side neighbours as the current value, last frame's march at the same pixel as history,
    and their own march limit from the scene depth (temporal binding 7). Per dense sample: a sun march (`Light steps` over `Light distance`), three Wrenninge
    multiple-scattering octaves — octave 0 on the HG + Draine phase (Jendersie & d'Eon 2023: a fit to Mie
    scattering on water droplets; the CPU turns `Droplet size (um)` into its four parameters,
    `u_cloudLight0`; **the HG part's g is capped by "Forward peak limit" (0.95)**: uncapped it is the droplets'
    diffraction peak, g ≈ 0.995 at 20 um - ~5400 / sr in a ~0.3 degree lobe, the size of the sun disc - and behind
    thin cloud that lobe clipped to white after the exposure: the sun looked bigger and brighter behind clouds
    than in clear sky), the multiple-scattering octaves ISOTROPIC (a flattened droplet phase kept ~half the energy
    in a g ≈ 0.66 forward lobe, and the side away from the sun went far too dark), summed over ALL octaves in
    CLOSED FORM: the first exactly, `a e^(-τa)`, plus the geometric tail `a²/(1-a) · e^(-τ a^(1+1/(1-a)))` through
    one effective extinction (constants per ray; per sample 2 exp, no loop), x "Multi-scatter strength"
    (`u_cloudLight2.w`, default 1, non-physical above 1). With the sun BEHIND the viewer the lit side is seen near
    180°, where the droplet phase is ~0, so its brightness is this term alone: the old two octaves (~0.14 of the
    sunlight) left thick sunlit clouds gray; all of them at a = 0.9 give ~0.7 (2026-09-29; an N-octave loop came
    between). Then a
    powder term, and an ambient: the sky map's CLEAR layer weighted by hf = the height within the sample's OWN
    LAYER (`cloudLayerHeightFraction`: main or upper band, not the union shell - a main-layer top would otherwise
    lose its sky light under a tall shell; the sun's bottom / top blend uses it too), plus the
    ground bounce falling off EXPONENTIALLY from below (its albedo `u_cloudLight2.rgb` = the sky's "Ground Albedo"
    COLOUR - its hue, not its intensity, which scales the sky-sphere ground plane and defaults to 0 - x the cloud
    "Ground albedo"), `exp(-h / depth)` with h = metres above the MAIN layer's base and depth =
    "Ground light depth (m)" (`u_cloudShape4.y` = 1 / depth, default 297 m) - it lights the undersides only (2026-09-28; the
    old linear `1 - hf` lit the whole cloud); the energy-conserving step integral.
    The sun at the cloud uses the atmosphere transmittance along the LOCAL up of the entry point (the
    sunset on distant clouds). The aerial perspective in front of the cloud is a 4-step single-scatter
    segment from the camera altitude (`cloudAerialScatter`; "Lighting/Aerial perspective strength",
    `u_cloudLayer3.w`, 0-5, default 1: x the ADDED air light only - the cloud's dimming through the air stays
    physical; 0 = off entirely, the cloud as lit with no air in front). The sun march runs only where the shadow map
    does not cover the sample (or "Self-shadow from map" is off - the default: the user keeps it off for
    quality; on, it measured march 0.63 -> 0.35 ms). **The sun march** (`cloudLightOpticalDepth`, "Light steps" 4,
    quadratic spacing) runs over a PER-SAMPLE reach: the way out of the sample's OWN layer toward the sun,
    `(layer top - altitude) / L.y`, capped by "Light distance (m)" (default 8000; 2026-09-29 - it was a fixed
    2 km: steps in empty air over a sample near the top, and a base sample stopped short of its tower). It reads each step's base noise at the mip of ITS OWN LENGTH (at least the view sample's) and
    stops past `odCut = 7 / min(msTailExt, ms)` (every multi-scattering octave under 0.1 %): march 0.63 ->
    0.56 ms, sky 0.14 -> 0.10 ms (2026-09-29). Output: in-scatter + transmittance, and
    log2 distances (first hit, transmittance-weighted, march limit).
  * **Temporal** (`cloud_temporal.cs.glsl`): reprojects the weighted cloud distance minus this frame's
    wind displacement, rejects a history whose cloud distance lies OUTSIDE the neighbourhood's range of
    this frame's distances (+-0.2 in log2), clamps its colour to the neighbourhood. **Not a per-pixel
    distance test:** a ray flat through the layer passes clouds at very different distances, so its one
    weighted distance jumps with the step jitter every frame - the per-pixel test rejected the history
    exactly where the raw march is noisiest (diagonal stripes of the old interleaved-gradient jitter; the
    jitter is now a per-pixel hash + golden-ratio step, grain instead of lines where history is missing).
    The history is the OTHER frame slot's accumulation; all images are cleared to "no cloud" at creation, so
    there is no history-valid flag. **The history's march LIMIT must also match this frame's** (±0.15 in
    log2): at a silhouette the bilinear history fetch pulled part of the sky neighbour's cloud into the texel
    over the near surface, the neighbourhood clamp let it through, and the feedback built it up - that texel's
    first-cloud distance is the surface's own, so the upsample kept it: a cloud-coloured outline on every edge
    against clouds, thicker with a higher blend. The checkerboard fill takes its front only from neighbours
    that hold cloud (an empty march stores its limit as the front - the same leak).
    **The opposite leak** (2026-09-29): a texel over a near surface holds "no cloud" (its march stopped under the
    clouds). The checkerboard fill's plain 4-neighbour average and the neighbourhood clamp's range both took it
    into the SKY texels beside it: a clear-sky-coloured outline around every object against clouds. So the fill
    weights its neighbours by how well their march limit matches its own (`1 / (1 + 16·Δ)`, as the upsample), and
    the clamp range takes only neighbours within `LIMIT_MATCH` (0.15 in log2) of the pixel's limit.
  * **Apply** — the depth-aware 4-tap upsample is `cloud_upsample.inc.glsl` (a texel whose first cloud
    lies behind this pixel's surface counts as "no cloud"), used by TWO passes:
    * **Fog ON: the fog apply composites the clouds** (`vol_apply.fs.glsl`, bindings 6/7). Fog laid over
      finished clouds fogged them as if they stood at the scene depth (over far terrain: a grey band over
      every cloud). With the cloud (S, T) at its weighted distance tc and the fog F to the scene, C to tc:
      `out = T·F.rgb + C.a·S + (1−T)·C.rgb + scene·(T·F.a)` — exact for a cloud at one distance and linear
      in the scene, so the blend state is unchanged. `fogTo` (froxel volume + far field) runs twice on
      cloudy pixels - ONCE when the cloud and the scene both lie past the froxel volume and "Fog/Far
      Field/Max distance" (default 40 km, `u_fogParams8.z`; the far field adds no fog past it): both then
      see the same fog.
    * **The fog's sun term is cloud-shadowed everywhere**: the froxels per froxel (`vol_scatter.cs.glsl`), the
      FAR FIELD per sub-segment (`volFarField`: 2 jittered shadow-map taps each, weighted by the sub-segment's
      share of the in-scatter; the apply binds the cloud shadow map at 8). **"Fog/Sun scatter"** (`u_fogParams8.w`,
      default 1): a non-physical gain on the SUN in-scatter only (froxels, far field, reflection fog) - sunlit fog
      and the shafts brighten, shadowed fog (ambient only) and the extinction do not: strong god rays through thin
      fog without fogging up the world.
    * **"Fog/Shaft haze"** (Density (1/m), default 0.0025, 0 = off; Height (m), default 300; `u_fogParams10`): the fog is a
      HEIGHT fog, nearly gone a few tens of metres up, so shafts from the clouds showed only after cranking the base
      density. The haze is a separate thin medium reaching up to the clouds (its own scale height from the fog's
      height base) that adds SUNLIT in-scatter only (x "Sun scatter"): no extinction, no ambient - non-physical on
      purpose. Froxels: per froxel (`heightFogMean`), its visibility the fog's where the fog computed one, else the
      clouds' alone (terrain shadows skipped above the fog). Far field (`volFarHazeStep`): per sub-segment, with the
      same cloud-visibility taps, behind the fog in front of it and bounded by its OWN extinction (a level ray to
      the horizon stays finite; it does not dim the scene).
    * **Fog OFF: the "Cloud apply" scene stage** (`cloud_apply.fs.glsl`, before the fog apply slot, gated on
      `cloudsEnabled() && !fog`). `colorWriteAlpha = false`.
* **THE CLOUD SHADOW MAP** (a Beer shadow map: `cloud_shadow.cs.glsl` writes, `cloud_shadow.inc.glsl`
  reads). Two sun-aligned ortho cascades in ONE 2-layer RGBA32F array (`SHADOW_RESOLUTION` 1024), recorded
  straight into the primary after the pre-scene stages (before GI, fog and the scene).
  * A texel = one line along the sun through the MAIN layer's band (not the union shell: the upper layer casts
    no map shadow, 2026-09-30): x = the along-light coordinate `dot(p - centre, L)`
    of the first cloud coming from the sun, y = the mean extinction to the last cloud, z = the whole optical
    depth. A receiver's optical depth is `min(y * max(x - a, 0), z)`, so ONE fetch serves the ground, a
    mountain or fog froxel inside a cloud, and the clouds' own samples. Because x is a coordinate and not a
    distance from a start plane, the map needs no per-cascade start distance.
  * **PROGRESSIVE UPDATES, one buffer, no toroidal addressing.** Each frame a cascade renders 1 / split of
    its texels ("Near update split" 1/4, "Far update split" 1/64 since 2026-09-29 - it was 1/16; the far
    cascade did ~2/3 of the samples, and a 64-frame cycle moves the clouds ~11 m at 10 m/s, under a third of
    its ~39 m texel; Cloud shadow 0.175 -> ~0.09 ms): one texel of each 2x2 / 4x4 / 8x8 block, the
    rotating phase's (`shadowTexel` in cloud_shadow.cs.glsl), one thread per rendered texel - a constant cost
    per frame instead of a spike every Nth frame (the old "Far update interval"). Neighbouring texels differ in
    age by a few frames, well under a texel of cloud motion. That needs ONE texel-to-world mapping for all of
    them, so `buildUboClouds` FREEZES each centre (snapped to whole texels in light space, double, world
    space) and re-centres only when the camera is 1/8 of the extent away (125 m near, 1 km far), or the sun,
    the extent or the enable changed: that frame renders the whole cascade (a rare spike; a CONTINUOUSLY
    moving sun re-renders every frame). The lookup is unchanged: only the frozen centre's camera-relative
    offset is rebuilt each frame. **Steps per cascade** ("Near steps", "Far steps", `u_cloudShadow4.y` /
    `.z`). **So the map is ONE image, not per frame slot.** It is cleared to "no cloud" at creation and
    stays bound while the clouds are off (`u_cloudShadow4.x = 0` makes the lookup return 1).
  * **THE FAR CASCADE IS FILTERED ON THE RESULT** (`cloudShadowFarODFiltered`): its texels are tens of
    metres, and the OD is non-linear in the stored terms, so the sampler's bilinear blend of (x, y, z) stayed
    blocky. Instead it `texelFetch`es the 2x2 texels, computes each one's transmittance and blends those
    bilinearly (PCF-style), then returns `-log(T)`. **"Far softness"** (`u_cloudShadow4.w`, texels, default
    1.5) jitters the lookup per pixel and frame (a world-anchored hash: vertex-stage consumers have no
    gl_FragCoord); TAA and the fog's temporal blend average it into a penumbra. The near cascade keeps its
    one hardware-bilinear fetch, and so does `cloudSunTransmittanceSoft` (GI).
    **Only SURFACES take the filter** (`cloudSunTransmittance`: lit core, terrain, ocean, film, decals).
    **`cloudSunTransmittanceBilinear`** (the far cascade's one bilinear fetch) serves what already averages over
    space and time: the fog froxels + shaft haze, the far field's jittered taps (up to 10 per pixel), the clouds'
    air term and particles. Measured 2026-09-29: the far field on the filter cost the fog apply ~0.025 ms.
  * Past the far cascade the transmittance fades to a rough mean from the coverage (not measured).
  * **Consumers** multiply their sun term by `cloudSunTransmittance(worldPos)`: the lit core (lit, masked,
    transparent, terrain; forward set binding 22), the ocean (`sunTint`, so glint, body, foam and SSS), the
    terrain wet film (its own `sunTint` copy: in-scatter, reflection fog, mirror hits; AND its own sun
    visibility tap in main, `g_sunVisSurface` -> `sunSurfaceRadiance()`: glint, whitewater and the bubble
    cloud - both lacked the cloud term until 2026-09-29, and the film's foam glowed brighter than the ocean
    under cloud shadow. `g_sunVisSurface` already HOLDS the cloud and the eclipse, so never multiply it by
    `sunTint`: that squares the cloud shadow - the film's bubbles and foam did, darker than the ocean's, 2026-09-30.
    The tap sits at the WATER SURFACE (`in_pos`, up normal), not the ground under it (`TERRAIN_LIT_POS`,
    ground-normal gate), since 2026-09-30),
    the fog scatter (13 — the shafts through cloud gaps), LIT particles
    (draw set 11, vertex stage), LIT decals (6), and the cloud march's self-shadow (9; outside the map it
    falls back to the sun march).
  * **GI under cloud shadows: dimmed at the LOOKUP, not in the trace.** The trace's gather-hit sun has NO
    cloud shadow (the trace does not bind the map). Cloud-shadowed hits made each probe lag a moving cloud
    shadow by its update interval, so the GI flashed under shadows that fall AWAY from the camera (long
    intervals; a cloud straight overhead did not show it). Instead every probe keeps its **sun DC luminance**
    (probe vec4 `[6].x`, `GI_SUN_V4`: the direct sun at gather hits + the sun share of the multi-bounce +
    the sunlit-ground term `skyGroundSun` of every DOWNWARD MISS (past `rayMax` or the TLAS range: the sky
    map below the horizon is a sunlit ground; left out of the sun part, each visit's random downward misses
    added undimmed sun under a cloud shadow - probe flashes at shadow borders, worst with a low sun) + the
    UPWARD misses inside a cone around the sun (all sun within 15°, fading out by 30°: the Mie aureole and
    the sunlit cloud edges, which the shadowing cloud blocks at a shaded point),
    blended at the SH's alpha), the bake stores the **sun fraction s × W** in the tail image's `.b` (RGBA16F),
    and `giIrradiance` returns `E × (1 − s (1 − T))` with `T = cloudSunTransmittanceSoft` — the FAR cascade
    only (~8 m texels, one fetch): the bounce comes from tens of metres around, so the soft value fits.
    Indoors s ≈ 0, so a roof is not dimmed by the cloud above it. `evalProbeCoverage` stays UNDIMMED (it
    returns s as well): the trace's multi-bounce needs the undimmed value. The dimming compiles in only
    when the includer has `cloud_shadow.inc.glsl` BEFORE `gi_probe.inc.glsl` and `CLOUD_SHADOWS` is set.
    s is the DC fraction (not per direction) and one scalar for RGB: the warm sun bounce and the blue sky
    part are dimmed alike. **Measured (2026-09-27): no register or local-memory change** in the lit, terrain,
    ocean, decal, particle, fog scatter, trace or bake pipelines.
* **THE SKY CLOUDS** (`cloud_sky.cs.glsl`, `CloudPipeline::recordSky`, in the primary after the shadow map
  and before GI): one invocation per sky-map texel, `cloudRaymarch` with 64 steps, written as (in-scatter,
  transmittance) into a lat-long image on the sky map's own texel grid; the sky-map bake composites it
  into its GI and mirror layers. So the ocean / wet-film reflections, the GI miss rays, the sky SH and the
  forward pass's sky ambient all see the clouds (overcast darkens the ground's sky light). **TEMPORAL**
  (2026-09-29): 64 steps are far too coarse near the horizon (the steps grow to kilometres; the ocean reflects
  mostly those directions), and the samples sit relative to the camera, so a FAST camera slid them through the
  noise and the reflected sky changed colour every frame. Each frame now marches with a new jitter (per-texel
  hash + golden ratio) and blends into the texel's own last value (the image is read-write, cleared to "no
  cloud" at creation). The history weight is FRAME-TIME based: `u_cloudShape4.z = exp(-3 dt / T)` with T =
  "Sky/Clouds/Quality/Sky map history (s)" (default 1; 95 % of a change after T seconds at any frame rate; real
  time, so it still converges while the sim is paused; 0 = no history). **PROGRESSIVE:** each frame marches
  ONE texel of every 2x2 block (rotating phase, the shadow map's order; `CloudPipeline::SKY_UPDATE_FRAMES` = 4,
  so the CPU's dt spans 4 frames), and only the UPPER hemisphere is dispatched (rows [0, H/2); the lower half
  stays "no cloud" from the clear). 4096 threads cannot fill the GPU, so the pass is now bound by its longest
  ray's serial chain, not its thread count. **The observer
  stands on the GROUND under the camera** (`ATMOS_OBSERVE_HEIGHT`), not at the camera: every reader of the
  sky map sits under the clouds, even while the camera flies above them. The ambient reads the sky map's
  CLEAR layer from LAST frame (no feedback loop through the clouds' own image).
* **The visible sky is seen from the camera altitude** (`sky.fs.glsl`, `atmosphereScatter`'s observer
  height): the air above thins, and the horizon dips (`cosHorizon`). Below the dipped horizon the march ends
  at the ground, so the pixel is the haze in front of it plus the lit ground through the march's
  transmittance; the sun disc, moon and stars are left out there. The view optical depth is
  `atmosSegmentOD` / `atmosRayBegin` + `atmosRayOD` (atmosphere.inc.glsl; the per-ray form keeps the
  origin's Chapman values, so a march pays one evaluation per step), stable for descending rays (see the cloud haze fix: a "to space"
  depth through the planet is float noise). The sky MAP and every indirect path keep the ground observer.
* **Known limitation:** particles and transparents draw before the apply, so one inside a cloud gets the
  cloud of the opaque pixel behind it — the fog apply has the same limitation.
* **The three toggles are BAKED defines**, injected into every shader compile by `buildLayoutPreamble` from
  `RendererVKLayout::g_cloudShaders` (`Renderer::syncCloudDefines`): `CLOUDS` ("Sky/Clouds/Enabled"),
  `CLOUD_SHADOWS` (+ "Shadows/Enabled"), `CLOUD_SELF_SHADOW_MAP` (+ "Self-shadow from map", off by
  default), `CLOUD_POWDER` ("Lighting/Powder" above 0; the strength stays in the UBO), `CLOUD_CHECKERBOARD`
  ("Quality/Checkerboard"; also sizes the march dispatch, which the reload re-records) and `CLOUD_DEBUG_MODE`
  ("Quality/Debug mode": the march / temporal debug views compile out at 0). A change of a DEFINE waits for
  the GPU and reloads every shader, like the GI grid tweaks; `syncCloudDefines` returns whether one changed,
  so dragging the Powder slider reloads only when it crosses 0.
* **The aerial perspective's distance is NOT the weighted cloud distance:** the loop also sums
  `absorbed * exp(-kAir * t)` (kAir = the ray's mean air extinction), and the haze is taken where the air
  transmittance equals that in-scatter-weighted mean. A flat ray at the camera's altitude holds the fog
  around the camera AND clouds tens of km out; at the weighted distance the near cloud's light was dimmed by
  the far cloud's air - a dark band at the camera's altitude (gone with "Max distance" 20 km, absent in
  "Density only").
* **Register budget** (pipeline stats, 2026-09-27; registers / local bytes): march 56/0, sky clouds 48/16
  (the driver's +-16 B split; a 32k-thread pass),
  temporal 43/0, shadow map 36/0, cloud apply 39/0; the fog apply 48/0 with clouds (44 without). The cloud
  lookup adds nothing to the lit, terrain, ocean, fog scatter, GI trace, particle and decal shaders. Two rules
  came out of it: **the march applies its four light colours AFTER the loop** (the in-scatter is linear in
  them, so the loop sums three scalar weights: 56 -> 48 on the sky pass), and **the fog apply folds the
  cloud part before the scene's fog** (4 live values across the second `fogTo`, not 8: 56 -> 48). What a define
  cannot hold stays a runtime UBO flag: `u_cloudShape0.w` = the march ran this frame (the game suppresses
  it), `u_cloudShadow4.x` = the map was rendered this frame (suppressed, or the sun at the horizon).
* **Debug mode** ("Sky/Clouds/Quality"): step count heat, density only, history rejection.

## Far-tree volume (`TreeVolumePipeline`, "Trees/Far ..." tweaks, `FarTreeParams`)

**PROTOTYPE (Docs/TreeRenderingPlan.md T2 / P1), desktop only** (on by default; the user's 2026-10-02 pick:
start 500 m, end 20 km, overlap 32 m, 2048 × 1024 × 10 slices over 22 m (angular: keep a power of two - at 3072 a
band showed along u = 1/3, cause not found), checkerboard on, blob shrink 0.433, step scale 0.85, 500 steps, ambient
1, sun 1, self shadow 4, normal strength 1, ground darkening 1, interior shadow 2 / radius 0.077, forward scatter
−0.1, albedo 1, rebake 64 m). The FAR trees —
beyond the billboards, `Far start` to `Far end` — as ONE marched volume:

* **The volume:** camera-centred (snapped: re-baked once the camera leaves `Far rebake distance` of the bake
  centre, or a tree set / a geometry setting changes), **POLAR** (`tree_volume.inc.glsl`): image x = the angle
  around the centre (`Far angular resolution`; it WRAPS — the sampler repeats u), image y = `log(r / rMin) /
  log(end / rMin)` (`Far radial resolution`), so nothing is stored inside the ring and the cells grow linearly
  with the distance (a constant angular size). Image z =
  the height above the column's TREE FLOOR, `[0, Far height]`: an R32UI 2D image, the base of the column's
  **DOMINANT** tree - the one whose tent covers it most (floor pass 1, `m_floorCover`: the largest coverage per
  column, atomic max; floor pass 2: the trees within 4 quantization steps of that coverage write their base, the
  lowest among them - NOT an exact match: the passes are separately compiled variants whose float math can round
  a tree's coverage a few bits apart, and with an exact match some columns got no floor at all and the splat
  skipped them: radial gaps in the volume, a moire changing with every rebake). The
  LOWEST base of any tree reaching the column put a cliff-top tree above the layer in its edge columns, where the
  splat moved it down - blobs out of line with their billboards at every cliff (2026-10-02; a larger `Far height`
  hid it, messily). A column only the rectangle's **ring** reaches (coverage 0) keeps the lowest base there
  (atomic max of inverted order-preserving bits;
  0 = no tree; the ring: the march filters the density bilinearly but reads the NEAREST column's floor, so a
  filter-reached column without a tree floor fell back to the height map and put the leaked density tens of
  metres off - thin hatched spikes above the crowns, 2026-10-02; and the march's PRIMARY sample,
  `densityAtColumns`, filters per column: the 4 columns around the point, each read at its own height above its own
  floor, blended bilinearly - the hardware filter put a neighbour's crown in at this column's height, a spike over
  every steep floor step at peaks and slopes; the lighting taps keep the cheap `densityAt`), written
  by the splat shader's `TREE_FLOOR_PASS` variant before the splat. Splat and march both measure from it (the
  march: the nearest column; a column without a tree falls back to `terrainHeightAt`). **Not the height map**:
  its far cascade's ~132 m texels put the ground tens of metres off on mountains, and trees fell out of the
  15 m layer — missing far trees on mountainous terrain that popped in as billboards (2026-10-02; the user's A/B:
  a larger `Far height` brought them back). A tree that would reach past the layer's top (a lower tree set the
  column's floor) is moved down into it, as far as its base allows. The march's under-ground break uses the
  LOWER of floor and map, its above-layer skip the HIGHER, capped at 4 cells. R16F extinction (1/m) + an RGBA8
  colour map. 2048 × 512 × 16: ~100 MB (R32UI accum + R16F + colour). (A SEPARABLE per-axis warp was built first
  and dropped the same day: it must keep fine cells over the whole inner square, thousands of wasted texels per
  axis at a 5 km start.)
* **Bake** (`tree_volume_splat.cs` → `tree_volume_resolve.cs`): one workgroup per tree of every GPU tree set
  (`createTreeInstanceSet` uploads `TreeVolumePieceGpu` per piece + `TreeVolumeTypeGpu` per type + the types'
  float extinction mip chains — `TreeInstanceType::density`, box-filtered to 1³ on upload). Every texel the
  tree's box touches (+ one texel each way) samples the type volume at the mip matching the texel footprint, at
  the texel centre clamped into the box, × the tree's TENT weight horizontally (per polar axis, one cell wide
  each way, `tentWeight`) and the slice's box overlap vertically, and ADDS it. **The tent is load-bearing:** with
  the trilinear reconstruction it keeps a small tree's blob centred ON the tree whatever the grid's offset; the
  first (box-overlap) splat put it at the cell centre, up to half a cell (8-60 m) off, so every re-bake (a new
  grid centre) shifted every blob — "shaking" while flying. The splat ADDS with fixed-point `imageAtomicAdd` (crowns overlap → extinction sums). Past 65535 trees the
  dispatch wraps into y. The colour column takes the tree type's albedo (last writer wins).
* **The ring's inner radius is a FIXED `Far start` − `Far rebake distance` − 30 m** (horizontal), around the BAKE
  centre: the camera may move a rebake distance before the next bake, and the ring must still hold every tree
  past the hand-over.
* **The hand-over follows the camera's HEIGHT** (`Renderer::farTreesStart`): `sqrt(start² + h²)`, h = the camera's
  height above the ground under it (`setFarTreeCameraGround`, fed by TreeSystem from the terrain sampler; the
  baked sea level until set) — the 3D distance of a ground tree at horizontal `start`. The tree cull's
  volume start (`u_treeCullParams.z`, + overlap) and the march's start both use it, so from the air the hand-over stays at horizontal
  `Far start`, where the ring begins. (With the 3D `start` alone, the billboards vanished under a high camera
  with no volume there. Tried and dropped the same day: shrinking the RING with the height instead —
  `sqrt(start² − h²)` — which spread the log mapping's radial texels from ~10 m to the far end and smeared the
  crowns into stripes, also with an asinh mapping.)
* **The ray** is `vol_apply`'s `fogRay`: from `u_mvp`'s x / y / w rows (not `u_invMvp`, whose float32 error re-rolls
  every frame — kilometres out it moved the blobs) through this frame's TAA-JITTERED sub-pixel position, as every
  raster pass samples (an unjittered ray had TAA un-jitter content that never was jittered); the scene distance
  from `viewRelFromDepth` at the jittered uv. The `u_invMvp` ray was the visible "position jitter" (independent
  of resolution, step and TAA - the user's A/B, 2026-10-02); the jitter alignment is kept for correctness.
* **March** (`tree_volume_march.cs`, full res, every pixel, no temporal): from `Far start` (camera distance; at
  least the ring's entry, exact circle roots; a vertical ray never enters) to the scene surface or `Far end`; the
  **Lighting tweaks:** `Far sun scale` (the direct factor), `Far self shadow` (× the sun taps' optical depth),
  `Far normal strength` (the sun term toward `max(N·L, 0)` with N = −∇density by forward differences — the
  volume's "normal map"; 3 more taps per lit step, only when > 0; a crown-scale gradient blended into it was
  tried and removed 2026-10-02 - not worth its 3 taps), `Far forward scatter` (Henyey-Greenstein g),
  `Far ground darkening` (the sky light's drop toward the ground), `Far albedo scale`, `Far ambient`, and
  `Far interior shadow` / `radius` (the volume's "Foliage interior shadow": `exp(−strength × the mean extinction of
  6 taps at radius × the cell size around the sample × that distance)` on the sun and the sky — a blob's core is
  dense on every side; 6 taps per lit step, only when > 0).
  The volume FADES IN over `Far overlap` ON THE RESULT - of the BAND only: the ray accumulates two segments split
  at the fade end (`Far start` + `Far overlap`), the band fades as a whole by its own weighted mean distance, and
  the segment behind it composites under it unfaded (an opaque band skips to the fade end unless it is already
  fully in). (Fading each sample's DENSITY thinned a blob's front, the ray reached its dark core, and a half-faded
  tree read too dark. ONE fade for the whole ray let a ray grazing a band tree's blob beside its billboard pull its
  mean distance into the band and fade out the far trees behind it too: a sky-coloured outline along the
  billboard silhouettes - 2026-10-02.) and `Far blob shrink` (1/m)
  comes off the baked extinction first, so a blob shrinks toward its dense core. Steps of one cell × `Far step
  scale` (default 0.85; so they grow like the cells), the WHOLE ray shifted by a per-pixel, per-frame fraction of
  its first step (TAA averages it — the steps are measured from the camera and slide through the volume as it
  moves; jittering only the first sample left the rest on that sliding grid and the blobs shimmered), the height
  gap above the volume's top. Per sample: `albedo / π ×` (the sun × 0.6 (a leaf's mean cosine) × its
  transmittance through the crown (3 taps out to 14 m) × the terrain's sun visibility (`terrainSunVisibility`,
  once per ray) + a COOL sky term, `Far ambient` × a blue tint × the sun's luminance, darker near the ground) +
  `u_ambientColor`. (First version: `2 × L.y` full sun + a warm ambient from the sun colour - the crowns read flat
  and yellow.) Ends at transmittance 0.01. Out: in-scatter + transmittance (RGBA16F) and the transmittance-weighted
  mean distance (R16F, m); the temporal variant instead writes log2 distances in `cloud_temporal`'s format
  (RGBA16F: x = the first tree, y = the mean, z = the march limit).
* **Temporal** (`Far temporal blend`, the history's weight; **default 0 — and 0 costs NOTHING**: the plain march
  variant writes the slot's result (R16F linear distance) directly with the original barriers, the temporal
  images are freed and the two temporal pipelines are compiled only at the first use; the user tested: the
  full-res march is stable without it, also at larger steps - it is for half res / checkerboard later). On: the
  `TREE_TEMPORAL_OUT` march variant writes a raw pair (log2 distances), then `cloud_temporal.cs.glsl`'s
  **`TREE_TEMPORAL` instance** (history distances per slot; it also writes the R16F distance the fog apply reads) —
  the CLOUDS' pass, shared rather than copied (2026-10-02, the user's choice): full res, no checkerboard, no wind,
  the weight from the push constant. It reprojects the previous slot's result at the mean distance, rejects it
  when that distance leaves the neighbourhood's range or the march limit changed (a silhouette), clamps it to
  the neighbourhood and blends. The history counts only when the previous frame marched (`frameNumber`).
  **`Far pixel skip`** (Off / **1 of 2 (checkerboard), the default** / 1 of 4) **needs no pass on its own**: one
  thread per BLOCK - a horizontal pair, the marched pixel alternating per frame and row, or a 2x2 block, the marched
  pixel cycling through it over 4 frames - marches ONE pixel: the first of its schedule whose surface lies FARTHER
  than `Far start` (a nearer pixel's result is exactly "no trees" - the march would stop before the volume - so it
  is written directly; a block with no far pixel marches nothing). At a near crown's alpha-tested fringe (leaf and
  gap alternating per pixel and per frame) the background side is then marched this frame and donates to the
  block's other far pixels; marching the scheduled pixel blindly left the gaps only leaf-side copies - a bare-sky
  speckle. The march writes into the persistent **latest-march images**
  (`m_latest` / `m_latestDepth`, bindings 8 / 9: render size, not per slot - a skipped pixel was last marched up to 3
  frames ago; only while the plain path skips), and COPIES the block's other pixels from them - 1 to 3 frames late
  under camera motion (no reprojection). A RESTART (the first frame, a resize, a mode change, a frame without the
  plain skip: `m_lastPlainMarchFrame` / `m_lastSkipMode`) clears them to "no trees, distance 0", which fails every
  copy test. The copy must fit THIS frame's surface: the TAA jitter moves silhouettes by up to a pixel per
  frame, and a copy marched against the other side of an edge brought the bright outline back. With trees (T <
  0.999) the surface must lie behind their mean distance (×0.7); without, not farther than last frame's march
  reached (×1.5) - so the plain variant stores, for a tree-less pixel, its march LIMIT as the distance (the scene
  distance, sky = 65000 m; the composites read the distance only where trees are). **A failed test never
  marches** (a second march stalled the whole warp - with it the checkerboard cost MORE than off): trees in front of
  a nearer surface → "no trees" (exact); a surface farther than that march reached → MORE DONORS through the same
  test (`copyFits`): the block's marched pixel (this frame), then the 4 direct neighbours' latest marches, and
  only if none fits the marched pixel anyway. (Taking the marched pixel blindly gave a bare-sky halo along every
  near silhouette whenever it sat on the near side - worst at 1 of 4.) The wide tolerances keep the TAA jitter on grazing terrain (more than a
  few % per sub-pixel) from counting as a border. ONE `marchAt` call site (two inlined copies raise every warp's
  register count). The barrier before the march also orders last frame's latest-image writes (and a restart's
  clear) before this frame's reads. (Tried first: copying this frame's NEIGHBOUR - a bright outline at every
  silhouette against the volume; then marching the pair at depth edges - correct, but a warp with one edge pixel
  paid the double march, and the checkerboard saved little; then the previous SLOT as the copy source - fine for 1
  of 2, but it holds one frame only.) Under the temporal path the pass reconstructs instead, and 1 of 4 runs as 1
  of 2 (the pass knows the checkerboard only). **The scale and the pixel skip are BAKED** (`TREE_MARCH_SCALE` /
  `TREE_MARCH_SKIP` in the march, `TREE_TEMPORAL_SCALE` / `TREE_TEMPORAL_CHECKER` in the temporal pass), so the
  dead paths and their registers go: `prepare()` rebuilds a variant when its define set changes (one GPU drain per
  toggle; a failed rebuild keeps the old one), and `record()` dispatches by the BAKED values
  (`m_plainBakedSkip`, `m_temporalBakedScale` / `Checker`), never by the live settings. The march's push block is
  full (128 B; two pad words where the runtime flags were).
  **`Far half res`** (off) - or the blend - runs this path (`FarTreeParams::temporalPath`): the march at the temporal images' SCALE (2 = one ray per 2x2 block, its centre,
  to the block's FARTHEST surface) and/or this frame's checker parity only (the dispatch covers half the columns,
  as `cloud_march`). The temporal pass takes the scale, the checkerboard, the far end (the unmarched pixels' limit)
  and whether to write the R16F distance as push constants - the clouds keep their baked / UBO values through the
  same `TT_*` macros. The march's stored LIMIT is `min(scene distance, Far end)`, the rule the pass applies to an
  unmarched pixel. At half res the history colour is a half-res image per slot, and **`tree_volume_upsample.cs`**
  (`cloud_upsample`'s depth-aware 2x2 rules, copied) writes the slot's full-res pair, so the three composites
  read the same images in every mode. A far crown is ~1.5 px at 10 km: half res softens it.
* **Apply — FOG OFF:** without clouds, the "Far trees apply" scene stage: `colour + scene × T`. **With clouds,
  the cloud apply composites the trees itself** (`cloud_apply.fs`, bindings 4 / 5) - the two layers front to back
  by distance, `vol_apply`'s layering without the fog - and the tree stage is skipped. (Composited separately, the
  clouds came AFTER the trees: the volume writes no depth, so the clouds' march limit was the terrain behind the
  trees, and clouds between the two drew over them - 2026-10-02.)
  **FOG ON: the fog apply composites the trees itself** (`vol_apply.fs`, bindings 9 / 10; `u_foliageParams4.w` =
  it marched this frame — the fog apply is a cached secondary, so the flag rides the UBO), at the march's
  transmittance-weighted MEAN DISTANCE (`exp2` of the distances' y): over the finished trees, the fog fogged them at
  the scene depth — the terrain BEHIND them, km farther — and they read twice as hazy as the billboards beside
  them. Trees and clouds are two layers composed front to back by distance (the cloud formula per layer).
* **Hand-over:** the tree cull gets `u_treeCullParams.z` = `Far start` + `Far overlap`: a tree whose CENTRE lies
  past it drops its mesh AND its billboard from MAIN (the billboard stays the shadow caster). Over the overlap band
  both draw while the volume fades in — an overlap, not a seam; the billboards switch off at its end. (A dithered
  billboard fade-out over the band was tried and removed 2026-10-02 at the user's request.)
* Not yet: per-tree species colour beyond "last writer", half res, the analytic tail (P7), placement
  beyond the grove, a dithered billboard fade-out at the overlap's end.

## TAA off bypasses the pass completely

The secondary CB is not recorded, the dispatch and its GPU scope are skipped, and eye adaptation and
composite sample this frame's SceneColor directly (`resolvedLayout` SHADER_READ_ONLY instead of TAA's
GENERAL storage image; **the scene-colour barrier gains the fragment stage**, since the composite then
reads it).

> It used to only zero the feedback, so a disabled TAA still ran a full-screen resolve that copied —
> **a "TAA" scope in the profiler for a pass doing nothing.**

The tweak carries `onReRecord`, so toggling rebuilds the descriptors either way.

## DLSS (NVIDIA Streamline 2.14.1, "Post/DLSS" tweaks)

DLSS Super Resolution + DLAA, **desktop only; any mode but Off REPLACES TAA** (`taaActive()` = TAA on AND no
DLSS; `resolveActive()` = either, the old "TAA on" test of the post chain). No frame generation / Reflex.

* **Optional, never linked** (`RendererVK:Streamline`): `sl.interposer.dll` is signature-checked
  (`sl_security.h`, hence `wintrust` + `crypt32`) and `LoadLibrary`'d from the exe's folder; it loads
  `sl.common.dll`, `sl.dlss.dll` and the model `nvngx_dlss.dll` from there (the App POST_BUILD copy). No DLL,
  no NVIDIA GPU, VR, or any SL failure = the plain path, and `Streamline::dlssAvailable()` is false.
* **MANUAL HOOKING.** `Streamline::load()` runs BEFORE the instance (slInit needs to precede it), the
  instance / device take DLSS's extensions, 1.2/1.3 features and extra queues (`appendInstanceExtensions`,
  `appendDeviceExtensions`, `mergeDeviceFeatures`, `extraGraphicsQueues`), `onDeviceCreated` calls
  `slSetVulkanInfo`. **While SL is loaded the five swapchain calls MUST go through its proxies**
  (`Streamline::createSwapchain / destroySwapchain / getSwapchainImages / acquireNextImage / queuePresent`,
  used by SwapChain and Framebuffers) - SL's per-frame `presentCommon` hangs off them. A new swapchain call
  site uses the wrappers too. `slShutdown` is in `Device::destroy`, before the device. No OTA: the shipped
  `nvngx_dlss.dll` is the model. `eUseFrameBasedResourceTagging` is REQUIRED: without it SL rejects
  `slSetTagForFrame`. `sl.common` always loads `NvLowLatencyVk.dll` (Reflex), so it ships too. Its warnings
  "Hook sl.common:Vulkan:CmdBindPipeline / CmdBindDescriptorSets / BeginCommandBuffer is NOT supported" are
  expected: they are command-buffer state-tracking hooks, which manual hooking does not use.
* **The SL log file** (and NGX's, when it writes one) is in `Assets/Local/Streamline/`; the console gets only
  SL's warnings and errors. "Verbose log (restart)" (saved) sets SL's verbose level, read once at slInit.
* **The OUTPUT must start at (0, 0)**: SL never sets NGX's `DLSS.Enable.Output.Subrects`, and NGX fails the
  evaluate (0xbad00005, "the output subrect base must be set to 0 ...") for any other output offset. So a viewport
  at the origin (the game) is written straight into TAA's resolved image; any other (the editor panel) goes to
  `DlssPipeline`'s swapchain-size output image at (0, 0), then one `copyImage` to the viewport's place (hence
  TRANSFER_DST on the resolved images, and the copy stage in `beginExternalWrite` / `endExternalWrite`). The
  INPUT sub-rects (colour, depth, motion vectors) are fine at any offset.
* **Two more NGX `InvalidParameter` traps** (both from the sl.dlss source): SL describes a Vulkan
  image to NGX as a COLOR subresource unless an `sl::SubresourceRange` is chained to it, so the depth tag
  carries one (`Streamline::Image::aspect`); and SL creates the NGX feature at the optimal render size for the
  output size, so with DLSS active `m_renderRect`'s SIZE is `getDlssRenderSize(mode, viewport size)` (only its
  origin scales), never the rounded swapchain ratio, which could come out 1 px larger than the feature.
* **RENDER RESOLUTION.** The render-size targets - SceneColor (colour, depth, motion), RTAO, the clouds, the
  force interval target, the DLSS motion vectors - are `m_renderExtent` = the swapchain extent x
  `m_renderScale` (the DLSS optimal size for the whole swapchain; 1 for Off / DLAA). The scene draws through
  `m_renderRect` = `m_viewportRect` x the same scale, and **the UBO's `u_screenSize` / `u_viewportRect` are the
  render target and the render rect**, so every scene / screen-space shader works unchanged. Everything
  after the resolve - TAA's resolved image (the DLSS output), motion blur, eye adaptation, bloom, composite -
  stays at the swapchain extent and `m_viewportRect` (the CPU passes it; nothing post reads the UBO rect
  except the motion blur, which is off while upscaling). The jitter is one RENDER pixel. The projection
  aspect and `m_mipPixelScale` stay on the output viewport. **A new render-size image joins
  `recreateRenderTargets()`; a new scene pass sizes from `renderExtent()` / `m_renderRect`, never the
  swapchain.**
* **The renderer never reads the "Mode" tweak live**: `m_dlssMode` is the APPLIED mode, set only in
  `updateRenderExtent` with the sizes it implies. The UI writes the tweak a frame or more before its `onChange`
  runs; reading it live rendered one frame with the new mode and the old sizes (NGX creates the feature at the new
  optimal size, the input is bigger: 0xbad00005) or, from Off, executed the never-recorded DLSS secondary
  (device lost, black screen).
* **Mode change** ("Mode" tweak): GPU idle + `applyRenderResolution()` - the targets re-created only when the
  size changes, the DLSS history reset, the scene texture sampler's LOD bias = log2(render / output) ("Mip
  bias"; the static-mesh sampler only: materials + terrain; decals keep 0).
* **Per frame** (`recordDlssEvaluate`, in the primary, where TAA would run): the cached `DlssPipeline`
  secondary (`dlss_mvec.cs.glsl`: full motion vectors - the motion target has object motion only, so a w = 0
  pixel reprojects through the camera from its depth, RG16F render px, current -> previous, unjittered; plus
  the ocean bias mask below),
  `TaaPipeline::beginExternalWrite`, `Streamline::evaluateDlss` (options only on a change, a frame token, the
  constants, the four tags `eValidUntilEvaluate`, `slEvaluateFeature` into the primary), `endExternalWrite`.
  Auto exposure is on: the engine's exposure is computed after the upscale. No command buffer state to restore:
  every pass after it is a secondary.
* **Jitter sign**: DLSS gets the engine's NDC jitter in render pixels with y down: (+x, -y) x size / 2.
  Verified on screen with an A/B: the UE convention (-x, +y) wobbles and softens the image.
* **The ocean** (no motion vectors: dual-source blend) gets TAA's ocean feedback cap as DLSS's
  **bias-current-colour mask** (`kBufferTypeBiasCurrentColorHint`: lerp(history, current, bias)): the mvec pass
  also writes an R16F mask (NOT R8_UNORM: SL's Vulkan format table has no entry for it and logs "Cannot have
  undefined format" - a new tagged format must be in `sl.chi` `Vulkan::getFormat`), "Ocean current bias" (0.8 = TAA's 0.2 history weight) on TAA's ocean flag pixels
  (scene colour alpha < 0.004, depth > 0), 0 elsewhere. It reads the scene colour (only where depth > 0: the
  sky is never ocean), so it runs after the colour's barrier to the resolve.
* **Preset default M** (`Post/DLSS/Preset`; sl_dlss.h: L / M are the newer models with less ghosting, M near K's
  cost). DLAA's own default, K, SMEARED the trees' alpha-tested foliage under camera rotation while TAA did not
  (the user's A/B, 2026-10-02: not the edge-fade dither, not the far volume; L and M clean). A far-tree bias in
  this mask was tried first and removed: it did not help.
* Not while upscaling: **motion blur** (`motionBlurEnabled()`: its velocity and gather assume one resolution).
  **Under DLAA the mvec pass writes the motion blur velocity + sub-tiles** (the side product TAA writes;
  `motion_blur_tiles` runs only with no resolve at all: `velocityPass = !resolveActive()`). The sub-tile grid
  starts at pixel 0, so with the blur on the pass dispatches the WHOLE target (`pc.base` = 0, as TAA), otherwise
  only the render rect. Its opening barrier's source stages include the fragment stage (the composite's gather).
* The GPU profiler shows the mvec pass as "DLSS mvec" and the upscale as "DLSS". Measured 2026-09-28
  (RelWithDebInfo, sandbox, RTX 4090, DLAA 1920x1080 with the fused motion blur): DLSS mvec 0.032 ms, DLSS
  0.314 ms, the motion blur's neighbour pass 0.012 ms.
* **A viewport that does not fit the swapchain skips the upscale** (the editor layout before it adapts to the
  window - seen on a command-line start: 1983x1237 in 1920x1080). Everything else crops it, but DLSS cannot:
  the render rect clamps below the feature's dynamic range (NGX InvalidParameter), and **`slEvaluateFeature`
  still returns OK when NGX fails inside it**, so the output copy ran out of bounds: device lost.

## Motion blur (`MotionBlurPipeline`, "Post/Motion blur" tweaks)

Desktop only (`motionBlurEnabled()`: off in VR, and bypassed when off - its pass is not recorded, not executed,
and TAA / the composite skip their parts). **FUSED into the passes around it: it has no full-res pass of its
own** (measured 2026-09-28, sandbox, still camera: motion blur 0.065 -> 0.013 ms, TAA +0.004, composite +0.002,
GPU frame -0.04..0.05 ms). The shared code is `motion_blur.inc.glsl`; **eye adaptation and bloom keep the
unblurred colour.** Its images (RG16F) are used within the frame only - one set for every slot, GENERAL for life.

1. **TAA writes the velocity** (`motionBlurVelocity`; TAA already reads the depth and the motion target; under
   DLAA, DLSS's `dlss_mvec` pass does the same - see "DLSS") and
   the longest velocity per 8 x 8 SUB-tile (`MOTION_BLUR_SUBTILE`, TAA's workgroup; a shared-memory reduction
   at the top of the shader, before any early return). Velocity = (this - last frame) uv x "Shutter" in px,
   clamped to 2 x "Max radius": the camera part from the depth (`prevScreenUVClip`), the object part = the
   motion target - the camera part, so "Camera motion" scales the camera's share alone; a frame motion over
   ~30 % of the screen diagonal is a cut or a teleport: no blur. With TAA OFF, `motion_blur_tiles` does it.
   TAA's opening storage transition (source stages compute + fragment) orders last frame's reads first.
2. `motion_blur_neighbor` (the "Motion blur" scope) - per `MOTION_BLUR_TILE` (32) tile the longest velocity
   of its 3 x 3 tiles, read straight from the sub-tiles. **The radius is capped at the tile size**, so that
   neighbourhood holds every blur that can reach a pixel: that is what lets a moving object smear past its
   silhouette.
3. **The COMPOSITE gathers** (`motionBlurGather`): a pixel whose tile neighbourhood moves runs McGuire et
   al. 2012's reconstruction filter, "Samples" taps along the neighbourhood velocity (interleaved-gradient
   jitter per frame), each weighted by whether its blur covers this pixel or this pixel's blur covers it, with
   soft depth tests (2 % of the distance, on 1 / the scene depth) choosing the front one; any other pixel reads
   the resolved colour once, as without the blur. So no blurred image is written or read. The composite binds
   the frame UBO for it (binding 6, `UBO_BINDING`).

"Shutter" is the exposure as a fraction of the frame, so a higher frame rate blurs less (physical); > 1
exaggerates. The ocean writes no motion vectors, so its waves blur only with the camera.

## Colour

Everything upstream of composite is **linear HDR**. `EyeAdaptationPipeline` (log-luminance histogram
auto-exposure) feeds `CompositePipeline` (HDR → display into the swapchain before ImGui; tonemap
off / Reinhard / ACES / AgX). The histogram bins EVERY viewport pixel (the reduce divides by the area),
4×4 pixels per thread: a run of equal bins is one shared atomic and only non-empty bins go global. One
pixel per thread measured 0.155 ms at 1440p (Nsight: 43% long-scoreboard + 36% misc stalls on the
atomics). `PIXELS_PER_THREAD` in the shader and the dispatch's group size must match. A thread's block is
2 x 2 QUADS of 2 x 2 pixels, because each quad is also one texel of bloom level 0 (below).

## Bloom (`BloomPipeline`, "Post/Bloom" tweaks)

Desktop only (`bloomEnabled()`), in HDR before the exposure, in two modes:

* **"Threshold" > 0 (default 1):** a SOFT threshold (the Unity / Unreal knee, "Knee" half width) on the
  brightest channel in EXPOSED units (1 = display white before the tonemap), so only light that will show
  as bright goes into the blur, which the composite ADDS: `scene + blur * intensity`. Dark and mid tones
  never blur. The threshold uses LAST frame's auto exposure (the histogram pass runs before the reduce;
  eye adaptation's first barrier makes last frame's write visible) - the adaptation is smoothed over seconds.
* **"Threshold" 0:** the physical, energy-conserving `scene * (1 - intensity) + blur * intensity`: every pixel
  spreads a share of its light, so the whole image softens as the intensity rises (keep it small).
* **"Radius"** weighs the levels, level k by 2^(k (2 radius - 1)) (0.5 = equal; the default 0.75 favours the
  wide levels), normalized by the weights' sum (`getNormalize`). With equal weights the half- and quarter-res
  levels, barely blurred, read as a haze over the whole image.

**Built to add no full-res pass:**

* **Level 0 (half res) is written by the eye-adaptation HISTOGRAM pass**, which already reads every pixel of
  the resolved colour: each 2 x 2 quad of its block becomes one texel, a Karis average (weights 1 / (1 + luma),
  values clamped finite), so one very bright pixel cannot make a flickering blob; the threshold is applied
  to that average (once per quad). It reads TAA's output, not
  the motion blur's (bloom is wide anyway). Off = the pass skips the writes (a push-constant flag).
* **The chain** (`record`, after eye adaptation): downsamples to the smallest of "Levels" levels
  (`bloom_downsample`, Jimenez's 13-tap filter - stable under sub-pixel motion), then upsamples back
  (`bloom_upsample`, 3 x 3 tent + add, IN PLACE into the larger level, each level times its "Radius" weight -
  the smallest one on the first step). Level 0 then holds the WEIGHTED SUM of every level; the composite
  divides by the weights' sum.
* **The composite** mixes level 0 in with one bilinear fetch (`u_bloomUv` maps full-frame uv to level 0).
* ONE B10G11R11 image with a mip chain for all frame slots, sized to the full render target; each level uses
  only its VIEWPORT region (from the origin, taps clamped inside it), so a viewport change needs no re-create.
  Last frame's reads of level 0 are ordered before the histogram's write by eye adaptation's first barrier.

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

**Pass masks** `PASS_MAIN` / `PASS_SHADOW` / `PASS_GI` (Layout.ixx — the same bits Spatial uses). The
GPU word is the mask byte plus the PUSH FRAME above it (`InstanceStream::stampedPassMask`; the motion
vectors' "was this node pushed last frame" test), so a reader tests bits, never the whole word. Main
cull, shadow cull and the GI TLAS writer each early-out on their bit; TLAS also range-bounds by
`RT/TLAS Range`. **A node carries its own default mask** (`RenderNode::setPassMask`, a byte in the
padding, `PASS_ALL` unless set): `renderNode(node)` pushes with it, `renderNode(node, mask)` overrides
it. `renderNode` returns at once for a 0 mask and for a node with no instances (never spawned, or
destroyed — `freeRenderNode` clears them), so an owner can push a list of node pointers blindly.

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
  state base, the upload-state byte, the default pass-mask byte, local bounds and the mesh-instance vector. Everything else the
  push needs is derived from renderer tables: per-mesh instance counts from the instances themselves,
  an instance's LOD chain from `MeshLodRegistry::getGroupIdxForMesh` (instances reference the LOD0 mesh), the
  skinning palette from the bundle (`setSkinningPalette(node, palette)`). Do not add per-node side
  vectors.
* **Destroying a RenderNode recycles without GPU sync**: transform slots are free-listed, skinned
  bundles parked in place per container, and rebased sub-node offsets shared.
* `~ObjectContainer` frees ALL renderer resources (`Renderer::removeObjectContainer`).
* The container keeps its own copy of the source `Skeleton`, **so animators can retarget against it at
  spawn.**

## Single meshes (`RenderMesh`)

For generated geometry that lives and dies one mesh at a time with a SHARED material (terrain chunks,
ocean sectors), a container is pure overhead: its own material slot, node tables, name strings and
path map per mesh. `RendererVK:RenderMesh` is the lean path (main thread):

* `RenderMeshData::build(MeshGeometryDesc)` — pure, from any job: the final `MeshVertex` array (the
  container's tangent-handedness formula and null-attribute defaults), the indices, the bounds.
* `createMeshMaterial(pipeline, rayTraced)` — one permanent material per SYSTEM (fallback textures,
  opaque, `MATERIAL_FLAG_OCEAN` / `_TERRAIN` by pipeline, `NO_RAYTRACING` unless ray traced, BC5 flag
  from the fallback normal's format — exactly what a container override produced).
* `createMesh(data)` → a move-only RAII `RenderMesh`: the vertex/index uploads and ONE `MeshInfo`
  (through `addMeshInfos`, so the BLAS registers like any mesh). No LOD chain, no stream set. Offsets
  and counts are stored as 32-bit element units.
* `spawnMeshNode(mesh, material, pipeline, transform)` → a plain `RenderNode` with one instance on a
  shared identity instance offset (`m_identityInstanceOffsetIdx`, created at the first spawn). The
  instance's alpha mode is the MATERIAL's (the TLAS writer's opacity flag reads it).
* The `TreeImpostor` pipeline variant (12, opaque family): `tree_impostor.vs.glsl` + the `LitMasked` FS. Its
  quad mesh carries per-piece constants instead of geometry (Procedural "Branch-module impostors"); push its
  nodes with `PASS_MAIN` only — the shadow pass would draw the degenerate quad with its own VS.
* **BAKED TREE RECORDS** (`createTreeInstanceSet` / `renderTreeInstanceSet` / `destroyTreeInstanceSet`,
  RendererTrees.cpp + `tree_cull.inc.glsl`): a SET of placed procedural tree pieces sharing a table of piece
  TYPES (bark / bark-fade / leaves / leaves-fade / billboard representations + the crossfade band), uploaded
  ONCE to DEVICE-LOCAL buffers (`TreeCullPieceGpu` 64 B: transform + band centre/radius + type + LOD state
  base; `TreeCullTypeGpu` 48 B). Trees do not move, so nothing about them is written per frame: no render
  nodes, no stream entries, no pass of their own (the earlier `tree_expand` pass, which rewrote every piece's
  records into the HOST-VISIBLE stream each frame, cost ~7.5 ms of GPU). Per frame the CPU claims ONE instance
  range (3 records per piece: bark, leaves, billboard) and notes the bucket sizes once per distinct level-0
  mesh — nothing per piece. The UBO carries the range (`u_treeCull` x = base, y = count; **patched in
  `present()`** by `uploadTreeCullUbo`, like `giTlasNumInstances` — the UBO uploads in beginFrame, before the
  claim, and a 0 / 0 range made the culls read the never-written entries as instances: garbage mesh indices,
  out-of-bounds bucket writes, glitching terrain) and
  `u_treeCullParams` (x = far distance scale, y = forceFar, z = the far-tree volume's start + overlap, 0 = no
  volume). **The culls build the records**: a thread whose instance index lies in the range skips the stream
  and calls `treeCullMain` (main cull: the mesh / crossfade / billboard / none decision from the distance to
  the centre view — the same rules as the CPU preview) or `treeCullShadow` (shadow + rain-shelter culls: the
  BILLBOARD is the caster at every distance; a type without one casts its mesh). Record k's LOD hysteresis
  slot = the piece's `lodStateBase + k`; prev transform = none (w 0 — trees never move). The buffers are
  bound at bindings 20/21 (main cull) and 13/14 (shadow culls) — descriptors written at RECORD time, so the
  culls bind ONE set (`m_treeCullSet`, the set rendered; changing it or destroying it re-records; a second set
  in one frame asserts). With no set, `m_treeCullDummy` is bound. **In the TLAS** (GI, RTAO, RT shadows, the ocean / film
  reflections): `gi_tlas_instances` (bindings 9 / 10) builds a tree record's instance from the same static data
  with `treeCullShadow` - the billboard, or the meshes for a type without one - and, since the range's stream
  entries are never written, puts the MATERIAL in the custom index: **bit 23 set = a tree, bits 0..15 = its
  material** (a stream instance's custom index stays its index, below 2^23). Every ray-query consumer reads only
  the material from `in_instances[custom]`, so each takes it from the custom index when bit 23 is set
  (`rt_shadow.inc`, `gi_probe_trace`, `rtao`, `ocean.fs`, `terrain_film.fs`) - no extra binding anywhere. The
  geometry comes from the sbt-offset RT mesh index as for any instance; the billboards keep RTAO's mask 0x02. Destroying a set drains
  the GPU (rare: respawn / reload). Texture / mesh streaming notes are skipped: tree textures are pinned and
  `RenderMesh`es never stream.
* `createMeshLodChain(levels, errors)` — a GPU LOD chain (`addMeshLodGroup`) over `createMesh`'d meshes,
  level 0 first, errors in mesh-local units (0 for level 0; nonzero → the screen-space-error selector).
  `spawnMeshNode` on a chain's level 0 allocates the node's hysteresis slot itself, and the cull redirects
  each instance like a container chain. `freeMeshLodChain` BEFORE destroying the meshes.
* `createTextureMaterial(w, h, mips, alphaCutoff, name)` — a material with its OWN generated sRGB RGBA8
  diffuse from a caller-built mip chain (`TextureManager::uploadRgba8Mips`; no blit-generated mips, so the
  caller can keep alpha-test coverage per level). `alphaCutoff > 0` → `EAlphaMode::Mask` with the cutoff in
  `opacity` (the Mask discard's threshold); draw it on `LitMasked`. An optional `normalMips` chain uploads
  a LINEAR RGB tangent-space normal map (x along U, y along V); `extraFlags` adds material flags.
  **`MATERIAL_FLAG_BILLBOARD`** (bit 26, LitMasked only): the FS's early sun shadow is NOT rejected by the
  geometric normal's facing (`sunShadowFirstFoliage`: sampled from the sun side, the normal-mapped normal's
  facing test in `doSunLight` decides) — for flat cards standing for a foliage clump. Its shadow lookup uses
  the CONSTANT depth bias only (`SHADOW_FOLIAGE_BIAS` / `g_shadowFoliage` in shadows.inc.glsl: no slope scaling,
  no normal offset): a crossed billboard's back half sits in the crossing card's shadow by a small depth step,
  which the slope-scaled bias and normal push erased — that strip lit up through the tree. Even at ZERO bias a
  line stayed (the cards touch at the crossing line, so no bias separates them), so the lookup also moves OFF
  the card to the texel's BAKED DEPTH: the normal map's alpha, signed along the card's FRONT normal, in units
  of the card's u length. Both are rebuilt from the screen derivatives of position and uv (before the discard):
  d(pos)/du and d(pos)/dv, front normal = cross(dv, du) on both faces — so the instance scale needs no data,
  and a back face agrees with the front. Both passes scale the offset by `Trees/Foliage depth offset`
  (`u_foliageParams.x`, 0 = on the card plane). **The CASTER writes the same points** (pixel depth offset, as Unreal's
  / Amplify's impostors do): the shadow cull fills `OutShadowMeshInstance::foliageNormalTexIdx` /
  `foliageShift` (world bounding radius) for FOLIAGE materials (not for rain occlusion); `shadow_depth.vs`
  pulls those casters toward the light by the radius, and `shadow_depth.fs` adds back the radius plus the
  leaf's offset — always ≥ 0, so the FS declares `layout (depth_greater)` and the early / hierarchical reject
  stays valid for the whole pass. Every other caster writes `gl_FragCoord.z` (its depth without the write,
  the rasterizer's bias included). With the receiver only offset, card B stayed a flat plane in the map and the
  line stayed. (An up-sun SKIP was also tried and rejected: it only widened the lit strip.) **Transmission**:
  the leaves at their depths still left a HARD edge on the axis (half the crown sits behind the crossing card,
  and the map treats every leaf as opaque). `foliageTransmit` (shadows.inc.glsl, in `pcssCascade` /
  `pcssBorder`) keeps only `1 - exp(-gap / length)` of a foliage receiver's shadow, `gap` = metres to the
  PCSS average blocker (`cascadeDepthRange`), `length` = `Trees/Foliage shadow length (m)`
  (`u_foliageParams.z`, 0 = hard). ~0 on the axis, deepening into the crown; far blockers shadow fully. All
  foliage values live in `FoliageParams` (Settings.ixx; registered as "Foliage ..." in the TreeSystem's
  "Trees" tweak category) and ride `u_foliageParams` / `u_foliageParams2` (Ubo, after `lodParams1`). **Interior**: the leaf's real 3D point (card
  point + baked depth, returned by `sunShadowFirstFoliage`) against the crown sphere below — near its centre
  = seen through the gaps deep inside — darkens the sun visibility AND the ambient (`computeLitColor`'s
  `texAO`) down to `1 - Trees/Foliage interior shadow` (`u_foliageParams.w`), so a fully lit crown is not
  flat: full inside `Foliage interior inner radius`, gone outside `outer radius` (leaf distance / crown radius,
  default 0 / 1.1, `u_foliageParams2.w` / `u_foliageParams3.x`); on a whole tree's horizontal card the term is
  raised to `Foliage interior shadow top card scale` (`u_foliageParams3.z`, default 1; an exponent: > 1 darker -
  a strength multiplier saturated at strength 1), then × `|V.y|` (the view's steepness; plain `V.y` went negative
  from below and turned the card black). **Crown normal**:
  each crossed card's baked normals shade the crown side ITS bake view saw, so the shading split hard where
  card A gives way to card B on screen (it stayed with shadows off). `foliageCrownNormal` blends the shading
  normal toward the view ray's hit on a sphere on
  the card's axis — the line from the instance origin along +u, centred at the card's u centre, radius half
  the u length — which depends on the ray only, so both cards agree. The blend is 1 at the axis (where the
  cards disagree most) and falls to `Trees/Foliage crown normal` (`u_foliageParams.y`) at half the radius from
  it, so the outer crown keeps its baked detail. On a whole tree's HORIZONTAL card
  (`MATERIAL_FLAG_BILLBOARD_TOP_CARD`, bit 23, on the card whose normal points up - `foliageTopCard`; whole trees
  stand upright) `foliageCrownFrame` takes the card normal as the
  axis and the card's own point on it as the centre; the radius is half the u length on every card. The LitMasked variant's VS (defined
  `ALPHA_MASK`) and `tree_impostor.vs` add the instance origin as flat location 5. **Leaf transmission**
  (`MATERIAL_FLAG_LEAF`, bit 30, LitMasked: the tree leaf cluster material and the billboards — not FOLIAGE,
  which carries the billboard-only terms): after `computeLitColor` the FS adds the sun THROUGH the leaf, tinted
  by its colour — `saturate(-N·L)` (lit from behind) + `saturate(V·-L)^focus × glow` (the backlit rim looking
  toward the sun), × `Trees/Foliage transmission`. Its visibility = `mix(1, sun shadow, Foliage transmission
  shadow)` × the interior term: a leaf seen from the shaded side is in its own crown's shadow, which would
  leave no glow. A LEAF mesh pixel's shadow is looked up from its SUN side (`sunShadowFirstLeaf`: the bias normal
  flipped toward the sun) - `sunShadowFirst`'s facing reject gave every leaf facing away from the sun 0, exactly
  the ones the back term lights, so the shadow weight moved the whole tree as one (fixed 2026-10-02). `u_foliageParams3.w` / `u_foliageParams4` (focus, glow, shadow weight). **No RTAO either way**: a
  FOLIAGE instance gets TLAS mask 0x02 (`gi_tlas_instances.cs`) and RTAO traces with cull mask 0x01, so its
  opaque rays no longer hit whole card rectangles (every other ray uses 0xFF and still hits them); and the lit
  FS skips the AO read on a card (`FOLIAGE_NO_RTAO` / `g_noRtao`) — traced from depth, it was the flat card's
  occlusion by the crossing card and the ground. A foliage card also
  FADES OUT EDGE-ON (dithered, `smoothstep(start, end, |N·V|)` on the geometric normal, its own decorrelated
  pattern so it composes with the distance fade): a grazing card smeared its texture into bright streaks, and
  the crossed card faces the view right then. `Trees/Foliage edge fade start` / `end` (default 0.1 / 0.6, centre scale 1.33;
  `u_foliageParams2.xy`); near the crossing axis both are × `Foliage edge fade centre scale` (`.z`, back to ×1
  at half the crown radius, `foliageAxisDistance`), and on a whole tree's horizontal card also × `Foliage edge
  fade top card scale` (`u_foliageParams3.y`, default 1). **Not from below:** the fade blends out as the view
  turns upward (`smoothstep(0, 0.2, -V.y)`, the first ~11° below the pixel) - looking up into a crown, the
  edge-on cards are what fills it, and fading them left it see-through.
  **`MATERIAL_FLAG_DISTANCE_FADE`** (bit 25, LitMasked only; + `MATERIAL_FLAG_FADE_IN`, bit 24): a dithered fade
  over a camera-distance band packed into the LOW flag bits (start m in bits 0..11, width m in 12..21;
  `makeDistanceFadeFlags`). Fade-out keeps `dither < 1 − f`, fade-in `dither ≥ 1 − f`, so an out/in pair over
  the same band shares no pixel; interleaved gradient noise shifted per frame for TAA. `MaterialInfo` stays 16 B
  (it is redeclared in ~10 shaders). `deriveMaterial(src, flags)` copies a material sharing its textures
  (free with `releaseMaterial`, the slot only); `get/setMaterialFlags` rewrite flags in place (no re-record).
  `destroyTextureMaterial` queues the
  textures' free and releases the material slot. Used by the procedural tree leaf cards and bark.
* `~RenderMesh` frees the ranges and neutralizes the `MeshInfo` slot (`freeMeshInfoRange`), like
  `removeObjectContainer` for an unstreamed mesh. **Destroy its nodes first** (owners declare the
  mesh before the node).

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

# Draw-list compaction (`DrawCompactPipeline`, `draw_compact.cs.glsl`)

The culls (main, shadow, rain) accumulate into **PER-MESH-SLOT** sequences: one 24 B entry per registered
mesh, LOD levels and streamed-out meshes included (`out_indirectCommands[meshIdx]`, `atomicAdd` on its
`instanceCount`). Walking them all cost one front-end draw per slot and list: Nsight counted 66k draws in
Scene opaque, nearly all with 0 instances.

* Right after each cull's dispatch, ONE workgroup (1024 threads, subgroup ballots) moves the entries with
  `instanceCount > 0 && indexCount > 0` to the front of the SAME buffer and writes one count per list:
  main cull `[0]` opaque, `[1]` transparent, `[2]` tess ground, `[3]` terrain film; shadow / rain cull one
  count. The DGC executes take them as `sequenceCountAddress`, the tess draws as the `countBuffer`.
* **Slot order is kept**, so it is the same every frame: the transparent list is not depth sorted, and a
  per-frame order change would flicker its overlaps. That is why it is one workgroup and not a global
  atomic append.
* In place is safe: a kept entry moves to an index <= its own slot, the chunk's reads finish before its
  writes (barrier), and later chunks are never written. Past the count the entries are stale; the cull's
  full-capacity fill clears them next frame. **Nothing may read these buffers by meshIdx after the
  compaction.**
* No descriptor set: the buffers go in as device addresses (push constants). `meshCount[0]` (the CPU's
  registered mesh count) is the slot count it walks.
* Measured (sandbox, RelWithDebInfo): Static meshes 1.621 → 1.569 ms, GPU frame 3.075 → 3.019 ms (3-4 runs
  each, no overlap); the main cull + compaction 15 µs (17 µs before). Shadow draw unchanged (~0.36 ms).

---

# Layout under `Private/`

| Directory | Contents |
|---|---|
| `Objects/` | Thin Vulkan wrappers: Device, SwapChain, Buffer, ComputePipeline / GraphicsPipeline, AccelerationStructure, SceneColor (colour + THE scene depth), ShadowMap, GpuProfiler, BakedWorldMap, Texture, Shader, **VrEyeTargets** (the two per-eye LDR composite targets, re-created with the swapchain), ... |
| `Pipeline/` | One class per pass or feature: StaticMeshGraphics, GIProbe, RTAO, TAA, Dlss (its motion vectors), MotionBlur, Bloom, VolumetricFog, Cloud, EyeAdaptation, Composite, Skinning, DebugLine, Particle, Decal, ForceField, OceanSimulation, TerrainWetness, LightGrid, IndirectCull, ShadowCull (both own a DrawCompact), ShadowMapGraphics. **Each registers its own tweaks.** |
| `Data/` | The GPU-resident scene, carved out of the Renderer. Streaming and managers: MeshDataManager, TextureManager, TextureStreamer, MeshStreamer, StagingManager, ShaderDatabase, GpuCrashTracker (Aftermath, runtime-loaded, optional). Plus the four registries the Renderer owns and delegates to — each takes its frame-wide effects as callbacks (`onGpuIdle` before a buffer is re-created, `onInvalidate` to re-record) and knows nothing about the device or the pipelines: |
| | **`InstanceStream`** — THE per-frame push surface: the six mapped buffers `renderNode` writes into (transforms, pass masks, LOD bias, mesh instances, first instances, mesh count - the slots the draw-list compaction walks), one set per frame slot, the device-local PREVIOUS transforms + masks the motion vectors read (`recordPrevCopy`), plus the lock-free monotonic instance claim, the transform slot free list and the two capacity growths. **A claim past the capacity is never rolled back** — see the header. **BOTH growths run only in `checkFrameCapacities`, never mid-frame:** re-creating the node buffers after some nodes pushed dropped their transforms / masks / biases for that frame (a whole-scene one-frame flicker). A node spawned past the node capacity is skipped by `renderNode` until then. |
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
  vertex shader when it has none, extra variants add `#i`), every `GpuAllocator` image and buffer that
  passes a name, textures (their file path). Variants of one file with different defines share a name.
* **The same name is the VRAM view's box.** `GpuAllocator` keeps a registry of every live allocation
  (a vector under a mutex; each allocation's VMA `pUserData` holds its index, swap-remove on destroy).
  `Renderer::forEachGpuAllocation` visits it for the UI Memory panel's VRAM metric, which splits the
  name into a path (`/` or `\`, else `.`). **An unnamed allocation shows as `<unnamed>`**, so pass a
  name. The class is `GpuAllocator`, NOT `Allocator`: Core exports a global `Allocator` class too, and a
  TU that imports both resolves to the wrong one.
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
stats"** (off): pipelines created while it is on carry `CAPTURE_STATISTICS`, and
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
    `final = body * bodyWeight + mirror * mirrorWeight + C`, so the glint, SSS, the bubble cloud, the blur's
    sky share and the foam fold into one half `C` BEFORE the traces (see both shaders). The bubble cloud is
    `ocean_bubbles.inc.glsl`, shared by both; the ocean's parallax tap for its coverage runs
    before the sun shadow ray, so only one float of it is live across that query.
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
* **Bisected 2026-09-22 - NO single peak left** (regs/local, "demand" = regs + local / 4):
  * Lit core (lit 64/32, terrain ground 72/16, demand ~72-76): the terrain splat alone is 54 (63
    tessellated), so the peak is `computeLitColor`. Removing the PCSS search (64/48), the light shadow rays
    (72/0), the underwater sun transmittance (64/48), the relief sun gate (72/16) or the two-cascade border
    PCSS (a dithered cascade pick: 64/48) - EACH ONE alone - leaves the demand the same. Several sites
    reach it; the terrain pays ~4 registers over lit.
  * Ocean (80/16, demand ~84): no underside path 72/32, no light loop 72/32, no scene rays 64/64, no rays AND
    no seabed splat 64/48. Same pattern.
  * `gi_probe_trace` (96/0): the sun ray, the light loop and the bounce each add ~16 over a ~80 base; the
    hit path whole is 72. `[[dont_unroll]]` on the ray loop: 96. Shared-memory accumulators instead of the
    per-thread ones: 109 (RMW) / 108 (`atomicAdd`, VK_EXT_shader_atomic_float).
  * The AO upsample's tap arrays as index bits: terrain 72/0 but lit 72/48; fully unrolled: terrain 72/32.
  * Rebuilding `specularColor` / `diffuseColOverPi` from albedo + metalness per light (4 live values, not
    6): a BYTE-IDENTICAL binary - the compiler hoists the loop-invariant values back out.
  * What is left is structural: split a peak into its own pass (the film's overlay precedent), or a
    thread-per-ray trace.
* **Measured 2026-09-23** (the surface-water rework; regs/local):
  * **A dual-source OUTPUT written early costs registers.** The ocean wrote `out_factor = vec4(0)` at the
    top of main as the default for its early returns. With the edge fade's dynamic store at the end, that
    kept the output live through the whole shader: 80/32 against 80/16. Write every output AT each return,
    never as an up-front default. Constant stores at both ends fold, so the bug only shows once one store
    becomes dynamic.
  * The ocean's edge fade (`oceanEdgeCover`: terrain height + the wetness pool level, 5 taps) costs nothing
    once it reads no value of main's. A fallback to `shoreHW.x` alone kept that value live across the
    traces and the light loop. It sits after the underside path with its early-out: 80/16.
  * **The film's peak is its light loop's shadow rays**: `doLight` in place of `doLightShadowed` measured
    56/0 against 56/48 (demand 56 vs 68); without the loop, 48/0. Lights-first ordering (the RT mirror is
    off now) and accumulating straight into `color` measured the same 56/48. Shadowless film highlights
    would be the lever, a visual trade (a light behind a wall would glint on a puddle).
  * The ground's wet sky reflection costs the TESSELLATED ground 16 B (72/16 -> 72/32; untessellated
    unchanged): its inputs stay live across `computeLitColor`. Every split (weight before, R.xz across,
    the whole reflection before) measured the same or worse; see the comment at the reflection.
  * The terrain TES is 70/0 for both the ground and the film: the relief taps are 6 of it (64 without),
    the rest is `terrainLayers` (the climate walk, per tessellated vertex). Its per-layer indices cannot be
    interpolated across a patch, so moving it per control point is not a small change.
* **Measured 2026-09-29** (regs/local): lit FS **72/32** (#10 variant 72/48; it was 64/32 on 09-22), ocean
  **80/32** (was 80/16), film 56/48 (same), terrain ground 64/48 and 56/48, cloud_march / cloud_sky 64/0,
  cloud_shadow 38/0, cloud_temporal 48/0, vol_scatter 72/0, vol_apply FS 56/16. Bisected against that day's
  changes: the lit core's filtered cloud lookup (bilinear: still 72/32), the ocean's filtered cloud lookup, its
  SH blur share and its SH ambient (each removed: still 80/32) - NONE is the cause. The lit +8 regs came in
  between 09-23 and 09-29 (GI cloud-shadow fixes 09-27, motion vectors / terrain opt 09-28): not bisected yet.
* **Measured 2026-09-30** (the foam / bubble rework; regs/local, Debug exe - the FS stats match RelWithDebInfo):
  ocean **80/32** (the rework had taken it to 80/48), film **64/16** (was 56/48: the same demand, 68).
  * The ocean's +16 B was the bubble cloud's THREE half-vec3 `exp`s (sun path down, sky path down, path up)
    in the middle of the top side. The two DOWN paths and the albedo are per-frame constants: `buildUboOcean`
    folds them into `u_oceanBubble0/1`, and `oceanBubbleRadianceFrame` keeps the one per-pixel `exp` (up).
    The film's depth is per pixel (capped at its water), so it keeps the full `oceanBubbleRadiance`.
  * The ocean's blurred bubble tap (up to 16 B-spline taps) moved to the top of main: NEUTRAL, but kept there
    (the cheapest point for it). It refracts through the level plane since.
  * `foamSlope` -> `foamNoL` (one half across the sun ray query, not two; the film's struct likewise): neutral.
  * Film, each stubbed, NO change: the bubble radiance (the frame form), the blurred milk tap, the sharp
    foam-field tap with the stuck foam. Its 64/16 is not this session's foam; its known peak is the light
    loop's shadow rays (below).
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
  AND rock. **Per material THREE textures, packed** (Procedural's bake; one fetch fewer per layer than
  the old four - L1TEX long scoreboard was the top stall of Static meshes, 16.8%, Nsight 2026-09-28):
  diffuse = sRGB albedo + linear ROUGHNESS in the alpha (BC3); normal (BC5 sets
  `MATERIAL_FLAG_BC5_NORMAL`); height = HEIGHT (R, 0.5 = flat) + AO (G) (BC5). **Metalness is always 0**
  (the ARM texture is gone; `metalRoughnessTexIdx` stays none). **`MaterialInfo` has no free slot, so the
  height + AO index rides the UBO per slot** (`u_terrainSplatHeightTex[s >> 2][s & 3]`, 0xFFFF = flat, AO 1),
  like the climate boxes. **The splat reads its other texture indices from the UBO too**
  (`u_terrainSplatTex[s >> 1]`, .xy even slot / .zw odd: diffuse | normal << 16, BC5 flag;
  `terrainSplatTex()`), not from `in_materialInfos`: each fetch then does not wait on a storage-buffer load
  first. `sampleTerrainXZ` issues its three fetches back to back. The materials are still registered
  (`addMaterials`) for everything else. The full contract is on the definition in `Renderer.cpp`.
* **Result of the L1TEX round** (2026-09-28: 4x anisotropy, UBO texture indices, the packed splat, biplanar rock,
  the batched AO read): Nsight L1TEX long scoreboard 16.8 -> 10.5% of Static meshes, IMC miss 0.25 -> 0.75;
  the user saw a noticeably higher fps (Static meshes 3.15 ms of a ~6.0 ms GPU frame after it).
* **Rock is BIPLANAR** (`sampleTerrainTriplanar`, kept its name): only the two world projections the normal
  leans on most are sampled, each weight being |n| MINUS the smallest |n| (0 for the plane being dropped where
  the second and third swap: no seam), a second plane under 5% of the pair skipped too. At most 2 x 2 taps +
  the height/AO tap (triplanar paid up to 3 x 3 + 1). Roughness rides each plane's diffuse alpha; height AND
  AO come from the top plane (uvY) alone, so on a steep face the AO is the top projection's, stretched (it
  multiplies only the ambient term).
* **"Renderer/Textures/Anisotropy"** (Off / 2x / 4x / 8x / 16x, default 4x - a user decision; was a fixed 16x): the max anisotropy of
  `StaticMeshGraphicsPipeline::m_sampler`, the sampler of EVERY scene texture slot (materials + the splat). A
  change waits for the GPU, recreates the sampler (`recreateSampler`) and re-records; `record()` writes all
  texture slots with it. Added as the A/B for the L1TEX latency: the terrain is seen at grazing angles.
* **Terrain relief** (`TERRAIN_SPLAT_RELIEF`, defined by the terrain FS only; "Terrain/Textures/Parallax*",
  "Height blend contrast"; `u_terrainTexParams6/7`):
  * **Height blend:** `terrainMixInto` steepens each layer's coverage ramp and shifts it by the height
    difference, so borders follow the texture relief. Contrast 0 = the old linear blend.
  * **Parallax occlusion mapping, displaced along the geometric normal** (mesh = top of the relief). The
    texture offset is the ray's TANGENTIAL part, `-(V - N NoV) / NoV * depth`, so ONE 3D offset serves
    every layer, each with its own UV scale. **Never march along world Y:** that ignores the base slope, and
    over the ray's sideways run the slope changes height by more than the relief (slopes broke).
    `terrainReliefAt` runs the SAME layer chain on the height maps alone (explicit gradients; one tap in a
    climate-box interior), and `terrainParallaxOffset` marches it: a linear search with steps ~ 1/NoV
    (capped by "Parallax steps"), 4 bisection steps, then a secant. Grazing angles need the bisection:
    a secant alone leaves the linear steps visible as slices. `NoV` is clamped at 0.1.
    **Silhouettes** (`TERRAIN_POM_SILHOUETTE`, a define in the include, **default 0 - it did not work**:
    holes in front of the terrain behind a ridge plus a strip on the sky, whatever the facet handling):
    curved relief mapping.
    The base surface's curvature along the ray (from `dFdx/dFdy` of the position and the geometric normal)
    bends the ray: `rayH(s) = 1 - s + c s^2`. A ray that climbs back out over a crest with no hit is
    discarded, the terrain's form of SPOM's "ray left the UV square". As the interpolated normal turns
    edge-on, the march frame eases to the FACET normal (`cross(dFdx P, dFdy P)`). The facets are chords
    under the smooth vertex-normal surface, so a coarse ridge has a mesh-sized band with NoV <= 0 on the
    interpolated normal. Keeping that band left a floating strip on the sky, and discarding it all cut
    mesh-sized holes in front of the terrain behind. On the facet frame, only the curvature test decides.
    `c` uses a NoV floor of 0.05; a higher one underestimates `c` for grazing rays, so they miss the test.
    **This puts a `discard` in the
    ground pass**, which defers the depth write for every terrain pixel: measure it, and set it to 0 to
    compile it out. The TLAS, the shadow map and the collider keep the mesh crest.
    **Relief self-shadow** ("Parallax self-shadow"): 6 steps from the hit up toward the sun (Tatarchuk soft
    shadow), written to the lit core's `g_sunVisMaterial`. It scales the ground's sun radiance in
    `doSunLight` only, not `g_sunVisSurface`, so the water film keeps its glint. It is faded by
    camera distance and on slopes past ~50°, and skipped past the fade end. The coverages and the lighting
    stay at the mesh point. Rock uses its TOP plane's height only.
    `pom.inc.glsl` (UV-space SPOM, height in the normal-map alpha, needs `in_tbn`/`in_uv`) does not fit
    the splat: several layers, no TBN, BC5 normals with no alpha.
  * All of it runs before `computeLitColor`; only the shifted XZ outlives the splat, so it should not move
    the lit core's register peak. **Measure it (pipeline stats + profile) before you extend it.**
  * The ocean's seabed splat has no relief (no derivatives at a ray hit): its layer borders stay linear.
  * The relief also carries the SURFACE WATER: see "Terrain surface water" below.

# Terrain surface water

ONE wetness field feeds ONE water surface ("Terrain/Water" tweaks, `TerrainWetTweaks`,
`terrainWetParams0..9`). It is both the rain puddles on the ground and the continuation of the ocean onto
the sand, so the two can never disagree.

* **The field** is the wetness clipmap (`TerrainWetnessPipeline`, see its entry below): rain everywhere,
  1 under the live ocean surface (its swash tongue included), decaying back with "Dry time (s)" and
  spreading with "Diffusion (1/s)". "Slope drain" applies on both sides: the compute pass divides wet-in and
  rain by it (map gradient), the terrain FS raises the decay by it per pixel (mesh normal).
* **The LEVEL** (`terrainPoolLevel`, `terrain_wetness.inc.glsl`) is how far that wetness fills the splat
  RELIEF - the same height composite the tessellation displaces by: 0 = the relief's low points, 1 = its top.
  "Fill start" / "Fill full" bracket it in wetness, "Fill curve" shapes the rise. **The SLOPE sinks it**
  ("Film max slope (deg)" / "Film slope fade (deg)", `u_terrainWetParams6.yz` as smooth-normal y): full
  below max - fade, 0 at the max, so a pool on a steepening slope recedes into the relief's low points and is
  gone, instead of alpha-fading. The slope drain only thins the wetness, and a wet enough slope still
  filled its relief. The live-ocean part of the film is NOT slope-limited. Clamped to 1: the level is
  a height INSIDE the relief, and an unclamped one lifts the film off the ground.
* **The SURFACE: the film is NEVER tessellated** (a user decision: it follows the water level, not the
  relief). The terrain VS under `TERRAIN_OVERLAY_PASS` lifts each vertex along the normal to
  `max(level, live ocean surface)` in the relief band (`terrainFilmLevel`), inside the tessellation range
  only, with the TES's own distance fade and slope gate and the GROUND relief depth (the rock's is a
  per-pixel coverage). A pool's surface is flat across a crevice, and at the waterline the film rides the
  ocean's own live displaced surface (the baked water level + `underwaterLiveWaveY`). Capped at the relief top
  - above that the ocean draws its own surface and a film lifted to the same height would z-fight it.
  Elsewhere (and with tessellation off) there is no lift. **Depth test GREATER_OR_EQUAL**
  (`PipelineVariant::depthGreaterOrEqual`): with no lift the film is bit-identical to the flat ground
  (`invariant gl_Position`), and the displaced ground hides it where it stands higher. The VS hands the lift
  on (`out_meshLift`, location 3, one float): the film FS rebuilds the mesh point from it
  (`TERRAIN_LIT_POS`) and lights and measures the relief from there. The wetness
  clipmap (18), the ocean maps (7) and the terrain data (19) are bound to the vertex stage for this.
* **The COVERAGE** is how much water stands over a pixel: `(level - relief height) x relief depth` METRES
  (or the live ocean over the ground where deeper, see the hand-over below), faded over the last "Edge
  fade (m)". The film therefore always dies exactly where
  its surface meets the terrain, per pixel and shaped by the texture relief - the geometry's own outline is
  the film surface crossing the ground (a per-triangle, stepped edge) over the clipmap's 0.5 m texel contour,
  which pops in blocky on its own.
* **No SKIRT:** the film re-draws the chunk's whole index range, the border skirt walls included. The FS
  discards a pixel whose UNDISPLACED face (derivatives of `TERRAIN_LIT_POS`) is vertical (|n.y| < 0.05):
  the skirt is exactly vertical there, the terrain surface never is. Without this, the film showed as a
  translucent wall along chunk edges.
* **The GROUND** under it: albedo x "Wet darkening" and the wet gloss, each with its own wetness
  threshold ("Darkening threshold", "Roughness threshold", `u_terrainWetParams8.xy`, both 0.35 by
  default). Each amount is `smoothstep(0, threshold, wet)`: a plateau while the ground is soaked, and a
  smooth fade to dry below the threshold, as the pre-rewrite "damp knee" did.
  * Replaced along the way: log10 ramps with "reach" tweaks (decades of wetness), a gain on the wetness
    (a hard edge where it saturated), and an exponential curve.
* **THE DRYING PATTERN** (the ground pass's darkening + gloss): ground dries in metre-scale ISLANDS, not
  uniformly.
  * P (0..1, low = holds its water longest) is a world-anchored value fBm (`terrainFbm`) of "Drying
    pattern size (m)" (`9.y` = 1 / size), stretched from its central bunching toward 0..1 by "Drying
    pattern contrast" (`9.w` = contrast / 2, clamped: higher = more ground fully dry or fully wet, stronger
    islands). "Drying
    pattern relief" (`9.z`) of `surf.height` is mixed in, for fine breakup at the island edges.
  * Each wet amount (above) becomes a LEVEL through P. Ground below it is wet, and above it is a dry
    island. The islands grow as the ground dries. Darkening and gloss share P, so with a higher roughness
    threshold an island loses its gloss first, then its darkness.
  * Each level has its own soft band. "Darkening edge" (`9.x`, 0.3) is wide, so the darkening fades over
    a larger range. "Roughness edge" (`7.z`, 0.1) is narrow, so the gloss islands stay crisp. "Drying
    pattern" (`7.w`, 0..1) mixes from uniform drying to the patterned look. The pattern fades out where its cells shrink below a few pixels
    (`fwidth`), because the noise would shimmer there.
  * Where the field is at 1 (under the swash), the level is above all of P: all wet.
  * History:
    * This is the pre-rewrite look (an fBm pooling mask).
    * The relief ALONE as the pattern was tried in between. It tiles at the splat texture's scale, so it
      gave speckle, not drying patches.
    * A faster-drying power on the peaks' wetness gave no islands at all, because it has no threshold.
* **The wet SPECULAR** (the ground pass, with the gloss weight `glossW`):
  * The lit core has NO environment specular (diffuse GI + the lights' GGX lobes only), so wet ground
    showed one sun highlight and read as merely darker - "diffuse". It was not the fp16 BRDF: its GGX takes
    1 - NoH^2 as |N x H|^2, and at the wet alpha that error is ~1e-6 against a^2 = 0.0064.
  * "Wet normal scale" (`8.z`): with the gloss, the normal map's tilt off the shading base is scaled
    (1 = unchanged). Below 1, water fills the micro relief and the highlight stays sharp instead of
    scattering over the wet area. Above 1, the tilt is exaggerated. It is kept on the base's side of the
    horizon.
  * The wet SKY REFLECTION (no tweak, always on at full strength): after `computeLitColor` (past the light
    loop's register peak), the film's sky model: the baked mirror sky along R, water Fresnel (F0 0.02), the
    film's roughness-to-blur rule (rough wet ground mirrors nothing), the texture AO as specular
    occlusion, and the mirror fog rule. Gated by `aboveLive` and off under the film (`underFilm`, below),
    so the two never both mirror the sky. Two sky-map fetches on reflecting pixels only. Its register
    cost is unmeasured.
* **GLINTS** ("Glint size (m)" `u_terrainWetParams10.z` = 1 / size, "Glint coverage" `10.w` (0 = off),
  "Glint roughness" `11.x`): wet sand is not uniformly glossy. Beaded water and flat wet grains catch
  the sun in small sharp points.
  * Sparse world-anchored patches: a single-octave `terrainValueNoise` over glint-size cells, its peaks
    thresholded (`smoothstep(1 - coverage, 1 - coverage / 2, n)`). They drop the roughness to a near-mirror
    alpha, on the wet gloss only (x `glossW`, not under the film or the live ocean).
  * Small and sparse, so the overall specular barely changes. They fade out where a cell shrinks below
    ~0.3-0.7 pixel (`fwidth`, shared with the drying pattern), where they would only shimmer.
  * Register cost: none measured (ground 72/16 both variants).
* **Three ROUGHNESSES:**
  * "Water roughness" (`u_terrainWetParams2.z`, perceptual) is the FILM's base, in the ocean's microfacet
    model. The film used the ocean's own "Roughness" before.
  * "Wet roughness" (`7.x`, GGX alpha like the splat's `rough`) is the wet ground's roughness. It is full at
    the "Roughness threshold" and is broken into islands by the drying pattern (above).
  * "Underwater roughness" (`7.y`, GGX alpha) is the ground's roughness under WATER: the live ocean
    (`1 - aboveLive`, shows through the ocean's edge fade) and the FILM (`underFilm`). `underFilm` is the
    film's own coverage recomputed in the ground pass: the pool level through `surf.height`, over its
    "Edge fade (m)", with the ground relief depth as the metres. It is not a wetness gate, so the wet
    gloss stops exactly at the film's outline instead of sitting under its surface.
  * The darkening stays under the ocean, because the ocean's traced seabed is darkened the same.
* **The HAND-OVER to the ocean** (two tweaks, so the seam is neither the film's nor the ocean's hard edge):
  * "Ocean blend (m)" (`u_terrainWetParams5.x`): `aboveLive` fades from 1 to 0 over this much LIVE water
    over the GROUND (`g_liveDepthBelow` at the tested point + that point's height over the relief ground).
    The film draws from its OWN list BEFORE the transparent execute (below), so the ocean always covers it
    and only the ocean's fade band shows the film. (In the transparent sequence, the order of film and
    ocean was undefined: where the film drew after the ocean, the ocean's depth cut it - a seam.) The film
    must still be gone before its surface sinks under the ocean's. The old gate (a 10 cm
    step at "Ocean margin") let the film run over the whole shallow band in a different tone, then the
    depth test cut it with a hard edge. Keep the blend under the relief depth.
  * The film's coverage depth is `max(pool depth, live water over the ground)`. So the film always covers
    the ocean's thin edge, also where the wetness has not caught up yet. The Beer-Lambert tint runs on the
    same value.
  * "Ocean edge fade (m)" (`u_terrainWetParams6.x`, 0 = off): **the ocean** (`ocean.fs.glsl`) fades out
    over this much water column, so its mesh no longer ends in a hard line where it cuts the ground; the
    film under it carries the water. A real blend: the Ocean variant composites DUAL-SOURCE
    (out = ocean x a + scene x (1 - a)), and **the cull puts ocean instances in the TRANSPARENT sequence** so
    they draw after the ground: order = opaque execute → tess ground → film → transparent execute (ocean,
    blended meshes). The ALPHA blends too (`PipelineVariant::dualSourceAlpha`):
    out.a = 0 + dst.a x (a > 0.5 ? 0 : 1), the TAA ocean flag where the ocean is most of the pixel.
    Top side only; a fully faded pixel returns before the shading. The band is an ease-out,
    cover = 1 - (1 - t)^2 with t = column / fade: always 0 at the bottom (the mesh edge stays hidden) and
    mostly opaque above it. The fade width alone sets how fast.
    **The column is to the FILM's water surface**, where the ocean hands over to it. That is the film's
    tessellated surface without the splat relief: `terrainHeightAt(in_pos.xz)` + (`terrainPoolLevel(`the
    wetness clipmap`)` - 0.5) x the ground/beach relief depth, with the tessellation's distance falloff
    (0 past it or with tessellation off). The ocean therefore samples the wetness clipmap (binding 18,
    `terrain_wetness.inc.glsl`). It reads at the pixel's own position, not `in_uv`'s undisplaced lattice
    point. The pool level's slope term ("Film max slope") takes the ground normal's y from the baked map's
    forward gradient over 4 m: two more taps. The film reads its smooth mesh normal, and the map is what the
    ocean has. The whole fade is `oceanEdgeCover`; it adds no registers (80/16).
    * **Edge foam was tried and REMOVED.** It added surf foam across the fade band that held the ocean's
      opacity. Its patches kept shifting with the camera through several fixes: calm-plane noise, a
      calm-depth band read on that plane, and the same patches continued on the film. The likely remaining
      cause: the ocean's own edge (its LOD'd, clipmap-snapped surface sinking under the ground) steps as the
      clipmap scrolls, and any foam that makes the fading edge opaque shows that outline ("delayed, stepping
      like a clipmap scrolling").
    * **Not a dither:** an IGN discard (TAA-resolved) was tried first. TAA caps the history on ocean
      pixels (the ocean flag, to keep the glints), so the dither stayed as flickering noise.
    * **Cost:** the ocean no longer early-Z rejects the tessellated seabed under it (the tess ground drew
      after it before); that ground is now shaded, then covered. Profile a shoreline view if it matters.
* **The FLOW** ("Film flow speed (m/s)" `u_terrainWetParams6.w`, "Film flow min slope (deg)"
  `u_terrainWetParams10.xy`, "Film flow cycle (s)" `8.w`;
  `terrainFilmSurface`): the inland ripples run DOWNHILL.
  * Direction: `+N.xz` of the smooth mesh normal (a normal leans toward the DOWNHILL side), exact per
    pixel. The terrain-data map's flow bits
    (`terrainFlowEncAt`: toward land offshore, downhill on land, authored rivers) are 8-bit angles on ~8 m
    NEAREST texels, which would step. They stay available for rivers.
  * Speed: the tweak (at 45 degrees) x sqrt(tan slope), x a slope GATE: none below half of "Film flow min
    slope", full from it (smoothstep on tan; default 8 degrees). Gentle ground and flats stay still and pools
    lie still, while the slopes keep sqrt(tan)'s speed. Measured alternatives: sqrt(tan) alone moved gentle
    beaches too much; a tan^2 curve stilled them but slowed the moderate slopes with them. Off the shore only (x (1 - shore)): the shore band
    carries the ocean's own waves.
  * A two-phase flow map on the finest cascade's slope tap and the detail tap. Each is sampled again half a
    cycle apart and crossfaded by a triangle, so neither phase's reset shows. The finest cascade's
    phase 1 is one slope-only tap after the cascade loop. The moments and the Jacobian (shore foam) do not
    flow.
  * **On the whole film** since the film is one untessellated variant. When only the tessellated film
    had it, a second-phase tap cost the untessellated film 16 B (56/48 -> 56/64); not measured since.
* **The LOOK** is the ocean's own (`terrainFilmSurface` / `terrainFilmShade`, below): "Waviness",
  "Normal scale", "Wind ripples". The film's Beer-Lambert TINT runs on the SAME water depth as the
  coverage - a puddle deepens toward its middle - instead of a fixed "virtual depth" tweak.

* **Both relief switches are BAKED** (`Renderer::setTerrainTextureParams` compares them against what the
  pipelines were built with; a flip → GPU idle, reload, re-record, the ocean's pattern):
  * "Terrain/Textures/Parallax" → `TERRAIN_POM` 0/1 on the terrain fragment shaders. With 0, the march,
    the self-shadow and their derivatives are compiled out; the height blend stays.
  * "Terrain/Tessellation/Enabled" → the cull's `TERRAIN_TESS_ROUTE` 0/1, and whether
    `m_terrainTessPipeline` is built and its draws recorded. The RTAO skip still reads the runtime flag, which
    always matches.
  * The pipeline defaults match `TerrainTexTweaks` (parallax off, tessellation on), so the first push
    rebuilds nothing.
* **Terrain TESSELLATION** ("Terrain/Tessellation/*", `u_terrainTessParams0/1/2`; default ON):
  * **It cannot live in the DGC execution set.** Every pipeline in an indirect execution set must have the
    initial pipeline's shader stages (VUID-vkUpdateIndirectExecutionSetPipelineEXT-11152), plus identical
    static state and fragment outputs. So `StaticMeshGraphicsPipeline` owns a second `GraphicsPipeline`,
    `m_terrainTessPipeline` (the ground only; the film is never tessellated), built by `buildTerrainTessLayout` from the main
    layout: the same bindings (identically defined set layout, so the SAME descriptor set binds), vertex
    input, push ranges and baked fragment defines. `indirectBindable = false`.
  * **Distance routing (the cull):** only a chunk whose bounding sphere reaches inside "Fade end" (+1 m for the
    VR eyes) goes to the tess draws; a chunk wholly past it is the same surface (edge factor 1, no
    displacement) and takes the plain DGC path. Nsight showed the geometry stages
    launch-stalled on ISBE 31% of Static meshes while every chunk out to ~33 km ran them. Measured in the
    sandbox view: 1.559 → 1.552 ms, inside the noise. Relies on ONE instance per terrain mesh (one
    `RenderMesh` + node per chunk): the DGC entry is per mesh, the routing per instance.
  * **Routing (the cull):** with tessellation on, a `TerrainLit` instance still allocates its slot in the
    DGC sequence (`atomicAdd`), but that sequence draws `indexCount = 0`. The real draw goes to
    `out_terrainTessCommands` (binding 16, `atomicMax(idx + 1)` like the film). Same 24 B per-mesh-slot
    layout.
  * **Draws:** `record()` draws them with `drawIndexedIndirectCount` (offset 4 skips `pipelineIndex`, stride
    24) between the opaque and the transparent executes, then rebinds everything (a generated-commands
    execute leaves the bound state undefined). The count is the compacted one (draw count [2], see
    Draw-list compaction); with tessellation off the cull routes nothing there, so the recorded draw walks
    nothing.
  * **Shaders:** `instanced_indirect_terrain.vs.glsl` with `TERRAIN_TESS` hands on control points (the baked
    fields still per vertex). `terrain_tess.tcs.glsl` computes an edge factor from the edge's two end points
    only (projected length / "Target edge (px)", eased to 1 across the fade band by `1 - t^p`, "Factor falloff
    exponent" = `u_terrainTessParams0.w`; the displacement height fades by its OWN `1 - t^p`, "Height falloff
    exponent" = `u_terrainTessParams2.y`, read by the TES, the ground FS pixel normal, the film VS lift and the
    ocean's film estimate - split 2026-09-28; equal = the old shared curve, a factor dropping before the height
    leaves displaced relief on coarse triangles), which keeps the edges
    crack-free, and culls patches outside the frustum. `terrain_tess.tes.glsl` displaces along the
    interpolated normal by `terrainReliefAt` (splat include with `TERRAIN_SPLAT_HEIGHT_ONLY`), CENTRED
    (height 0.5 = the mesh), faded by distance and on slopes past ~50°. The domain origin is LOWER_LEFT, so
    `ccw` follows the GL rules.
  * **Lighting uses the UNDISPLACED position** (`in_meshPos`, location 3; `TERRAIN_LIT_POS` in the terrain FS
    under `TERRAIN_TESS`). **Tried and reverted 2026-09-28:** one float (the displacement along the normal)
    instead of the vec3, the FS rebuilding `in_pos - normalize(in_normal) * disp` - 2 components fewer in the
    ISBE and TRAM (Nsight: launch stalled on registers 44%, TRAM 38%, ISBE 31%), but the compiler kept the
    rebuilt point live across the shader: tessellated ground FS 64 -> 72 registers. A vec3 interpolant is
    re-interpolated at each use and holds no register. The FILM keeps the one-float form (`in_meshLift`): there
    it cost no register (56 either way, 16 -> 14 inputs). The shadow map and the TLAS hold the flat mesh, and the centred relief puts half
    the surface below it: lit from the displaced point, that half self-shadowed in bands. The splat samples
    the displaced `in_pos`.
  * **RTAO** rebuilds its origins from that displaced depth but traces the flat TLAS, so a relief low hit
    its own mesh's underside at once (dark blotches in the lows). A TERRAIN hit nearer than the relief depth,
    inside the tess fade, continues once past it (`rtao.cs.glsl`). The instance + material buffers (bindings
    8, 9) are therefore declared and written in BOTH RTAO variants, not only the alpha-test one.
  * **Displaced normals = the PER-PIXEL height gradient** (ground pass): `terrainTessPixelNormal` is the TES's
    displacement function (the same layers, height composite, depth and falloff, at `in_meshPos`)
    differentiated per pixel by forward differences over the whole displacement range. **The step and the
    mip are the TES's footprint** (the target edge at `max(distance, freeze)`), not the pixel's: at a pixel
    step close up, mip 0 magnified the height map's texel grid (a constant bilinear gradient per texel) and
    its BC4 steps into a pixelated contour pattern. The 3 points come from ONE walk of the layer chain (`terrainReliefAt3`: 3 taps per visited layer,
    vectorized height blends). The coverages are computed ONCE, at `in_meshPos`, and shared with the splat
    (`terrainSplatLayers`; `terrainSplat` is its wrapper for everyone else).
    Tried and dropped: an interpolated TES normal (only roughly matched the geometry), the displaced facet
    normal (flat-shaded the coarse mid-range tessellation), and the relief's facet tilt
    `facet(in_pos) - facet(in_meshPos)` (facets close up).
    **The normal maps are faded to half** toward that normal at full displacement (they carry the same
    relief again and re-tilted faces toward the sun), and **the relief normal gates the sun**: N·L <= 0 on
    it → no ground sun (`g_sunVisMaterial`), whatever the normal map says.
    `in_normal` stays the SMOOTH normal (`coverN`), and it drives the splat's layer coverages
    (`terrainSplat(pos, geoN, coverN, f)`) and the wet slope drain, so the bumps do not scatter rock or
    snow. Without it, rock faces turned away from the sun were lit as if facing it. An interpolated TES
    normal (forward differences of the height composite) was tried first: better, but it only roughly
    matched the geometry, and it cost two more height evaluations per vertex.
  * **No swimming near the camera:** the edge factor comes from the DISTANCE (edge length × projection
    y scale / distance), not from projecting the end points, because a projected length changes under a
    pure camera rotation and every factor change slides the fractional vertices onto other heights. Closer
    than "Freeze distance (m)" (`u_terrainTessParams2.x`), the factor and the TES's height mip footprint hold
    at that distance, so nothing moves there. Past it, the fractional spacing still slides vertices slowly
    with the distance.
  * **Crack-free vertices:** a vertex on an edge interpolates its two corners in a fixed (lexicographic)
    order, and a corner is the control point itself. The height mip footprint is a function of the position
    only (the pixel size at that distance), not of the patch.
  * **The CENTRE view decides every level of detail** (2026-09-28): the TCS factor, the TES fade + mip
    footprint, the ground FS `terrainTessPixelNormal` and the film VS lift read `u_views[VIEW_CENTER]`, never
    the eye's `u_viewPos` / `u_mvp` - per-eye decisions gave the two VR eyes different geometry. Only
    `gl_Position` projects with the eye.
  * **The TCS skips work the TES would not use** (2026-09-28, for the ISBE launch stall):
    * edge factor 1 where both end points are past the slope gate (normal y < 0.35: the TES displaces
      nothing there), and on VERTICAL edges (the same xz: the skirt walls). Both are per-edge and symmetric,
      so still crack-free;
    * a SKIRT patch (a vertical face) gets inner level 1 (its top edge keeps the surface's factor);
    * a patch whose three smooth corner normals all face away from the camera (dot(N, V) < -0.3, a margin
      for the relief's tilt) is culled with the frustum cull.
  * **No `precise`**: the engine's glslang crashes (access violation in
    `PropagateNoContraction`) on it, although the SDK's glslc accepts it.
  * **Not tessellated:** the shadow map (a user decision: near relief casts the flat mesh's shadow), the
    TLAS, the collider.
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
  (`instanced_indirect.cs.glsl`) emits it for `TerrainLit` instances into its OWN list (binding 17, draw count
  [3]; same instance list, count raised with `atomicMax` so every overlapping instance is inside the drawn
  range). `record()` draws it with `drawIndexedIndirectCount` and variant 11 bound normally (it stays in the
  transparent execution set but is never routed there), after the tessellated ground and BEFORE the
  transparent execute, so the ocean always blends over it. **The film has its own FS, `terrain_film.fs.glsl`**; the
  part it shares with the ground's `instanced_indirect_terrain.fs.glsl` (the wetness, the mirror sky, the
  splat include, `terrainFields`, `terrainWetness`) is `terrain_common.inc.glsl`. The film is NEVER
  tessellated (also over tessellated chunks): the terrain VS compiled with `TERRAIN_OVERLAY_PASS` lifts it
  to its water level (see "The SURFACE" above). The pipeline's terrain define loops (lit debug, `LIT_RT_*`,
  `TERRAIN_POM`) match both FS paths (`isTerrainFragment`). Depth test GREATER_OR_EQUAL / write off,
  `early_fragment_tests`, uncovered pixels discard, and the FS composites
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
  cascade keeps a slope weight of its own there ("Terrain/Water/Wind ripples",
  `u_terrainWetParams5.w`) - the film loop's existing taps, no extra fetch. The film also carries the
  ocean's sub-band DETAIL slope (the `oceanDetailSlope` math inlined, "Ocean/Shading/Detail *", one
  extra tap), its crest foam (`oceanInstantFoam` inlined, shore-gated) and its own normal knob
  ("Normal scale", `u_terrainWetParams5.z`, x the ocean's normal strength); no foam inland, and the amplitude follows the ocean wind through the spectrum.
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
  stuck foam's edge takes screen derivatives), a foam patch is laid over the window + mirror as a backlit
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
  seam) and derives EVERYTHING from it - see "Terrain surface water" below. **Disabled = the pass is skipped and the presence flag is 0**; re-enabling parks the previous
  origin out of range so nothing stale shows. Rain from weather, particle hits and script splats are
  the planned injection sources.
  **The pass runs on a FIXED TICK** ("Terrain/Water/Update rate (Hz)", default 20), not per frame:
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
  The debug info may contain `OpExtInstWithForwardRefsKHR` (a debug type that refers to one declared later,
  e.g. a struct passed to a function), so `Device` enables `VK_KHR_shader_relaxed_extended_instruction`
  (`shaderRelaxedExtendedInstruction`) when the driver offers it.
* **Per-pixel work hoisted to the UBO / per pixel:** `u_sunTransmittance` is the CPU mirror of
  `atmosTransmittanceToLight(0, sun, up)` (`buildUboSky`; keep the constants in sync with
  atmosphere.inc.glsl) — the lit sun term never runs the Chapman function per pixel; `u_sunDirection` is
  normalized on the CPU, so shaders use it raw; PCSS takes 6 blocker GATHERS (`textureGather`: 2x2 texels
  each, 24 texels, since 2026-09-28 for the L1TEX latency; 12 single-texel taps before, 16 before that) + 12
  filter taps. The gathers stay spread over the Vogel disk; the four texels of one gather are neighbours.
  **Umbra early-out** (`pcssCascade`, not the border path): all 24 search texels occluded AND one
  hardware-PCF tap at the centre = 0 → return 0, the
  filter skipped. The centre tap guards a small hole over the pixel that the sparse 15-texel search misses
  (a close grate / canopy gap the filter's small disk would see). Static meshes 1.635 → 1.606 ms (sandbox,
  3-4 runs each, no overlap); lit FS 64/48 → 72/16, terrain unchanged. The PCSS Vogel disk rotates its compile-time tap angles
  by ONE per-pixel `(cos, sin)` (`rotSC`) instead of a sincos per tap (keep the tap loops fully
  unrolled: 4-tap batches with a run-time-indexed offset table measured 93 registers against 83); `u_cascadeSunSizeTexels` holds
  the per-cascade PCSS penumbra scale (`buildUboSunShadow`, from the matrices' bottom-row scalars);
  divide-by-PI is `* INV_PI`; the AO bilateral weights use `exp2` of the squared distance.
* **The GI probe buffer is `vec4[]`** (every includer declares it so; layout table at the top of
  gi_probe.inc.glsl): a probe is 6 wide loads — SH in 3, depth moments in 2, backface + relocation
  offset packed in 1 — not 24 scalar ones; the write side packs the same way. The Chebyshev weight
  exponent is the fixed `GI_VIS_CHEB_POWER` define (2, gi_probe.inc.glsl; no tweak), unrolled to
  multiplies.
* **`shared.inc.glsl` / `ubo.inc.glsl` structs must stay in sync with `Private/Layout.ixx`.** So must the
  main cull's `OutMeshInstance` (64 B): it is declared in the cull and in the three scene vertex shaders
  (lit, terrain, ocean) - a shader that declares fewer fields reads with the wrong stride.
* `motion_vector.inc.glsl`: the fragment side of the motion vectors (`fragPrevClip`, `motionVector`);
  fragment shaders only (`gl_FragCoord`). The readers' side is in shared.inc.glsl (`prevScreenUVMotion`).
* **THE SUN SHADOW FIRST** (`SUN_SHADOW_FIRST`: the lit FS, instanced_indirect.fs.glsl, and the terrain GROUND,
  instanced_indirect_terrain.fs.glsl - not the film, terrain_film.fs.glsl, which has its own hard tap). `sunShadowVisibility`
  (the shadow + the terrain march + the eclipse + the clouds) is split out of `doSunLight`; these shaders call it
  at the top of main (LitMasked: right after its discard), before the material, from the GEOMETRIC normal (the
  lit mesh normal; the terrain's smooth mesh normal `coverN`, also when tessellated: the shadow is evaluated at the
  flat mesh, which the shadow map and the TLAS hold, and the displaced faces turned away from the sun are gated
  by `reliefSunGate`), and
  `doSunLight` reads `g_sunShadowFirst`. So a normal map never bends the shadow's offset; `doSunLight`'s facing
  test on the shading normal still applies. The shadow search is the lit FS's register peak (Nsight live
  registers: 70 in `pcssCascade`, with the whole surface live across it). Measured 2026-09-28: lit FS 72/16 ->
  **64/48**; game mode (64 units) Static meshes 0.281/0.284 -> 0.257/0.263 ms, GPU frame 1.553/1.562 ->
  1.508/1.515. Terrain: speed-neutral (#8 72/16 either way, tessellated ground 64/48 -> 72/16) - there it is a
  quality choice. The same day, rebuilding the world position in the lit FS instead of interpolating it was tried
  and reverted (see "Motion vectors"). Later the same day: the terrain ground's shadow moved BEFORE
  `terrainLayers` (so the layers and the tessellated pixel normal are not live across it), and `V` moved down to
  just before `computeLitColor` (Nsight: 69 live at the tessellated ground's `terrainLayers` call). Also
  `ClimatePick` (terrain_splat.inc.glsl) packs its three indices into one uint (8 bits each), and `pickClimate`
  keeps its four running weights in HALF (user-checked: no visible artifacts). Together: both ground variants
  72 -> 64 regs (next item).
* **The terrain ground FS is 64 regs** since 2026-09-28 (pipeline stats): untessellated #8 72/16 -> **64/32**,
  tessellated 72/16 -> **64/48**. From three changes measured together: the shadow before `terrainLayers` (from
  `coverN`), `V` computed just before `computeLitColor`, and `ClimatePick`'s packed indices + half `pickClimate`
  weights (see "SUN SHADOW FIRST" above). The film (#11, one untessellated variant, flow on everywhere) is 56/48.
  The frame time is not measured yet. Before that, 64 LIVE at most yet 72 allocated: the step needed a peak a
  few registers lower, not a different place. Tried earlier the same day, no change to #8: the shadow first (above; kept for its quality), `terrainFbm` not unrolled, and the wet
  section's two noises (drying fBm, glints) hoisted above the splat (reverted). **Kept: `terrainFbm` is
  `[[dont_unroll]]`** (terrain_splat.inc.glsl; its includers enable GL_EXT_control_flow_attributes): unrolled,
  the 12 corner hashes overlapped - the tessellated terrain's TES dropped 70 -> 49 registers with it (the
  then tessellated film FS 64/16 -> 56/48; everything else unchanged. The film is no longer tessellated).
* **Only `EPipelineIndex::LitMasked` discards** (the lit fragment compiled with `ALPHA_MASK`). A
  `discard` anywhere in a pipeline's shader costs it early depth WRITES, and with no prepass that is
  every opaque pixel's overdraw. `ObjectContainer` sends a Mask material that resolved to `LitOpaque`
  (after the `.oc` overrides) to `LitMasked`. Keep `discard` out of `LitOpaque`.
* **The RT shadow toggles are BAKED into the lit-core fragments** (lit, masked, transparent, terrain):
  `LIT_RT_SUN_SHADOW` / `LIT_RT_LIGHT_SHADOWS`, always defined 0/1 from `RTParams::effectiveSunShadow()`
  / `effectiveLightShadows()` (master AND toggle — the same expression as the UBO flags). The lit
  core `#error`s without them. Why: register allocation covers every compiled path, so the PCSS
  search and the RT sun loop in one shader cost the occupancy of the larger one. "RT/Enable RT",
  "RT Sun" and "RT Lights" reload the pipeline; the setter runs BEFORE the idle test, because an override
  fires at registration and `initialize()` must build with it. The ocean, fog and GI still read
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
