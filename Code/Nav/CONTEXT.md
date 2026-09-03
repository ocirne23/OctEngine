# Nav

> Library documentation for `Code/Nav`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

## Overview

Sparse per-TEAM flow fields for crowd pathfinding. Module `Nav`, partitions `:Grid` / `:ChunkMap` /
`:Field` / `:Density` / `:System`. `Globals::navSystem` sits in init_seg `OC_SEG_NAV` and is the first
to destruct — its dtor waits on in-flight build jobs.

The library is entity-agnostic. The GAME feeds it, and UNITS read the results inside the parallel
entity pass.

**Inputs the game supplies**

* `NavObstacle` AABBs.
  * cost 0 = **Blocked**.
  * a non-zero cost = a **BREACHABLE** obstacle whose cells stay walkable at a `(1 + cost)` step
    multiplier. Rasterized in a second pass after the Blocked stamping, never overriding Blocked. The
    Dijkstra therefore routes through it where the detour is longer, while steering, density and
    line-of-sight treat only Blocked as a wall — the game's walls.
* Per-team `NavSource`s: pos, `stopRadius` (footprint half), id, and kind (0 structure, 1 player).

## Grid

* 2D over XZ, 2 m cells — the same as the structure grid and as Spatial's finest cell.
* 16×16-cell CHUNKS (32 m) in `Nav::ChunkMap<T>`: open-addressed `uint64 → unique_ptr<T>`, the
  Spatial CellMap pattern. Values are heap-owned, so growth never moves a chunk.
* Chunk key = 28-bit packed chunk coords (`Nav::chunkKey`). Everything in `Nav:Grid` is pure
  coordinate math.
* **Storage scales with ACTIVE AREA, not world size** — several far-apart battle sites coexist.

## `TeamField`

Immutable once published, one per team. Per chunk:

| Array | Meaning |
|---|---|
| `cost[256]` | u8 — 0 free, 255 blocked, else an extra step multiplier. A clearance ring of "Clearance cost" hugs walls off. |
| `dist[256]` | u16 in 1/8 m — geodesic distance to the NEAREST source of that team. |
| `src[256]` | The winning source index. |

Built by a radius-bounded multi-source Dijkstra: 8-connected, no corner cutting, seeds = every cell
of each source's footprint, stopping past "Field radius". The front CREATES chunks as it goes, so a
field is exactly as big as its reach.

### The build jobs are internally multithreaded

1. `rasterizeObstacles` pre-creates every reachable chunk serially, then parallelFors the Blocked
   stamping (per obstacle) and the clearance ring (per chunk) — racing writers all write the same
   byte value.
2. The flood itself is a **CHUNK-WAVE Dijkstra**. Each wave parallelFors `floodSolveChunk` over the
   dirty chunks: a 256-cell mini-Dijkstra to the LOCAL fixpoint, with 3×3 chunk pointers resolved
   once, all cell access by index, writes staying inside the chunk, and a local lazy heap of packed
   `(dist<<16|idx)` u32s.
3. A serial step turns each solve's `borderImproved` mask (8 dirs) into the next wave, creating
   chunks the front wants to enter.

Distances only decrease and the relaxation fixpoint is unique, so waves converge to EXACTLY the
serial result. Cross-chunk reads race benignly — a missed improvement just re-queues the chunk.
"Nav build wave" is the per-wave worker span.

> **Standing rule for anything running inside these jobs:** gather buffers must be STACK locals,
> never `thread_local`. A parallelFor wait parks the fiber and the worker may pick up another team's
> job, which would clear a shared thread_local mid-use.

### Sampling

`sample(xz, seed)` is a 3×3 lowest-neighbour scan crossing chunk borders, with seed-jittered ties so
plateaus do not stall a crowd. It returns `{valid, dist, srcIndex, descentDir}`; `sourceAt(i)` names
the target.

## `NavSystem::update`

Runs on main, from main.cpp's kick/join window that overlaps the physics step and the two jobs. It
touches neither the spatial index nor the renderer, so it overlaps the "Spatial cull" and
"Begin frame" jobs.

`GameMatch::feedNav` (in `game.update`, after `service`) only STAGES obstacles and sources — its unit
sweep is a spatial query, illegal in the window — and the entity pass consumes the published fields
after the joins.

