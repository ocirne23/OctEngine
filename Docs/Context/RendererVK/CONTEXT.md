# RendererVK

> Library documentation for `Code/RendererVK`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Modern Vulkan renderer. Links Animation, File and Threading (+ vulkan, glslang PRIVATE). Nsight
Aftermath is NOT linked: `Util/Aftermath.ixx` (`RendererVK:Aftermath`) `LoadLibrary`s
`GFSDK_Aftermath_Lib.x64.dll` in `GpuCrashTracker::Initialize` and holds one function pointer per
entry point. **The DLL is optional** — when it is absent, App.exe runs with no GPU crash dumps and
the ShaderDatabase stays empty (`Aftermath::loaded()` gates every call). **Streamline / DLSS is optional the
same way** (`Util/Streamline.ixx`, see "DLSS" in [PostProcess.md](PostProcess.md)).
**Minimum spec RTX 2000.**

* **Device Generated Commands for everything.**
* **Reversed-Z main view** (near = 1, far = 0). Shadow maps stay standard; fullscreen passes disable
  the depth test.
* `NUM_FRAMES_IN_FLIGHT` = 2.
* `Renderer::isInitialized()` is false in headless server mode — the global exists (static init_seg)
  but `initialize` never ran. **Anything that may run headless and would touch GPU or mapped state
  must gate on it.**

## The documentation files

This file holds the frame loop, the source layout and the cross-cutting rules. **Read the topic file of every
feature you touch BEFORE changing it.**

| File | Covers |
|---|---|
| This file | Frame pacing, screenshots, the frame slot fence, the begin-frame job + cull view, **frame order** (no depth prepass, recording vs submitting, ReBAR buffers, barriers and overlap), the feature list, layout under `Private/`, debug names, the graphics-queue mutex, VR |
| [`FrameUbo.md`](FrameUbo.md) | **The frame UBO (generated GLSL), the tweak locks that bake it into the shaders**, push blocks |
| [`Scene.md`](Scene.md) | Per-frame instance flow, scene objects (single meshes, THE SKY DRAW, tree records, per-entity tint), texture + mesh streaming, mesh LODs, draw-list compaction |
| [`Lighting.md`](Lighting.md) | The scene focus, GI (probes, sky map, irradiance volume), RTAO, the sun cycle (time of day / latitude), long-range sun shadows + their cost, aerial perspective |
| [`Clouds.md`](Clouds.md) | Volumetric clouds |
| [`Vegetation.md`](Vegetation.md) | Procedural grass, ground clutter, the wind, tree wind, the far-tree volume |
| [`PostProcess.md`](PostProcess.md) | Motion vectors, TAA bypass, DLSS, motion blur, colour, bloom |
| [`Terrain.md`](Terrain.md) | Terrain and ocean integration, river and lake water, the rock material, terrain surface water, the rain occlusion map |
| [`Shaders.md`](Shaders.md) | Shaders (folders, includes, compile, hoisted work), pipeline statistics (register counts), **half floats (fp16)** |

---

# Frame pacing

`Core.Time` owns all of it; the "Time" tweaks are registered by `Settings::registerTime` (`TimeSettings`).
**`beginFrame(windowFocused, vr, vsync, displayRefreshHz, pumpWindow, waitFence)`** — ONE call at the
loop top does the fence wait, the frame-rate limit, the event-pump kick and the next frame's clock.

The fence goes through a function pointer main passes (`Renderer::waitFrameSlot`), **because Core
cannot see the renderer.**

## The limit

Waits until the desired end — last frame start + 1/target, where target is `Max FPS` (0 = uncapped),
or `Inactive max FPS` (30) when unfocused, further capped by `Time::setFpsCeiling` (main sets 60 while a
menu is up). **Never in VR: `xrWaitFrame` owns pacing.**

It sleeps in 1 ms steps while more than `Busy-wait window (ms)` (2.5) remains — **real milliseconds
thanks to the window thread's `timeBeginPeriod(1)`** — then spins the last stretch.

## `Stable frame time` (on)

**When it waited, the frame's END is ATTRIBUTED as the desired end, not the clock after the wait.**
The OS's post-target lateness is charged to the NEXT frame as processing time, **which keeps the delta
flat instead of jittering with scheduler tails.** An already-late frame ends now.

## Uncapped + vsync

With `Max FPS` 0 and FIFO present, **every frame lands on a whole number of refresh periods BY
CONSTRUCTION — only the CPU's view of it jitters**, because the throttle lands either in the loop-top
fence or in the end-of-frame acquire. (Loop-top intervals read like 4/8 ms around a 6 ms period, so a
tolerance-gated snap rejects most frames.)

So the start is QUANTIZED to the nearest whole number of periods (a dropped frame = 2), using
the display's reported refresh (`Window::getDisplayRefreshHz`) or the measured period EMA when the
platform reports none. **Rounding against the real clock every frame bounds the drift to half a
period.** Without vsync the intervals are arbitrary and the raw clock stands.

**A frame that rounds to ZERO periods (under half a period since the last start) keeps the raw
clock**, and the limiter never waits more than one period. Such a present did NOT throttle (an occluded
window, a refresh-rate mismatch); snapping it forward advances the attributed clock a period per frame,
until the limiter sleeps the whole lead out at once (it reads as a deadlock).

## The pump kick

`Window::requestPump()` fires `Input pump lead (ms)` (2.0) BEFORE the frame starts, so the window thread pumps
while main still waits.

