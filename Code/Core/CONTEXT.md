# Core

> Library documentation for `Code/Core`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Cross-library utilities and the wrappers for the big third-party headers (glm, SDL, imgui, Windows —
the Vulkan wrapper is RendererVK-private, `Util/VK.ixx`). **Core links nothing.**

Core uses DOTTED module names, one module per `.ixx`, and `Core.ixx` re-exports the vocabulary
modules. It defines the `int8`..`uint64` typedefs, the `INT*_MAX` / `FLT_*` constants, `ARRAY_SIZE`
and `Clock` (steady high-resolution).

---

# `Core.OcSTL` — the backing seam

[OcSTL.ixx](Public/OcSTL.ixx). **The ONE file that names the backing library.** Every container,
string, view, smart pointer, atomic and the utilities around them are spelled `oc::<name>` everywhere
else, so swapping the backing is an edit HERE, not at ~6500 call sites.

**ALIASES ONLY — one line per type, nothing with a body.** The engine's own containers live in
`Core.SmallVector`, which imports this one; never the reverse. OcSTL's only imports are header units,
so it sits BELOW everything including `Core.Profiler` (which imports it instead of Core, since Core
re-exports Profiler).

## The backing is EASTL

`#define EASTL` at the top of the file — deliberately a define in this file rather than a compiler
flag, since this is the only TU that has to see it. **Comment that one line out to go back to the std
containers.**

It is translated at once to an internal `OC_EASTL` and `#undef`'d, because **`EASTL` is also the
include-DIRECTORY name, and an `import <EASTL/x.h>;` macro-expands its header name** — a `#include`
does not, since there the lexer forms one header-name token. Leaving the macro defined would rewrite
every import into `</x.h>`.

**EASTL arrives as HEADER UNITS, never textual GMF includes.** A GMF include leaves EASTL's free
operators — `string ==`, `string +`, container comparisons — unreachable to every importer of Core,
since only the class templates are decl-reachable from the aliases.

The vocabulary is written against `namespace ocstl` (`= eastl` or `= std`), so a name both backings
have needs no `#ifdef`.

> **`oc::` is a convention the compiler cannot enforce.** A header unit re-exports its whole include
> closure, so the exported `<iostream>` still hands out `std::vector` regardless of the private
> imports. Code review is what keeps `std::` containers out — but in practice a `std::vector` meeting
> an `oc::` type at any boundary IS a hard compile error.

## What genuinely differs between the backings

| Name | Difference |
|---|---|
| `oc::allocator<T>` | EASTL allocators are **not element-typed** — one allocator serves every T and hands out raw bytes. The parameter is ignored so both backings share one alias signature; `oc::allocateArray` / `deallocateArray` absorb the byte-vs-element difference, and that is what `Core.SmallVector` allocates through. |
| `oc::make_unique<T[]>` | **Spelled out, not aliased.** EASTL's is `new T[n]` (**DEFAULT**-init) where std's is `new T[n]()` (**VALUE**-init) — a silent divergence that shipped garbage into every zero-assuming array: `BitRangeAllocator`'s grown tail read as fully allocated and asserted at startup, and script data, profiler records and char buffers all assume zeroing. |
| `oc::as_bytes` | Ours in BOTH backings — `std::as_bytes` does not accept an `eastl::span`. |
| atomics | **std in BOTH backings, deliberately.** An atomic is not a container: it allocates nothing and is a thin wrapper over intrinsics. EASTL's is a strict subset — no `atomic_ref`, no C++20 wait/notify (which the JobSystem eventcount and JobMutex are built on), and its memory orders are distinct **TAG TYPES** rather than one enum, which would not mix with the `std::atomic`s the engine still has to touch. |
| `initializer_list`, `char_traits` | std in both. |
| `allocator_traits`, `basic_string`, `basic_string_view` | **Not aliased at all** — EASTL cannot express them at the same arity (its strings take no traits parameter). **A vocabulary entry that silently means something different per backing is worse than no entry** — name the concrete string types instead. |

Alias templates CANNOT be specialized: a `std::hash` specialization is still written
`namespace std { template<> struct hash<X> ... }`, and is read back through `oc::hash`.