What `update` does:

* Publishes finished builds — a `shared_ptr<const TeamField>` swap. Workers only ever read published
  pointers, which change only here.
* Kicks one Low-priority `navFieldBuild` job per team when any of these hold:
  * its sources changed (id/kind, or moved more than half a cell),
  * the obstacle raster changed (change-detected by hash in `setObstacles`; the obstacle snapshot is
    shared by every job, so it only swaps when NO build is in flight),
  * the "Rebuild interval" elapsed,
  * or nothing is published.
* A team with no sources publishes null.

### The flow and pressure fields

The per-team `FlowField` and `PressureField` (`Nav:Density` partition — the name is historical) use
the same chunk machinery. A splat into a missing chunk queues the key through `PerWorker`, and main
creates it next frame.

The field STEPS run as ONE POST-UPDATE JOB (`runFieldSteps`, queued at the end of
`NavSystem::update` through `submitPostUpdate`), so main kicks it just before `present` and joins it
at the TOP OF THE NEXT FRAME.

* It fills the present + frame-mark + fence/vsync window and costs main nothing.
* The entity pass therefore samples fields built from the PREVIOUS frame's splats and seeds — a
  deliberate frame of staleness.
* NOTHING waits on it during a frame. There is no `finishSteps` or `m_stepCounter` any more: that
  window is the one stretch where no game code runs on main, so `seedPath` (which writes the same
  chunk buffers) needs no fence, and `waitAll` and the dtor only drain the per-team BUILD jobs. By
  the time `clear()` / `~NavSystem` runs, main has left its loop and done the final
  `joinPostUpdateJobs`, and a batch still unkicked in the queue is dropped by `JobSystem::shutdown`
  without ever being invoked.
* The job body fiber-parks on its nested parallelFors, so its gather buffers are STACK locals per the
  standing rule above.

Inside it the steps are MULTITHREADED PER CHUNK across ALL teams:

1. `beginStep` per field — drain, flip, evict, gather, plus a `prevActive` snapshot, stashing the
   step params on the field.
2. ONE `parallelFor` over every team's flow chunks.
3. A barrier, then ONE over every pressure chunk.
4. `endStep` flips pressure. Two barriers total.

A chunk task writes only its own write buffer, peak and touched flags and reads neighbours' READ
buffers. The pressure → flow push splats atomically into flow buffers the flow phase has already
settled, and pressure growth requests ride the `PerWorker` touch queue, materializing next update.

Fields evict chunks idle for "Chunk keep frames".

## Goal fields

`setGoal(slot, dest, radius)` / `clearGoal` / `goalField`, `MaxGoals` 4.

* The SAME TeamField with ONE source (kind 2 = a destination), non-periodic: rebuilt only when the
  destination moves more than half a cell or the raster changes. Radius is per order.
* Slot 0 is the local player's RMB move order (`GamePlayer::tickMovement`): it sets the goal each
  frame while an order stands, steers by the field's descent when it covers the capsule, goes
  straight line until then, and clears on arrival or no order.
* Works on CLIENTS too — `feedNav` runs there with obstacles only (walls plus mirrored structures, no
  sources), so the goal field is local.
* "Debug team" 8 draws goal slot 0.

## Unit integration (`GameUnitComponent::update`)

**CONTEXT STEERING over the fields** — no search, and no per-unit path state beyond the last heading.

### Picking the goal direction

* The ENEMY team fields: every other team is sampled at the unit's cell, the geodesically nearest
  enemy wins → `targetPos`, and `goalDir` = its `descentDir`. This skips the spatial-query target
  search. A unit is a source of its own team's field, so unit-vs-unit works the same way.
* No covering field → the old local search, with `goalDir` straight at the target.
* Route waypoints or DSL-locked targets → `goalDir` straight at them.

### Scoring candidates

Each tick, 16 candidate headings ANCHORED ON THE GOAL plus the exact lane direction are scored.

* k = 0 IS `goalDir` exactly. The other 15 carry a per-unit offset under half a slot, so a crowd
  sharing one goal heading does not deviate onto the same sixteen world directions and walk in
  columns.
