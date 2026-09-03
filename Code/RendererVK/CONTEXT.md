# RendererVK

> Library documentation for `Code/RendererVK`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

## Overview

Modern Vulkan renderer, minimum spec RTX 2000. Device Generated Commands for everything.
Reversed-Z main view (near = 1, far = 0); shadow maps stay standard; fullscreen passes disable the
depth test.

**Features**

* Lights culled through a world-space hash table into distance-scaled grids.
* GI through scrolling clipmap radiance cascades (ray-traced probes).
* RTAO, TAA, PCSS cascades, volumetric fog, GPU compute skinning.
* An "RT sun shadow" toggle replaces the PCSS cascades entirely — the shadow cull and draw are
  skipped.

### Long-range sun shadows

"Shadows/Terrain march *". Past the PCSS `Max distance` and `RT/TLAS Range`, distant pixels
cone-march the baked terrain height cascades (`terrainSunVisibility` in `terrain_height.inc.glsl`)
and take `min` with the shadow term. The march fades in inside cascade range, so the handover is
seamless. Steps double from `Terrain march bias`, which is also the self-shadow bias. Deterministic —
no jitter, no temporal integration.

### TAA off

"TAA/Enabled" off BYPASSES the pass completely: the secondary CB is not recorded, the dispatch and
its GPU scope are skipped, and eye adaptation and composite sample this frame's SceneColor directly
(`resolvedLayout` SHADER_READ_ONLY instead of TAA's GENERAL storage image; the scene-colour barrier
gains the fragment stage, since the composite then reads it).

It used to only zero the feedback, so a disabled TAA still ran a full-screen resolve that copied — a
"TAA" scope in the profiler for a pass doing nothing. The tweak carries `onReRecord`, so toggling
rebuilds the descriptors either way.

### Colour

Everything upstream of composite is linear HDR. `EyeAdaptationPipeline` (log-luminance histogram
auto-exposure) feeds `CompositePipeline` (HDR → display into the swapchain before ImGui; tonemap
off / Reinhard / ACES / AgX).

## Frame pacing

`Core.Time` owns it all. `Time::registerTweaks()` plus
`Time::beginFrame(focused, vr, vsync, displayRefreshHz, Window*, waitFence)` — ONE call at the loop
top does the fence wait, the frame-rate limit, the event-pump kick, and the next frame's
`update(attributedTime)`. The pacing state and the "Time" tweaks, all Saved, live on `Time`.

The fence wait goes through the function-pointer hook main passes,
`Renderer::waitFrameSlot(timeoutNs)` — bounded or `UINT64_MAX` blocking, returning false only on
timeout — because Core cannot see the renderer.

**The limit.** The loop top limits to `Max FPS` (0 = uncapped, so the fence/vsync alone throttles),
or to `Inactive max FPS` (30) when the window is unfocused. Never in VR — `xrWaitFrame` owns pacing.
It sleeps in 1 ms steps while more than `Busy-wait window (ms)` (2) remains — real milliseconds
thanks to the window thread's `timeBeginPeriod(1)` — then `_mm_pause` spins to the target.

**`Stable frame time` (on).** When it waited, the frame's END is ATTRIBUTED as the desired end, not
the post-wait clock, and that becomes the next frame's START fed to `Time::update(now)`. The OS's
post-target lateness is charged to the next frame as processing time, so dt stays flat instead of
jittering with scheduler tails — the CryEngine / Star Citizen "tail jitter" rule. An already-late
frame skips the wait and ends now.

**Uncapped + vsync.** With `Max FPS` 0 and `Renderer::isVSyncEnabled` (FIFO present), every frame
lands on a WHOLE number of refresh periods by construction; only the CPU's view jitters, because the
throttle lands either in the loop-top fence or in the end-of-frame acquire. Loop-top intervals read
like 4/8 ms around a 6 ms period, which is why a tolerance-gated snap rejected most frames. So the
start is ALWAYS quantized to the nearest whole number of periods (min 1; a dropped frame = 2). The
period is the display's reported refresh (`Window::getDisplayRefreshHz`, queried on the window thread
at creation and on display/mode-change events), or the measured EMA when the platform reports none.
Rounding against the real clock every frame bounds drift to half a period. Without vsync the
intervals are arbitrary and the raw clock stands.

The period EMA (seeded 1/60, converging in ~20 frames) tracks RAW frame starts and drives the
uncapped pump prediction. "Frame limit" is the wait scope.

**VSync is a runtime tweak.** "Time/VSync" (Saved) is registered by `Renderer::initialize` on
`m_vsyncEnabled`. Present mode FIFO vs Immediate is swapchain creation state, so `onChange` runs
`recreateSwapchain()` (device idle + re-init), guarded on `m_initialized` because a saved or override
value fires `onChange` at registration before the swapchain exists. `--no-vsync` is just
`setOverride("Time/VSync=0")`, pinned for the run and never written back. `Renderer::initialize` no
longer takes a vsync parameter.

## Frame slot wait

