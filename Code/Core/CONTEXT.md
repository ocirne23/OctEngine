# Core

> Library documentation for `Code/Core`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Cross-library utilities, plus wrappers for the big headers (glm, SDL, imgui, Windows). The Vulkan
wrapper is RendererVK-private, `Util/VK.ixx`.

## Contents at a glance

* `Allocator` — global new/delete routed through it.
* `Log` — a ring buffer that the UI Log panel snapshots.
* `Time` / `Timer`.
* Math types: `Transform`, `Frustum`, `AABB`, `Sphere`, `Rect`, `Camera`
  (`Camera::screenToWorld` for viewport picking).
* Containers: `LPMultiMap`, `LockFreeList`, `ExpiryCache`, `BitRangeAllocator`, and
  `CheckedPtr` / `RefCheckable` (debug-checked non-owning pointers).
* `Core.VrSession` — the `IVrSession` interface (opaque handles, no OpenXR types), implemented by
  RendererVK's `OpenXRSession` and consumed by Input's `VrInput`, so Input and RendererVK stay
  unlinked.

## Global pause

Tweak "Time/Paused" — Synced, so the server's pause freezes clients; deliberately NOT Saved. The
Pause/Break key toggles it, game mode included.

`Time` keeps TWO clocks:

* **The REAL clock** (`getDeltaSec` / `getElapsedSec`) never stops. Frame pacing, Timers, UI, camera,
  tweak debounce and the network transport (receive/send keepalives, RTT, snapshot cadence) all stay
  on it.
* **The SIM clock** — `getSimDeltaSec()` is 0 while paused, and `getSimElapsedSec()` stands still
  through an accumulated paused-seconds offset. It feeds every simulation consumer in main.cpp:
  physics, `world.update`, `game.update` / `updatePlayer`, `scriptContext` (dt AND elapsed),
  particles, force, nav.

It also feeds the three renderer-side wall-clock users that would otherwise keep animating:
`ubo.timeSeconds` (shader animation — ocean waves, force pulses — now `getSimElapsedSec`), the GPU
particle dt in `present()`, and the ocean wind steer.

A zero dt alone would not stop per-call script or component actions, so:

* `Entity::updateSelf` folds `Globals::time.isPaused()` into its frozen gate. Sim ticks skip, while
  transform, render, spatial, audio-follow and the placement components keep running so the frozen
  world stays inspectable.
* `ScriptEventManager::fireEvent` drops events, like Frozen does.
* `OcTweakSync` is intercepted BEFORE that gate (`NetworkManager::fireEventAttributed`), so a
  server's UNpause always reaches paused clients.
* StructureSystem's gen-rate stats guard their `/ dt` against the zero sim delta (0/0 NaN in the
  HUD).

## `Core.OcSTL` (`OcSTL.ixx`)

THE backing seam. It owns every std/EASTL header unit the engine imports and hands out the `oc::`
vocabulary every other file spells — ALIASES ONLY, one line per type.

Its only imports are header units, so it sits BELOW everything including `Core.Profiler`, which
imports it instead of Core (since Core re-exports Profiler).

### The backing is EASTL

`#define EASTL` at the top of the file, translated at once to an internal `OC_EASTL` and `#undef`'d —
because `EASTL` is also the include-DIRECTORY name, and an `import <EASTL/x.h>;` macro-expands its
header name where a `#include` does not. Comment that one line out for the std containers.

EASTL arrives as HEADER UNITS, never as textual GMF includes: a GMF include leaves its free operators
(`string ==`, `+`, container comparisons) unreachable to every importer of Core, since only the class
templates are decl-reachable from the aliases.

The vocabulary is written against a `namespace ocstl` alias (`= eastl` or `= std`), so a name both
libraries have needs no `#ifdef`.

### What genuinely differs

* `oc::allocator<T>` — EASTL allocators are not element-typed, so the parameter is ignored;
  `oc::allocateArray` / `deallocateArray` absorb the element-vs-byte difference for
  `Core.SmallVector`.