* Anchoring matters because a one-cell gap subtends about one 22.5° slot from a few metres out, so a
  free-floating fan often had no candidate pointing through it.

```
free·(Goal·dot(goal) + Flow·dot(lane)·w(|lane|) + Persist·dot(lastDir))
     − Pressure·dot(∇p̂)·w(|∇p|)
     + Wall·dot(wallAway)
```

* `w(x) = x/(x+knee)` is a COMPRESSIVE response — "Pressure knee" 0.2, "Flow knee" 0.15 × moveSpeed
  (low, so even a half-faded lane still counts as "there is a lane here"). Steep at low values so ONE
  stuck unit registers, flattening toward 1 so a hundred never saturate a cell.
* `free` = the open fraction of a "Look-ahead" CENTRE-LINE march (a 1-tile gap reads as open).
* `wallAway` = a proximity-weighted wall push. `wallPush` pushes away from blocked cells within body
  radius + "Wall keep" 0.6 m: in a gap both walls cancel and the unit centres itself, and at a corner
  it swings wide.
* A candidate whose cell one body radius ahead is blocked is out, and two LATERAL body samples at
  that point subtract "Corner clip penalty" each — the run is measured on the centre line so a
  one-cell gap stays usable, and clipping a corner is discouraged rather than forbidden.

**The cost window.** Both `free` and `wallAway` read a `TeamField::CostWindow`, the per-tick stack
SNAPSHOT of the raster around the unit. `snapshotCosts` costs a few chunk lookups plus row memcpys,
after which every probe — marches, body/corner samples, wall push — is a plain array read, where the
chunk-hash helpers paid a `find()` per cell, 100+ per unit per tick. Cells beyond the window read as
OPEN, so size the radius to the longest probe.

### Seed paths

`NavSystem::seedPath(team, from, to, speed, width)` plans ONE route with A* over the raster
(`TeamField::findPath` — straight-shot test first, octile A* capped at 8192 cells, string-pulled by
`lineOfSight`), then writes it in TWO places.

**1 — into the team's flow as a lane** (`FlowField::seedPath`)

* Max-magnitude into both buffers, plus an override when the new vector is more than 60° off the
  existing one, so a RE-plan down a different route wins even where a milling crowd's splats sum
  higher than the lane speed. It is visible immediately.
* The written VECTORS blend between the two segments meeting at each junction over 2 m, so a 90°
  corner reads as an arc while the polyline stays put — a rounded path could cut a wall corner. A
  vector that would AIM AT A WALL falls back to the pure segment direction, since at a one-cell gap
  the blend otherwise points at the wall beside the opening and units follow it there.
* "Lane width" metres wide (0 = one cell). Blocked lateral cells hand their share to the surviving
  ones, so a lane squeezing past a wall FOCUSES on the centre instead of thinning (capped 2×).
* Written 1.6× stronger at CONSTRICTIONS — a cell with a blocked lateral neighbour — since a gap has
  no surrounding flow to support it and a crowd waits at its mouth. This is separate from the
  planning CLEARANCE that keeps the route off walls. Blocked cells are skipped.

**2 — as a NEGATIVE-pressure TROUGH along the same route** (`PressureField::seedPath`)

* Depth "Nav/Seed trough", deepened by 1 + 4× "Nav/Seed trough squeeze" ONLY in a strictly ONE-CELL
  gap (both immediate lateral neighbours blocked). Wider gaps, diagonal staircases, and a lane merely
  running ALONGSIDE one wall stay plain. Min semantics, so it is idempotent.
* Pressure is where ATTRACTION lives — the steering reads −∇p and the flow is pushed by −∇p — so the
  lane pulls units and the surrounding flow INTO itself, instead of only existing where it was drawn.

Only the first "Nav/Seed range" metres (40) of a plan are WRITTEN. The A* still runs to the real
destination, since a truncated search would pick the wrong way round an obstacle, but a lane far
ahead of the group is stale by the time anyone gets there.

**Consumers**

1. A fresh RMB move order for the selected units, started at the centroid of their LARGEST CLUSTER
   (single-linkage flood fill at "Group cluster radius" 12 m — a straggler must not drag the lane's
   origin off the bulk). One plan, at the press; the units' own periodic requests keep it fresh
   afterwards.
