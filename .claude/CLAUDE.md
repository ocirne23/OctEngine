# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Index

* [Response Rules](#response-rules)
* [Project Description](#project-description)
  * [Building](#building)
  * [Style](#style)
* [Libraries](#libraries) — dependency direction, and where each library's documentation lives
  (all in [`Docs/Context/`](../Docs/Context/), indexed in [this table](#where-each-library-documents-itself))
* [Asset files](#asset-files)
* [`Docs/TODO.md`](../Docs/TODO.md) — loose list of improvements and open issues (not a plan). Add ideas there.

---

# Response Rules

* **DO NOT PRAISE THE USER.** If the user is correct in a statement, just say "correct" — nothing
  more.
* Always talk in ASD-STE100 Simplified Technical English.
* Always read CLAUDE.md / CONTEXT.md files. Library-specific documentation lives in
  `Docs/Context/<Lib>/CONTEXT.md` (see [Libraries](#libraries)) — **read the CONTEXT.md of every library you
  touch BEFORE changing it**, plus the topic file of the feature you change when the library has topic files
  (RendererVK, Procedural, Entity, Game).
* For each assertion, add a tag for your confidence in the statement, rather than relying on English:
  * `[Certain]` — hard evidence
  * `[Likely]` — strong guess
  * `[Guessing]` — filling gaps
* Do not commit or push files.
* Do not notify about App.exe linking failed due to in-use. The user knows to re-link if the linking
  failed due to an in-use executable.
* Do not tell the user that a changed tweak default might not apply because of a saved value (a local
  `.cfg` file). The user knows how tweak values persist.
* Prefer to let the user test out changes, rather than looking at log output or the screen yourself.
* **ALWAYS make file changes with the Edit/Write tools, NEVER via shell scripts** (python, sed,
  heredocs through Bash). The user reviews every change as the inline diff those tools render, and
  script edits show nothing. This overrides any session default that prefers shell-based editing.
* Keep this file and the `Docs/Context/<Lib>/` files up to date. Cross-library rules (style, build,
  dependency direction, asset formats) go here; everything about ONE library goes in that library's
  CONTEXT.md, or in a topic file next to it in `Docs/Context/<Lib>/` when the CONTEXT.md gets too big.
  **Every doc file must be listed in the [Libraries](#where-each-library-documents-itself) table** — that
  table is the only index a new session has. Docs describe the CURRENT design and its rules: no build dates, status
  notes, phase labels or "used to" history; a failed experiment stays only as a one-line "tried X, dropped because Y".

---

# Project Description

**OctEngine** (`C:\Github\OctEngine`). A modern C++ game engine. Windows / VS2026 only.

The `App` executable is the testbed: an ImGui editor — scene hierarchy, properties, asset browser,
DSL script editor, entity editor, tweaks, profiler, memory panel — around a viewport.

App.exe with NO mode flags boots into a fullscreen MAIN MENU: co-op / PvP / sandbox plus host/join, a
LOBBY for multiplayer picks, and a text CHAT in the lobby and in-game. See
[`Docs/Context/App/CONTEXT.md`](../Docs/Context/App/CONTEXT.md) for the menu/lobby/chat flow, **the frame loop** and
the testbed keys.

The actual game — top-down tactical PvP plus co-op PvE — is `Code/Game`; see
[`Docs/Context/Game/CONTEXT.md`](../Docs/Context/Game/CONTEXT.md).

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
* VS launch targets: `run-headless`, `run-server`, `run-client`, `run-netfuzz`, `run-cooked`.
  `SCRIPTS_STATIC` is ON by default (scripts compiled into App, see Script). In a tree configured with
  `SCRIPTS_STATIC=OFF`, the `cooked` custom target configures and builds a sibling `Build-Cooked/` tree with it ON.
* **Unattended performance measurement:** `Tools/profile.ps1` (see Profiling in
  [`Docs/Context/Core/CONTEXT.md`](../Docs/Context/Core/CONTEXT.md)) builds, runs
  `App.exe --profile-after <sec> --quit-after <sec> --no-vsync --tweak ...`, and prints the text
  report. This is Claude's measure → change → re-measure loop; the user does not need to be at the
  screen. **Always `-Config RelWithDebInfo`.** GPU work: **`-Fullscreen -After 60`** (the user's 1440p, after the
  sandbox streaming), each case twice - see `Tools/profile.ps1` in Core's CONTEXT for the noise.
* **Shader fp16 work:** `Tools/fp16conv.ps1 <shader>.glsl` lists every fp16 <-> fp32 conversion in the
  shader's SPIR-V dump by source line (glslang never warns about implicit widening). Rules and results in
  "Half floats" in [`Docs/Context/RendererVK/Shaders.md`](../Docs/Context/RendererVK/Shaders.md).
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

* `forceReferenceBudget` / `forceSphereFold` (Force/ForceSystem.cpp) — reached from the merge parallelFors
* `AnimStateMachine::getCurrentStateName`'s `"<none>"` (Animation) — reached from a script thunk on
  workers
* `layerNames()` (Physics) — reached from the terrain collider's tile-build jobs
* `g_bodyLifecycleMutex` (Physics/Body.ixx) — a `std::mutex` is constant-initialized, so static init
  is safe, but it sits at namespace scope for the same reason

`Procedural`'s diffusion `.cpp` TUs override to `/fp:precise /wd5050` — **load-bearing; see
`Code/Procedural/CMakeLists.txt`.**

### Dependencies

Prebuilt in `Dependencies/` (Include / Lib / Dll):

Vulkan-Hpp (no exceptions, no constructors), SDL3, ImGui docking branch, EASTL, Assimp,
glslang/shaderc + SPIRV-Tools (glslang 16.3, copied from Vulkan SDK 1.4.350: `Lib/` + `Include/glslang`,
`spirv-tools`, `shaderc` - 15.4 wrote NonSemantic debug info that spirv-val rejects, `DebugTypeMember` lines /
columns from the wrong source), OpenXR loader, Nsight Aftermath, box3d, Steam Audio (static; `phonon(d).lib` bundles
pffft, mysofa and zlib), miniaudio (single header), meshoptimizer, zstd, onnxruntime, NVIDIA Streamline
2.14.1 (`Include/sl` headers + the five DLSS runtime DLLs, LoadLibrary'd and optional - see DLSS in
[`Docs/Context/RendererVK/PostProcess.md`](../Docs/Context/RendererVK/PostProcess.md); update = copy `include/` and
`bin/x64/{sl.interposer,sl.common,sl.dlss,nvngx_dlss,NvLowLatencyVk}.dll` from the release zip).

**EASTL is 3.27.01 with ONE LOCAL PATCH:** `Include/EASTL/internal/config.h` defaults
`EASTL_NAME_ENABLED` to 0 (upstream: `EASTL_DEBUG`), marked `OCTENGINE LOCAL PATCH`. No allocator debug
names, so an `oc::` container has the SAME size in Debug as in the other configurations (an
`oc::vector` is 24 bytes). `EASTL.lib` / `EASTLd.lib` are built from those same patched headers, so
there is no compiler flag on either side. **To update EASTL:** clone the tag, re-apply the patch, build
with CMake (`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, Debug with `/Z7` so the lib needs no PDB; EABase comes
through FetchContent), then replace `Include/EASTL`, `Include/EABase` (`include/Common/EABase`) and both
libs. **Any EASTL layout switch must be the same in the lib build and the engine**, and the release
`EASTL.lib` cannot link into Debug (CRT + `_ITERATOR_DEBUG_LEVEL` mismatch).

**`Dependencies/` holds ONLY `Include/`, `Lib/` and `Dll/`** — no vendored source. Everything links
prebuilt `<name>.lib` / `<name>d.lib` pairs; the recipes to rebuild them from an upstream clone (box3d,
steam-audio, meshoptimizer, zstd) are the comments in `Dependencies/CMakeLists.txt`.

## Style

### Modules

Exclusively C++20 modules (`.ixx`), no headers.

* Most libraries use module partitions: `export module RendererVK:Renderer;` in the interface `.ixx`,
  `module RendererVK;` in the implementation `.cpp`. The public surface is re-exported from
  `Public/<Lib>.ixx`.
* `Core` and `Settings` use dotted module names (`Core.Time`, `Settings.Render`, one module per `.ixx`). UI uses partitions
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
  links File. Core and Settings therefore cannot do IO: `Settings.Tweaks` takes injected read/write hooks
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
* **Tweak names say what the knob does, never whose name the model carries** ("Evaporation curve", not "Budyko w";
  "Haze scattering", not "Mie"). The scientist's name may go in a code comment. Exception: "Fresnel".
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
| `XCA3` | `Globals::settings` (every tweak value; up before any library global reads it) |
| plain `XCU` | input, audio, spatialIndex, entityAllocator, scriptContext, scriptHost, assetRegistry, time, gameHud, Procedural's `g_diffusionRuntime` (destructs after terrain has joined its chunk pumps), ... |
| `XCU1..XCU4` | VK instance → device + gpuAllocator → rendererVK + openXR → the data managers |
| `XCU5` | **jobSystem** |
| `XCU51` | physics |
| `XCU6` | networkManager |
| `XCU7` | scriptEvents |
| `XCU8` | world |
| `XCU9` | ui |
| `XCUA` | terrain, terrainCollider, ocean, trees, rocks, clutter |
| `XCUB` | navSystem |

So **destruction** runs: nav → procedural → ui → world → scriptEvents → networkManager → physics →
jobSystem → renderer globals → plain XCU → settings → memoryTracker → profiler → allocator.

The constraints that shape it:

* **Every EntityPtr holder destructs before `~JobSystem`**, which runs `shutdown()`: entity
  destruction reaches `PerWorker::local()`, needing the main thread's live worker context.
* `~PhysicsWorld`'s `b3DestroyWorld` fans tasks onto the job system, so it must beat `~JobSystem` —
  and bodies died in `~World` above it.
* NetworkComponents unregister before `~NetworkManager` closes the host.
* ScriptComponents unregister from `scriptEvents` in `~World`, so `scriptEvents` goes after World —
  but its undrained EntityChange queue needs World's spawn templates, so **`~World` releases that
  queue first**.
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

`target_link_libraries`, PUBLIC unless noted. Everything links Core; every library but Animation, File, Script and
Network also links Settings directly (left out of the table). A library NEVER links one
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
   Procedural     RendererVK, File, Spatial, Physics             (+ onnxruntime, zstd, meshoptimizer)
   RendererVK     Animation, File, Threading                     (+ vulkan, glslang/shaderc/SPIRV-Tools, openxr_loader, meshoptimizer; Aftermath DLL is LoadLibrary'd, optional)
   ------------------------------------------------------------------------------------------------
   Script         File          (ScriptHost compiles/loads .dsl from disk; disk access is File-only)
   Spatial        Threading
   Nav            Threading     (flow fields — knows nothing about entities; Game feeds it, units read it)
   Physics        Threading     (box3d's solver fork/join runs as jobs)   (+ box3d)
   File           Animation                                      (+ assimp, zlib, meshoptimizer)
   ------------------------------------------------------------------------------------------------
   Audio          -                                              (+ Steam Audio, miniaudio)
   Threading      -
   Animation      -                                              -- Core-only
   Network        -                                              (+ Ws2_32, Bcrypt)
   ------------------------------------------------------------------------------------------------
   Settings       -             (every tweak's value: Globals::settings + the tweak registry)
   ------------------------------------------------------------------------------------------------
   Core           std header units, oc:: containers (OcSTL), math, Profiler + MemoryTracker
                  (+ EASTL, vulkan, SDL3, imgui, Windows libs — all PRIVATE wrappers)
```

Standalone executables: `NetFuzz` (Core + Network) and `DslCompiler` (Core + Entity + Script + File).

## Where each library documents itself

| Library | Covers |
|---|---|
All docs live in `Docs/Context/<Lib>/`, NOT next to the code.

| Library | Covers |
|---|---|
| [App](../Docs/Context/App/CONTEXT.md) | The testbed executable: **the frame loop table**, init order, command line, main menu + lobby + chat flow, escape menu, testbed keys |
| [Core](../Docs/Context/Core/CONTEXT.md) | `Core.OcSTL` (the EASTL backing seam and the `oc::` vocabulary), `Core.OcBit`, SmallVector, the two clocks + global pause, frame pacing entry, `Core.GameHud`, plus the **Profiling** and **Memory** sections |
| [Settings](../Docs/Context/Settings/CONTEXT.md) | **Every tweak**: `Globals::settings` (one copy of each value, read directly), the domain modules + `registerAll`, listeners (`Tweak::onChange`), and the tweak registry (`Settings.Tweaks`: groups, Saved / Synced / Runtime, overrides, locks) |
| [RendererVK](../Docs/Context/RendererVK/CONTEXT.md) | The Vulkan renderer: frame pacing, the fence slot, the begin-frame job, **frame order**, source layout, debug names, the graphics-queue mutex, VR. **Read it first; the topic files below hold the features** |
| ↳ [FrameUbo](../Docs/Context/RendererVK/FrameUbo.md) | **The frame UBO (generated GLSL) + the tweak locks that bake it into the shaders**, push blocks |
| ↳ [Scene](../Docs/Context/RendererVK/Scene.md) | Per-frame instance flow, scene objects (single meshes, the sky draw, tree records, tint), texture + mesh streaming, mesh LODs, draw-list compaction |
| ↳ [Lighting](../Docs/Context/RendererVK/Lighting.md) | The scene focus, GI (probes, sky map, irradiance volume), RTAO, long-range sun shadows, aerial perspective |
| ↳ [Clouds](../Docs/Context/RendererVK/Clouds.md) | Volumetric clouds |
| ↳ [Vegetation](../Docs/Context/RendererVK/Vegetation.md) | Procedural grass, ground clutter, the wind, tree wind, the far-tree volume |
| ↳ [PostProcess](../Docs/Context/RendererVK/PostProcess.md) | Motion vectors, TAA bypass, DLSS, motion blur, colour, bloom |
| ↳ [Terrain](../Docs/Context/RendererVK/Terrain.md) | Terrain and ocean integration, river and lake water, the rock material, terrain surface water, the rain occlusion map |
| ↳ [Shaders](../Docs/Context/RendererVK/Shaders.md) | GLSL in `Assets/Shaders/`: folders, includes, compile, hoisted work, pipeline statistics (register counts), **half floats (fp16)** |
| [Entity](../Docs/Context/Entity/CONTEXT.md) | The ECS: entity layout + flags, contiguous tree allocation, CullMode, the parallel update pass, parallel spawning/destruction, World, assets + tint, script glue. **Read it first; the topic files below hold the rest** |
| ↳ [SimLod](../Docs/Context/Entity/SimLod.md) | **SIM LOD**: focus, the visit set, tiers, the delta, dormant / wake edges |
| ↳ [Components](../Docs/Context/Entity/Components.md) | The components: the bone-model animators (HumanoidAnimator grammar), Light, the Game components |
| ↳ [Multiplayer](../Docs/Context/Entity/Multiplayer.md) | **Multiplayer**: ownership, roles, the wire protocol, snapshots, the spawn stream, claims, validation, correction, ownership transfer, events, session |
| [Script](../Docs/Context/Script/CONTEXT.md) | ScriptHost DLL compilation + the cooked build, the ScriptAPI ABI (append-only table, require slots, `OcArray`), the DSL rules, fault containment, the DSL subsystem |
| [Physics](../Docs/Context/Physics/CONTEXT.md) | box3d wrapper: the job-driven solver + task ring, the body-command queue, buoyancy, PhysicsComponent, park/suspend, contacts, layers |
| [Particle](../Docs/Context/Particle/CONTEXT.md) | GPU particles + decals, `.pfx` effects, ParticleComponent |
| [Force](../Docs/Context/Force/CONTEXT.md) | Forcefield bubbles: the analytic field, live team count, the baked pressure field, shell tiers / union march, emitter merging, ForceComponent |
| [Spatial](../Docs/Context/Spatial/CONTEXT.md) | The spatial index: the one update call + its kick/join window, the Morton hierarchy, per-cell 8-lane blocks, layers, visibility stamps, occlusion |
| [Nav](../Docs/Context/Nav/CONTEXT.md) | Per-team flow fields: TeamField Dijkstra builds, the post-update field steps, goal fields, seed paths + the rate limiter, unit context steering |
| [Threading](../Docs/Context/Threading/CONTEXT.md) | The fiber job system: worker sizing, post-update jobs, `ForeignWait`, the external helper, scheduling, JobSync, JobGraph, the JobCounter invariants |
| [Procedural](../Docs/Context/Procedural/CONTEXT.md) | The six globals, parking, `ITerrainSampler` (+ climate encoding), the terrain collider, the generated-mesh cache. **Read it first; the topic files below hold the systems** |
| ↳ [Generator](../Docs/Context/Procedural/Generator.md) | `TerrainGenV3` (the diffusion generator), world origin, `TerrainPreview`, generated bounds, `TerrainSeeder`, world scale |
| ↳ [Rivers](../Docs/Context/Procedural/Rivers.md) | Rivers: the coarse network, the units, the sampler |
| ↳ [TerrainStreamer](../Docs/Context/Procedural/TerrainStreamer.md) | `TerrainStreamer` (quadtree, node memory, edge stitching, the world datum, the terrain-data map), `HeightMapBaker` (water reach / flow) |
| ↳ [Ocean](../Docs/Context/Procedural/Ocean.md) | `OceanGenerator`: the clipmap, sectors, buoyancy field, wind, near-field shading |
| ↳ [Trees](../Docs/Context/Procedural/Trees.md) | Trees: the `.tree` grammar, LODs, far representations, textures, world records, world mode |
| ↳ [Rocks](../Docs/Context/Procedural/Rocks.md) | Rocks: the `.rock` grammar, DEAD WOOD, ground placement |
| ↳ [Clutter](../Docs/Context/Procedural/Clutter.md) | **Ground clutter**: the `.clutter` grammar, the meshes |
| [Network](../Docs/Context/Network/CONTEXT.md) | Winsock transport: `NetHost`, deliveries/channels, handshake + encryption, abuse limits, serialization, sockets |
| [NetFuzz](../Docs/Context/NetFuzz/CONTEXT.md) | The protocol fuzzer — **the regression gate for wire changes** |
| [Audio](../Docs/Context/Audio/CONTEXT.md) | miniaudio + Steam Audio HRTF, buffers/sources, AudioComponent |
| [Animation](../Docs/Context/Animation/CONTEXT.md) | Skeletons, clips, AnimationPlayer, the state machine, retargeting |
| [File](../Docs/Context/File/CONTEXT.md) | `FileSystem` (**THE disk seam**, main-thread IO assert), AssetParser, the cooked scene cache + GC, TextureConvert |
| [UI](../Docs/Context/UI/CONTEXT.md) | ImGui editor: the pipelined widget-pass job + draw-data snapshot, the prepare/render split, panels, game layout, HUD overlay, gizmo split |
| [Input](../Docs/Context/Input/CONTEXT.md) | The window thread (`Core.Window`), the ImGui capture gate, listeners, GizmoController, VR input, camera controllers |
| [Game](../Docs/Context/Game/CONTEXT.md) | The game: the architecture rule, partitions, the frame (the three ticks), the world, determinism, save / load. **Read it first; the topic files below hold the rest** |
| ↳ [Modes](../Docs/Context/Game/Modes.md) | Co-op PvE (generated map, teams, waves, the spawn blob) and PvP (arenas, teams, structure damage) |
| ↳ [Sync](../Docs/Context/Game/Sync.md) | Multiplayer sync: the sync layers, the game blob, player capsules, client intents |
| ↳ [Economy](../Docs/Context/Game/Economy.md) | The cable transport, physical cables, construction, grid placement |
| ↳ [Hotbar](../Docs/Context/Game/Hotbar.md) | The RTS grid hotbar pages, bases, structure health + labels, unit selection, interaction modes |
| ↳ [Units](../Docs/Context/Game/Units.md) | Units (split, targets, damage, far tick, parked spawns) and the player (the shield battery) |
| [DslCompiler](../Docs/Context/DslCompiler/CONTEXT.md) | **The DSL language reference** + the DslCompiler tool — how Claude authors `.dsl` scripts |

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

## `.pre` — Prefab

A `Prefab <name>` root with `Component Render/Animator/Script/Physics/Audio/Particle/Force/Light/
Network/Scene` children, plus the game components `Component GameUnit/GameStructure/GameProjectile`.
`Component Scene` nests child `Prefab` references.

`Component Render` requires `ObjectContainer <name>` plus `Node <path>`,
`Type StaticMesh|SkinnedMesh` (skinned adds a nested `Rig <name>`), plus `Position` / `Rotation` /
`Scale`, and optionally `Color r g b` for the per-entity tint.

`Component HumanoidAnimator` is a rigid-part model inside ONE entity plus its walk, with no `.anm` /
`.apl`: `Bone <name>` blocks — a transform and an optional `Component Render`, authored like an
entity but NOT one — and a fixed, bare-minimum animation: four limb bones about their local Z
(CubeGuy, the body of every game unit). The grammar is in
[`Docs/Context/Entity/Components.md`](../Docs/Context/Entity/Components.md); demo `Entities/Debug/CubeGuy.pre`.
`Component SceneAnimator` (the same bone model with general `Layer` / track animation) is PARKED: the
code exists but has no component id, so a prefab that authors it gets a warning and no component.
`Component Animator` is for skinned meshes only.

Entity-level `Enabled false` authors the disabled state; `Global true` (**root only, never
inherited**) makes the World visit it every frame regardless of the SIM LOD.
`CullMode PerEntity|RootOnly|None` sets the spatial registration: `PerEntity` (default) — every
entity culls itself; `RootOnly` — this entity culls for its whole subtree (one entry over all child
render bounds, e.g. a box-limb character); `None` — no entry, never culled, always visited. See
CullMode in [`Docs/Context/Entity/CONTEXT.md`](../Docs/Context/Entity/CONTEXT.md).

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
`Code/Particle/Private/Effect.ixx`**; demos `Assets/Effects/fire.pfx`, and the camera-following
weather volumes `rain.pfx` / `snow.pfx` (`Volume` + `FollowCamera` + `Occlude`, see Particle).

## `.tree` — tree species

`TreeSpecies <name>` with `Trunk` / `Crown` / `Module` (+ `Level` children) / `Leaves` / `Color` blocks, in
`Assets/Trees/`. A species describes a piece LIBRARY that trees are composited from per seed, not one tree. `Kind Bush`
species grow around the trees of their `Climate` (picked by `Weight`); a tree's `Undergrowth` block sets how many and
how far out.
A trunk may grow several stems (`Trunk Stems`: birch clumps).
**The grammar is in [`Docs/Context/Procedural/Trees.md`](../Docs/Context/Procedural/Trees.md)**; placeholders
`Oak.tree`, `Pine.tree`, `Acacia.tree`, `Willow.tree`, `Birch.tree`, `BirchClump.tree`, `Cedar.tree`, `Elm.tree`, plus bushes per tree `Climate` (a bush may list several climates; the birches' own `BirchWood` has hazel and raspberry). Loaded by `TreeSystem` itself (not `AssetRegistry`, which lives in
Entity, above Procedural). Their generated textures are saved once to `Assets/Local/Trees/Textures/*.png`
(not in git) and reused (regenerate with `Trees/Regenerate textures`).

## `.rock` — rock types

`RockType <name>` in `Assets/Rocks/`: the SHAPE of a rock family as an SDF recipe (`Shape`, `Aspect`, `Squareness`,
`Erosion`, `Warp`, `Fracture`, `Strata`, `Noise`, `Pits`, `Split`, `Pile`, `Profile`, `Group`, ...); the colour comes from the
climate, never the type. DEAD WOOD is rock types too: `Shape Trunk` (a fallen log, a stump, a snag: `Lying`, `Break`,
`RootPlate`, `Stubs`, `Roots`, `Hollow`) with `Surface Wood` (`Bark <TreeSpecies>`: that species' bark texture, x a `Color` tint). Any
number of `Placement` blocks say where the world puts it (a climate box, the ground: `Slope`, `Crag`, `Talus`,
`Plains` / `Rugged` / `Valley`, `Forest`); their densities add. **The grammar is
in [`Docs/Context/Procedural/Rocks.md`](../Docs/Context/Procedural/Rocks.md)**.
Loaded by `RockSystem` itself (not `AssetRegistry`).

## `.clutter` — ground clutter types

`ClutterType <name>` in `Assets/Clutter/`: a pebble, a fallen branch, a mushroom or a flower (`Kind`; the flower path
also builds reeds, sedges, wild grains, ferns and weeds - `Head Grain|Fern|Rosette|Leafy`, ...), its shape
(a pebble takes the `.rock` shape keys; a flower has no mesh: `Head`, `Petals`, `Open`, ...), and any number of
`Placement` blocks (density per m², a climate box, and `a b` TERMS over the terrain and the FOREST FLOOR - `Grass`,
`Crag`, `Beach`, `Canopy`, `Trunk`, `Rock`, `Wet` - plus `Cluster` and fairy `Ring`s); their densities add. Placed on the
GPU every frame, nothing per object on the CPU. **The grammar is in
[`Docs/Context/Procedural/Clutter.md`](../Docs/Context/Procedural/Clutter.md)**. Loaded by
`ClutterSystem` itself (not `AssetRegistry`).

## `.dsl` — scripts

A dual-purpose file: **generated C++ on top, the `//@`-commented DSL block below**, between
`//@@dsl 1` and `//@@end`. Author them through DslCompiler — see
[`Docs/Context/DslCompiler/CONTEXT.md`](../Docs/Context/DslCompiler/CONTEXT.md).

## `.scr` — legacy

The removed node editor's format: generated C++ plus `//@graph` metadata. Legacy reference only — it
reads as plain text in Content, is never cooked, and has no editor. **Author `.dsl` instead.**

## `Assets/Scenarios/`

Checked-in inputs for repeatable runs:

* `*.txt` game saves — the F9 format: structures (cable segments included), units, and the co-op wave / trickle
  state — for `--scenario <path>` and `Tools/profile.ps1`. The only save now is `coop-base-38k.txt`, a co-op match at
  wave 18 — **the co-op perf scenario** (entity-count stress: spatial, SIM LOD). A co-op scenario loads as saved,
  with NO march order (`runScenario` skips it when `--coop`):
  `Tools/profile.ps1 -Game -Server -AppArgs "--coop" -Scenario "Scenarios/coop-base-38k.txt" -ScenarioAt 2 -After 30`.
  **`profile.ps1`'s `-Scenario` default (`march-64-units.txt`) no longer exists**: pass `-Scenario` explicitly.
* `*.tweaks` override files for `--tweaks <path>`. `cpu-profile.tweaks` = GPU features off.

## `Assets/Local/`

Generated output — **never hand-edit.** SPIR-V plus shader dumps, the SPIR-V cache (`ShaderCache/`, see
[`Docs/Context/RendererVK/Shaders.md`](../Docs/Context/RendererVK/Shaders.md)), compiled script DLLs and PDBs
(`Scripts/`), cooked scenes (`Cooked/*.vsc` plus `<stem>_tex/` converted `.dds`), `Trees/Textures/` (the
generated tree textures and far bakes), `Trees/Meshes/` / `Rocks/Meshes/` / `Clutter/Meshes/` (the generated meshes -
"The generated-mesh cache" in [`Docs/Context/Procedural/CONTEXT.md`](../Docs/Context/Procedural/CONTEXT.md)), `TerrainTex/`
splats, the `Diffusion/<seed>/` tile cache, `imgui.ini` (the editor layout),
`gamesave.txt` and `profile.txt`.