`Renderer::waitFrameSlot(timeoutNs = UINT64_MAX)` waits on the current slot's fence.

* Returns true once signaled or already waited this frame; false only on a bounded timeout.
* Timeout 0 is a marker-free STATUS POLL that still claims the slot when signaled.
* A device error recreates the swapchain and reports signaled.

It is called FIRST THING in the main loop (through `Time::beginFrame`) — before the "main loop"
profiler scope opens and before input is sampled — so the vsync/GPU stall shows as its own top-level
"Fence wait" (the main-loop scope measures only real work) and input → sim → present stays tight. The
wait used to sit inside `beginFrame`, between input sampling and the sim: about 4 ms of stale input
at 165 Hz.

**Nothing may write a slot's host-visible per-frame buffers before it** — renderNode instance
memcpys, LOD redirects, sparse transforms. Torn slot data was the one-frame LOD-flash bug.
`beginFrame` asserts it ran (and waits anyway); `present()` re-arms it when the slot advances. It is
idempotent within a frame.

## Begin frame as a job (desktop)

`Renderer::kickBeginFrameJob(camera, viewportRect)` / `joinBeginFrameJob()`. Camera and rect are
copied into members; one High "Begin frame job".

* main.cpp kicks BEFORE `physics.update`, next to "Spatial cull" — contact dispatch is deferred out
  of the step, see Physics. It overlaps the physics step, `audio.update` and the nav publish, and
  joins before the contact dispatch and `world.update`.
* Legal because the window is renderer-QUIESCENT: tweak `onChange` is flushed at the top of the
  frame, force and terrain param setters run after `world.update`, and the streamers' Vulkan work
  happens in present on main. Also because `StagingManager` is thread-safe with every graphics-queue
  call behind Device's queue mutex.
* **NOTHING added between the kick and the join may touch renderer frame state.** `beginFrame`'s
  outputs — counter resets, `m_cameraPos` / `m_mipPixelScale`, `m_centerViewProj`,
  `m_sunCascadeViewProj`, the frustum — are valid only after the join.
* **VR** calls `beginFrame` synchronously on main: the kick only stores and DEFERS, and
  `joinBeginFrameJob` then runs it, because `xrWaitFrame` owns VR pacing and would pin a worker.
  Meanwhile the spatial cull job runs beside it on last frame's head view, through
  `Renderer::getCullView` (see Spatial). OpenXR poll, beginFrame and the head pose live inside it.

## Frame order

`recordCommandBuffers()`, called from `present()`:

```
skinning → ocean sim → indirect cull → light grid → force compute → particle sim
  → shadow cull+draw (unless RT sun shadow) → G-buffer → GI probes → RTAO
  → volumetric fog → scene forward → TAA → eye adaptation → composite + ImGui
```

Inside scene colour:

```
static mesh forward → decals → GI-probe debug → debug lines → force field → particles → fog apply
```

## Layout under `Private/`

| Directory | Contents |
|---|---|
| `Objects/` | Thin Vulkan wrappers: Device, SwapChain, Buffer, ComputePipeline / GraphicsPipeline, AccelerationStructure, GBuffer, SceneColor, ShadowMap, GpuProfiler, BakedWorldMap, ... |
| `Pipeline/` | One class per pass or feature: StaticMeshGraphics, GBuffer, GIProbe, RTAO, TAA, VolumetricFog, EyeAdaptation, Composite, Skinning, DebugLine, Particle, Decal, ForceField, OceanSimulation, light grid, indirect/shadow cull compute, shadows. Each registers its own tweaks. |
| `Data/` | MeshDataManager, TextureManager, TextureStreamer, MeshStreamer, StagingManager, ShaderDatabase, GpuCrashTracker (Aftermath). |
| `Renderer.ixx` / `.cpp` | Orchestrates everything; per-pass `record*()` methods. |
| `OpenXRSession.ixx` | VR (`Globals::openXR`, implements `IVrSession`). |

**StagingManager** is THREAD-SAFE — one internal mutex over the ring and region lists, and `upload*`
is callable from jobs. Its ring-overflow path submits to the graphics queue, which is why EVERY
graphics-queue call in the engine goes through Device's queue mutex: submits through
`CommandBuffer::submitGraphics`, presents through `SwapChain::present`, idles through
`Device::graphicsQueueWaitIdle()`. Never call `getGraphicsQueue().submit()` or `waitIdle()` raw; lock
order is staging mutex → queue mutex.

Known unlocked queue holders, both main-thread-only by construction: the ImGui backend (texture-upload
submits in the post-join window) and OpenXR (`xrEndFrame`; VR stays synchronous on main).
`update()` fence-waits under the staging mutex, so concurrent uploads block for that long — keep bulk
uploads off latency-critical threads.

**OpenXRSession** — two-phase init around Vulkan device creation; stereo is a 2-layer multiview
swapchain; in VR the editor viewport sub-rect is ignored. `Renderer::beginFrame(camera, viewportRect)`
applies the rect (`setViewportRect` is private — rect changes only at frame boundaries, and
`beginFrame` builds the projection from its aspect).