* **CAPPED:** the end is exact, so the limiter kicks inside its own wait.
* **UNCAPPED:** the fence decides, so the kick targets **the EARLIEST plausible unblock** — the raw
  last start plus a decayed running MINIMUM of the raw interval. *Under FIFO the CPU unblocks early on
  alternate frames, and a mean-based prediction kicks too late on those.* The fence is waited
  (bounded) until a lead before that, then kicked, then waited out.

## VSync is a runtime tweak

`Time/VSync` is `Globals::settings.renderer.vsync` (registered by `Settings::registerRenderer`). **Present mode FIFO
vs Immediate is swapchain creation state**, so the Renderer's listener runs `recreateSwapchain()` (device idle +
re-init), guarded on `m_initialized` **because a saved or override value fires `onChange` at
registration, before the swapchain exists.**

`--no-vsync` is just `setOverride("Time/VSync=0")` — pinned for the run.

---

# Screenshots

`Renderer::requestScreenshot(path)` (main thread; App's `--screenshot-after`): the NEXT `present()` writes the
swapchain image - scene and UI, exactly what is shown - as a PNG, before it presents it (after `vkQueuePresentKHR` the
image belongs to the presentation engine). `captureScreenshot`: after the frame's submit, a one-shot command buffer on
the graphics queue (PRESENT_SRC -> TRANSFER_SRC, `copyImageToBuffer` into a host-visible buffer, back to PRESENT_SRC;
the barrier's first scope covers the frame's submission on the same queue), a queue idle, then BGRA -> RGBA with alpha
forced to 255 (the scene writes TAA flags there) and `ImageIO::writePngRgba8`. 8-bit RGBA / BGRA swapchains only.
It stalls that one frame. Independent of the window's focus or occlusion, unlike a screen grab.

# The frame slot fence

`waitFrameSlot(timeoutNs = UINT64_MAX)` blocks until this frame slot's previous GPU submission has
retired. **It is the vsync/GPU throttle of the whole loop.**

* Returns true once signaled or already waited this frame; false only on a bounded timeout. **Timeout
  0 is a marker-free STATUS POLL** that still claims the slot when signaled. A device error recreates
  the swapchain and reports signaled. Idempotent within a frame.
* **Called FIRST THING in the loop, through `Time::beginFrame`** — before the "main loop" profiler
  scope opens and before input is sampled. So the vsync/GPU stall shows as its own top-level
  "Frame wait" (the main-loop scope measures only real work), and **input → sim → present stays
  tight** (a wait between input sampling and the sim makes the input a frame stale).
* **NOTHING may write a slot's host-visible per-frame buffers before it** — renderNode instance
  memcpys, LOD redirects, sparse transforms. **Torn slot data shows as one-frame LOD flashes** (the
  cull's buckets overflow into neighbouring meshes' draw ranges). `beginFrame` asserts it ran (and
  waits anyway); `present()` re-arms it when the slot advances.

---

# Begin frame as a job

`setFrameView(camera, viewportRect)` (below) stores the camera and rect in members (the job outlives the
caller's stack); `kickBeginFrameJob()` then submits ONE High `"Begin frame job"`, and `joinBeginFrameJob()`
waits for it.

* main.cpp kicks right after `setFrameView` and the spatial index's update kick, BEFORE `physics.update`, and
  joins after the physics step, `audio.update` and `navSystem.update`
  ([main.cpp](../../../Code/App/main.cpp#L298)).
* `kickBeginFrameJob` first runs `applyUboLocks` on main (see [FrameUbo.md](FrameUbo.md)).
* **Legal because the window is renderer-QUIESCENT**: tweak `onChange` is flushed at the top of the
  frame, force and terrain param setters run after `world.update`, and the streamers' Vulkan work
  happens in `present` on main. Also because `StagingManager` is thread-safe with every graphics-queue
  call behind Device's queue mutex.
* **`beginFrame` is the SERIAL-OWNER of the render chain — ONE call in flight.** Nothing may touch
  renderer frame state between kick and join, and its outputs — counter resets, `m_cameraPos`,
  `m_mipPixelScale`, `m_centerViewProj`, `m_sunCascadeViewProj`, the frame UBO — **are valid only after
  the join.**
* **VR:** the kick only DEFERS; `joinBeginFrameJob` then runs `beginFrame` synchronously on main,
  **because `xrWaitFrame` owns VR pacing and would pin a worker.** The OpenXR poll, the XR begin frame
  and the head pose all live inside it (`applyVrHeadPose`).

## The cull view

`setFrameView(camera, viewportRect)` publishes the frame's view and returns the spatial cull's `CullView`
BEFORE `beginFrame`. It applies the viewport rect first (`setViewportRect`, before any projection is built
from its aspect).

* **Desktop** — **the exact frustum `beginFrame` will build**, because the camera and viewport are final by
  then and **TAA jitter is never baked into the mvp.** It is bit-identical by construction: both go through
  `computeCenterViewProj`, **the ONE place the centre view-projection is built**, and `m_centerViewProj` is
  published early.
* **VR** — LAST frame's head view (`m_lastCullCamera`), since the head pose only exists after `xrWaitFrame`
  inside `beginFrame`. **One frame of cull latency, absorbed by the culling margin and near-ball slack**, and
  the first VR frame's view is invalid, so culling is skipped that frame.

---

# Frame order

Desktop, as recorded by `recordCommandBuffers` (`recordPrimaryPreScene` + `recordPrimaryDesktop`) and
`recordPresentCommandBuffer`:

```
GPU Frame
  Skinning → Ocean sim (+ its spray step: particle spawn requests)
    → Shadow cull → Shadow draw            (both skipped under RT sun shadow; FIRST: they borrow the main cull's buffers, see Scene.md)
    → Indirect cull (+ the previous-transform copy, see "Motion vectors") → Light grid → Force compute
    → Terrain wetness                      (tick frames only)
    → Grass cull                           (desktop; off: the draw count is cleared instead; see "Procedural grass")
    → Clutter cull                         (desktop: cull + bucket prefix + sort; off: its draws are emptied; see "Ground clutter")
    → Grass near shadow                    (desktop, its own 2048 map; after the grass and clutter culls)
    → Cloud shadow                         (the cloud shadow map; only the cascades due this frame; see "Volumetric clouds")
    → Cloud sky                            (the clouds of the GI sky map, before GI bakes it)
    → GI                                   (BLAS builds + THIS frame's TLAS, then the probe trace)
    → Rain occlusion                       (ray query on that TLAS; only while a weather volume requested the map; see Particle)
    → Particle sim                         (after the TLAS: it reads this frame's rain map)
    → Volumetric fog
    → Scene opaque                         (WRITES the scene depth, then parks it read-only)
    → Far tree bake                        (only while a bake runs; see "Far-tree volume")
    → Post-scene compute                   (RTAO + cloud march + far-tree march, phase by phase; reads this frame's depth; see below)
    → Force intervals → Force union march  (own render passes in the primary around cached draw secondaries, half-res, gated on the force enable; see Force)
    → Scene forward → TAA | DLSS (mvec pass + the upscale, see "DLSS")
    → Motion blur (desktop, "Post/Motion blur": tile max only; the composite gathers)
    → Eye adaptation (+ bloom level 0)
    → Bloom (desktop, "Post/Bloom": the down/up chain; the composite mixes it in)
  Composite + UI                           (CB.present, see "Recording vs submitting")
```

RTAO's result is read by the NEXT frame's forward pass. The quoted sections are in the topic files: "Motion vectors"
and "DLSS" in [PostProcess.md](PostProcess.md), "Procedural grass", "Ground clutter" and "Far-tree volume" in
[Vegetation.md](Vegetation.md), "Volumetric clouds" in [Clouds.md](Clouds.md).

> ### THERE IS NO DEPTH PREPASS / G-BUFFER
>
> Measured: a prepass cost about as much as the whole forward pass (both are geometry-bound) and
> its early-Z saved the forward pass ~10 %. **`SceneColor` owns THE scene depth**, and every
> screen-space reader samples that one image: RTAO, the force march, decals, particles, fog apply,
> TAA — and, as "last frame's depth" out of the OTHER frame slot, the forward pass's AO reprojection,
> the AO temporal pass and the particle collision.
>
> * **The depth has exactly TWO layouts:** `DEPTH_STENCIL_ATTACHMENT` while the opaque stages write
>   it, and `SCENE_DEPTH_SAMPLED_LAYOUT` (= `DEPTH_STENCIL_READ_ONLY`, SceneColor.ixx) the rest of the
>   time — the layout of EVERY sampling descriptor and of the layered stages' read-only depth
>   attachment, which is why those stages may sample the depth they test against. One barrier per
>   frame and eye (`recordSceneOpaqueToSampled`, which also moves the motion target to
>   SHADER_READ_ONLY); the first stage clears from `UNDEFINED`. A new depth reader uses that layout
>   constant and runs after "Scene opaque".
> * **The depth-writing stages have a SECOND colour attachment, the motion target** (see "Motion
>   vectors" ([PostProcess.md](PostProcess.md))). Their passes are a separate render-pass family (`getOpaqueRenderPass`,
>   `getOpaqueFramebuffer`; a stage variant with `depthReadOnly = false` is of it), so the layered
>   stages and their pipelines never see it. **A pipeline drawn in the opaque group sets
>   `GraphicsPipelineLayout::motionTarget`** (two blend states), and its secondary begins with
>   `beginScenePassSecondary(frameIdx, cb, true)`.
> * **Nothing reads a normal target.** Normals come from depth (`normalFromDepth`, see RTAO in [Lighting.md](Lighting.md)).
> * **TAA's ocean flag is the scene colour's ALPHA:** `ocean.fs.glsl` writes 0, every other opaque
>   surface its material alpha (> 0), and TAA reads `alpha < 0.004` on non-sky pixels. So nothing
>   layered over the opaque scene may write alpha: `GraphicsPipelineLayout::colorWriteAlpha = false`
>   on decals, debug lines, force shells / union / upsample, particles, the far-tree apply, cloud apply
>   and fog apply, and `GraphicsPipeline` masks alpha on every BLENDED variant (a transparent mesh keeps
>   the alpha of the opaque surface behind it). The exceptions are the Ocean and River variants
>   (`dualSourceAlpha`): they blend their edge over the ground, so they composite the flag themselves (see
>   "Terrain surface water" ([Terrain.md](Terrain.md))). **A new pipeline that draws into scene colour after the opaque
>   stages sets `colorWriteAlpha = false`.**
> * **The Sky variant does not write depth** (`depthWrite = false`): a sky pixel's depth stays at the
>   cleared far plane, which is how every reader tells "sky". See THE SKY DRAW ([Scene.md](Scene.md)).

The scene renders as **one render-pass INSTANCE per enabled stage** (separate secondaries, explicit
attachment barriers between instances — `sceneInstanceBarrier`, RendererRecord.cpp), in two groups:

```
Scene opaque   (depth WRITTEN):            Static meshes → GI probe debug
Scene forward  (depth READ-ONLY + sampled): Decals → Debug lines → Force shells → Force union blend → Particles → Far trees apply → Cloud apply → Fog apply
```

(With the fog on, the fog apply composites the clouds and far trees itself; with the fog off and clouds on, the cloud
apply composites the far trees.) `SceneColor::getStageRenderPass(first, last, depthReadOnly)`: the first instance
clears, the last hands colour to TAA, and the depth is either written or a read-only attachment. All variants are
compatible with the base pass (never begun; pipelines and secondaries are built against it) since only
load/store ops and layouts differ (**the deps are verbatim — they are part of compatibility**), and
one framebuffer serves them all. **The GI probe debug impostors write depth** (`gl_FragDepth`, they
sort among themselves), so they run in the opaque group; the AO trace and the decals then see them as
geometry (debug only). **VR** records the same split inline per eye: static meshes → barrier → AO →
clouds → one layered instance.

**Line width.** Every pipeline that rasterizes lines — the debug-line pass (line topology) and the
wireframe variants (`eLine` polygon mode) — is created with `lineWidth = LINE_WIDTH` (3 px, a
constant in `Objects/GraphicsPipeline.cpp`). This needs the core `wideLines` device feature, enabled
in `Objects/Device.cpp`; it is NOT an extension.

> The split exists so `GpuProfiler` can time each stage: timestamps must be written **outside** render
> passes, because multiview would replicate them and a SECONDARY_COMMAND_BUFFERS subpass admits no
> other primary commands. See Profiling in [`Docs/Context/Core/CONTEXT.md`](../Core/CONTEXT.md).

## Recording vs submitting

`recordCommandBuffers()` records the CACHED SECONDARIES, **only on invalidation frames**
(`setHaveToRecordCommandBuffers`). The primary is re-recorded every frame, **so enable toggles take
effect immediately.**

* **Indirect dispatches never re-record**: emitter, query and spawn changes for force and particles,
  and the skinning dispatch (CPU-written dims per frame), so instances spawned later run without a
  re-record.
* **Bindless descriptor writes** for streamed textures happen first, before anything records against
  them — safe because `present()` waited this slot's fence (`waitFrameSlot`) and the arrays are
  UPDATE_AFTER_BIND.
* The baked terrain-data cascades, the RTAO view and the TLAS are all UPDATE_AFTER_BIND too, rewritten
  only when this slot re-records or the underlying handle changed.

**Call `setHaveToRecordCommandBuffers()` after invalidating changes.**

**TWO BATCHES PER FRAME.** `CB.primary` (`recordCommandBuffers`) holds ALL the frame's work but the
swapchain pass and is submitted BEFORE `acquireNextImage`, with no image wait and no fence; `CB.present`
(`recordPresentCommandBuffer`: the composite + ImGui render pass into the acquired image's framebuffer) is recorded
after the acquire and submitted through `SwapChain::submitCommandBuffer` - the only batch that waits `presentComplete`,
signalling `renderComplete` and the slot's fence (a fence signal covers every batch submitted before it on the queue).
**Why:** the driver starts a batch only once its wait semaphores signal, whatever the stage mask, and the image is
released only after the previous frame's present - with one primary, every GPU frame waited for that, VSync off too.
The composite reads the work batch's results through that batch's own barriers (a barrier's second scope reaches later
batches). The "GPU Frame" profiler scope opens in `CB.primary` and closes in `CB.present`. An acquire failure after
the work batch drains the queue (the slot does not advance).

## Per-frame CPU -> GPU buffers live in ReBAR, not system memory

A buffer the CPU writes and the GPU reads every frame is `eHostVisible` + `BufferHostAccess::eSequentialWrite`, and
VMA (`MEMORY_USAGE_AUTO`) places it in DEVICE-LOCAL host-visible memory (ReBAR). `eHostCached` / random access puts it
in cached SYSTEM memory, where every GPU read crosses PCIe (latency-bound: measurably slower BLAS builds and culls).
**Cached system memory is for READBACKS only** (the GPU writes, the CPU reads: ocean, force, particle counters, LOD
stats). A ReBAR mapping is uncached for CPU READS: a table the CPU also reads in loops keeps a CPU mirror
(`AccelerationStructure::m_meshAlias`, written through `writeMeshAlias`); a rare read-back on growth
(`growInstances`, `resizeBlasAddressBuffer`) is acceptable.

## Barriers and overlap

A compute->compute barrier waits for ALL earlier compute work in the queue. **The post-scene compute group**
(`recordPostSceneCompute`: RTAO, the cloud march + temporal, the far-tree march) records the three passes PHASE by
phase (`RTAOPipeline::recordPhase`, `CloudPipeline::recordPhase`, `TreeVolumePipeline::marchPrepare` /
`recordMarchPhase`; one cached secondary per phase for RTAO and the clouds) with ONE barrier between phases - their
union - instead of each pass's own. GPU scope "Post-scene compute", with one child scope per PHASE ("Trace + marches",
"Temporal passes", "Blur + upsample") and inside each one per PASS ("RTAO trace", "Cloud march", "Far trees march",
...). In practice the passes of a phase still run one after the other (Nsight GPU Trace), so a pass's stamp is exact.
"Renderer/Overlap compute" off: the same barriers around each pass alone, each in its own scope ("RTAO", "Cloud
march", "Far trees") - for per-pass timing. **Measured gain: ~0.01-0.02 ms**: those passes each fill the GPU, only
their tails overlap.

**Tried and dropped:** the cloud shadow + sky recorded after the TLAS build (to run beside it) - no overlap in
practice; the TLAS built PREFER_FAST_BUILD - the build gets faster by as much as the traces get slower. Overlapping
BIG passes gains nothing here.

**Synchronization validation**: `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` with `EValidation::ENABLED` in main.cpp. A
hazard is an error, so the callback's break stops the run - set `setBreakOnValidationLayerError(false)` to collect them
all. `StagingManager::updateNoLock` puts a copy -> copy barrier before a copy that overlaps the range written to its
buffer since the last one (a growth's whole-buffer re-upload vs a one-entry update in the same batch). The one known
remaining report is Streamline's own legacy `vkCmdPipelineBarrier` on the depth in the DLSS evaluate (a WAW against
the depth store) - not ours.

---

# Features

* **Lights** culled through a world-space hash table into distance-scaled grids. `MAX_LIGHTS` is
  `USHRT_MAX - 1`.
* **GI** through scrolling clipmap radiance cascades (ray-traced probes).
* **RTAO**, **TAA**, **PCSS cascades**, **volumetric fog**, **volumetric clouds** (sandbox), **GPU compute skinning**,
  **motion vectors** (moving + skinned meshes; see "Motion vectors" ([PostProcess.md](PostProcess.md))).
* **"RT/RT Sun" replaces the PCSS cascades entirely** — the shadow cull and draw are skipped.

Each feature's details are in its topic file (see [The documentation files](#the-documentation-files)).

---

# Layout under `Private/`

| Directory | Contents |
|---|---|
| `Objects/` | Thin Vulkan wrappers: Device, SwapChain, Buffer, ComputePipeline / GraphicsPipeline, AccelerationStructure, SceneColor (colour + THE scene depth), ShadowMap, GpuProfiler, BakedWorldMap, Texture, Shader, `Allocator.ixx` (`GpuAllocator`), **VrEyeTargets** (the two per-eye LDR composite targets, re-created with the swapchain), ... |
| `Pipeline/` | One class per pass or feature: StaticMeshGraphics, GIProbe, RTAO, Taa, Dlss (its motion vectors), MotionBlur, Bloom, VolumetricFog, Cloud, EyeAdaptation, Composite, Skinning, DebugLine, Particle, Decal, ForceField, OceanSimulation, TerrainWetness, LightGrid, Grass, Clutter, TreeVolume (the far-tree volume), RainOcclusion, IndirectCull, ShadowCull (both own a DrawCompact), ShadowMapGraphics. **Each registers its own tweaks.** |
| `Data/` | The GPU-resident scene, carved out of the Renderer. Streaming and managers: MeshDataManager, TextureManager, TextureStreamer, MeshStreamer, StagingManager, ShaderDatabase, GpuCrashTracker (Aftermath, runtime-loaded, optional), TreeRecordPool (see [Scene.md](Scene.md)). Plus the registries the Renderer owns and delegates to — each takes its frame-wide effects as callbacks (`onGpuIdle` before a buffer is re-created, `onInvalidate` to re-record) and knows nothing about the device or the pipelines: |
| | **`InstanceStream`** — THE per-frame push surface: the six mapped buffers `renderNode` writes into (transforms, pass masks, LOD bias, mesh instances, first instances, mesh count - the slots the draw-list compaction walks), one set per frame slot, the device-local PREVIOUS transforms + masks the motion vectors read (`recordPrevCopy`), plus the lock-free monotonic instance claim, the transform slot free list and the two capacity growths. **A claim past the capacity is never rolled back** — see the header. **BOTH growths run only in `checkFrameCapacities`, never mid-frame:** re-creating the node buffers after some nodes pushed drops their transforms / masks / biases for that frame (a whole-scene one-frame flicker). A node spawned past the node capacity is skipped by `renderNode` until then. |
| | **`SharedTable<T>`** — an append-only device-local scene table with slot recycling and a CPU mirror: the mesh infos, the materials and the mesh instance offsets are three instances of it. Growth doubles, re-uploads the mirror and re-records. |
| | **`SkinnedMeshRegistry`** — the skinning jobs + their parallel skinned-BLAS builds, the bone palette store, the per-container sources, and the spawn BUNDLES (parked in place on death, reused wholesale by the next spawn of the same container). |
| | **`MeshLodRegistry`** — the LOD chains, the per-mesh chain mapping, the three GPU selection buffers and `IndexRangeFreeList`. The Renderer keeps only the RT-alias half of `addMeshLodGroup`, because aliases are AccelerationStructure state. |
| | **`FrameSubmission`** — what the entity pass ADDS rather than draws: lights, fog volumes, decals, each a lock-free bump claim whose overflow is simply dropped. It also owns the light grid's per-slot GPU scratch and **its between-frames job** (`buildLightGrid` at the kick, `applyLightGridGrowth` at the join), because the table entries are 4x the pipeline's grid capacity — the CPU claim table IS the GPU table — so both grow together. |
| | **`TerrainResources`** — what Procedural pushes in: the world params, the splat material set + its climate boxes, `TerrainTexTweaks` / `TerrainWetTweaks`, the CPU-baked height/water map every terrain-aware pass samples, and the fixed-tick **wetness state machine** (`advanceWetness`). `buildUboTerrain` stays in the Renderer and reads it. |
| | **`ParticleState`** — the GPU particle system's CPU side: the emitter slot table (KILL flag drains through the sim before a slot recycles), this frame's spawn requests, the `"Particles"` tweaks, the weather volume's rain box (latched by present for the NEXT frame's begin-frame job) and the one-shot pool reset. |
| | **`ForceFieldState`** — params, the emitter + point-query slot registries (STABLE indices across the ~2-frame readback latency), this frame's bake chunk set, the shell-cull build, and **its between-frames job** (`buildGrid` / `applyGridGrowth`), mirroring `FrameSubmission`'s. |
| | **`BindlessTextures`** — the two counts that are not the same (the fixed LAYOUT CAP baked into every pipeline layout vs the LIVE descriptor count the variable-count sets are allocated with), the streamer's pending slot writes, and the deferred free queue. The seven consumers (the static mesh and shadow draw sets, GI, RTAO, particles, decals, clutter) are reached through two callbacks the Renderer wires in `initBindlessTextures`. |
| | **`RayTracingScene`** — owns `AccelerationStructure` plus the CPU bookkeeping around it: the per-MeshInfo vertex counts (BLAS maxVertex) and skinned-output flags, the TLAS instance capacity, and **the one-time build watermark**. A static BLAS builds once, so `takeBuildList` scans forward from the watermark — which is exactly why a RE-STREAMED mesh and a RECYCLED slot below it must be queued explicitly, and why a skinned output region is never in the list at all. |
| `Layout.ixx` | `RendererVKLayout` — every GPU struct and `MAX_*` cap. The structs must stay in sync with `shared.inc.glsl`; the frame UBO is not here (registered: `UboBlock.ixx`). |
| `UboBlock.ixx` | `UboBlock`: the frame UBO's registered layout + bytes - every value one `add` / `addArray` line with its sources (tweaks = lockable, `UboLive`, `UboPresent`). |
| `UboDeclaration.cpp` | `buildUboDeclaration`: the UBO's GLSL text from the block's entries, each baked value as a `const`. |
| `PushBlock.ixx` / `.cpp` | `PushBlock`: a registered push block (self-binding `PushValue` handles, lockable values, the generated `push.generated.glsl`), `PushData` (one dispatch's bytes). See "Push blocks" ([FrameUbo.md](FrameUbo.md)). |
| `RendererUboBake.cpp` | **The tweak locks**: `registerUboLocks`, `applyUboLocks` (bake / unbake / re-bake, then reload every shader). |
| `RenderParams.ixx` | `OceanParams` (pushed by Procedural every frame) and the GPU allocation report types. The renderer settings types (`SkyParams`, `FogParams`, `ParticleParams`, `ForceFieldParams`, `GiSettings`, LOD, RT, ...) are **`Settings.Render`**'s (Code/Settings) and are NOT re-exported: an importer of RendererVK that touches their fields imports Settings itself. See "No `.ixx` imports Settings" below. |
| `RendererSettings.cpp` | The `RendererState` constructor (binds the settings reference members to `Globals::settings`), the small settings-reading members (`grassActive`, `taaActive`, `syncCloudDefines`, the setters, ...), the static_asserts Settings.Render cannot hold (it cannot import `:Layout`: `ForceFieldParams::teamColors` vs `MAX_FORCE_TEAMS`, `GrassParams::MAX_BLADES` vs `GRASS_MAX_BLADES`), and `Renderer::attachSettingsListeners`: every settings reaction (lit shader reload, full reload, re-record, render resolution, swapchain, GI grid / volume) as a `Tweak::onChange` listener, plus the hand-over of the baked state (debug modes, RT shadow flags, cloud defines). Called from `initialize()`, before `registerUboLocks`. |
| `Util/` | `VK`, `DDS`, `LightingUtils`, `glslang`, `stb_image`, `Aftermath` / `Streamline` (the runtime-loaded DLLs), `GridClaim` (the light/force hash grid's CPU side), `SlotTable` (`RecycledSlotTable<T>` — the lock-free deferred-recycle slot table every emitter/query registry uses) and `SlotAlloc` (the lock-free slot primitives: `SlotStack`, `PagedArray`, `SlotRangeAllocator`). |
| `Public/` | **The API files** (see "The public API split" below), beside `RendererVKModule.ixx` (the primary interface, `export module RendererVK;`): `Renderer.ixx`, `RenderNode.ixx`, `RenderMesh.ixx` (+ `RenderMeshData`), `RenderMaterial.ixx` / `RenderTexture.ixx` (the material and texture handles), `RenderTreeSet.ixx` (a set of procedural tree pieces + its value types), `RenderParticleEmitter.ixx` / `RenderForceEmitter.ixx` / `RenderForceQuery.ixx` (the GPU emitter and query slots), `ObjectContainer.ixx` (+ `NodeSpawnIdx`, `ContainerMaterialOverrides` = `ObjectContainer::MaterialOverrides`), `Light.ixx` (the light shapes: plain structs, no state of their own), `TreeRecords.ixx` (the world tree records' API types; the pool itself is `Data/TreeRecordPool`), `RendererVK.fwd.ixx`. |
| `RendererState.ixx` | `RendererState`: every data member and every private helper, all `protected`, plus the friends (ObjectContainer + ObjectContainerState, RenderNode, RenderMesh, AnimatorComponent, MeshStreamer, and every handle below). |
| `RenderNodeState.ixx` / `RenderMeshState.ixx` / `RenderMaterialState.ixx` / `RenderTextureState.ixx` / `RenderTreeSetState.ixx` / `RenderParticleEmitterState.ixx` / `RenderForceEmitterState.ixx` / `RenderForceQueryState.ixx` / `ObjectContainerState.ixx` | The other classes' state (and RenderNode's `TransformSlots` + `Globals::renderNodeTransforms`; ObjectContainer's load steps `initializeMeshes` / `Materials` / `Nodes`), all `protected`, plus their friends. |
| `RendererTypes.ixx` | `RendererTypes` (the API's value types, named `Renderer::X` outside: the terrain splat aliases, `GrassGroundChunk`, `ClutterMesh`; the tree piece types are `RenderTreeSet`'s). A base of `RendererState`, because the state and the helpers use the types too. `EValidation` / `EVr` (namespace scope, `initialize`'s) are in `Renderer.ixx`, opaque-declared in `RendererState.ixx`. |
| The `.cpp` files | **Seven implementation units** below — they all say `module RendererVK;`, so a member may move between them freely. An API member is defined `Renderer::x`, a state member `RendererState::x`. |
| `Renderer.cpp` | Construction and the frame loop. **`kickGridBuilds` / `joinGridBuilds`** are the between-frames window: two jobs on one counter (the light grid merge and the force compaction + grid build), each a one-line call into the object that owns that state, with the rare exact-fit growth applied at the join. Construction: `initialize()` runs `attachSettingsListeners` + `registerUboLocks` → `initDeviceAndSwapchain` → `initPipelines` → `initPerFrameResources` → `initSharedBuffers`; per frame `waitFrameSlot` → `setFrameView` → `beginFrame` (+ its job kick/join) → `present`. |
| `RendererUbo.cpp` | **The frame UBO**: `buildFrameUbo`, the `buildUbo*` frame-state steps (the views, the camera velocity, GI's one-frame flags, the cloud wind + shadow recentre, the sun cascades, the force bake box, the wetness tick, the grass cascade), `registerUboValues` (every value, live and lockable, by subject). Pure CPU math over the param blocks and the `Data/` registries — it records nothing and touches no device object. Runs wherever `beginFrame` runs; `m_views` keeps last frame's mvps (the reprojection). Compiled with `/bigobj` (RendererVK CMakeLists): one lambda per UBO value passes the COFF section limit in Debug. |
| `RendererScene.cpp` | The scene the outside owns, in two halves. **Residency**: container add/remove, `renderNode` (the push), the spawn-path entry points, the bindless descriptor upkeep, and the cross-cutting work a capacity growth needs (`onUniqueMeshCapacityGrown`). **Submission** (bottom of the file, mostly called from jobs): lights / fog volumes / decals and the bodies of the emitter + query handles, each a thin delegate into the `Data/` object that owns the contract. |
| `RendererTrees.cpp` | The tree instance sets and tree records (see [Scene.md](Scene.md)) and the far-tree volume's recording. |
| `RendererRecord.cpp` | Command-buffer recording: one `record*()` per pass, plus the primaries (desktop / VR / present). **`buildSceneStages()` is THE scene stage table** — name, gate, cached secondary and per-eye inline recorder for every stage inside the scene-colour pass. `recordSceneSecondaries`, `recordPrimaryDesktop` and `recordPrimaryVR` all drive off it, so a stage is added, re-ordered or re-gated in ONE place; a null `recordInline` means desktop only. |
| `OpenXRSession.ixx` | VR (`Globals::openXR`, implements `IVrSession`). |

## The public API split

Every class used outside the library (`Renderer`, `RenderNode`, `RenderMesh`, `RenderMaterial`, `RenderTexture`, `RenderTreeSet`,
`RenderParticleEmitter`, `RenderForceEmitter`, `RenderForceQuery`, `ObjectContainer`) is split in two: an API file in `Public/` that holds only
the public members, and its base `<Class>State` in `Private/` with every data member and private helper, all `protected` (single, non-virtual
inheritance: the same layout and calls as one class, no vtable). A class keeps its partition name (`:RenderNode`), so no import changes; the state is a
partition of its own (`:RenderNodeState`). Multi-line bodies of API members go below the class as `inline` definitions; the class holds declarations and
one-line accessors. A friend goes on the State class (a friend of the base reaches its protected members through the derived object). **A State partition
declares every class it friends before the friend line** (`export class RenderNode;`): `RendererVK.fwd` declares `Renderer`, `ObjectContainer` and
`RenderNode` in `extern "C++"`, and a friend line that first introduces the name made MSVC mangle one importer's `oc::vector<RenderNode>` against the
unattached class (an LNK2019 on `VectorBase<RenderNode>`).

**The resource handles** (`RenderMesh`, `RenderMaterial`, `RenderTexture`, `RenderTreeSet`, `RenderParticleEmitter`,
`RenderForceEmitter`, `RenderForceQuery`) are move-only RAII: a static `create`, `isValid`, `destroy` (the destructor). Their bodies call protected
`RendererState` helpers through `Globals::rendererVK` (the State friends each handle), so the Renderer API names none of them. Declaration order sets the
destruction order (a tree set after its meshes, a derived material after its source), and **a global that holds handles must destruct before the
renderer** (InitSeg.h: the Procedural systems, `OC_SEG_GPU_EFFECTS` for Particle's and Force's systems). The emitter and query handles are safe to create
and destroy on a worker (lock-free slot tables).

`Renderer.ixx` reads as the API at a glance; everything else is in its base `RendererState`. A `RendererState` helper cannot call the API by itself
(the base does not know the derived class), so **a helper calls an API member through `self()`**: `this` as the
`Renderer` (a `static_cast`, defined in Renderer.cpp; `Renderer` is `final` and the only derived class), e.g.
`self().sceneFocusOrCamera()`. Members stay plain `m_x`. Tried C++23 deducing `this` (`this Renderer& self`) instead,
dropped because every member access in such a function must be `self.m_x` and every caller needs a `Renderer` too. A
class that friends the renderer friends `Renderer` (for an API body), `RendererState` (for a helper), or both.

## No `.ixx` imports Settings

**A RendererVK interface (`.ixx`) imports `Settings.fwd`, NEVER `Settings` / `Settings.<Domain>`** (`Settings.Tweaks` is
allowed: it holds no defaults). Every `.cpp` here is `module RendererVK;`, which imports the whole RendererVK interface,
so one `.ixx` on that import graph that imports Settings makes a settings DEFAULT change rebuild the whole library. With
the rule, only the `.cpp` files that `import Settings;` rebuild. So, in an interface:

* a settings type is named only as a reference, a pointer or a parameter (`Settings.fwd` lists them);
* a reference member to `Globals::settings.x` is bound in the class's constructor in a `.cpp` (`RendererState`,
  `MeshStreamer`, `TextureStreamer`);
* a body that reads a settings field or `Globals::settings` goes to a `.cpp` (`grassActive`, `isEnabled`, ...);
* a COPY of a settings struct is an `oc::unique_ptr` made in the constructor (`ForceFieldState::m_params`,
  `TreeVolumePipeline::m_bakedSettings` / `BakeJob::settings`), or a local struct of the fields it needs
  (`LightGridComputePipeline::LodParams`).

A `.cpp` that reads settings fields adds `import Settings;` itself (nothing leaks it through the partitions any more).

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
  Each entry keeps a `GpuAllocationInfo` (`:RenderParams`) with the create-time fields — image extent /
  mips / layers / samples / format / usage, buffer size / usage (read from the maintenance5
  `VkBufferUsageFlags2CreateInfo` in the pNext chain) — and the visit adds the VMA side (size, block
  size, dedicated, memory type flags, mapped).
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
* **Never record an EMPTY scope** (a begin and an end with no command between them): Nsight's GPU Trace times a
  zero-length range at the NEXT workload, and when that is the first command inside a render pass begun with
  SECONDARY_COMMAND_BUFFERS contents (the force interval pass, a scene stage), its vkCmdWriteTimestamp is illegal -
  `VUID-vkCmdWriteTimestamp-commandBuffer-recording`, under Nsight only. Gate the scope with the work, not the work
  inside the scope (e.g. "Far tree bake" opens only when `TreeVolumePipeline::updateBake` reports a running bake).
* **Nsight 2026.3.1 GPU Trace: turn OFF "multi-pass metrics"** in the capture settings. With it on,
  opening a capture that holds these labels crashes Nsight (heap corruption in its "Per Shader Warp
  Occupancy" code) for every labelled range that does not start at the beginning of the primary. The
  label structure is valid; the object names and the shader debug info were each ruled out as the
  trigger.
* Not named: the OpenXR session's own command pool and clear buffer (raw C API, no `:Device` import).

## The graphics-queue mutex

**`StagingManager` is THREAD-SAFE** — one internal mutex over the ring and region lists, `upload*`
callable from jobs — and **its ring-overflow path submits to the graphics queue.** The mutex covers
only the CLAIM (the ring range plus its copy region): `upload*` memcpys into its range after the
unlock (`copyAndUnlock`), so concurrent uploads copy in parallel (the terrain's chunk uploads fan out
on it), and a per-buffer writer count makes `updateNoLock` wait for every copy into the buffer before
its flush and submit (a writer takes no lock to finish, so the wait under the mutex cannot deadlock).

> That is why **EVERY graphics-queue call in the engine goes through Device's queue mutex**: submits
> through `CommandBuffer::submitGraphics`, presents through `SwapChain::present`, idles through
> `Device::graphicsQueueWaitIdle()`. **Never call `getGraphicsQueue().submit()` or `waitIdle()` raw.**
> Lock order: staging mutex → queue mutex.

**`MeshDataManager` allocates LOCK-FREE** (`BitRangeAllocator<true>` per pool) under a `std::shared_mutex` that only
fences a GROW: every allocate / `upload*` / `free*` holds it SHARED - over the staging copy too, which names the
buffer by handle - and `growBuffer`, which replaces the buffer AND the allocator's bit array, holds it
EXCLUSIVE (a full allocator re-tries under the exclusive lock before growing: another thread may have grown it).
A lock over the allocation alone is not enough: a grow on another thread could replace the buffer between an upload's
allocation and its staging copy. Lock order: grow → staging → queue.

**`present()` holds a `MeshDataManager::BufferHold`** (the grow lock, shared) from its mesh-generation check through
the submit, so the check, the recording and the submit see ONE set of mega-buffers; a worker's grow waits for the
release right after the submit. It starts AFTER `joinGridBuilds` and after an early join of the GPU-profiler collect
job (`recordCommandBuffers`' own join of it is then a no-op): **NO helping `JobSystem::wait` inside the hold** - main
helps High / Normal jobs there, and a helped job that allocates mesh data (the terrain's upload job) would take the
grow lock again on the holding thread (deadlock behind a waiting grow). `allocate` / `release` assert that the
calling thread is not the holder.

Known unlocked queue holders, both main-thread-only by construction: the ImGui backend (texture-upload
submits in the post-join window) and OpenXR (`xrEndFrame`; VR stays synchronous on main).

`update()` fence-waits under the staging mutex, so concurrent uploads block for that long — **keep bulk
uploads off latency-critical threads.**

**`uploadShared`** is the upload into a SHARED buffer (not per-frame-in-flight) that the draws, GI / RTAO or the BLAS
builds read every frame: the mesh mega-buffers (MeshDataManager), the `SharedTable`s (MeshInfos, materials, instance
offsets) and the MeshLodRegistry tables. Such a copy can rewrite bytes a frame in flight reads, and a ring overflow
submits a batch the moment it fills. The batch that holds one (`m_sharedWrite`) therefore opens with a barrier from
`eAllCommands` to the copy: a barrier's first scope is every command earlier in submission order on the queue,
earlier submissions included, so the GPU orders the copies after those frames and **the CPU never waits** (a CPU
queue drain here cost 11 ms on main through a terrain upload's join). A batch without one keeps its overlap.

## VR

Two-phase init around Vulkan device creation; stereo is a 2-layer multiview swapchain. **In VR the
editor viewport sub-rect is ignored** (VR renders full-extent; `setViewportRect` returns at once).

`setFrameView` applies the rect — **`setViewportRect` is private, so rect changes only happen at frame
boundaries** — and the projection is built from its aspect.
