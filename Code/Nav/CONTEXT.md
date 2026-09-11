# Nav

> Library documentation for `Code/Nav`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Sparse per-TEAM fields for crowd pathfinding. Links Threading only.

**Entity-agnostic by design.** The GAME feeds obstacles and sources every frame — it knows footprints
and teams — and UNITS read the results from the parallel entity pass. This library knows nothing
about entities.

`Globals::navSystem` sits in init_seg `OC_SEG_NAV` and is the first global to destruct; its dtor
waits on in-flight build jobs.

## What it holds

| Field | Per | Mutability |
|---|---|---|
| `TeamField` | team (`MaxTeams` = 8), plus one obstacle-only **raster**, plus goal fields by key | **Immutable once built.** Built on a job, published by a pointer swap on main. |
| `FlowField` | team | Mutable. Units splat into it from the pass; main steps it. |
| `PressureField` | team | Mutable. Units inject; main diffuses. |

### Thread contract

* `setGoal` / `clearGoal` / `update` / `drawDebug` / `seedPath` / `requestSeedPath` are **MAIN
  THREAD**, outside the entity pass. `seedPath` only QUEUES: its A* runs on a `"Nav seed path"`
  job and `update` writes the finished lanes (see Seed paths).
* `setObstacles` / `setTeamSources` are main thread OR **the game's nav-feed POST-UPDATE job**: that
  batch runs during present, when nothing on main touches Nav, and the field-step job in the same
  batch holds only the fields — so the change-detect compare and the source copy run off main. The
  batch joins before the next `update`.
* `teamField()` / `goalField()` / `raster()` / `flow()` / `pressure()` reads are **worker-safe during
  the pass**, because the published pointers and the goal key map only change inside `update()`.

## Inputs from the game

`GameMatch::gatherNavFeed` rebuilds both lists as a POST-UPDATE job (joined at the top of the next
frame) and calls the setters from that job; Nav change-detects them there. The unit sweep is SLICED
over the "Rebuild interval" (`rebuildInterval()`), publishing once per cycle — sources are only ever
consumed at that cadence.

**`NavObstacle`** — an XZ AABB plus a `cost` byte:

* **0 = Blocked**, impassable.
* **Non-zero = BREACHABLE.** Its cells stay walkable at a `(1 + cost)` step multiplier, rasterized in
  a second pass after the Blocked stamping and never overriding Blocked. So the Dijkstra routes
  THROUGH it where the detour around is longer, while steering, density and line-of-sight treat only
  Blocked as a wall — **the game's walls, which units then walk into and chew down.**

**`NavSource`** — `pos`, `stopRadius` (footprint half; it seeds every cell it covers, so a wall is a
target too), `id`, `kind`:

| kind | Fed by |
|---|---|
| 0 | structures (alive, not the invulnerable Base; walk-through pieces — cables, crossings, solars — are excluded) |
| 1 | player capsules — our own plus every client twin |
| 2 | a goal destination (set by `setGoal`) |
| 3 | units, from `NpcSystem`'s roster — **no spatial sweep**, and **CULLED to units with another team's unit or player within "Nav unit source reach" (64 m, a coarse cell hash: reach .. 2× reach)** — a unit's field is only ever read by other teams within `navFollowRadius`, so far ambient enemies contribute nothing but flood area |

Clients run no unit sim: the feed there stages **obstacles only**, for the local player's goal field.

## Grid

* 2D over XZ, **2 m cells** — the same as the game's structure grid and Spatial's finest cell.
* **16×16-cell CHUNKS (32 m)** in `Nav::ChunkMap<T>`: open-addressed `uint64 → unique_ptr<T>`, the
  Spatial CellMap pattern. **Values are heap-owned, so growth never moves a chunk.** Chunks are
  POOLED inside the map: `reset` / `erase` park them, `getOrCreate` re-initializes a parked one, and
  `reset` keeps the table capacity — so a rebuild or an evict/regrow cycle allocates nothing once the
  peak is reached. The team fields ROTATE too (`TeamSlot::live` / `retired`): `kickBuild` rebuilds
  into the field the last publish retired unless a seed-path job still holds it.
* Chunk key = 28-bit packed chunk coords (`Nav::chunkKey`), sign-extended on the way back.
  `Nav:Grid` is pure coordinate math.