## Filling EASTL's C++20 gaps

`oc::contains` (no member `.contains()`), `oc::startsWith` / `oc::endsWith`, `oc::getline`,
`oc::format` (**returns an `oc::string`; ALL formatting goes through it, never `std::format`**), plus
in-module shims — `operator<<` for eastl strings in `namespace eastl` so ADL finds them, and
`std::formatter` specializations next to the primary template so an `oc::string` is a valid
`std::format` argument.

## What stays `std::`

**Exported by OcSTL** (platform/runtime surface, not container surface; no replacement library ships
them, and routing them through here means Core.ixx has no std import of its own):

`<iostream>` `<sstream>` `<format>` `<mutex>` `<shared_mutex>` `<condition_variable>` `<thread>`
`<future>` `<coroutine>` `<chrono>` `<execution>` `<charconv>` `<type_traits>` `<new>` `<cmath>`
`<random>`

**Add a missing std header there.** Two deliberate absences:

* **`<bit>`** — see `Core.OcBit`.
* **`<filesystem>` and `<fstream>`** — ALL file and directory access goes through the File library's
  `FileSystem`, so a library that needs the disk links File. **Core itself therefore cannot do IO:**
  `Core.Tweaks` takes injected read/write hooks (`setFileIo`, installed by main from FileSystem).

## Crossing to `std::`

`oc::toStd` / `oc::fromStd` for strings and vectors — the sanctioned conversion, and a no-op copy
under the std backing so call sites stay backing-agnostic. Every place that needs it is marked at the
call site:

* Vulkan-Hpp `enumerate*` (returns `std::vector`), `vk::ClearColorValue` (`std::array`) and
  `vk::ResultValue`'s tuple conversion (`std::tie`)
* glslang `preprocess` / `GlslangToSpv` / `OutputSpvBin` (`std::string` + `std::vector<unsigned>` by
  reference)
* ONNX Runtime `Session::Run` (`std::vector<Ort::Value>` — **move-only, so it cannot even be
  converted** and stays std entirely)
* `<filesystem>` inside File, the iostream/sstream parsers, and the Nsight Aftermath `to_string`
  overloads

## EASTL's application hooks

Core links `EASTL$<$<CONFIG:Debug>:d>.lib` and supplies the three hooks EASTL declares but expects the
APPLICATION to define:

* Its two named and aligned `operator new[]` forms live in `Core.Allocator`, next to the engine's
  other global `operator new` definitions — **so EASTL allocations land in `Globals::allocator` and
  the MemoryTracker.**
* `EA::StdC::Vsnprintf` — the one that is not an allocation — sits in `Core/Private/Core.cpp`.

**The ALIGNED form deliberately does not over-align.** `eastl::allocator::deallocate` frees every
block with plain `delete[]`, so an `_aligned_malloc` block (what the `align_val_t` overloads return)
would reach `Allocator::deallocate` and corrupt the heap. It asserts instead; EASTL only calls it for
over-aligned element types, so nothing reaches it today.

---

# `Core.OcBit`

[OcBit.ixx](Public/OcBit.ixx). **THE bit vocabulary, and the reason OcSTL does not export `<bit>`.**

Every std spelling in `<bit>` is written for any x86 baseline, so it carries a runtime CPU-support
dispatch or a portable BSF/BSR fixup around what is ONE instruction under the engine's `/arch:AVX2`,
where POPCNT, LZCNT and all of BMI1/BMI2 are baseline.

The module imports nothing (GMF `<intrin.h>` / `<immintrin.h>`) and sits below Core next to
`Core.OcSTL` and `Core.Profiler`; `Core.ixx` re-exports it.

**Everything is width-EXACT** through `if constexpr (sizeof(T))` on the `oc::UnsignedInt` concept: a
`uint8` answers in 8-bit terms, and **a SIGNED argument is a compile error** — bit math on a sign bit
wants an explicit cast.

