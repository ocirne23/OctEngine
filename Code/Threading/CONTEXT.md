# Threading

> Library documentation for `Code/Threading`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

## The scheduler

Fiber-based work-stealing job scheduler. Jobs run ON Win32 fibers, so a `wait()` inside a job PARKS
the fiber and frees the worker; the fiber may resume on a different thread.

> Hence `/GT` everywhere: **never cache a `thread_local` address (or `PerWorker::local()`) across a
> wait.**

## `Globals::jobSystem`

### Worker sizing

Workers default to *cores − 2*: the MAIN THREAD and the WINDOW THREAD are helper contexts of their
own — main runs jobs inside `wait()`, the window thread runs High jobs between event pumps — so
workers + main + window exactly covers the count.

WHICH core count is used flips with machine size at `c_logicalWorkerCoreMax` (16) PHYSICAL cores
(`RelationProcessorCore` topology; fallback is logical/2 if the query fails;
`JobSystemDesc::numWorkers` overrides):

* **At or below it — LOGICAL processor count.** A narrow machine has too few cores to fill a
  parallelFor, so the second thread per core is worth having even at half efficiency.
* **Above it — PHYSICAL only.** Hyper-threads share a core's execution units, and a wide machine
  already holds the frame's parallel work; logical sizing there only buys scheduling pressure and
  cache contention.

| Machine | Workers |
|---|---|
| 6c / 12t | 10 |
| 12c / 24t | 22 |
| 24c / 48t | 22 |

### Lifetime

* `initialize()` MUST run on the main thread — it registers main as a helper, which is what makes
  main-thread `wait()` run jobs.
* There is deliberately NO `isInitialized()`: every consumer assumes the system is up. Main
  initializes it before anything that submits, and teardown users sit in init_seg sections that
  destruct before XCU5 (e.g. `OC_SEG_PHYSICS`).
* Contexts = workers + main + ONE reserved external-helper slot. The window thread claims that slot
  with `registerExternalHelper()` and runs `tryRunOneHighJob()` — High ring only — between event
  pumps. Its context is `isWorker = false`, so continuations it spawns go to the shared queues and
  never to a deque nobody drains.
* Shutdown is `~JobSystem` at init_seg XCU5 (idempotent `shutdown()`; atexit runs on the main
  thread — see Style for the teardown scheme).

### API

`submit(callable, JobProfile, priority, &counter)` · `submitDelayed` (timer thread) · `wait(counter)`
· `parallelFor(begin, end, grain, JobProfile, fn)` (shared cursor, caller participates, nestable).

* The `JobProfile{ name, EProfileCategory }` is REQUIRED (name = string literal, asserted).
  `execute()` opens that ProfileScope around every job body — and parallelFor around each
  participant's whole run, the CALLER's span starting BEFORE the helper submits so the fan-out has no
  unscoped head; the final join is `wait()`'s own "Job wait" sibling. A worker can therefore NEVER
  run something invisible to the profiler. Job bodies open finer scopes inside it, never a duplicate
  of the job's own name.
* Callables store INLINE (`Job::StorageSize` 64, `sizeof(Job)` 128, both static_asserted) — capture a
  pointer to bigger state.
* Counters and data captured by reference must outlive the wait.

## Post-update jobs

`submitPostUpdate(callable, JobProfile[, priority])` does NOT run at submit time. The job is built
into a pooled `Job` and parked in an MPMC queue (`JobSystemDesc::postUpdateQueueCapacity` 1024) until
the MAIN LOOP kicks the batch:

* `kickPostUpdateJobs()` right BEFORE `Renderer::present()`.
* `joinPostUpdateJobs()` AT THE TOP OF THE NEXT FRAME — first thing inside the "main loop" scope,
  plus once after the loop for the final batch.

This is the UI widget pass's pipelining: the batch runs during present, the frame mark and the
fence/vsync wait, where the workers are idle, so it costs nothing on the critical path.

**Consequence for callers.** The job OVERLAPS `present()` and everything after it, so it must not
touch what `present()` reads or what the next frame mutates — think "somewhere in the present
window", not "before present". What IS guaranteed is completion before the next frame does any work.

**Mechanics.**

