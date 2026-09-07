# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Index

* [Response Rules](#response-rules)
* [Project Description](#project-description)
  * [Building](#building)
  * [Style](#style)
* [Libraries](#libraries) — dependency direction, and where each library's documentation lives
* [Asset files](#asset-files)

---

# Response Rules

* **DO NOT PRAISE THE USER.** If the user is correct in a statement, just say "correct" — nothing
  more.
* Always talk in ASD-STE100 Simplified Technical English.
* Always read CLAUDE.md / CONTEXT.md files. Library-specific documentation lives in
  `Code/<Lib>/CONTEXT.md` (see [Libraries](#libraries)) — **read the CONTEXT.md of every library you
  touch BEFORE changing it.**
* For each assertion, add a tag for your confidence in the statement, rather than relying on English:
  * `[Certain]` — hard evidence
  * `[Likely]` — strong guess
  * `[Guessing]` — filling gaps
* Do not commit or push files.
* Do not notify about App.exe linking failed due to in-use. The user knows to re-link if the linking
  failed due to an in-use executable.
* Prefer to let the user test out changes, rather than looking at log output or the screen yourself.
* **ALWAYS make file changes with the Edit/Write tools, NEVER via shell scripts** (python, sed,
  heredocs through Bash). The user reviews every change as the inline diff those tools render, and
  script edits show nothing. This overrides any session default that prefers shell-based editing.
* Keep this file and the `Code/<Lib>/CONTEXT.md` files up to date. Cross-library rules (style, build,
  dependency direction, asset formats) go here; everything about ONE library goes in that library's
  CONTEXT.md.

---

# Project Description

**OctEngine** (`C:\Github\OctEngine`). A modern C++ game engine. Windows / VS2026 only.

The `App` executable is the testbed: an ImGui editor — scene hierarchy, properties, asset browser,
DSL script editor, entity editor, tweaks, profiler, memory panel — around a viewport.

App.exe with NO mode flags boots into a fullscreen MAIN MENU: co-op / PvP / sandbox plus host/join, a
LOBBY for multiplayer picks, and a text CHAT in the lobby and in-game. See
[`Code/App/CONTEXT.md`](../Code/App/CONTEXT.md) for the menu/lobby/chat flow, **the frame loop** and
the testbed keys.

The actual game — top-down tactical PvP plus co-op PvE — is `Code/Game`; see
[`Code/Game/CONTEXT.md`](../Code/Game/CONTEXT.md).

## Building

CMake (min 3.14), the Visual Studio 18 2026 generator, build dir `Build/`.

| Task | Command |
|---|---|
| Reconfigure | `cmake -B Build` |
| Build | `cmake --build Build --config Debug` (also `RelWithDebInfo`, `Release`) |

* Reconfigure ONLY when files are added or removed — all sources are globbed. **Never edit
  CMakeLists to add files.**
* Output: `Build/Code/App/<Config>/App.exe`. Run it from anywhere inside the repo:
  `FileSystem::initialize()` walks up to find `Assets/` and makes it the working directory, so all
  asset paths are relative to `Assets/`.
* VS launch targets: `run-headless`, `run-server`, `run-client`, `run-netfuzz`, `run-cooked`. The
  `cooked` custom target configures and builds a sibling `Build-Cooked/` tree with
  `SCRIPTS_STATIC=ON` (see Script).
* **Unattended performance measurement:** `Tools/profile.ps1` (see Profiling in
  [`Code/Core/CONTEXT.md`](../Code/Core/CONTEXT.md)) builds, runs
  `App.exe --profile-after <sec> --quit-after <sec> --no-vsync --tweak ...`, and prints the text
  report. This is Claude's measure → change → re-measure loop; the user does not need to be at the
  screen. **Always `-Config RelWithDebInfo`.**
* No test suite, no linter. Verifying = it compiles, plus the user runs it.

### Global compile flags

```
/std:c++latest /Zc:__cplusplus /fp:fast /arch:AVX2 /GT /MP /Oi /Ot /W4 /Gw /GS-
/Zc:tlsGuards- /Zc:threadSafeInit-
non-Debug:  /GL /Ob2        + link /LTCG /INCREMENTAL:NO
Debug:      /JMC /ZI        + link /INCREMENTAL
```

| Flag | Why |
|---|---|
| `/GT` | Fiber-safe TLS — job code must not cache `thread_local` addresses across a `wait()`. **See the rule below before writing `thread_local` anywhere a job can reach.** |
| `/arch:AVX2` | The baseline `Core.OcBit` names instructions against. |
| `/GS-` | No stack cookies — a deliberate mitigation trade. |
| `/Gw` | One COMDAT per global, so `/OPT:REF` can drop unreferenced data (pairs with `/GL`). |
| `/Zc:tlsGuards-`, `/Zc:threadSafeInit-` | Guard removal. See below. |

* **Do NOT raise `/arch` to `/arch:AVX512`.** Intel fused AVX-512 off on consumer parts from Alder
  Lake onward, so it would exclude every recent mainstream Intel CPU while including a 2022 AMD one.
  `Core.OcBit` also relies on TZCNT/LZCNT being baseline — see its section.
* The two guard-removal flags are worth more here than in a normal build, **because `/GT` stops the
  TLS address behind each guard from being cached.**
* **`/Zc:tlsGuards-`** drops the per-ACCESS on-demand-init check on dynamically initialized
  `thread_local`s — the ~20 hot `thread_local oc::vector` scratch buffers in the entity pass,
  Npc/Structures and the Nav A*. Safe because all of it is static TLS in App.exe, which every thread
  initializes at creation. The runtime-compiled script DLLs are built by ScriptHost's own command
  line and keep their guards.

> ### `thread_local` IN JOB CODE IS A STANDING RULE too
>
> Jobs run on FIBERS: **any wait — `jobSystem.wait`, a nested `parallelFor`, a JobCounter join —
> can park the job and resume it on ANOTHER worker thread.** A `thread_local` (or a
> `PerWorker::local()` / `getWorkerIndex()`) taken before the wait then points at the wrong
> thread's slot: two jobs sharing one scratch buffer, or a write landing in a slot another job
> is reading.
>
> **A `thread_local` (or a held `PerWorker::local()` reference) in job code IS ALLOWED, on two
> conditions:** the code between taking it and its last use never waits, AND a `ThreadLocalScope`
> (Threading, debug-only) is alive over that whole span. The pin asserts at any fiber park or
> inline job on the thread, so a wait added later is caught at once instead of corrupting scratch.
> Pattern: the Nav chunk solve's heap (`Field.cpp`), the entity batch job's staging (`World.cpp`),
> every script entry point (`ScriptComponent.cpp`). When the wait cannot be ruled out, use instead:
>
> * **consume inline** — `SpatialIndex::forEachInSphere` / `forEachInFrustum` hand every hit to a
>   callback straight out of the traversal, so a probe needs NO result buffer (every game/entity
>   probe works this way now);
> * **owner-sliced scratch** — one buffer sized for the whole problem, each job working its own
>   index range (the transport's per-run BFS queue), or **one slot per parallelFor CHUNK**
>   (`JobSystem::numChunks(count, grain)` slots, indexed by `begin / grain` — the Force merge
>   stagings): memory scales with the work, not the context count;
> * a member of the object that owns the job when only one such job is in flight (the labels
>   job's unit list);
> * `PerWorker<T>` when a serial phase must DRAIN every slot with `forEach` (a merge) — a
>   thread_local cannot be enumerated from another thread, so every merge staging stays PerWorker;
> * a plain local `oc::vector` / `oc::small_vector` when the allocation is cheap enough.
>
> **Unpinned by design** (they cannot or need not carry the scope): the JobSystem's own worker
> context (re-read after every wait), Core's profiler / memory-tracker / allocator TLS (Core and
> File cannot link Threading; they follow the thread on purpose), `FileSystem`'s main-thread IO
> scope (kept tight around the call, never around a wait), and single-expression
> `PerWorker::local().push_back(...)` calls (no span for a wait to land in).

> ### `/Zc:threadSafeInit-` IS A STANDING RULE, not just a flag
>
> A function-local `static` that is not constant-initialized has **NO guard any more**, so one first
> reached from two workers at once is a race — a half-built object, or the initializer running twice.
>
> **Anything job code can reach must be constant-initialized or hoisted to namespace scope** (no
> per-access guard there at all, and `InitSeg.h` orders it if it needs ordering).

That is why these hold namespace-scope objects:

* `forceReferenceBudget` / `forceSphereFold` (Force/System.cpp) — reached from the merge parallelFors
* `AnimStateMachine::getCurrentStateName`'s `"<none>"` (Animation) — reached from a script thunk on
  workers
* `layerNames()` (Physics) — reached from the terrain collider's tile-build jobs
* `g_bodyLifecycleMutex` (Physics/Body.ixx) — a `std::mutex` is constant-initialized, so static init
  is safe, but it sits at namespace scope for the same reason

**Known and deliberately left:** the two `static PFN_vkSetDebugUtilsObjectNameEXT` in
RendererVK/Objects/Allocator.cpp are worker-reachable through the streamers, but a racing reader sees
the zero-initialized slot and just skips the debug name — **no torn value on an aligned pointer.**

`Procedural`'s diffusion `.cpp` TUs override to `/fp:precise /wd5050` — **load-bearing; see
`Code/Procedural/CMakeLists.txt`.**

### Dependencies

Prebuilt in `Dependencies/` (Include / Lib / Dll):

Vulkan-Hpp (no exceptions, no constructors), SDL3, ImGui docking branch, EASTL, Assimp,
glslang/shaderc, OpenXR loader, Nsight Aftermath, box3d, Steam Audio (static; `phonon(d).lib` bundles
pffft, mysofa and zlib), miniaudio (single header), meshoptimizer, zstd, onnxruntime.

box3d, steam-audio, meshoptimizer and zstd sources stay vendored for rebuilding the prebuilt libs —
recipes in `Dependencies/CMakeLists.txt`. They are otherwise unused; everything links prebuilt
`<name>.lib` / `<name>d.lib` pairs.

## Style

### Modules

Exclusively C++20 modules (`.ixx`), no headers.

* Most libraries use module partitions: `export module RendererVK:Renderer;` in the interface `.ixx`,
  `module RendererVK;` in the implementation `.cpp`. The public surface is re-exported from
  `Public/<Lib>.ixx`.
* `Core` uses dotted module names (`Core.Tweaks`, one module per `.ixx`). UI uses partitions
  (`UI:Scene`, `UI:ProfilerPanel`) except `UI.Gizmo` / `UI.fwd`.
* `Public/` `.ixx`s are importable from outside the library; `Private/` is internal.
* `Core.fwd` / `*.fwd.ixx` hold forward declarations.
* **Partitions leak unexported types across same-module files**, which is easy to miss: a type an
  outside importer needs must be `export`ed.
* **One exception to the no-headers rule:** `Script/Public/ScriptAPI.h` is a plain header — the ABI
  shared with runtime-compiled script DLLs, `#include`d on both sides. (`ScriptCtxMacros.h` and
  `Core/Private/InitSeg.h` / `forceinclude.h` are the other plain headers, all for preprocessor
  reasons.)

### Std headers

Std headers are re-exported as header units from `Code/Core/Public/OcSTL.ixx` — NOT Core.ixx, which
has no `import <std>` line left; it just `export import Core.OcSTL`.

OcSTL owns the container backing (currently EASTL — see Core) and additionally `export import`s the
std facilities that have no `oc::` spelling: iostream/sstream, format, mutex/shared_mutex/
condition_variable, thread/future/coroutine, chrono, execution, charconv, type_traits, atomic, new,
cmath, random. **Add a missing std header there.**

* `<bit>` is NOT among them — see `Core.OcBit`.
* **DELIBERATE EXCEPTION:** `<filesystem>` and `<fstream>` are NOT exported. All file and directory
  access goes through the File library's `FileSystem` (see File), so a library that needs the disk
  links File. Core itself therefore cannot do IO: `Core.Tweaks` takes injected read/write hooks
  (`setFileIo`, installed by main from FileSystem).

### Containers are `oc::`, never `std::`

`Core.OcSTL` (`Code/Core/Public/OcSTL.ixx`, `export import`ed by Core.ixx, so every importer of Core
has it) is the ONE file that names the std originals. Everything else says `oc::vector` /
`oc::string` / `oc::span` / `oc::unique_ptr` / `oc::atomic` / `oc::move` / `oc::sort` /
`oc::memory_order_relaxed` / ... **so swapping a backing implementation is an edit THERE instead of at
~6500 call sites.**

**Aliased:** containers plus adaptors, strings and views, pair/tuple/optional/variant, smart pointers,
`function`, `hash` and comparators, atomics plus the memory_order enumerators, and the `<algorithm>` /
`<utility>` verbs the codebase uses.

**STILL `std::` on purpose:** iostreams/sstream, chrono, mutex/lock_guard/thread/future, type_traits,
and the C runtime — platform surface, not container surface. **Formatting is the exception: use
`oc::format`, never `std::format`, so the result is an `oc::string`.**

With the EASTL backing live, `std::vector` / `std::string` in engine code is a hard COMPILE ERROR at
any boundary that meets an `oc::` type, not just a convention. Crossing deliberately is
`oc::toStd` / `oc::fromStd`, marked at every call site.

**Alias templates CANNOT be specialized:** a `std::hash` specialization is still written
`namespace std { template<> struct hash<X> ... }`, and is read back through `oc::hash`.

**Deliberate holdouts — leave them `std::`:**

* `Script/Public/ScriptAPI.h` (a plain header, no module access)
* the vendored `UI/Private/lib/imgui-node-editor`
* the Nsight Aftermath `namespace std` `to_string` blocks in GpuCrashTracker / ShaderDatabase — their
  calls resolve by qualified lookup into std, which a using-declaration snapshot cannot serve

### General

* No new/delete, only memory-safe types. Raw pointers are non-owning.
* Minimal comments — only where intent is genuinely hard to communicate.
* Prioritize performance.
* `int8` / `uint64`-style typedefs from `Core`; glm through `Core.glm`; `assert` compiled out in
  non-debug through `forceinclude.h`.

### Globals and teardown

Engine singletons live in `export namespace Globals` inside the owning `.ixx`:
`Globals::rendererVK`, `device`, `input`, `ui`, `world`, `physics`, `scriptHost`, `scriptEvents`,
`assetRegistry`, `time`, `allocator`, `jobSystem`, `spatialIndex`, `navSystem`, `forceSystem`, ...

**Cross-library teardown is ordered ENTIRELY by init_seg.** `Code/Core/Private/InitSeg.h` — in every
TU through forceinclude.h — is the single authority: the `OC_SEG_*` section defines plus the
`OC_INIT_SEG(seg)` macro (`__pragma`, since `#pragma` does not macro-expand its arguments).

**Construction runs top to bottom; destruction (atexit, main thread) runs bottom to top.** A global
with NO pragma lands in plain `.CRT$XCU`, which constructs after the XCA sections but BEFORE every
numbered XCU section — **so plain globals destruct after all of them.**

| Section | Globals |
|---|---|
| `XCA` / `XCA1` / `XCA2` | allocator → profiler → memoryTracker (**first up, last down**) |
| plain `XCU` | input, audio, spatialIndex, entityAllocator, scriptContext, scriptHost, assetRegistry, time, gameHud, ... |
| `XCU1..XCU4` | VK instance → device + gpuAllocator → rendererVK + openXR → the data managers |
| `XCU5` | **jobSystem** |
| `XCU51` | physics |
| `XCU6` | networkManager |
| `XCU7` | scriptEvents |
| `XCU8` | world |
| `XCU9` | ui |
| `XCUA` | terrain, terrainCollider, ocean, scatter |
| `XCUB` | navSystem |

So **destruction** runs: nav → procedural → ui → world → scriptEvents → networkManager → physics →
jobSystem → renderer globals → plain XCU → memoryTracker → profiler → allocator.

The constraints that shape it:

* **Every EntityPtr holder destructs before `~JobSystem`**, which runs `shutdown()`: entity
  destruction reaches `PerWorker::local()`, needing the main thread's live worker context.
* `~PhysicsWorld`'s `b3DestroyWorld` fans tasks onto the job system, so it must beat `~JobSystem` —
  and bodies died in `~World` above it.
* NetworkComponents unregister before `~NetworkManager` closes the host.
* World's caches release into the still-live renderer and audio.
* The procedural systems go FIRST — they free render residency and collider bodies and may wait on
  in-flight jobs. **Order among them is link-order-undefined and deliberately independent.**

**main() has NO explicit teardown calls.** A new global that holds EntityPtrs, or whose dtor calls
another library's global, **must slot into InitSeg.h.** Globals in *different* libraries otherwise
have no defined construction order — see `ScriptEventManager::initialize` for the register-from-main
pattern.

---

# Libraries

## Dependency direction

`target_link_libraries`, PUBLIC unless noted. Everything imports Core, and a library NEVER links one
printed above it — read the stack bottom-up, where each row may use every row below it. Third-party
libs in parentheses are PRIVATE: they never leak through a public interface.

```
   LIBRARY        DEPENDS ON (beyond Core)
   ------------------------------------------------------------------------------------------------
   App            everything + Game + AppScripts
   Game           Entity, Force, Physics, Input
   Input          UI                                             (+ openxr_loader)
   UI             Entity, RendererVK, Script                     (+ imgui)
   Entity         RendererVK, File, Script, Physics, Audio, Particle, Force, Spatial, Threading, Network, Nav
   ------------------------------------------------------------------------------------------------
   Particle       RendererVK, File
   Force          RendererVK, Threading   (the merge passes are parallelFors; neighbour search is a private candidate cell list, NOT the SpatialIndex)
   Procedural     RendererVK, File, Spatial, Physics             (+ onnxruntime, zstd)
   RendererVK     Animation, File, Threading                     (+ vulkan, glslang; Aftermath DLL is LoadLibrary'd, optional)
   ------------------------------------------------------------------------------------------------
   Script         File          (ScriptHost compiles/loads .dsl from disk; disk access is File-only)
   Spatial        Threading
   Nav            Threading     (flow fields — knows nothing about entities; Game feeds it, units read it)
   Physics        Threading     (box3d's solver fork/join runs as jobs)   (+ box3d)
   File           Animation                                      (+ assimp, zlib, meshoptimizer)
   ------------------------------------------------------------------------------------------------
   Animation      -                                              -- these four are Core-only
   Network        -                                              (+ Ws2_32, Bcrypt)
   Audio          -                                              (+ Steam Audio, miniaudio)
   Threading      -
   ------------------------------------------------------------------------------------------------
   Core           std header units, oc:: containers (OcSTL), math, Profiler + MemoryTracker
                  (+ EASTL, vulkan, SDL3, imgui, Windows libs — all PRIVATE wrappers)
```

Standalone executables: `NetFuzz` (Core + Network) and `DslCompiler` (Core + Entity + Script + File).

## Where each library documents itself

| Library | Covers |
|---|---|
| [App](../Code/App/CONTEXT.md) | The testbed executable: **the frame loop table**, init order, command line, main menu + lobby + chat flow, escape menu, testbed keys |
| [Core](../Code/Core/CONTEXT.md) | `Core.OcSTL` (the EASTL backing seam and the `oc::` vocabulary), `Core.OcBit`, SmallVector, the two clocks + global pause, frame pacing entry, Tweaks (Saved/Synced/overrides), `Core.GameHud`, plus the **Profiling** and **Memory** sections |
| [RendererVK](../Code/RendererVK/CONTEXT.md) | The Vulkan renderer: frame pacing, the fence slot, the begin-frame job, frame order, instance flow, streaming, LODs, terrain/ocean integration, shaders |
| [Entity](../Code/Entity/CONTEXT.md) | The ECS: entity layout + flags, contiguous tree allocation, the parallel update pass, **SIM LOD**, parallel spawning/destruction, World, the components, script glue, and the **Multiplayer** section |
| [Script](../Code/Script/CONTEXT.md) | ScriptHost DLL compilation + the cooked build, the ScriptAPI ABI (append-only table, require slots, `OcArray`), the DSL rules, fault containment, the DSL subsystem |
| [Physics](../Code/Physics/CONTEXT.md) | box3d wrapper: the job-driven solver + task ring, the body-command queue, buoyancy, PhysicsComponent, park/suspend, contacts, layers |
| [Particle](../Code/Particle/CONTEXT.md) | GPU particles + decals, `.pfx` effects, ParticleComponent |
| [Force](../Code/Force/CONTEXT.md) | Forcefield bubbles: the analytic field, live team count, the baked pressure field, shell tiers / union march, emitter merging, ForceComponent |
| [Spatial](../Code/Spatial/CONTEXT.md) | The spatial index: the one update call + its kick/join window, the Morton hierarchy, layers, visibility stamps, occlusion, static tier |
| [Nav](../Code/Nav/CONTEXT.md) | Per-team flow fields: TeamField Dijkstra builds, the post-update field steps, goal fields, seed paths + the rate limiter, unit context steering |
| [Threading](../Code/Threading/CONTEXT.md) | The fiber job system: worker sizing, post-update jobs, `ForeignWait`, the external helper, scheduling, JobSync, JobGraph, the JobCounter invariants |
| [Procedural](../Code/Procedural/CONTEXT.md) | `ITerrainSampler`, the V3 diffusion generator, TerrainStreamer, HeightMapBaker (water reach / flow), ocean clipmap, terrain collider, scatter |
| [Network](../Code/Network/CONTEXT.md) | Winsock transport: `NetHost`, deliveries/channels, handshake + encryption, abuse limits, serialization, sockets |
| [NetFuzz](../Code/NetFuzz/CONTEXT.md) | The protocol fuzzer — **the regression gate for wire changes** |
| [Audio](../Code/Audio/CONTEXT.md) | miniaudio + Steam Audio HRTF, buffers/sources, AudioComponent |
| [Animation](../Code/Animation/CONTEXT.md) | Skeletons, clips, AnimationPlayer, the state machine, retargeting |
| [File](../Code/File/CONTEXT.md) | `FileSystem` (**THE disk seam**, main-thread IO assert), AssetParser, the cooked scene cache + GC, TextureConvert |
| [UI](../Code/UI/CONTEXT.md) | ImGui editor: the pipelined widget-pass job + draw-data snapshot, the prepare/render split, panels, game layout, HUD overlay, gizmo split |
| [Input](../Code/Input/CONTEXT.md) | The window thread (`Core.Window`), the ImGui capture gate, listeners, GizmoController, VR input, camera controllers |
| [Game](../Code/Game/CONTEXT.md) | The game: the three ticks, co-op PvE (generated map, waves), PvP arenas, sync layers, teams, economy/cables/construction, hotbar + interaction modes, units, player |
| [DslCompiler](../Code/DslCompiler/CONTEXT.md) | **The DSL language reference** + the DslCompiler tool — how Claude authors `.dsl` scripts |

---

# Asset files

All text formats are parsed through `AssetParser` and registered by `AssetRegistry`.

## `.oc` — ObjectContainer

`ObjectContainer <name>` plus `Path`. Optional:

* `Loader Procedural`, `MergeNodes`, `PreTransformVertices`
* `DecimationFactor` — below 1 it meshopt-simplifies at cook. Cache-only, so skinned and procedural
  containers never decimate.
* `MaterialOverrides` — per material: `PipelineIdx <pipelineName>`, `ExcludeFromRayTracing`,
  `UseSceneTextures`, `DiffuseTexIdx` / `NormalTexIdx` / `MetalRoughnessTexIdx`.

**Skinned containers MUST keep `PreTransformVertices false`.**

The `.oc` is also what the procedural scatter imports through, **so scatter and World share one cooked
`.vsc` per model.**

## `.pre` — Prefab

A `Prefab <name>` root with `Component Render/Animator/Script/Physics/Audio/Particle/Force/Light/
Network/Scene` children, plus the game components `Component GameUnit/GameStructure/GameProjectile`.
`Component Scene` nests child `Prefab` references.

`Component Render` requires `ObjectContainer <name>` plus `Node <path>`,
`Type StaticMesh|SkinnedMesh` (skinned adds a nested `Rig <name>`), plus `Position` / `Rotation` /
`Scale`, and optionally `Color r g b` for the per-entity tint.

Entity-level `Enabled false` authors the disabled state; `Global true` (**root only, never
inherited**) makes the World visit it every frame regardless of the SIM LOD.

Spawn by prefab name (`world.spawn`) or by path (`world.spawnAssetFile`).

## `.anm` — animation clips

`Animation <name>` clips: source `ObjectContainer`, `Loop`, `Skip` (channels), and
`Event <name> <normalizedTime>` notifies, fired into the entity's script or animator `onEvent`.

## `.apl` — animator graph

`Animator <name>` graph:

* `Parameter`s — Float / Bool / Trigger
* `Clip <name>` plus `Anim <clipName>` bindings
* `BlendSpace1D` with `Sample`s
* `StateMachine` — `Entry <state>`; states with `Play` / `SpeedParam` / `SpeedScale`;
  `Transition` / `AnyTransition` with `Condition` / `ExitTime` / `Fade`

## `.pfx` — particle effects

`ParticleEffect <name>` with `Emitter <name>` children. **The full grammar is documented at
`Code/Particle/Private/Effect.ixx`**; demo `Assets/Effects/fire.pfx`.

## `.dsl` — scripts

A dual-purpose file: **generated C++ on top, the `//@`-commented DSL block below**, between
`//@@dsl 1` and `//@@end`. Author them through DslCompiler — see
[`Code/DslCompiler/CONTEXT.md`](../Code/DslCompiler/CONTEXT.md).

## `.scr` — legacy

The removed node editor's format: generated C++ plus `//@graph` metadata. Legacy reference only — it
reads as plain text in Content, is never cooked, and has no editor. **Author `.dsl` instead.**

## `Assets/Scenarios/`

Checked-in inputs for repeatable runs:

* `*.txt` game saves — the F9 format: structures (cable segments included) and units — for
  `--scenario <path>` and `Tools/profile.ps1`. `march-64-units.txt` is the corridor with 64 team-0
  units, the standard perf scenario. `coop-wave9.txt` is a late co-op session (wave 9, a built
  base, thousands of ambient units) — **the co-op perf scenario**; a co-op scenario loads as saved,
  with NO march order (`runScenario` skips it when `--coop`):
  `Tools/profile.ps1 -Game -Server -AppArgs "--coop" -Scenario "Scenarios/coop-wave9.txt" -ScenarioAt 2 -After 30`.
* `*.tweaks` override files for `--tweaks <path>`. `cpu-profile.tweaks` = GPU features off.

## `Assets/Local/`

Generated output — **never hand-edit.** SPIR-V plus shader dumps, compiled script DLLs and PDBs
(`Scripts/`), cooked scenes (`Cooked/*.vsc` plus `<stem>_tex/` converted `.dds`), `TerrainTex/`
splats, the `Diffusion/<seed>/` tile cache, `tweaks.cfg`, `imgui.ini` (the editor layout),
`gamesave.txt` and `profile.txt`.