| Group | Names |
|---|---|
| Counting | `popcnt`, `tzcnt`, `lzcnt`, `trailingOnes`, `leadingOnes` |
| Widths | `bitWidth`, `bitCeil`, `bitFloor`, `hasSingleBit` |
| Rotates / swap | `rotl`, `rotr`, `byteSwap` — per-width overloads, since the MSVC intrinsics are width-exact |
| BMI1 | `blsr`, `blsi`, `blsmsk`, `bextr`, `andn` |
| BMI2 | `bzhi`, `pdep`, `pext` — the Morton primitives |
| Cast | `bitCast<To>(from)` |

* **`tzcnt` / `lzcnt` are DEFINED on zero as the operand width**, matching `std::countr_zero` /
  `countl_zero`. That is a property of the TZCNT/LZCNT encodings, **so do not lower `/arch` without
  revisiting this.**
* `bitCast` is the one piece of `<bit>` that was never a dispatch problem — a compiler builtin, and
  the only entry usable in constant expressions — which is why `std::bit_cast` is gone from the
  codebase too.

---

# `Core.SmallVector`

The engine's own contiguous containers, re-exported by `Core.ixx` alongside OcSTL.

* **`oc::small_vector<T, N>`** keeps the first N elements INSIDE the object and spills to the heap
  past N — the per-frame scratch list that rarely exceeds N does zero allocations.
* **`oc::fixed_vector<T, N>`** is `small_vector<T, N, false>`, which refuses to spill: it asserts and
  drops, **so a release build never writes out of bounds.**

---

# `Core.Time` — the two clocks

[Time.ixx](Public/Time.ixx).

## Global pause

Tweak `Time/Paused` — **Synced**, so the server's pause freezes clients; deliberately NOT Saved. The
Pause/Break key toggles it, game mode included.

| Clock | Reads | Consumers |
|---|---|---|
| **REAL** | `getDeltaSec` / `getElapsedSec` | Frame pacing, `Timer`s, UI, camera, tweak debounce, and the network transport — receive, send keepalives, RTT, snapshot cadence. **Never stops.** |
| **SIM** | `getSimDeltaSec()` (0 while paused), `getSimElapsedSec()` (stands still through an accumulated paused-seconds offset) | physics, `world.update`, `game.update` / `updatePlayer`, `scriptContext` (dt AND elapsed), particles, force, nav |

The SIM clock also feeds the three renderer-side wall-clock users that would otherwise keep
animating: `ubo.timeSeconds` (ocean waves, force pulses), the GPU particle dt in `present()`, and the
ocean wind steer.

**A zero dt alone would not stop per-call script or component actions**, so:

* `Entity::updateSelf` folds `isPaused()` into its frozen gate. Sim ticks skip, while transform,
  render, spatial, audio-follow and the placement components keep running, so the frozen world stays
  inspectable.
* `ScriptEventManager::fireEvent` drops events, like Frozen does.
* **`OcTweakSync` is intercepted BEFORE that gate** (`NetworkManager::fireEventAttributed`), so a
  server's UNpause always reaches paused clients.
* StructureSystem's gen-rate stats guard their `/ dt` against the zero sim delta (0/0 NaN in the HUD).

The flag only flips on the main thread between frames — tweak poll or key handler — and workers read
it mid-pass, which is race-free by that timing.

## Frame pacing

`registerTweaks()` plus **`beginFrame(windowFocused, vr, vsync, displayRefreshHz, pumpWindow,
waitFence)`** — ONE call at the loop top does the whole frame boundary: the fence wait, the frame-rate
limit, the window thread's event-pump kick, and the start of the next frame's clock.

The fence goes through a function pointer main passes (`Renderer::waitFrameSlot`), **because Core
cannot see the renderer**; both it and `pumpWindow` may be null.

Tweaks (all Saved): `Max FPS` (0 = uncapped), `Inactive max FPS` (30), `Busy-wait window (ms)` (2.5),
`Stable frame time` (on), `Input pump lead (ms)` (2.0), `VSync`.

The full behaviour — the limit, stable-time attribution, the vsync period quantization and the pump
prediction — is documented under **Frame pacing** in
[`Code/RendererVK/CONTEXT.md`](../RendererVK/CONTEXT.md), where the fence and swapchain live.

## `Timer`