* **Storage scales with ACTIVE AREA, not world size** — several far-apart battle sites coexist and
  cost nothing at each other.

## `TeamField`

[Field.ixx:37](Private/Field.ixx#L37). Per chunk, three parallel arrays:

| Array | Meaning |
|---|---|
| `cost[256]` | u8 — 0 free, `Blocked` (255), else an extra step multiplier `(1 + cost)`. A clearance ring of `clearanceCost` hugs walls off. |
| `dist[256]` | u16 fixed-point at `DistScale` 8 units/m — **geodesic** distance to the NEAREST source of that team. u16 gives 8 km of reach. |
| `src[256]` | Index into `sources()` — WHICH source won. |

**One cell read therefore gives a unit of any OTHER team both "which way to the closest enemy" and
"which enemy", nearest by walking distance around walls rather than as the crow flies.**

Built by a radius-bounded multi-source Dijkstra: 8-connected, no corner cutting, seeds = every cell
of each source's footprint, stopping past the build radius. **The front CREATES chunks as it goes, so
a field is exactly as big as its reach.**

### The build is internally multithreaded

1. `rasterizeObstacles` pre-creates every reachable chunk serially, then parallelFors the Blocked
   stamping (per obstacle) and the clearance ring (per chunk) — racing writers all write the same
   byte value.
2. The flood is a **CHUNK-WAVE Dijkstra**. Each wave parallelFors `floodSolveChunk`: a 256-cell
   mini-Dijkstra to the chunk's LOCAL fixpoint, writing ONLY that chunk, with a local lazy heap of
   packed `(dist<<16|idx)` u32s.
3. A serial step turns each solve's `borderImproved` mask (8 dirs) into the next wave, creating
   chunks the front wants to enter.

**Distances only decrease and the relaxation fixpoint is unique, so the waves converge to EXACTLY
the serial Dijkstra's result.** Cross-chunk reads race benignly — aligned u16, monotone-decreasing
min — and a missed improvement just re-queues the chunk.

### The build is STEPPED across frames

A build is also **pre-emptible** (see Threading). Every `parallelFor` in it — the three raster
phases and the waves — runs at the build's own LOW priority, so their automatic between-chunk
points yield to Normal work as well as High; explicit points sit at every raster phase boundary,
after the raster, every 64 sources of the seed loop, and after each wave's serial queueing in
`stepBuild`. Nothing is half-done at any of them — the front lives in members. The field-step job
adds one per team gather. `floodSolveChunk` and the seed-path A\* carry a `ThreadLocalScope` and
therefore have NO points inside; their granularity is the chunk / the job.

`beginBuild` (raster + seeds) runs in the `"Nav build"` job together with the first chunk budget;
the front (`m_wave`/`m_nextWave` plus `m_waveCursor`) lives in the field, and `NavSystem::update`
submits one `"Nav build step"` job per frame with a CALCULATED chunk budget (`buildStepBudget`):
**the last completed build's chunk total × dt / "Build spread (s)" (0.25 = the rebuild interval),
rounded up** — so a build costs the same slice of every frame and lands just as the next rebuild is
due; a build that grew runs a few extra frames at that slice, a slot with no history (its first
build) runs in one step. A wave is cut wherever the budget runs out and its remainder resumes next
step (the unsolved remainder keeps `waveDirty`, so it cannot be double-queued, and it reads the
improved neighbours when its turn comes — relaxation order never changes the fixpoint) — until
`isBuildDone`, then publishes. **The wave parallelFors run at LOW priority**, so the chunk solves
fill the gaps between entity batches instead of competing for workers. A dirty obstacle set still
waits for the in-flight builds to finish (bounded: no NEW build kicks while it is dirty), then
re-kicks all.

> **STANDING RULE for anything inside these jobs: gather buffers must be STACK locals, never
> `thread_local`** — unless the code between taking and last using it provably never waits, AND
> a `ThreadLocalScope` pins it (the chunk solve's `heap` in `floodSolveChunk` is the one such
> buffer). A parallelFor wait parks the fiber, and the worker may pick up another team's job and
> clear a shared thread_local mid-use; the pin asserts at that park.

### Reading it

| Call | Notes |
|---|---|
| `sample(xz, seed)` | 3×3 lowest-neighbour scan crossing chunk borders → `{valid, dist, srcIndex, descentDir}`. **`seed` jitters ties** so a plateau equidistant between two sources does not stall a whole crowd on one line. `descentDir` is zero AT a source. |
| `sourceAt(i)` | Names the target. |
| `isBlocked`, `lineOfSight(a, b, radius)`, `freeDistance(a, dir, maxLen, radius)`, `wallPush(xz, range)` | Raster reads, any thread. `radius > 0` also tests the two parallel offset lines — a body, not a point. |
| `findPath(from, to, maxExpand, radius, outPath, scratch)` | A* string-pulled into a minimal polyline. **NOT for per-unit use** — this is the one-shot planner behind `seedPath`. `scratch` is the caller's `PathScratch` working set (the seedPath job's `thread_local`, pinned by a `ThreadLocalScope` over the call — findPath never waits, so the fiber cannot migrate mid-search, and the pin asserts if that changes). |
| `steerPoint`, `avoid`, `chooseSide` | Per-walker helpers the PLAYER uses. `avoid` picks the nearest clear whisker (±30/60/90/120°) and carries a `side` hysteresis so a slide along a long wall does not flip-flop. `chooseSide` is deterministic from geometry, **so a whole group agrees**. |