* The kick snapshots the queue into a `small_vector` FIRST, then adds the whole batch to a persistent
  `JobCounter` (only the main-thread kick ever `add()`s, always after the join emptied it), then
  submits. A job queued from inside a running one therefore rides the NEXT frame's batch instead of
  racing `add()` against the counter's zero transition.
* Empty queue = one pop, no scope. Nothing in flight = `joinPostUpdateJobs` returns without even a
  profile record.
* Queue from any thread; one submit = one run; priority defaults to Normal (nothing blocks on it).
* Headless kicks at the same point (it has no present) — an unkicked queue only fills up.
* Both fallbacks (job pool exhausted, queue full) run the callable immediately with a debug assert:
  out of phase, but never dropped.

## Scheduling

* Per-context Chase-Lev steal deque (`Threading:StealDeque`), 3 shared Vyukov MPMC priority rings
  (`Threading:MPMCQueue`), a resume queue, and free-fiber/job pools — all preallocated, hot paths
  lock-free, zero runtime allocation.
* A worker's own continuations go to its LOCAL deque regardless of priority (fork/join locality).
* Idle workers spin, then sleep on an eventcount.
* On pool/queue exhaustion a submit executes INLINE with a debug assert — raise the `JobSystemDesc`
  capacities.

## `Threading:JobSync`

* `JobMutex` — fiber-parking mutex. Use it for long exclusive sections such as V3 inference, so waits
  hold a fiber rather than a worker.
* `JobEvent` — manual-reset, idempotent signal.
* `PerWorker<T>` — cacheline-aligned per-context slots, `.local()` / `.forEach()`. The substrate for
  per-worker queues and dirty lists.

## Job cost heuristic

* Graph nodes are wall-time EMA'd; timing is opt-out through `EJobFlag_Untimed`, and parked waits
  count. Each `run()` recomputes critical-path ranks and promotes top-rank Normal jobs to High.
* `parallelFor(begin, end, JobCost&, JobProfile, fn)` auto-grains toward `parallelForTargetChunkNs`
  (25 µs) with per-call feedback.
* Ad-hoc submits are deliberately untimed.

## `JobGraph` + `JobResource` (`Threading:JobGraph`)

Declared-access DAG. `addJob(name, fn).reads(a).writes(b).after(id)` derives hazard edges
render-graph-style in declaration order; edges always point earlier → later, so cycles are
impossible.

`compile()` once, `run()` / `wait()` per frame, zero allocation, re-runnable. A `JobResource` is a
process-unique id created next to the data it names.

## Stress harness

`Globals::jobSystemStress`, Threading/Stress tweaks — throughput, parallelFor, hazard-checked graph
and fiber benchmarks, plus "Run self test". The way to re-verify changes here is a temporary console
torture target.

## Invariants

Each of these guards a race that was actually hit.

* A waiter may FREE the counter the instant it observes zero, so `isDone` means "the FULL value is
  zero". Fiber-wait registrations ride the high 8 bits (`JobCounter::WaiterInc`), keeping the value
  nonzero. The zero transition claims the wait list with ONE exchange of a taken-sentinel, and the
  only post-zero touch is an address-only `notify_all`.
* A parked fiber's wait node lives on its stack — the signaler spins on `switchDone`, and `runFiber`
  reads fiber state BEFORE publishing `switchDone`.
* Recycling freelists are LIFO tagged-index stacks, NOT rings: a preempted ring pop fakes
  full/empty, which exhausted the job pool in practice.
* Inline helping in `wait()` is depth-capped, and at depth ≥ 1 it REFUSES `EJobFlag_ForeignWait` jobs
  (requeuing them instead). A job whose body waits on a counter it did not create
  (`UI::updateJob` → the prepare counter; the renderer's begin-frame job → the GPU-collect counter)
  can otherwise wait on exactly the job SUSPENDED beneath it on the same non-fiber stack — a real
  deadlock. (Observed: window thread → prepare job → parallelFor helpWait → picked `UI::updateJob` →
  waited on prepare.)
  **ANY new job that waits on a foreign counter MUST pass `EJobFlag_ForeignWait` at submit.** Jobs
  waiting only on their own parallelFor children never need it — fiber contexts park and are immune.
* Don't hold a fiber wait across `shutdown()` (asserted).