Constructed with a duration and an optional callback returning `REPEAT` or `DONE`; serviced by
`processTimers()` inside `Time::update`, so **a timer fires at the frame boundary**. Asserts on a
duration under 1 ms. `reset(duration)` re-arms. The one-shot `--profile-after` / `--quit-after` /
`--scenario-at` triggers are `Timer`s.

---

# `Core.Tweaks`

[Tweaks.ixx](Public/Tweaks.ixx).

```cpp
Tweak::floatVar("Category/Sub", "Name", &liveVariable, min, max, step, onChange, flags);
Tweak::intVar / boolean / color3 / float3 / ...
```

Once at init, this exposes a variable in the TweakPanel. **Pointers are non-owning and the variable
must outlive the registration.** This is the standard way to make anything runtime-configurable.

**Identity is `"Category/Name"` — renaming orphans the saved value.**

## Groups

The panel's top folds are **groups**, NOT part of the category string: `TweakGroups::c_table` in
Tweaks.ixx maps each ROOT category ("Sky" of "Sky/Clouds") to a group with a header colour —
Graphics / FX / System / Game — and a root listed nowhere lands in the trailing "Other" group.
`Tweak::groups()` / `Tweak::groupIndexOf(category)` are the lookups the TweakPanel uses. **A new
root category goes into that table**, otherwise it shows under "Other". Panel order = table order.

## `ETweakFlags`

Optional last parameter after `onChange`, or `Tweak::ScopedFlags` RAII to flag a whole
`registerTweaks` block. **Explicit per-call flags win over the block default.**

**`Saved`** persists to `Assets/Local/tweaks.cfg`:

* main calls `TweakRegistry::loadSaved()` right after `FileSystem::initialize`.
* `TweakRegistry::update(dt)` per frame **poll-detects** changes — the panel writes through raw
  pointers, so polling is the only reliable hook — and debounce-saves 0.5 s after the last change,
  plus at exit.
* Unknown file keys are preserved for other run modes.

**`Synced`** broadcasts server → clients:

* NetworkManager watches `syncGeneration()`, which the poll bumps on any Synced change.
* `packSynced` splits every Synced var into self-contained records chunked to fit one network message.
* It rides the engine-reserved `"OcTweakSync"` event, intercepted in `fireEventAttributed`, so it
  never reaches scripts or game hooks. **Only CLIENTS apply**, and `applySyncedBlob` ignores keys the
  receiver did not flag Synced and clamps to the receiver's own bounds.

