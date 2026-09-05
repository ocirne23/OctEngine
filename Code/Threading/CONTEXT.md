# Threading

> Library documentation for `Code/Threading`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Fiber-based work-stealing job scheduler. Core-only.

**Jobs run ON Win32 fibers.** A `wait()` inside a job PARKS the fiber and frees the worker thread;
the fiber resumes later on **whichever worker picks it up**, which may be a different thread.

> Everything else in this file follows from that one fact. It is also why the whole build is `/GT`:
> **never cache a `thread_local` address — `PerWorker::local()` or `getWorkerIndex()` included —
> across a wait.**

All memory is allocated in `initialize()`. The steady-state hot paths are lock-free and allocate
nothing.

## `Globals::jobSystem`

[JobSystem.ixx:60](Private/JobSystem.ixx#L60).

### Worker sizing

Default is `cores - 2`. The `-2` is the **main thread** and the **window thread**, both scheduler
contexts of their own — main helps inside `wait()`, the window thread runs High jobs between event
pumps — so *workers + main + window* exactly covers the machine.

**Which core count `cores` means flips with machine size**, at `c_logicalWorkerCoreMax` = 16
**physical** cores ([JobSystem.cpp:81](Private/JobSystem.cpp#L81)):

| Physical cores | `cores` | Why |
|---|---|---|
| ≤ 16 | `max(logical, physical)` — every hardware thread | Too few cores to fill a parallelFor otherwise; the second thread per core is worth having even at half efficiency. |
| > 16 | `physical` only | Hyper-threads share a core's execution units, and a wide machine already holds the frame's parallel work. Logical sizing there buys scheduling pressure and cache contention, not throughput. |

Physical cores come from `GetLogicalProcessorInformationEx(RelationProcessorCore, ...)`; a failed
query falls back to `logical / 2`. `JobSystemDesc::numWorkers` overrides everything.

| Machine | Workers |
|---|---|
| 6c / 12t | 10 |
| 12c / 24t | 22 |
| 24c / 48t | 22 |

**Contexts = workers + 2**: index 0 is the main thread, `1..numWorkers` the workers, and the last
slot is reserved for the external helper. `getNumContexts()` is what sizes `PerWorker` arrays.
Contexts 0 and the helper have `isWorker = false` — they help in `wait()` but never own
continuations, so nothing lands on a deque nobody drains.

### Lifetime

* **`initialize()` must run on the main thread.** It registers the caller as helper context 0, which
  is what makes a main-thread `wait()` run jobs instead of blocking.
* There is deliberately NO `isInitialized()`. Every consumer assumes the system is up: main
  initializes it before anything that submits, and teardown users sit in init_seg sections that
  destruct before XCU5 (for example `OC_SEG_PHYSICS`).
* `shutdown()` is idempotent and runs from `~JobSystem` at init_seg `OC_SEG_JOB_SYSTEM`; atexit runs
  on the main thread, so the helper-context teardown is valid. It joins the workers and the timer
  thread, then **drops** leftover queued jobs — destroying their captures and signaling their
  counters, so nothing deadlocks — including post-update jobs whose kick never came.
* `JobSystemDesc` defaults: 128 fibers (64 KB commit / 512 KB reserve each), 16384 pooled jobs,
  16384 per priority ring, 4096 per worker deque, 1024 post-update slots, 25 µs parallelFor target
  chunk. Capacities round up to a power of two.

## Submitting work

```cpp
submit(callable, JobProfile, priority = Normal, JobCounter* = nullptr, uint8 flags = 0)
submitDelayed(delaySec, callable, JobProfile, priority, counter)   // dedicated timer thread, min-heap
parallelFor(begin, end, grainSize, JobProfile, fn, priority)
parallelFor(begin, end, JobCost&, JobProfile, fn, priority)        // auto-grain
signal(JobCounter&)                                                // manual decrement, for external completion
```

Priorities are `High` / `Normal` / `Low`, one shared ring each.

### The `JobProfile` is REQUIRED

`JobProfile{ name, EProfileCategory }`, name a **string literal** (asserted; records store it by
pointer). `execute()` opens that scope around every job body, **so a worker can never run something
invisible to the profiler**. Job bodies may open finer scopes inside it, but never a duplicate of the
job's own name.

For `parallelFor` the scope covers **each participant's WHOLE run, not each chunk**. The caller's
span starts BEFORE it submits the helpers, so the fan-out has no unscoped head; the join that
follows carries `wait()`'s own scope (named after the job, `Wait` category) as a sibling
([JobSystem.ixx:291](Private/JobSystem.ixx#L291)).

### Callables store inline

`Job::StorageSize` is 64 and `sizeof(Job)` is 128, both static_asserted, and over-aligned captures
are rejected. **Capture a pointer to bigger state.** Counters and anything captured by reference must
outlive the wait.

### `parallelFor` mechanics

Chunks are pulled from one shared atomic cursor by `min(numWorkers, numChunks - 1)` helper jobs
**plus the calling thread**. A range that fits in one grain runs inline with no submit at all. It is
nestable.

## Post-update jobs

`submitPostUpdate(callable, JobProfile, priority = Normal, flags = 0, batch = Frame)` does NOT run at
submit time. The job is built into a pooled `Job` and parked in an MPMC queue until the main loop
kicks the batches.

**TWO BATCHES (`EPostUpdateBatch`), kicked together, joined at DIFFERENT points of the next frame:**

| Batch | Joined | For |
|---|---|---|
| `Frame` | FIRST thing in the next frame | The UI widget pass: the ImGui context must be quiescent before the main-thread flush and the input pump. |
| `Sim` | Just before the frame's first main-thread write to units/rosters — the game's windowed tick and the entity-change drains (`"Post-update sim join"`) | Jobs that only READ world/roster state and write their own sim outputs: Nav's field steps, the game's nav feed and ambient wander. They run through input, prepare and the camera as well. |

| Call | Where in main.cpp |
|---|---|
| `kickPostUpdateJobs()` | Right BEFORE `Renderer::present()`. Headless kicks at the same point; it has no present, but an unkicked queue only fills up. |
| `joinPostUpdateJobs()` (= Frame) | FIRST thing in the next frame, plus once after the loop for the final batch. |
| `joinPostUpdateJobs(Sim)` | Before `game->updateWindowed` (windowed) and before the script entity-change drain (both modes; the second is a no-op), plus once after the loop. |

The batches therefore run during present, the frame mark and the fence/vsync wait, where the workers
are otherwise idle, so they cost nothing on the critical path.

**What that costs the caller.** The job OVERLAPS `present()` and everything after it, so it must not
touch what `present()` reads or what the next frame mutates before its join — think *"somewhere in
the present window"*, not *"before present"*. A Sim job additionally overlaps the next frame's
input, UI prepare and camera work, so it must not read what THOSE mutate (nothing entity-side does).
What IS guaranteed is completion before its join point.

Users today: the UI widget pass ([UI.cpp:73](../UI/Private/UI.cpp#L73)) on Frame; Nav's field steps
([Nav/System.cpp:352](../Nav/Private/System.cpp#L352)), the game's nav feed and ambient wander on
Sim. The wait scope of a join is named after the batch's last-queued job (`JobCounter::label`).

**Mechanics.** The kick **snapshots the queue first**, then adds the whole batch to a persistent
`JobCounter` and submits. Only the main-thread kick ever `add()`s, and always after the join emptied
the counter — so a job queued from inside a running one rides the NEXT frame's batch instead of
racing `add()` against the counter's zero transition, which `JobCounter` forbids. Nothing in flight
makes `joinPostUpdateJobs` return without even a profile record. Queue from any thread; one submit =
one run.

## Waiting

`wait(counter)` has three behaviours, chosen by what the caller is:

| Caller | Behaviour |
|---|---|
| A job fiber | **Parks the fiber** (`fiberWait`); the worker keeps running other jobs. |
| A registered non-worker thread (main, the window helper) | **Helps** — runs ready jobs until the counter clears (`helpWait`). |
| Any unregistered thread | Blocks on the count atomic, notified on the zero transition. |

The fast path — an already-done counter — returns without opening a scope at all. Both waiting paths
open a scope in the `Wait` category **named after the job the counter last counted**
(`JobCounter::label`, set by every submit that takes a counter — so the profiler says WHICH job a
wait was for; a counter that never had a job reads `"Job wait"`); on a fiber it migrates with the
park, so the span is the true dependency time wherever the fiber resumes.

`tryRunOneJob()` lets a registered non-worker thread pump one ready job manually, e.g. to burn a
stall.

### `EJobFlag_ForeignWait` — required, and easy to miss

Inline helping is depth-capped at `MaxHelpDepth` = 4. **At depth ≥ 1 it REFUSES jobs flagged
`EJobFlag_ForeignWait` and requeues them.**

A job whose body waits on a counter it did NOT create can wait on exactly the job SUSPENDED beneath
it on the same non-fiber stack, wedging the thread: the suspended job can only finish when the frames
above it return, and the frame above is waiting on it. This was hit live — the window thread ran a UI
prepare job, its parallelFor wait helped into `UI::updateJob`, whose first act waits on the prepare
counter.

> **ANY new job whose body waits on a counter it did not create MUST pass `EJobFlag_ForeignWait` at
> submit.** Jobs that wait only on their own `parallelFor` children never need it, and fiber contexts
> park rather than nesting so they are immune either way.

Flagged today: the UI widget pass ([UI.cpp:75](../UI/Private/UI.cpp#L75)) and the renderer's
begin-frame job ([Renderer.cpp:643](../RendererVK/Private/Renderer.cpp#L643)).

## The external helper (the window thread)

The window thread owns the SDL event pump and would otherwise idle between pumps. It claims the
reserved context and runs jobs there.

| Call | Purpose |
|---|---|
| `registerExternalHelper()` | Once, on that thread — gives it `PerWorker::local()` and profiling. |
| `tryRunOneHighJob()` | Runs ONE job **from the High ring only**. |
| `externalHelperWait()` | Parks until a High job is submitted, or `wakeExternalHelper()` fires. |
| `wakeExternalHelper()` | The window's pump request wires this, so a pump request always outranks the nap. |

App wires all four through `window.setIdleWork(...)`
([main.cpp:158-161](../App/main.cpp#L158)).

**High ONLY, deliberately.** Normal and Low carry the multi-second jobs — V3 terrain tiles, nav
builds, the UI widget pass — and one of those would stall the next frame's event pump behind it. High
jobs are short by convention: physics tasks, spatial chunks, panel prepares.

**Its own eventcount, deliberately.** `m_helperEpoch` / `m_helperSleeping` are separate from the
workers' `m_wakeEpoch`. On the workers' eventcount the helper would wake for a Normal or Low job it
cannot take and eat a wake a real worker needed. Only **High** submits pay the extra check
([JobSystem.cpp:417](Private/JobSystem.cpp#L417)).

## The frame's physics step flag

`setFrameHasPhysicsStep(bool)` — main publishes `PhysicsWorld::willStep(dt)` at the frame top, before
any kick — records whether this frame runs the physics step, whose solver fork/join tasks land on the
workers. **`deferFromPhysicsFrame()`** is what optional work checks: true on a step frame whose
PREDECESSOR did not step, i.e. a step-free frame follows, so work that can wait a frame (the World's
periodic SIM LOD selection) moves there and the workers never carry the solver and that job in one
frame. **Below the step rate physics steps every frame**: the previous frame stepped too, waiting
gains nothing, so it reads false and the work runs as scheduled. `frameHasPhysicsStep()` is the raw
flag.

## Scheduling

* **Per-context Chase-Lev steal deque** (`Threading:StealDeque`) — the owner pushes and pops LIFO at
  the bottom (cache-hot continuations), thieves steal FIFO from the top. Fixed capacity instead of
  the paper's growable buffer: a full deque overflows to the shared queues, which keeps memory
  bounded and sidesteps buffer reclamation.
* **Three shared Vyukov MPMC rings** (`Threading:MPMCQueue`), one per priority, plus a resume queue.
* **`getWork` order:** own deque → High, Normal, Low rings → steal from a random victim, walking all
  contexts.
* **A worker's own continuations go to its LOCAL deque regardless of priority** — that fork/join
  locality is the point. `submitReadyBatch` is the exception: fan-out goes straight to the shared
  queues, because a local deque would serialize the start behind one steal per job.
* **Resumed fibers come first** in `workerMain` — they hold fibers and gate dependents.
* **Idle** = a short `_mm_pause` spin (~1 µs), then sleep on an eventcount (`oc::atomic::wait` →
  `WaitOnAddress`) with a Dekker announce-and-recheck against `wakeMany`. `anyWorkForWorker` also
  requires a free fiber: jobs cannot start without one, and the fiber release wakes sleepers.
* **On pool or queue exhaustion a submit executes INLINE** with a debug assert and a
  `numInlineFallbacks` bump — out of phase, but never dropped. Raise the `JobSystemDesc` capacities.

Recycling freelists (`Threading:FreeStack`, `TaggedIndexStack`) are **LIFO Treiber stacks with a
32-bit ABA tag, NOT rings**: a preempted ring popper leaves claimed-but-unpublished cells that make
the ring look empty to everyone else, which exhausted the job pool in practice. LIFO also hands back
the cache-hottest slot.

## `Threading:JobSync`

| Type | Notes |
|---|---|
| `JobMutex` | Fiber-parking mutex. A contended `lock()` inside a job parks the fiber; main helps; unregistered threads block. **Barging, not FIFO handoff** — do not use where fairness matters. Use it for long exclusive sections, such as V3 tile inference, where a seconds-long wait must hold a fiber and not a worker. `JobMutex::Scope` is the RAII guard. |
| `JobEvent` | Manual-reset event over a `JobCounter`. `signal()` is idempotent and releases all current and future waiters until `reset()`, which is only legal between batches. The building block for "wait until that tile/bake/upload exists". |
| `PerWorker<T>` | One cacheline-aligned `T` per **context**. `local()` / `at(i)` / `forEach`. Each slot is single-writer within a parallel phase; a downstream serial node drains them with `forEach`. **Re-read `local()` after any wait.** |

Both `JobMutex` and `JobEvent` are used by `Procedural`'s V3 generator — see
[GeneratorV3.cpp:606](../Procedural/Private/Diffusion/GeneratorV3.cpp#L606).

## `JobGraph` + `JobResource` (`Threading:JobGraph`)

A reusable declared-access DAG. `addJob(name, fn).reads(a).writes(b).after(id).priority(p)`.

**Hazard edges are derived immediately, render-graph style, from declaration order:** a reader
depends on the resource's last writer; a writer depends on every reader since the last write, or on
that writer if there were none. Reads of the same resource run in parallel, and no locks are ever
taken — the schedule IS the synchronization. Edges always point earlier → later, so **cycles are
impossible by construction**.

* `compile()` once — dedupes edges and builds the CSR successor arrays, dependency counts and root
  set. Then `run()` / `wait()` (or `runAndWait()`) per frame: zero allocations, no rebuild.
* `addParallelJob(name, countFunc, grain, fn)` fans out into a `parallelFor` whose range is
  re-evaluated every run. **Its node's fiber parks at the internal join, so budget one fiber per such
  node running concurrently.**
* Callables are invoked once per run and must be re-invocable. `clear()` destroys them so the graph
  can be redeclared.
* A `JobResource` is just a process-unique id; create one at init next to the data it names and keep
  it alive.

### Critical-path promotion

`run()` walks the nodes in **reverse index order — which is reverse-topological, since edges only
point forward** — and computes `rankUs = costEmaUs + max(successor ranks)` from the last run's
measured costs ([JobGraph.cpp:123](Private/JobGraph.cpp#L123)).

Nodes in the **top quartile** of the critical path are promoted Normal → High, so the longest chain
drains first; explicit user priorities are respected. The threshold uses the PREVIOUS run's max rank,
which converges with the cost EMAs — the first run has no costs and promotes nothing. Roots are then
sorted by rank descending.

## Cost measurement

* **Job wall time** is measured by default; opt out with `EJobFlag_Untimed`, and only for
  nanosecond-scale spam jobs where two clock reads (~40–60 ns) would dominate. `costEmaUs` is a 3/4
  EMA with a 1 µs floor. **Parked waits count** — they occupy dependency time on the critical path.
  The same graph node never runs concurrently with itself, so a plain EMA is race-free.
* **`JobCost`** is a live per-item estimate for one `parallelFor` call site. Create it next to the
  loop (static or member, like a Tweak) and pass it instead of a grain size: the chunk size is
  derived from measured ns/item to hit `parallelForTargetChunkNs`, and every run feeds back into a
  7/8 EMA. The runner's sample **includes the cursor overhead**, so overhead-dominated loops drive
  the grain up. Lossy racy EMA on purpose — a heuristic, not a measurement.
* **Ad-hoc submits carry no `JobCost`** and only feed their own `costEmaUs`.

## `JobCounter` invariants

Each of these guards a race that was actually hit.

* **A waiter may FREE the counter the instant it observes completion.** So the whole design keeps the
  zero transition from touching counter memory after zero is observable:
  * Parked-fiber registrations live in the HIGH 8 bits (`WaiterInc`) of the same atomic as the count
    (low 24 bits, `CountMask`). The zero transition sees them in its own `fetch_sub`, and an
    outstanding registration keeps the FULL value nonzero until its owner removes it.
  * `isDone()` is **"the FULL value is zero"**, not "the count is zero".
  * The wait list is a lock-free push stack whose head the transition claims with ONE exchange of a
    **taken sentinel**; a late waiter that sees the sentinel backs its own registration out.
  * The only post-zero operation is an **address-only** `notify_all` — `WakeByAddress` keys on the
    address and never dereferences.
* Re-`add()` only after the previous batch's waits returned. `add()` also clears a consumed
  (sentinel) wait list.
* **A parked fiber's wait node lives on its stack**, valid exactly as long as the fiber is parked, so
  the signaler must read `next` before making the fiber resumable. The resumer spins on
  `switchDone`, and `runFiber` reads the fiber state BEFORE publishing `switchDone` — the instant a
  parked fiber is resumable it can be picked up, finish elsewhere, and have every field rewritten.
* A debug tripwire (`Fiber::debugRunning`) asserts a fiber never runs on two workers.
* **Do not hold a fiber wait across `shutdown()`** — asserted per fiber.

## Stress harness

`Globals::jobSystemStress`, Threading/Stress tweaks. One measured batch per frame in the selected
mode: empty-job throughput, `parallelFor`, a 128-job hazard-checked graph, or fiber park/resume.
Timings and scheduler rates publish under Threading/Stats.

"Run self test" fires a one-shot correctness pass into the log: submit/wait sums, parallelFor
coverage, read/write hazard ordering, fiber waits, delayed jobs. The graph mode validates hazards
live — writers must be exclusive, readers must never overlap a writer — and counts violations.

The way to re-verify deeper changes here is a temporary console torture target.