* `oc::dynamic_extent` — EASTL's is `static`, so a header unit does not export it.
* `initializer_list`, `char_traits` and **ALL ATOMICS** are std in BOTH backings. An atomic allocates
  nothing, and EASTL's is a strict subset: no `atomic_ref`, no C++20 wait/notify (which the JobSystem
  eventcount and JobMutex are built on), and its memory orders are TAG TYPES rather than one enum.
* `allocator_traits` / `basic_string` / `basic_string_view` are absent — EASTL cannot express them at
  the same arity.
* `oc::make_unique<T[]>` is spelled out rather than aliased. EASTL's is `new T[n]` (DEFAULT-init)
  where std's is `new T[n]()` (VALUE-init) — a silent divergence that shipped garbage into every
  zero-assuming array: `BitRangeAllocator`'s grown tail read as fully allocated and asserted at
  startup, and script data, profiler records and the char buffers assume zeroing too.

> **A vocabulary entry must mean the same thing under either backing.**

### Filling the C++20 gaps EASTL has

`oc::contains` (no member `.contains()`), `oc::startsWith` / `oc::endsWith`, `oc::getline`,
`oc::format` (returns an `oc::string`; ALL formatting goes through it, never `std::format`),
`oc::as_bytes`, plus in-module shims — `operator<<` for eastl strings, and `std::formatter`
specializations so an `oc::string` is a valid `std::format` argument.

### Crossing to `std::`

`oc::toStd` / `oc::fromStd` (strings and vectors) is the sanctioned conversion, and a no-op copy
under the std backing, so call sites stay backing-agnostic.

Needed at every third-party boundary, all marked in place:

* Vulkan-Hpp `enumerate*` (returns `std::vector`), `vk::ClearColorValue` (spelled in `std::array`),
  and `vk::ResultValue`'s tuple conversion (`std::tie`)
* glslang `preprocess` / `GlslangToSpv` / `OutputSpvBin` (`std::string` + `std::vector<unsigned>` by
  reference)
* ONNX `Session::Run` (`std::vector<Ort::Value>` — move-only, so it stays std entirely)
* `<filesystem>` inside File (SceneCooker funnels through one local `toPath`)
* the iostream / sstream parsers
* the Nsight Aftermath `to_string` overloads

### EASTL's application hooks

Core links `EASTL$<$<CONFIG:Debug>:d>.lib`, and the engine supplies the three hooks EASTL declares but
expects the APPLICATION to define:

* Its two named and aligned `operator new[]` forms live in `Core.Allocator`, next to the engine's
  other global `operator new` definitions — so EASTL allocations land in `Globals::allocator` and the
  MemoryTracker.
* `EA::StdC::Vsnprintf` — the one that is not an allocation — sits in `Core/Private/Core.cpp`.

The ALIGNED form deliberately does not over-align. `eastl::allocator::deallocate` frees every block
with plain `delete[]`, so an `_aligned_malloc` block (what the `align_val_t` overloads return) would
reach `Allocator::deallocate` and corrupt the heap. It asserts instead; EASTL only calls it for
over-aligned element types, so nothing reaches it today.

## `Core.SmallVector` (`SmallVector.ixx`)

The engine's own contiguous containers. It imports `Core.OcSTL` (never the reverse) and is
re-exported by Core.ixx alongside it.

* `oc::small_vector<T, N>` keeps the first N elements INSIDE the object and spills to the heap past
  N — the per-frame scratch list that rarely exceeds N does zero allocations.
* `oc::fixed_vector<T, N>` is `small_vector<T, N, false>`, which refuses to spill: it asserts and
  drops, so a release build never writes out of bounds.

## `Core.OcBit` (`OcBit.ixx`)

THE bit vocabulary, and the reason OcSTL does NOT export `<bit>`.

Every std spelling there is written for any x86 baseline, so it carries a runtime CPU-support
dispatch or the portable BSF/BSR fixup around what is ONE instruction under the engine's
`/arch:AVX2`, where POPCNT, LZCNT and all of BMI1/BMI2 are baseline.

The module imports nothing (GMF `<intrin.h>` / `<immintrin.h>`), and sits below Core next to
`Core.OcSTL` and `Core.Profiler`; Core.ixx re-exports it.

