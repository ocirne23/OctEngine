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

So the start is QUANTIZED to the nearest whole number of periods (min 1; a dropped frame = 2), using
the display's reported refresh (`Window::getDisplayRefreshHz`) or the measured period EMA when the
platform reports none. **Rounding against the real clock every frame bounds the drift to half a
period.** Without vsync the intervals are arbitrary and the raw clock stands.

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
  Skinning → Ocean sim → Indirect cull → Light grid → Force compute → Particle sim
    → Shadow cull → Shadow draw            (both skipped under RT sun shadow)
    → G-buffer → GI → RTAO → Volumetric fog
    → Force intervals → Force union march  (own render passes, half-res; see Force)
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
lock-free too. **`addDebugLine` and the transform dirty lists are `PerWorker`-staged and merged in
`present()`.**

**Pass masks** `PASS_MAIN` / `PASS_SHADOW` / `PASS_GI` (Layout.ixx — the same bits Spatial uses). Main
cull, shadow cull and the GI TLAS writer each early-out on their bit; TLAS also range-bounds by
`RT/TLAS Range`.

**Transforms upload SPARSELY:** write only through `RenderNode::setTransform`, which is change-detected
into per-frame-in-flight dirty lists. **A mutable `getTransform` would bypass the tracking.**

`IndexRangeFreeList` recycles the contiguous slot ranges freed by destroyed ObjectContainers — mesh
infos, materials, instance offsets, skinning jobs. Ranges stay sorted and coalesced, and **allocation
is BEST-FIT so small requests do not shred the large holes.**

---

# Scene objects

`ObjectContainer` wraps one loaded `ISceneData`.

* `spawnNodeForIdx()` returns a move-only RAII `RenderNode`; `spawnSkinnedNode()` the GPU-skinned
  variant, where AnimatorComponent feeds the bone palette through `allocateSkinningPalette` /
  `setSkinningPalette`.
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
| `Pipeline/` | One class per pass or feature: StaticMeshGraphics, GBuffer, GIProbe, RTAO, TAA, VolumetricFog, EyeAdaptation, Composite, Skinning, DebugLine, Particle, Decal, ForceField, OceanSimulation, LightGrid, IndirectCull, ShadowCull, ShadowMapGraphics. **Each registers its own tweaks.** |
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
* **`setOceanParams`** — flipping `hitLighting` rebuilds the ocean fragment variant (GPU idle + shader
  reload). `setOceanWaveTrough` sizes the waterline band the fog scatter samples for the underwater fog
  boundary.

---

# Shaders

GLSL in `Assets/Shaders/`, **compiled at runtime with glslang — shader edits need no rebuild**, and F5
calls `reloadShaders()`.

* `*.inc.glsl` are includes.
* **`shared.inc.glsl` / `ubo.inc.glsl` structs must stay in sync with `Private/Layout.ixx`.**
* SPIR-V plus source dumps land in `Assets/Local/` for Aftermath crash analysis.