### `CostWindow` — the per-tick raster snapshot

[Field.ixx:87](Private/Field.ixx#L87). `snapshotCosts(centre, radiusCells, out)` fills a stack-local
window with a handful of chunk lookups and row copies.

**Why:** the chunk-hash helpers pay a `find()` PER CELL, and a steering unit makes 100+ such reads
per tick. After the snapshot every probe — whisker march, body and corner samples, wall push — is a
plain array read.

`MaxRadius` is 15 (31×31 = 961 bytes, meant for the caller's stack). **Cells outside the window read
as OPEN**, the same rule `costAt` has for absent chunks, so size the radius to cover the longest
probe.

## `NavSystem::update` (main)

Runs LAST in the frame, right before main's `kickPostUpdateJobs()` + `present()` (App row 25). It
used to sit in the kick/join window after `physics.update`, but the Low-priority build slices it
kicks there landed next to the "Spatial cull" chunks and delayed them (a worker holding a build
chunk finishes it first). From the tail of the frame they run through present and the fence wait
instead — the stretch where the workers are otherwise idle — and the field-steps job it queues
rides the same post-update kick. **A finished build publishes one frame later than the in-window
placement did**; the game tick and the entity pass of the NEXT frame are the first readers.

1. **Publish** finished builds — a `shared_ptr<const TeamField>` swap. Workers only ever read
   published pointers, which change only here.
2. Retire expired seed stamps, **apply the seed plans whose A\* job finished** (`applySeedPlans`:
   the lane + trough writes, in queue order), and expire idle goals.
3. **Swap the obstacle snapshot** if dirty — but only while NO build is in flight, since every job
   shares it. A dirty obstacle set waits for the fleet to drain, then marks every field dirty.
4. **Kick one Low-priority `"Nav build"` job per due slot.** A slot is due when nothing is published,
   or when the "Rebuild interval" has elapsed AND (its sources changed — id/kind, or moved more than
   half a cell — or the raster changed, or it is periodic). **The interval gates the dirty path too:**
   thousands of moving unit sources are dirty every frame, which used to chain builds back to back.
   A team with no sources publishes null and units fall back to the local search. **A rebuild of a
   published field also waits out a physics-step frame** that a step-free frame follows
   (`JobSystem::deferFromPhysicsFrame`), so the first chunk's job does not land next to the solver
   tasks; a slot with nothing published kicks at once.
5. **Queue the flow and pressure steps as ONE POST-UPDATE JOB.**

### The field steps are a post-update job

`runFieldSteps` ([System.cpp:366](Private/System.cpp#L366)), queued through `submitPostUpdate`, so
main kicks it just before `present` and joins it at the TOP OF THE NEXT FRAME.

* That window — present, the frame mark, the fence/vsync wait — is **the one stretch where no game
  code runs on main**, so the job has the fields to itself. NOTHING waits on it during a frame and no
  writer can overlap it.
* This replaced an in-frame kick that had to be fenced by a `finishSteps()` before the entity pass
  AND by a counter wait inside `seedPath`. Neither exists any more.
* **The entity pass therefore samples fields built from the PREVIOUS frame's splats and seeds** — one
  frame of staleness, bought for a step that costs main nothing.
* Teardown is safe without a fence: by the time `clear()` or `~NavSystem` runs, main has left its
  loop and done the final `joinPostUpdateJobs`, and a batch still unkicked in the queue is dropped by
  `JobSystem::shutdown` without ever being invoked.
* It runs on a JOB FIBER and its nested parallelFors park it, so **everything in it is STACK locals**
  per the standing rule.

**Inside, the work is PER-CHUNK across ALL teams**, two barriers total:

1. Each field's serial pre-work: `beginStep` — drain the touch queue, flip buffers, evict idle
   chunks, gather `StepItem`s, snapshot `prevActive` — stashing this step's parameters on the field
   so a `StepItem` stays small.
2. ONE `parallelFor` over **every team's** flow chunks (`"Nav flow step"`, grain 2, Low).
3. A barrier, then ONE over every team's pressure chunks (`"Nav pressure step"`).
4. `endStep` flips pressure.

One team's lone big field therefore no longer serializes behind the empty ones. **The flow phase must
fully settle before the pressure phase**: the pressure→flow push splats ATOMICALLY into flow write
buffers that the flow tasks write plainly.

A chunk task writes only its own write buffer, peak and touched flags, and reads neighbours' READ
buffers. Growth requests ride the field's `TouchQueue` — a BOUNDED lock-free append (256 keys, one
atomic bump, 2 KB per field; a request past the cap is dropped and re-issued by the next splat) —
and materialize before the NEXT step; the front moves well under a cell a frame, so the delay is
invisible. The step job's gathered `StepItem` lists and the `ChunkMap`'s eviction scratch are kept
members, so a step allocates nothing.

## Goal fields

**Keyed by `uint64`, not slot index.**

```cpp
setGoal(key, dest, radius);   clearGoal(key);   goalField(key) -> const TeamField*
static constexpr uint64 GoalKeyPlayer = 1;
static uint64 goalKeyRoute(structureId, waypoint);
static constexpr uint32 GoalExpireFrames = 60;
```

* The SAME `TeamField` with ONE source (kind 2, a destination), **non-periodic**: rebuilt only when
  the destination moves more than half a cell or the raster changes. Radius is per goal.
* **`setGoal` every frame the goal is wanted** — a key not set for `GoalExpireFrames` is dropped
  (only when idle; a building slot waits its turn).
* `GoalKeyPlayer` is the local player's RMB move order. `GamePlayer::tickMovement` sets it each frame
  while an order stands, steers by the field's descent when it covers the capsule, goes straight line
  until then, and clears on arrival ([Player.cpp:157](../Game/Private/Player.cpp#L157)).
* **Works on CLIENTS too** — the nav feed runs there with obstacles only, so the goal field is local.

## Seed paths

The single most important idea here: **nothing per-unit plans. One A\* writes a lane, and the crowd
follows it.**

`seedPath(team, from, to, speed, laneWidth, clearance)`
([System.ixx:62](Private/System.ixx#L62)) plans ONE route with A* over the raster
(`TeamField::findPath` — straight-shot test first, then octile A*, string-pulled by `lineOfSight`),
then writes it in TWO places.

> `laneWidth` is how wide the lane is PAINTED; `clearance` is the **planning** width — how much room
> the planned route keeps from walls. They are separate knobs.

### The A\* is a job; the write is main

`seedPath` returns as soon as it has QUEUED a `SeedPlan` (`m_seedPlans`, heap-owned — the plan
holds a `JobCounter`; applied plans return to `m_seedPlanPool` and are reused with their path
capacity, so the steady state allocates no plans) and submitted the `"Nav seed path"` job (Normal
priority). `TeamField::rasterizeObstacles` likewise keeps its clearance-ring work lists as members
(`m_ringChunks` / `m_ringKeys`), so a rebuild allocates nothing either. The job touches
only the plan and the raster, and **holds its own `shared_ptr` to that raster**, so a publish on
main during the search cannot free it. It also applies the "Seed range" cut. `findPath` never
waits, so the job's `thread_local` `PathScratch`, pinned by a `ThreadLocalScope` over the call, is
valid for the whole search. **The scratch is allocation-free in steady state:** the cell → node
index is `AStarIndex`, a flat open-addressing table cleared by a generation stamp (the node-based
`oc::unordered_map` it replaced freed and re-allocated one node per discovered cell on EVERY
search), and the node / open / cells vectors keep their capacity. Only a search larger than any
before it on that thread grows the arrays.

The NEXT `update` whose plan counter is done writes the lane and the trough — on main, outside the
pass, against the CURRENT raster — which keeps the write contract exactly what it was: the only
other writer of those buffers is the field-step job, which runs in the present window. **Cost:
one or two frames from order to lane,** invisible next to the step's own frame of staleness.
`seedPath`'s `true` therefore means "queued", not "a route exists": a failed search is dropped
silently at apply time. `waitAll` (`clear`, the dtor) joins in-flight plans before dropping them.

### 1 — Into the team's flow as a lane (`FlowField::seedPath`)

* **Max-magnitude into BOTH buffers**, so it is visible to units immediately and survives the next
  decay step.
* The written VECTORS blend between the two segments meeting at each junction, so a 90° corner reads
  as an arc while the polyline stays put — a rounded path could cut a wall corner.
* Blocked lateral cells hand their share to the surviving ones, so **a lane squeezing past a wall
  FOCUSES on its centre instead of thinning**.
* The `raster` argument is REQUIRED: a wide lane beside a building would otherwise write flow into
  cells nothing can stand in.

### 2 — As a NEGATIVE-pressure TROUGH (`PressureField::seedPath`)

* **Pressure is where ATTRACTION lives.** Everything reads pressure as "go the other way", so a
  trough pulls units in AND the pressure→flow push sucks the surrounding flow into it — instead of
  the lane only existing where it was drawn.
* `squeezeGain` deepens the trough per BLOCKED cell of the 8-neighbourhood: **a narrow channel is
  where a crowd most needs to be pulled in, and where the flow has no surrounding cells to support
  it**, so a gap ends up several times more attractive than the open stretches of the same route.
* **Min semantics**, so re-seeding the same route is idempotent instead of digging deeper each time.

Only the first "Seed range" metres of a plan are WRITTEN. The A* still runs to the real destination —
a truncated search would pick the wrong way round an obstacle — but a lane far ahead of the group is
stale by the time anyone gets there.

### `requestSeedPath` — the rate limiter

[System.ixx:70](Private/System.ixx#L70). **This is what makes a per-unit request affordable.**

* It refuses a plan when one of the same team was already made within "Seed area" of BOTH its start
  and its destination inside "Seed cooldown", and caps plan jobs queued at "Seed max/frame".
* **The test is a DISTANCE, not a bucket test** — two units either side of a bucket border are one
  request — but the candidates come from a hash of recent plans **BUCKETED BY START at exactly the
  dedup radius**, so only the 3×3 bucket neighbourhood is scanned. Constant cost per request, no
  matter how many units ask or how many plans are alive.
* Stamps retire through an **EXPIRY QUEUE**: one global FIFO of `(time, bucket)`. Stamps are made in
  time order and each bucket is append-only at the back, so `update` pops exactly what expired and
  **never walks the map** — usually nothing at all.

### Who asks

| Caller | Where |
|---|---|
| A fresh RMB move order for the selected units, started at the centroid of their LARGEST CLUSTER (single-linkage flood fill at "Group cluster radius" 12 m — a straggler must not drag the lane's origin off the bulk) | `GameMatch::orderSelectedUnits`, [MatchInput.cpp](../Game/Private/MatchInput.cpp) |
| Every barracks route change, leg by leg | `GameMatch::seedRouteLane`, [MatchInput.cpp](../Game/Private/MatchInput.cpp) |
| Each co-op wave, once, at `queueWave` | [MatchCoop.cpp](../Game/Private/MatchCoop.cpp) |
| **EVERY unit on its own jittered timer**, while it walks a ROUTE or a MOVE ORDER — a unit chasing a HUNTED enemy (nav field / local search / engage) requests one only on `GameUnitParams::huntSeedTeam` (the co-op AI team; -1 = none), so friendly units never carve lanes toward enemies | [Npc.cpp:397](../Game/Private/Npc.cpp#L397) |

The unit path queues a `SeedRequest{from, to, team, stuck}` every "Seed request interval" (×0.75–1.25
jitter, random phase at spawn). **The due time is on the SIM CLOCK** (`m_seedDue` against
`Time::getSimElapsedSec`), not a countdown of the tick delta — a throttled tick's delta is clipped by
"Max catch-up", so a countdown ran slow whenever the cap bit. The cadence is therefore independent of
the SIM LOD tier; the tier only bounds it below by its tick rate (1 s at tier 2). Requests are
drained on the main thread by `NpcSystem::service` into
`requestSeedPath` at "Order lane speed" — or "Stuck lane speed" when the unit is stalled past
"Unstick after".

**A crowd walking the same way therefore costs ONE plan, and the group's lane keeps up with the group
as it advances without anyone tracking the group.** There is no group re-seed timer.

## `FlowField`

[Density.ixx:21](Private/Density.ixx#L21). Fixed-point `int16` at `Scale` 1024 (±32 m/s summed per
cell), double-buffered.

* `splat(xz, velocity)` is worker-safe (atomic adds). The read buffer is an **EMA of the splatted
  velocities**, so the steady state is the velocity itself, not a sum.
* Fading is **FRAME-RATE INDEPENDENT**: configured as a HALF-LIFE in seconds, not a per-frame factor,
  and the splat gain that feeds the EMA derives from the same step.
* `sample(xz, raster)` is a 3×3 mean that **leaves BLOCKED cells out of the average entirely instead
  of averaging them in as zero**. Inside a one-cell gap two thirds of the neighbourhood is wall,
  which used to cut the lane's sampled strength to a third — exactly where a unit most needs to
  believe in it.
* `sampleArea(xz, radiusCells, raster)` is the same idea over `(2r+1)²` open cells: "which way is the
  crowd around here going".
* `maxSpeed` caps a cell's magnitude, because **splats SUM and a milling crowd must not out-shout a
  seeded lane**. A wall cell holds nothing.
* `clearArea(xz, radius)` zeroes both buffers so a fresh order can turn a group around without the
  old trail pulling it back. Currently unused — a fresh order seeds its own lane instead.

## `PressureField`

[Density.ixx:95](Private/Density.ixx#L95). A **SIGNED** scalar: positive = jams and crowd presence,
negative = seeded lanes.

* `inject(xz, amount)` is worker-safe. `value()` and `gradient()` are worker-safe reads.
* **ONE current buffer that units read AND inject into**; the other is the step's scratch, flipped by
  `endStep`.
* Diffusion is one Jacobi step read→write per chunk, resolving its own and its 4 neighbour chunks
  ONCE PER CHUNK — after which neighbours are plain array indices, and a chunk whose own and whose
  neighbours' cells are all quiet is skipped outright (that is what `prevActive` snapshots serially,
  since during the step the neighbours' `peak`/`touched` are being rewritten by their own tasks).
* Diffusion is dt-scaled and clamped to the 0.25 Jacobi limit; decay is a half-life. A
  `propagationFloor` stops sub-floor neighbours from propagating or draining.
  > A trough that spreads without paying for it grows until the half-life catches up, and a flat
  > field has no gradient to steer by.
* **Walls are reflective (no-flux), in the diffusion AND in `gradient(xz, raster)`**, which
  substitutes the centre value for a blocked neighbour. **Pass the raster.** Without it a wall reads
  as a plain 0, which against a negative seeded trough looks like a HIGH-pressure spot — and every
  vector near a wall then pushes away from it.
* `lowestNearby(xz, radiusCells, raster, out)` finds the centre of the lowest-pressure OPEN cell
  around — the escape target for a unit boxed into a corner.
* **The step PUSHES THE FLOW.** It hands each active cell's gradient to a callback that splats
  `-∇p · gain` into the lanes — one traversal, no second pass re-resolving neighbours through the
  hash. So a jam bends the stream upstream of it, and a seeded trough sucks the surrounding lanes in.
* Chunks grow where pressure reaches a border cell and evict once quiet.

## Unit steering

Implemented in `GameUnitComponent::update`
(`steerHeading` in [GameUnitComponent.cpp](../Entity/Private/Components/Game/GameUnitComponent.cpp)), documented here
because it is what the fields exist for. **CONTEXT STEERING — no search, and no per-unit path state
beyond the last heading.**

### Picking the goal direction

1. **The enemy team fields.** Every OTHER team is sampled at the unit's cell and the geodesically
   nearest source wins → `targetPos`, with `goalDir` = its `descentDir`. This skips the spatial-query
   target search entirely. A unit is a source of its own team's field, so unit-vs-unit works the same
   way.
2. **RANGE-GATED by "Target search radius"** (geodesic). Out of range, a non-locked `hasTarget` from
   an earlier tick is DROPPED at once — kept, the unit marched to the last known spot for up to a
   whole retarget interval and looked as if it ignored the follow radius.
3. **The NAV FOLLOW band** (search radius .. "Nav follow radius"): too far to target, but if the
   crowd FLOW field holds a lane at the unit that exceeds `flowKnee × moveSpeed`, it walks the lane.
   **Pursuit survives a player sprinting out of range** until the target returns or the lane decays.
4. **Fallback** where no field covers the unit: the old local search — a random pick among the 4
   nearest enemy structures by spatial query, with the nearest enemy player as fallback, re-rolled on
   a jittered "Retarget interval".
5. Route waypoints and DSL-locked targets point `goalDir` straight at them.

### Scoring the fan

16 candidate headings **ANCHORED ON THE GOAL**, plus the exact lane direction:

* **k = 0 IS `goalDir` exactly.** The other 15 carry a per-unit angular offset under half a slot, so a
  crowd sharing one goal heading does not walk in columns along the same sixteen world directions.
* Anchoring matters because a one-cell gap subtends about one 22.5° slot from a few metres out, so a
  free-floating fan often had no candidate pointing through it.

```
score = free · (Goal·dot(d, goal) + Flow·laneW·dot(d, lane) + Persist·dot(d, lastDir))
      − Pressure·gpW·dot(d, ∇p̂)
      + wallW·dot(d, wallAway)
      − clipped · CornerClip
```

* `laneW = knee(|lane|, FlowKnee × moveSpeed)` and `gpW = knee(|∇p|, PressureKnee)`, where
  `knee(x, k) = x/(x+k)` is a **COMPRESSIVE** response — the knee is the value scoring 0.5. Steep at
  low values so ONE stuck unit registers, flattening toward 1 so a hundred never saturate a cell.
* `free` = the open fraction of a `look`-metre **CENTRE-LINE** march, where
  `look = max(SteerLook, moveSpeed)` — so a 1-tile gap reads as open.
* `wallAway` = `CostWindow::wallPush` within `bodyRadius + WallKeep`. **In a gap both walls cancel and
  the unit centres itself; at a corner the single push swings it wide.** Its weight is
  `min(|wallAway|, 1) × WallPush`.
* **A candidate blocked one body-probe ahead is rejected outright.** Two LATERAL body samples at that
  point each subtract `CornerClip` — clipping a corner the centre-line run cannot see is *discouraged,
  not forbidden*, so a one-cell gap stays usable.
* The lane direction is considered separately because it is generally not on the fan's grid.

### Stall handling

Position is checkpointed every 0.75 s. **Moving less than 0.6 m since the last checkpoint accrues
stalled time** — displacement, not per-tick progress, because a jittering heading never accumulates
progress.

| State | Threshold | Weight changes |
|---|---|---|
| **stalled** | > 0.4 s | Goal ×0.3, Persist ×0.2, Flow ×2 — the fields become the preference when the unit has problems. |
| **unstick** | > "Unstick after" | Goal → **ESCAPE at weight 1.5**, Persist → 0, Flow ×0.5, Pressure ×3, plus a ±30° per-unit random rotation on the chosen heading to break symmetric two-unit locks. |

The ESCAPE direction backs away from whatever pins the unit: the wall push first, else away from the
nearest unit/structure/player from a small 4 m spatial query (scenery filtered out), else simply the
reverse of the intended direction.

### Live-target tracking

Within "Target track radius" (geodesic) the goal is the field's descent at the target's LIVE position,
~0.25 s fresh. So the goal weight floors UP to "Track goal" and the seeded lane near-mutes by "Track
flow mult" — **the lane's periodic re-plans lag a moving player badly.** Between track and search
radius the unit marches lane-friendly toward the target.

### Contributions back

* **Flow splat.** The unit splats its **MEASURED** planar velocity — never the command, since a pinned
  unit must not write "into the wall" — scaled by "Flow splat gain", **ONE CELL BEHIND itself**. A
  unit samples its own cell at double weight, so splatting there fed its heading back to itself; a
  trail belongs behind the walker.
* **Presence pressure**, injected every tick by every unit and player: a weak spacing term next to
  the seeded lane troughs.
* **Stuck pressure**, injected while stalled, scaled by `min(stall − 0.4, 1.5)`.
* **No lane splat while braking.** `GameUnitComponent::update` BRAKES — planar velocity to zero at
  the steering accel — whenever it has no walk target or has arrived. The unit capsules run
  `Friction 0`, so without an explicit brake a coasting unit never stopped, kept splatting its
  velocity into the crowd lane, and the pack followed the ghost trail.

## Tweaks

### `Nav/*` (defaults from [System.ixx:150](Private/System.ixx#L150))

| Tweak | Default |
|---|---|
| Enabled | on |
| Field radius | 400 m (the 600 m co-op map, corner to corner from the Base) |
| Rebuild interval | 0.25 s |
| Build spread (s) | 0.25 — a field build is spread evenly over this (per-frame chunk budget = last build's total × dt / spread); 0 = one step |
| Clearance cost | 1 |
| Chunk keep frames | 120 |
| Flow half-life | 5 s |
| Flow max | 20 m/s |
| Pressure diffusion | 0.1 |
| Pressure floor | 1.6 |
| Pressure half-life | 1 s |
| Pressure flow gain 10^x | 0.3 (**the slider is the EXPONENT** — one slider spans 0.01 .. 1000 with fine control at the low end) |
| Seed area | 10 m |
| Seed cooldown | 3 s |
| Seed max/frame | 2 |
| Seed trough | 20 |
| Seed trough squeeze | 10 |
| Seed range | 20 m |
| Debug draw / team / radius / flow min | 0 / 0 / 60 m / 0.1 m/s |

### `Game/Nav` steering (defaults from [GameUnitComponent.ixx](../Entity/Private/Components/Game/GameUnitComponent.ixx))

| Tweak | Default | | Tweak | Default |
|---|---|---|---|---|
| Goal | 0.3 | | Look-ahead | 6 m (min; scales with speed) |
| Flow | 1.0 | | Corner clip penalty | 0.7 |
| Flow splat gain | 0.5 | | Wall push | 1.0 |
| Persist | 0.4 | | Wall keep | 0.9 m |
| Track goal | 1.5 | | Presence pressure | 0.01 |
| Track flow mult | 0.15 | | Stuck pressure | 0.5 |
| Pressure | 0.5 | | Unstick after | 1.0 s |
| Pressure knee | 0.23 | | Seed request interval | 1.0 s |
| Flow knee | 0.15 × moveSpeed | | Order flow blind | 0 s |
| Order lane speed | 10 | | Stuck lane speed | 10 |
| Lane width | 3 m | | Group cluster radius | 12 m |

Related `Game/Enemies`: Target search radius 15 m, Target track radius 5 m, Nav follow radius 40 m,
Retarget interval 5 s, Route engage radius 10 m, Unit max speed 12 m/s (~1.5x the runner), Nav
fields on.

> **Flow above Goal is deliberate:** a lane is a proven route, so where one exists it should outweigh
> walking straight at the goal.

## Debug draw

`drawDebug(focus, lineSink)` — renderer-agnostic. "Debug team" 0..7 selects a team field; **8 selects
the local player's move-order goal field**.

| Mode | Draws |
|---|---|
| 0 | off |
| 1 | Chunk outlines plus descent arrows of the selected field within "Debug radius" of the focus. |
| 2 | **Crowd FLOW arrows** (green → white, longer with strength, scaled against "Nav/Flow max" so a reading means the same thing everywhere; hidden below "Debug flow min" so the haze does not bury the lanes) drawn over **PRESSURE columns** — a diamond, bar and top diamond, on a **log scale** so a jam does not flatten the map. Jams rise yellow → red; seeded troughs dip in cyan → deep blue. |

## Scope

Server and single-player only: units gate on authority, and clients feed obstacles only.