**Surface** — all in `oc::`, and all width-EXACT through `if constexpr (sizeof(T))` on the
`oc::UnsignedInt` concept. A uint8 answers in 8-bit terms, and a SIGNED argument is a compile error
(bit math on a sign bit wants an explicit cast).

| Group | Names |
|---|---|
| Counting | `popcnt`, `tzcnt`, `lzcnt`, `trailingOnes`, `leadingOnes` |
| Widths | `bitWidth`, `bitCeil`, `bitFloor`, `hasSingleBit` |
| Rotates / swap | `rotl`, `rotr`, `byteSwap` — per-width overloads, since the MSVC intrinsics are width-exact |
| BMI1 | `blsr`, `blsi`, `blsmsk`, `bextr`, `andn` |
| BMI2 | `bzhi`, `pdep`, `pext` — the Morton primitives |
| Cast | `bitCast<To>(from)` |

* `tzcnt` / `lzcnt` are DEFINED on zero as the operand width, matching `std::countr_zero` /
  `countl_zero` — a property of the TZCNT/LZCNT encodings, so do not lower `/arch` without
  revisiting this.
* `bitCast` is the one piece of `<bit>` that was never a dispatch problem — a compiler builtin, and
  the only entry usable in constant expressions — which is why `std::bit_cast` is gone from the
  codebase too.

## `Core.Tweaks`

`Tweak::floatVar` / `intVar` / `boolean` / `color3` / ... `("Category/Sub", "Name", &liveVariable, ...)`
once at init exposes a variable in the TweakPanel. Pointers are non-owning, and the variable must
outlive the registration. This is the standard way to make anything runtime-configurable.

### `ETweakFlags`

Optional last param on every helper, after `onChange`; or `Tweak::ScopedFlags` RAII to flag a whole
`registerTweaks` block. Explicit per-call flags win.

**`Saved`** persists to `Assets/Local/tweaks.cfg`.

* main() calls `TweakRegistry::loadSaved()` right after `FileSystem::initialize`.
* Per-frame `TweakRegistry::update(dt)` in the main loop poll-detects changes — the panel writes
  through raw pointers, so polling is the only hook — and debounce-saves 0.5 s after the last change,
  plus at exit.
* Unknown file keys are preserved for other run modes.

**`Synced`** broadcasts server → clients.

* NetworkManager sends a full `packSynced` chunk set to a joiner after the spawn replay, and
  re-broadcasts when `syncGeneration()` bumps.
* It rides the engine-reserved event `"OcTweakSync"`, intercepted in `fireEventAttributed`, so it
  never reaches scripts or game hooks. Only CLIENTS apply.
* `applySyncedBlob` ignores keys the receiver did not flag Synced, and clamps to the receiver's own
  bounds.

Identity is `"Category/Name"` — renaming orphans the saved value.

All `Game/*` tweaks are `Saved|Synced` except `Game/Camera` (Saved only — personal preference) and
`Game/Sim LOD` (NEITHER — per-process performance tuning registered by the Entity library's World,
where the code defaults rule every run).

### Command-line override

* `--tweak "Category/Name=v [v v v]"` (`TweakRegistry::setOverride`) works on any variable, Saved or
  not. It applies now or at the variable's registration, wins over the file, and is NEVER written
  back — `saveFile` skips overridden keys, and the snapshot is taken after the apply so it is not a
  "change". This is how an unattended profiling run pins settings without touching the user's
  tweaks.cfg.
* `--tweaks <file>` (`TweakRegistry::loadOverrides(content, name)`; main reads the `Assets/`-relative
  file through FileSystem) applies a whole file of them, in the tweaks.cfg line format with `#` and
  `//` comments. Later `--tweak` flags win over it.
* `Assets/Scenarios/cpu-profile.tweaks` = the heavy GPU features off, for CPU-focused runs.

---

# Profiling

CPU + GPU scope profiler, part of Core: `Core.Profiler` (`Globals::profiler`), re-exported from
`Core.ixx`. Its interface must not import Core back — it uses textual std includes in the GMF.

## `ProfileScope`

```cpp
ProfileScope scope("Name", EProfileCategory::Physics);
```