**Policy:** all `Game/*` tweaks are `Saved|Synced` except `Game/Camera` (Saved only — personal
preference) and `Game/Sim LOD` (**NEITHER** — per-process performance tuning registered by the Entity
library's World, where the code defaults must rule every run).

## Command-line overrides

* **`--tweak "Category/Name=v [v v v]"`** (`setOverride`) works on any variable, Saved or not. It
  applies now or at the variable's registration, **wins over the file, and is NEVER written back** —
  `saveFile` skips overridden keys, and the snapshot is taken after the apply so it does not read as
  a change. This is how an unattended profiling run pins settings without touching the user's
  tweaks.cfg.
* **`--tweaks <file>`** (`loadOverrides`) applies a whole file in the tweaks.cfg line format with `#`
  and `//` comments. Later `--tweak` flags win over it.
* `Assets/Scenarios/cpu-profile.tweaks` = the heavy GPU features off, for CPU-focused runs.

## The IO hooks

`setFileIo(read, write)` must be installed before `loadSaved()`. Core cannot include `<fstream>`, so
main injects `FileSystem::readFileStr` / `writeFileStr`. **Without them the `Saved` flag is simply
inert** and the registry keeps working in memory.

---

# `Core.Window`

The window thread. Fully documented in [`Code/Input/CONTEXT.md`](../Input/CONTEXT.md), because Input
is its only consumer: SDL init, window creation and the OS message pump all live on a dedicated
thread, which also helps the job system between pumps.

---

# `Core.GameHud`

[GameHud.ixx](Public/GameHud.ixx). `Globals::gameHud` — **the in-game HUD's data model. PURE STATE.**

Gameplay (script thunks, C++) writes it from **any thread**; the UI's `GameHudOverlay` snapshots and
draws it once per frame over the viewport.

| Element | Notes |
|---|---|
| **Hotbar** | `NumSlots` = 12. Default keys 1..9,0 in one row; `setHotbarLayout(columns, keyLabels)` folds it into a GRID — the RTS QWER/ASDF/ZXCV pattern. `HudSlot{ label, count, used }`; count > 0 draws a stack number. |
| **Bars** | `HudBar{ name, value, maxValue, color }` — keyed by display name, insertion order is display order. |
| **Counters** | `HudCounter{ name, value, decimals, color }`, stacked from the top left. |
| **World labels** | `HudWorldLabel` — gameplay **PRE-PROJECTS** the world position into viewport pixels (`Camera::worldToScreen`) and **replaces the whole list every frame**: no keys, no persistence, and off-screen entities are simply not submitted. Title, info lines, up to THREE stacked bars, and an `emphasized` flag for the selected entity. |
| **Popup** | `HudPopup` — a world-anchored BUTTON ROW (the barracks unit-type picker), rebuilt every frame like the labels. |

**Hit-testing works in both directions:** the overlay reports each drawn slot's and button's screen
rect back (`setSlotScreenRects` / `setPopupButtonRects`), so gameplay resolves clicks through
`slotAtScreenPos` / `popupButtonAtScreenPos`.

Mutex-guarded because scripts tick on workers: **every write is a short lock, and the UI takes ONE
snapshot copy per frame** — `snapshot(Snapshot&)` into a copy the overlay KEEPS, so the vectors and
strings reuse their capacity. The per-frame rebuilt lists (world labels, the popup) are handed over
by SWAP (`swapWorldLabels` / `swapPopup`): the builder gets last frame's list back and reuses it. Colours are linear 0..1 RGB, matching the DSL surface.

It is a plain `.CRT$XCU` global, **so it outlives `~World`'s script `OnDestroy` calls** — plain XCU
destructs last, see InitSeg.h.

---

# Other Core pieces

| Module | What |
|---|---|
| `Core.Allocator` | Global `new`/`delete` routed through `Globals::allocator`. Exposes the `g_memoryAllocHook` / `g_memoryFreeHook` atomics the MemoryTracker installs — **null until installed, one relaxed load and a branch of overhead**. Contract: the alloc hook fires AFTER a successful allocation, the free hook BEFORE release, so a recycled address can never race its own map entry, and **hook implementations must never allocate through these paths.** `g_memoryHookSuppress` is the per-thread suppression counter. |
| `Core.Log` | 4096-message ring behind a mutex with a `g_revision` counter; the UI Log panel snapshots it. Levels Verbose / Info / Warning / Error. **`Log` also prints to stdout**, which is what makes an unattended run's output file useful. |
| `Core.Camera` | `Camera` (position, viewMatrix, near, far) plus `screenToWorld` for viewport picking and `worldToScreen` for the HUD's world labels. |
| Math | `Transform`, `Frustum`, `AABB`, `Sphere`, `Rect`. |
| Containers | `LPMultiMap`, `LockFreeList`, `ExpiryCache`, `BitRangeAllocator`, and `CheckedPtr` / `RefCheckable` (debug-checked non-owning pointers). |
| `Core.VrSession` | The `IVrSession` interface — opaque handles, no OpenXR types — implemented by RendererVK's `OpenXRSession` and consumed by Input's `VrInput`, **so Input and RendererVK stay unlinked.** |
| `Core.glm` / `Core.SDL` / `Core.imgui` / `Core.Windows` | Wrappers for the big third-party headers. |
| `Core/Private/InitSeg.h` | The single authority on cross-library teardown order — see Style in CLAUDE.md. |
| `Core/Private/forceinclude.h` | In every TU; pulls in InitSeg.h and compiles `assert` out in non-debug. |

---

# Profiling

CPU + GPU scope profiler: `Core.Profiler` (`Globals::profiler`), re-exported from `Core.ixx`. **Its
interface must not import Core back** — it uses textual std includes in the GMF.

## `ProfileScope`

```cpp
ProfileScope scope("Name", EProfileCategory::Physics);
```

RAII, and **the name MUST be a string literal** — records store it by pointer. About 15 ns on the hot
path: two rdtsc plus one 32-byte store into the calling thread's lock-free single-writer ring.

`ProfileTrack` holds `CAPACITY` = 32K records (1 MiB per track), overwrite-on-wrap.
`MAX_TRACKS` = 64, `FRAME_HISTORY` = 512, `MAX_OPEN_DEPTH` = 32.

`stop()` exists for the cases where a scope must end early; the destructor is then a no-op.

### Categories

`EProfileCategory`: App, Core, Entity, Script, Animation, Physics, Audio, Particle, Force, Spatial,
Threading, Procedural, Network, File, Renderer, GPU, UI, Input, Game, Wait, Other.

The enum plus two POSITIONAL tables — `profileCategoryName` and `profileCategoryColor` (packed ABGR,
directly usable as an ImGui `ImU32`) — **must be edited together.**

`Wait` is for parked or blocked time; `GPU` for renderer GPU passes.

### Entity scopes are opt-in

The entity pass carries a scope only for entities flagged `EEntityFlag_Profiled`, a one-way
`setProfiled()` latch. Animator, Force, Script and GameUnit set it at spawn; machine structures
(barracks, turret) latch it in update, since `machineKind` is stamped post-spawn. **Static scenery
stays scope-free so it cannot flood the rings.**.

## Thread registration is explicit-only

Every profiling thread calls `registerThread(name, sortKey)` at startup. **A `ProfileScope` on an
unregistered thread debug-asserts and no-ops.**

Sort keys drive display order: `SORT_KEY_MAIN` 0, `SORT_KEY_NAMED` 1 (the GPU track),
`SORT_KEY_WORKER` 2 + index, `SORT_KEY_BACKGROUND` 100 + offset (streamers, timer, loaders).

* **Registered:** Main (in the profiler's static-init ctor), JobWorkerNN (in `workerMain`), GPU,
  TexStream / MeshStreamer, V3 Loader, JobTimer.
* **Deliberately unregistered:** transient startup pools and stress-test threads — tracks are
  permanent at 1 MiB each — and miniaudio's thread.

## Scopes across a job wait

Scopes MAY span a JobSystem `wait()`: **the open-scope stack MIGRATES with the parking fiber.**
`suspendScopes` (on the parking thread, before the switch) closes the on-thread segments, and
`resumeScopes` replays them on whichever thread resumes.

Timing is the sum of on-thread segments — **parked time is not counted** — and memory attribution
follows the fiber. `ProfileScope` caches NO track pointer: construction and destruction each address
the CURRENT thread's track. Max migrating depth 32.

## Frames and the clock

Records push at scope END, so rings are naturally ordered by end time — which the snapshot's early-out
relies on.

The tick domain is `__rdtsc` (invariant TSC), **calibrated against QPC every `endFrame`** so GPU
timestamps (which calibrate against QPC) and millisecond conversions stay accurate. The ratio is
estimated over the whole run and converges; the anchor is refreshed every frame so conversions never
extrapolate far.

`endFrame()` — main loop, after present — pushes a frame mark into the 512-frame ring.

The profiler **self-initializes at static init** (init_seg right after the allocator), which is what
makes a `ProfileScope` valid from any static initializer. `endStaticInit()` FIRST THING in `main()`
closes the synthetic `"Static init"` scope, so static-init allocations attribute there.

**Recording is always on.** The panel's PAUSE freezes the data, not the bookkeeping (`setPaused`:
ring pushes drop, while open/close keeps running so migration and Memory attribution stay live).
Resume lays a GAP pseudo-frame (`isFrameGap`) whose duration is meaningless.

## GPU pass timings

`GpuProfiler` (RendererVK `Objects/`, a member of Renderer) writes timestamp pairs in the primary CB
around each pass — **outside render passes only**, since multiview would replicate them and a
SECONDARY_COMMAND_BUFFERS subpass admits no other primary commands.

That is why the desktop forward pass records **one render-pass instance per stage** — see
[`Code/RendererVK/CONTEXT.md`](../RendererVK/CONTEXT.md).

One query pool per frame slot; results are read in `beginFrame` after the fence and pushed into a
named "GPU" track. Alignment uses `VK_KHR_calibrated_timestamps`, with fallback submit-tick anchoring.

## Text report

`Profiler::buildReport(ProfileReportOptions)` → `oc::string`. **Core does no IO**, so
`App.ProfileDump`'s `writeProfileReport(path, options)` writes it through FileSystem.

Contents — the last N completed frames, never across a pause gap:

* frame-time percentiles
* per-track busy/wait % over depth-0 scopes
* a global "top self time across CPU tracks" table
* per track, an aggregated CALL TREE: ms/frame inclusive, self, calls/frame, max call; children
  sorted by inclusive, and rows under `minMsPerFrame` folded into "(+N more)"
* a flat by-self table

JobWorker tracks MERGE into one "Workers (merged)" tree, or one each with `perWorkerTrees`. Parent =
the last record one level up that CONTAINS the record, by time. Records are per on-thread SEGMENT, so
a fiber-parked scope counts once per resume. **A "RING LAPPED" note flags a track whose 32K ring no
longer covered the window.**

### Triggers

* **Interactive** — key F7, or the panel's "Dump" button; both write `Assets/Local/profile.txt`.
* **Unattended**:

```
App.exe --profile-after <sec> [--profile-frames W] [--profile-out path] [--profile-workers]
        [--quit-after <sec>] [--no-vsync] [--tweak "Cat/Name=v"] [--tweaks <file>]
```

**TIMES ARE SECONDS of engine time since the loop start** — frame counts would shift with the frame
rate. `--profile-frames` clamps to `FRAME_HISTORY - 1` (511).

Each trigger is a one-shot `Timer` returning `Timer::DONE`, armed before the loop, **so it fires from
`Time::update()` at the frame boundary** — after the previous frame's mark and before the "main loop"
scope opens, which is exactly where the report (completed frames only) and its IO belong.

> main() re-bases the clock with ONE `Globals::time.update()` right before arming them. Nothing calls
> `update()` during init, so the clock still stood at STATIC-INIT time and **every timed trigger
> fired on frame 1 whenever init outlasted the flag's seconds.** The same re-base also stops the
> first frame's delta from being the whole init phase.

Either timed flag also makes the window count as FOCUSED — otherwise the "Inactive max FPS" cap is
what gets measured.

### `Tools/profile.ps1`

```
Tools/profile.ps1 [-Config RelWithDebInfo] [-After 10] [-QuitAfter After+0.5] [-Frames 256]
                  [-Out Local/profile.txt] [-Game] [-Scenario ...] [-ScenarioAt 1]
                  [-Server] [-Port 27999] [-VSync] [-Workers] [-NoBuild]
                  [-Tweak "..."] [-Tweaks Scenarios/cpu-profile.tweaks]
                  [-Show 80] [-TimeoutSec 300] [-AppArgs "..."]
```

Builds App, runs it, waits for the clean exit and prints the report head. **THE loop for Claude to
measure → change → re-measure without looking at the screen.** It always overrides `Time/Max FPS=0`.

Seconds are formatted InvariantCulture — a Dutch locale would print "10,5".

> **ALWAYS PROFILE `-Config RelWithDebInfo`.** Debug timings are not results; the user rejected them.

### Game scenario

`--scenario <save|default> [--scenario-at 1]`.

Script side: **`-Game -Server` is THE standard perf run.** `-Scenario` defaults to
`Assets/Scenarios/march-64-units.txt`, a checked-in F9 save of the corridor with 64 team-0 units; `""`
= none, `"default"` = F10's `Local/gamesave.txt`. `-Server` = `--server --port 27999`, so the
networking path is live without colliding with a manually running instance on 27888.

It calls `GameMatch::runScenario` on the authority:

1. `loadGame(path)`.
2. `issueScenarioOrder` RETRIES each update until three things are ready — the loaded units are
   queryable (spatial entries link at `commitFrame`, AFTER `game.update`), the enemy Base is in
   `StructureSystem`'s per-frame view (refreshed in `tickAuthority`, also after), and
   `navSystem.raster()` is published.
3. Then it selects EVERY live own-team unit (`NpcSystem::queryAllUnits`, a walk of the World's root
   list) and issues
   `moveOrderAt(pointOutsideFootprint(base))` — THE RMB move order, so locked targets plus one seeded
   lane. `pointOutsideFootprint` is the click-on-building push-out, **because the Base CENTRE is
   blocked cells, where the A\* fails and every unit's own plan request fails too**, which left the
   crowd milling at spawn.

It logs `Scenario: N units ordered to (x, z) after F frames, lane seeded|NOT seeded`.

## UI "Profiler" panel (`UI:ProfilerPanel`)

Frame bar graph (click = pause + inspect; Space = pause), per-track flame graphs (zoom / pan / fit), a
sortable stats table, and an **UNCAPPED fps readout** in the toolbar.

The readout is `max("main loop", "GPU Frame")` of the displayed frame — what the frame would run at
without the vsync/fence throttle, since **the main-loop scope EXCLUDES the fence wait by design.**
Per track it takes the depth-0 record with the largest overlap with the frame window and uses its
full duration, since the GPU frame straddles the marks. Live is a 0.9/0.1 EMA fed once per displayed
frame; paused is exact. Tagged CPU-bound / GPU-bound.

The live view shows `frameCount − 3`, so CPU and GPU data are complete.

---

# Memory

`Core.MemoryTracker`. **NOT re-exported from `Core.ixx`** — its only consumer is the UI Memory panel.

## The tracker

`Globals::memoryTracker` attributes every allocation to the **PROFILE-SCOPE PATH open on the
allocating thread.**

* The allocator hooks walk and build a `MemScopeNode` tree — `MAX_NODES` 4096, bump-allocated from a
  pool and never freed.
* Live pointers sit in a **64-shard open-addressing map** (`NUM_SHARDS` 64 × `SHARD_CAPACITY` 8192 =
  512K entries at 24 B each), preallocated, lock-striped.

**Construction through `#pragma init_seg`:** allocator `.CRT$XCA` → profiler `XCA1` → tracker `XCA2`
(`InitSeg.h`), so static-init allocations are tracked and these three destruct after everything else.

* **The hooks NEVER allocate** — everything is preallocated in the tracker ctor.
* **Pools deliberately leak at exit**: the dtor only uninstalls the hooks, since later global dtors
  still free tracked memory.
* `MemoryHookSuppress` (per-thread RAII) mutes the hooks for PERMANENT infrastructure allocations
  that would misattribute — a profiler track and its ring are allocated on a freshly registered
  thread BEFORE that thread's TLS track pointer exists. **Only for allocations that are never freed:**
  a suppressed alloc later freed unsuppressed is a harmless map miss, but a tracked alloc freed under
  suppression would leak its counts.
* Unregistered threads land in `<other threads>`; overflow counts into `getDroppedAllocs` (red in the
  panel).
* **Node identity is the name-literal pointer.**

## UI "Memory" panel (`UI:MemoryPanel`)

A squarified treemap: one box per scope path, area ∝ the selected metric, nested, coloured by
category. **A path's own bytes are the uncovered parent background.** Click zooms; the tooltip shows
live / self / churn / rate.

| Metric | Meaning |
|---|---|
| Live bytes | Currently allocated. |
| Cumulative | Total allocated since startup — the churn finder. |
| Churn/s | **ALLOCATOR BANDWIDTH**: per-path bytes/sec. |

Churn/s is computed **panel-side**, by sampling each node's cumulative counters every prepare and
folding the delta into a 0.5 s-half-life EMA keyed by `MemScopeNode` pointer (stable and
pool-bounded).

* A sample gap over 1 s only reseeds baselines, so reopening the panel never spikes.
* **Tracking-disabled FREEZES the rates** — the counters stand still, and folding zero deltas would
  decay every rate to 0.
* Sampling runs every frame whatever the metric, so switching to Churn/s shows warm data.
* Under Churn/s the header shows total bandwidth and all figures carry a "/s" suffix.

**The tracker itself is untouched — no per-frame state on the allocation hot path.** The panel
allocates nothing per frame once warm either (it would show in its own treemap): the snapshot is a
flat vector of trivial nodes whose children sit in a contiguous index range, and the treemap layout
runs on two kept stacks.