2. Every barracks route change (`GameMatch::seedRouteLane`, leg by leg).
   (1 and 2 both use "Order lane speed/width" — a player order writes a strong wide lane the group
   commits to.)
3. EVERY unit on its own timer. While it has somewhere to be, a unit queues a
   `SeedRequest{from, to, team, stuck}` every "Seed request interval" (jittered ×0.75–1.25, random
   phase at spawn), drained on the main thread by `NpcSystem::service` into
   `NavSystem::requestSeedPath` at "Order lane speed" — or the weaker "Stuck lane speed" when the
   unit is stalled past "Unstick after".

**`requestSeedPath` is the rate limiter** that makes a per-unit request affordable.

* It refuses a plan when one of the same team was already made within "Nav/Seed area" (16 m) of BOTH
  its start and its destination inside "Nav/Seed cooldown" (4 s), and caps plans at
  "Nav/Seed max/frame" (2).
* The test is a DISTANCE — a grid-bucket test let two units either side of a border both through —
  but the candidates come from a hash of recent plans BUCKETED BY START at exactly that radius, so
  only the 3×3 bucket neighbourhood is scanned. Constant cost per request, no matter how many units
  ask or how many plans are alive.
* Stamps retire through an EXPIRY QUEUE (one FIFO of (time, bucket)); stamps are made in time order
  and buckets are append-only, so `update` pops exactly what expired and never walks the map.

A crowd walking the same way therefore costs ONE plan, and the group's lane keeps up with the group
as it advances without anyone tracking the group — there is no group re-seed timer. Nothing per-unit
plans; the crowd follows the seeded lane, which then decays like any other.

### Stalled units

Position is checkpointed every 0.75 s. Moving less than 0.6 m since the last checkpoint accrues
stalled time, otherwise it resets — displacement, not per-tick progress, because a jittering heading
never accumulates progress.

* **Stalled** shifts the weights (Goal ×0.3, Persist ×0.2, Flow ×2), so the fields become the
  preference when the unit has problems.
* **Past "Unstick after" (1.5 s)** Persist drops to 0, Pressure ×3, and the GOAL is replaced by an
  ESCAPE goal (weight 1.5) that BACKS AWAY from whatever pins the unit: away from a nearby wall first
  (the CostWindow wall push), else away from the nearest unit/structure/player found by a small 4 m
  spatial query (scenery filtered out), else simply the reverse of the intended direction. Plus a
  ±30° per-unit random rotation — forced movement out of corners that also breaks symmetric two-unit
  locks.

### Steering tweaks ("Game/Enemies/Steer", Synced)

| Tweak | Default |
|---|---|
| Goal | 0.7 |
| Flow | 1.5 |
| Persist | 0.8 |
| Pressure | 0.5 |
| Look-ahead | 6 |
| Corner clip penalty | 0.4 |
| Wall push | 1.2 |
| Wall keep | 0.8 m |
| Pressure knee | 0.2 |
| Flow knee | 0.35 |
| Order lane speed | 4 |
| Stuck lane speed | 2 |
| Lane width | 2 m |
| Seed request interval | 5 s |
| Group cluster radius | 12 |
| Presence pressure | 0 |
| Stuck pressure | 0 |
| Unstick after | 1.5 s |
| Order flow blind | 0 s |
| Track goal | 1.5 |
| Track flow mult | 0.15 |

* Flow above Goal is deliberate: a lane is a proven route, so where one exists it outweighs walking
  straight at the goal. Goal below 1 is deliberate too — at 1.0 the goal term overrode the lane term
  at walls.
* **Track goal / Track flow mult** apply within "Target track radius" 10 — geodesically closer than
  that to a live team-field target. The goal weight floors UP and the seeded lane near-mutes, because
  the lane's periodic re-plans lag a moving player while the field descent is ~0.25 s fresh. Between
  track and search radius the unit marches lane-friendly toward the target. Between search and
  "Nav follow radius" 40 it has NO target but WALKS the crowd FLOW lane where one exceeds
  `flowKnee × moveSpeed` — pursuit survives a player sprinting out of range until the lane decays.