RAII, and the name MUST be a string literal. About 15 ns on the hot path: two rdtsc plus one 32-byte
store into the calling thread's lock-free single-writer ring (`ProfileTrack`, 32K records / 1 MiB,
overwrite-on-wrap).

### Categories

The `EProfileCategory` enum plus two POSITIONAL tables (name, colour) in Profiler.ixx — **add all
three together.**

`Game` (amber) covers the Game library's main-thread ticks: "Game update", "Game windowed" (and
inside it "Game mode input", "Structures debug draw", "Nav debug draw", "Barracks routes",
"Game world labels", "Game HUD update"), "Structures authority" and its sub-scopes, "Npc service",
and "Player ...".

The Nav WORKER spans use it too: "Nav build" (one per `navFieldBuild` job), "Nav raster blocked" /
"Nav raster clearance" (inside the build), and "Nav flow step" / "Nav pressure step" (one per
fan-out callback, covering that callback's grain of chunks — per-chunk scopes would flood the rings).

### Entity scopes

The entity pass carries a scope for OPT-IN entities only: `EEntityFlag_Profiled`, a one-way
`setProfiled()` latch.

* Animator, Force, Script and GameUnit set it at spawn. Machine structures (barracks, turret) latch
  it in update, since `machineKind` is stamped post-spawn. Static scenery stays scope-free so it
  cannot flood the rings.
* The scope is named by `Entity::name` — the entity's ONLY name storage, an INTERNED pointer
  (`Profiler::internName`: a permanent mutex-guarded dedup pool). Records store names BY POINTER and
  outlive entities, so an owned string would dangle. Set by `setName`; null or "Entity" when unnamed.
  The entity owns no name buffer at all.

## Thread registration

EXPLICIT-ONLY: every profiling thread calls `registerThread(name, sortKey)` at startup. Main
registers as "Main" in the profiler's static-init ctor; workers in `workerMain` with
`SORT_KEY_WORKER + index`. A ProfileScope on an unregistered thread debug-asserts and no-ops.

* **Registered:** Main, JobWorkerNN, GPU, TexStream / MeshStreamer, V3 Loader, JobTimer.
* **Deliberately unregistered:** transient startup pools and stress-test threads (tracks are
  permanent, 1 MiB each), and miniaudio's thread.

## Scopes across a job wait

Scopes MAY span a JobSystem `wait()`: the open-scope stack MIGRATES with the parking fiber —
`suspendScopes` at both park sites closes on-thread segments, and `resumeScopes` replays on the
resuming thread.

Timing is the sum of on-thread segments; parked time is not counted. Memory attribution follows the
fiber. `ProfileScope` caches NO track pointer. Max migrating depth is 32.

## Frames

Records push at scope END, so rings are ordered by end time.

`Profiler::endFrame()` (main loop, after present) pushes a frame mark into a 512-frame ring and
re-anchors the rdtsc ↔ QPC calibration.

The profiler self-initializes at static init. `endStaticInit()` FIRST THING in main() closes the
synthetic "Static init" scope, so static-init allocations attribute there.

Recording is always on. The panel's PAUSE freezes the data, not the bookkeeping (`setPaused`: ring
pushes drop, while open/close keeps running so migration and Memory attribution stay live). Resume
lays a GAP pseudo-frame (`isFrameGap`).

## GPU pass timings

`GpuProfiler` (RendererVK `Objects/`, a member of Renderer) writes timestamp pairs in the primary CB
around each pass — outside render passes only, since multiview would replicate and a
SECONDARY_COMMAND_BUFFERS subpass admits no other primary commands.

Which is why the desktop forward pass records ONE RENDER-PASS INSTANCE PER STAGE:

* SceneColor's split variants — the first clears and stores depth, middles load/store, and the last
  hands colour to TAA. Compatible with the original pass, since only load/store ops and layouts
  differ (the deps are verbatim — they are part of compatibility), with explicit attachment barriers
  between instances.
* "Scene forward" now nests Static meshes / Decals / GI probe debug / Debug lines / Force shells /
  Force union march / Particles / Fog apply scopes. The shell proxy draw and the union fullscreen
  march are separate secondaries (`ForceFieldPipeline::EDrawPart`); VR records Both into the one
  per-eye pass.