## Scene objects

* `ObjectContainer` wraps one loaded `ISceneData`.
* `spawnNodeForIdx()` returns a `RenderNode` (RAII, move-only); `spawnSkinnedNode()` the GPU-skinned
  variant, where AnimatorComponent feeds the bone palette through `allocateSkinningPalette` /
  `setSkinningPalette`.
* Destroying a RenderNode recycles without GPU sync: transform slots are free-listed, skinned bundles
  are parked in place per container, and rebased sub-node offsets are shared.
* `~ObjectContainer` frees ALL renderer resources (`Renderer::removeObjectContainer`).

## Per-frame instance flow

* `renderNode(node, passMask)` is LOCK-FREE and callable from any job between `beginFrame` and
  `present`: atomic cursor claims, monotonic. On overflow the tail drops for one frame,
  `m_instanceOverflowStart` CAS-min records it, `present()` clamps, and capacity regrows next
  `beginFrame`. **NEVER roll a bump cursor back.**
* `addLightInfo` / `addFogVolume` / `addDecal` are lock-free too. `addDebugLine` and the transform
  dirty lists are `PerWorker`-staged and merged in `present()`.
* Pass masks `PASS_MAIN` / `SHADOW` / `GI` (Layout.ixx = Spatial's bits). Main cull, shadow cull and
  the GI TLAS writer each early-out on their bit; TLAS also range-bounds by `RT/TLAS Range`.
* Transforms upload SPARSELY: write only through `RenderNode::setTransform` (change-detected,
  per-frame-in-flight dirty lists). A mutable `getTransform` would bypass tracking.

## Streaming

### Texture mips (`TextureStreamer`, "Texture Streaming" tweaks)

DDS textures load tail-only (mips ≤ 128 px) and stream against a VRAM budget by camera priority.

Residency changes recreate the image and rewrite the slot in all 7 bindless texture arrays
(UPDATE_AFTER_BIND, applied per frame slot after the fence wait). Disk reads happen on a worker
thread, and old images retire after `NUM_FRAMES_IN_FLIGHT`. Non-DDS, procedural and embedded textures
are pinned at full res.

### Mesh data (`MeshStreamer`, "Mesh Streaming" tweaks)

Cooked scenes register mesh sets: source mesh, LOD levels, and `.vsc` byte ranges. Vertex and index
mega-buffers are `BitRangeAllocator` free lists (`MeshDataManager`).

* Over "Mesh budget (MB)", least-recently-referenced sets unseen for "Mesh cold frames" evict:
  MeshInfos get `indexCount 0` (DGC, shadow and TLAS become no-ops with no re-record; static BLASes
  persist as geometry snapshots), and ranges free after `NUM_FRAMES_IN_FLIGHT`.
* Re-reference re-streams from the `.vsc` on a worker, and the LOD selector renders the nearest
  resident level meanwhile.
* A LOD chain shares ONE BLAS at "RT/BLAS LOD level" (alias table; the aliased RT meshIdx rides the
  TLAS instance `sbtOffset` for hit-shader fetches, and materials stay per-instance). Static BLASes
  copy-compact after build ("RT/BLAS compaction", ~30–50 % back).
* Only cooked static meshes stream. With Spatial culling everything visible stays referenced, so only
  despawned containers and `Col_` proxies go cold.

## Mesh LODs ("LOD" tweaks)

* `LodN_<name>` meshes and nodes form authored chains; level > 0 is never instanced, like `Col_`.
* Static meshes without authored LODs get meshopt-generated index-only chains, pre-baked by the scene
  cache.
* **Selection is per-instance ON THE GPU** (`mesh_lod.inc.glsl` in main and shadow cull; params live
  through the UBO). Instances push LOD0 and the cull redirects the draw: the CPU sizes every chain
  member's bucket to the chain's full count, hysteresis lives in per-instance GPU slots (benign
  races), shadow cull selects stateless 4× / +2 coarser, and GI needs no selection (one chain BLAS).
* The selection metric is screen-space error — meshopt simplify errors projected to pixels vs
  "Max error (px)". Authored chains without error data use projected diameter vs
  "Full-res pixels (Authored Lods)".
* `renderNode` keeps every level of a referenced chain warm, and the cull falls back to the nearest
  resident level during re-streams.
* Skinned meshes get bind-pose-generated chains too (per-instance `MeshLodGroups`, recycled with the
  bundle). Skinned output MeshInfos are excluded from static BLAS builds and compaction.

## Recording and shaders

* Record command buffers once, not per frame, where possible. Call `setHaveToRecordCommandBuffers()`
  after invalidating changes.
* Shaders are GLSL in `Assets/Shaders/`, compiled at runtime with glslang — shader edits need no
  rebuild, and F5 calls `reloadShaders()`.
* `*.inc.glsl` are includes. `shared.inc.glsl` / `ubo.inc.glsl` structs must stay in sync with
  `Private/Layout.ixx` (`RendererVKLayout`).
* SPIR-V plus source dumps land in `Assets/Local/` for Aftermath crash analysis.