The lane is sampled with `FlowField::sample(pos, raster)`, which leaves BLOCKED cells out of the 3×3
average instead of averaging them in as zero. Inside a gap two thirds of the neighbourhood is wall,
which used to cut the lane's apparent strength to a third.

### Contributions back into the fields

**Pressure injections** — "Presence pressure" per unit per tick, and "Stuck pressure" while stalled —
both default to 0. The pressure field now carries the seeded lane TROUGHS, and a crowd's own positive
haze only muddied them; being stuck REQUESTS A PLANNED PATH instead, which is a real answer rather
than a repulsion field. Both tweaks remain for re-enabling.

**Flow splats.** Moving units splat their MEASURED planar velocity into the FlowField ONE CELL BEHIND
them. A unit samples its own cell at double weight, so splatting there fed its heading back to itself
and biased everything toward going straight — a trail belongs behind the walker.

`Nav::FlowField` per team is an atomic int16 EMA: splat × (1−step), write buffer = read × step, where
`step = 0.5^(dt/"Nav/Flow half-life")` (10 s — a seeded order lane is still ~50 % after 10 s). It is
FRAME-RATE INDEPENDENT and configured as a half-life in seconds, not a per-frame factor. Lanes stay
usable ~10–20 s. A BLOCKED cell holds nothing (cleared every step).

**`Nav::PressureField`** — fed by the seeded troughs plus the optional unit injections above — is a
SIGNED scalar field: positive = jams and crowd, negative = seeded lanes.

* It DIFFUSES every frame (`PressureField::update`): one Jacobi step read → write that resolves its 5
  raster chunks plus 4 neighbour chunks ONCE PER CHUNK, after which neighbours are plain array
  indices; a chunk whose own and whose neighbours' cells are all quiet is skipped outright.
* "Nav/Pressure diffusion" (dt-scaled, clamped to 0.25) and a "Nav/Pressure half-life" of 6 s — the
  seeded trough has to outlive the walk it was planned for. Same frame-rate-independent half-life
  form as the flow.
* A "Nav/Pressure floor" (0 = off) sets the level below which a neighbour is too faint to diffuse at
  all. Note that a trough which spreads without paying for it grows until the half-life catches up,
  and a flat field has no gradient to steer by.
* **Walls are reflective**, in the diffusion AND in `gradient(pos, raster)`, which substitutes the
  centre value for a blocked neighbour. A wall cell holds 0, and against a negative seeded trough a
  plain read makes it look like a HIGH-pressure spot that pushes everything away from every wall.
* Chunks grow where pressure reaches a border cell and evict once quiet. There is ONE current buffer
  that units read and inject into.
* The step PUSHES THE FLOW: it hands each active cell's gradient to a callback that splats −∇p·gain
  into the lanes — one traversal, no second pass re-resolving neighbours through the hash — where
  `gain = 10^"Nav/Pressure flow gain 10^x"`. The tweak is the EXPONENT, so one slider spans
  0.01 .. 1000 with fine control at the low end.

### Nav tweaks

Enabled · Field radius 250 · Rebuild interval 0.25 · Clearance cost 1 · Chunk keep frames 120 ·
Flow half-life · Flow max (a per-cell magnitude cap — splats SUM, so a milling crowd could out-shout
a seeded lane) · Seed area / cooldown / max-per-frame / range / trough / trough-squeeze ·
Pressure diffusion / half-life / floor / flow gain (10^x).

**Debug draw** 0..2:

| Value | Draws |
|---|---|
| 0 | off |
| 1 | chunk outlines + descent arrows of "Debug team" within "Debug radius" of the player |
| 2 | crowd FLOW arrows (green→white and longer with strength, scaled against "Nav/Flow max" so a reading means the same thing everywhere; hidden below "Nav/Debug flow min" 0.5 m/s so the haze does not bury the lanes) + PRESSURE columns (jams yellow→red rising, seeded troughs blue dipping; diamond + bar sized by value) of "Debug team" |

"Debug team" 8 = the player's goal field.

## Scope and extras

* Server and single-player only: units gate on authority, and clients feed obstacles only.
* The Nav library ALSO offers per-walker helpers the PLAYER uses: `lineOfSight(a, b, radius)`,
  `steerPoint` string pulling, `avoid` raster whiskers, and `chooseSide`.