* A single-stage frame uses the original pass, and VR eye passes stay unsplit.

One query pool per frame slot; results are read in `beginFrame` after the fence and pushed into a
named "GPU" track. GPU ↔ CPU alignment uses `VK_KHR_calibrated_timestamps`, with fallback
submit-tick anchoring.

## Text report

`Profiler::buildReport(ProfileReportOptions)` → `oc::string`. Core does no IO, so
`App.ProfileDump`'s `writeProfileReport(path, options)` writes it through FileSystem.

**Contents** — the last N completed frames, never across a pause gap:

* frame-time percentiles
* per-track busy/wait % (depth-0 scopes; the Wait category is fence, frame limit and joins)
* a global "top self time across CPU tracks" table
* then per track an aggregated CALL TREE — ms/frame inclusive, self, calls/frame, max call; children
  sorted by inclusive, and rows under `minMsPerFrame` folded into "(+N more)"
* and a flat by-self table

JobWorker tracks MERGE into one "Workers (merged)" tree (`perWorkerTrees` for one each). Parent = the
last record one level up that CONTAINS the record, by time. Records are per on-thread SEGMENT, so a
fiber-parked scope counts once per resume. A "RING LAPPED" note flags a track whose 32K ring no
longer covered the window.

### Triggers

* **Interactive** — key F7, or the panel's "Dump" button; both write `Assets/Local/profile.txt`.
* **Unattended** —
  `App.exe --profile-after <sec> [--profile-frames W] [--profile-out path] [--profile-workers] [--quit-after <sec>] [--no-vsync] [--tweak "Cat/Name=v"] [--tweaks <file>]`

TIMES ARE SECONDS of engine time since the loop start; frame counts would shift with the frame rate.

Each trigger (`--scenario`, `--profile-after`, `--quit-after`) is a one-shot `Timer` armed before the
loop and returning `Timer::DONE`, so it fires from `Time::update()` at the frame boundary — after the
previous frame's mark and before the "main loop" scope opens, which is exactly where the report
(completed frames only) and its IO belong.

main() re-bases the clock with ONE `Globals::time.update()` right before arming them: nothing calls
`update()` during init, so the clock still stood at STATIC-INIT time and every timed trigger fired on
frame 1 whenever init outlasted the flag's seconds. The same re-base also stops the first frame's
delta from being the whole init phase.

Either timed flag also makes the window count as FOCUSED — otherwise the "Inactive max FPS" cap is
what gets measured.

### `Tools/profile.ps1`

```
Tools/profile.ps1 [-Config RelWithDebInfo] [-NoBuild] [-After 10] [-QuitAfter After+0.5]
                  [-Frames 256] [-Game] [-Tweak "..."] [-Tweaks Scenarios/cpu-profile.tweaks]
                  [-Show 80]
```

Builds App, runs it, waits for the clean exit, and prints the report head. **THE loop for Claude to
measure → change → re-measure without looking at the screen.** It always overrides
`Time/Max FPS=0`.

Seconds are formatted InvariantCulture — a Dutch locale would print "10,5".

> **ALWAYS PROFILE `-Config RelWithDebInfo`.** Debug timings are not results; the user rejected them.

### Game scenario

`--scenario <save|default> [--scenario-at 1]` (seconds; script `-ScenarioAt`).

Script side: `-Game -Server` is THE standard perf run. `-Scenario` defaults to
`Assets/Scenarios/march-64-units.txt` — a checked-in F9 save: the corridor with 64 team-0 units.
`Assets/Scenarios/` holds such scenario saves; `-Scenario ""` = none, `"default"` = F10's Local save.
It runs only with `-Game`. `-Server` = `--server --port 27999`, so the networking path is live
without colliding with a manually running instance on 27888.

It calls `GameMatch::runScenario` on the authority:

1. Loads the save (`loadGame(path)`; "default" = F10's `Local/gamesave.txt`).
2. `issueScenarioOrder` RETRIES each update until three things are ready — the loaded units are
   queryable (spatial entries link at `commitFrame`, AFTER `game.update`), the enemy Base is in
   `StructureSystem`'s per-frame view (refreshed in `tickAuthority`, also after), and
   `navSystem.raster()` is published.
3. Then it selects EVERY live own-team unit (`NpcSystem::queryAllUnits`, world-wide) and issues
   `moveOrderAt(pointOutsideFootprint(base))` — THE RMB move order: player `setMoveTarget` plus
   `orderSelectedUnits` → locked targets and one seeded lane. `pointOutsideFootprint` is the
   click-on-building push-out, because the Base CENTRE is blocked cells, where the A* fails and every
   unit's own plan request to it fails too, which left the crowd milling at spawn.

It logs `Scenario: N units ordered to (x, z) after F frames, lane seeded|NOT seeded`. `Log` prints to
stdout, so an unattended run's output file shows it — flushed on clean exit only.

## UI "Profiler" panel (`UI:ProfilerPanel`)

Frame bar graph (click = pause + inspect; Space = pause), per-track flame graphs (zoom / pan / fit), a
sortable stats table, and an UNCAPPED fps readout in the toolbar.

The readout is `max("main loop", "GPU Frame")` of the displayed frame — what the frame would run at
without the vsync/fence throttle, since the main loop scope EXCLUDES the fence wait by design. Per
track it takes the depth-0 record with the largest overlap with the frame window and uses its full
duration, since the GPU frame straddles the marks. Live is a 0.9/0.1 EMA fed once per displayed
frame; paused is exact. Tagged CPU-bound / GPU-bound.

The live view shows `frameCount − 3`, so CPU and GPU data are complete.

---

# Memory

Allocation tracking plus a treemap, part of Core: `Core.MemoryTracker`. It is NOT re-exported from
Core.ixx — its only consumer is the UI Memory panel.

## The tracker

`Globals::memoryTracker` attributes every allocation to the PROFILE-SCOPE PATH open on the allocating
thread.

* Allocator hooks (`g_memoryAllocHook` / `g_memoryFreeHook` in `Core.Allocator`; alloc fires after
  allocation, free before release) walk and build a `MemScopeNode` tree (4096-node pool).
* Live pointers sit in a 64-shard open-addressing map, preallocated.

**Construction through `#pragma init_seg`:** allocator `.CRT$XCA` → profiler `XCA1` → tracker
`XCA3`, so static-init allocations are tracked.

* The hooks NEVER allocate — everything is preallocated in the tracker ctor.
* Pools deliberately leak at exit: the dtor only uninstalls hooks, since later global dtors still
  free tracked memory.
* `MemoryHookSuppress` (per-thread RAII) mutes hooks for PERMANENT infrastructure allocations that
  would misattribute — never around blocks that get freed.
* Unregistered threads land in `<other threads>`; overflow counts into `getDroppedAllocs` (red in the
  panel).
* Node identity is the name-literal pointer.

## UI "Memory" panel (`UI:MemoryPanel`)

A squarified treemap: one box per scope path, area ∝ the selected metric, nested, coloured by
category. A path's own bytes are the uncovered parent background. Click zooms; the tooltip shows
live / self / churn / rate.

**Three metrics** (radio row):

| Metric | Meaning |
|---|---|
| Live bytes | Currently allocated. |
| Cumulative | Total allocated since startup — the churn finder. |
| Churn/s | ALLOCATOR BANDWIDTH: per-path bytes/sec. |

Churn/s is computed panel-side by sampling each node's cumulative counters every prepare and folding
the delta into a 0.5 s-half-life EMA (`m_rates`, keyed by `MemScopeNode` pointer — stable and
pool-bounded).

* A sample gap over 1 s only reseeds baselines, so reopening the panel never spikes.
* Tracking-disabled FREEZES the rates: the counters stand still, and folding zero deltas would decay
  every rate to 0.
* Sampling runs every frame whatever the metric, so switching to Churn/s shows warm data.
* Under Churn/s the header shows total bandwidth (also roughly per frame) and all figures carry a
  "/s" suffix.

The tracker itself is untouched — no per-frame state on the allocation hot path.
