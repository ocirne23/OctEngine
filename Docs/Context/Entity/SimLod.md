# SIM LOD

> Part of the Entity documentation ([`Docs/Context/Entity/CONTEXT.md`](CONTEXT.md)) — read that first.

`SimLodConfig` = `Globals::settings.world.simLod` (`Settings.World`, Code/Settings/Public/World.ixx; the World holds a const reference). "Game/Sim LOD" tweaks — per-process performance tuning, NOT Synced.

**Decided ENTIRELY in World.** The entity only sees its delta.

| Tweak | Default |
|---|---|
| `enabled` | true |
| `horizontal` | **true — XZ distance** (top-down game); off = full 3D |
| `radius[3]` | **25 / 50 / 225 m** |
| `intervalSec[3]` | 0.25 / 1.0 / 0.0 (dormant 0 = never) |
| `minFrames[3]` | 4 / 16 / 8 |
| `intervalJitter` | 0.25 |
| `dormantDisableBody` | true |
| `forceMaxTier` | 1 |
| `buoyancyMaxTier` | 1 (see Buoyancy gate) |
| `visibleMaxTier` | 2 (= distance rules everything) |
| `queryMargin` | 10 m |
| `zoneMargin` / `zoneTier2Band` | 5 m / 25 m (zones, see Focus) |
| `selectionIntervalSec` | 0.05 s of sim time between selection jobs, frame-rate independent (the visible set is fresh every frame regardless) |
| `maxCatchUpSec` | 1.0 s — **SECONDS, never frames**: a cap in frames was frame-rate bound (8 frames at 1000 fps handed a 1 s tick 8 ms, and far units could not accelerate at all) |
| Follows: units / structures / projectiles / scripts / animators | **on / off / off / off / on** |

## Focus

`World::setSimLodFocus(points, count)` — at most `MaxSimLodFocus` 16 — every frame BEFORE
`world.update`. `GameMatch::update` publishes every player capsule (own plus the server's client
twins); main.cpp publishes the camera in the plain testbed.

**Zones.** `World::setSimLodZones(spheres, count)` — at most `MaxSimLodZones` 64, xyz center plus
w radius — are spheres that stamp **tier 1 out to radius + `zoneMargin` and tier 2 over a further
`zoneTier2Band`, never tier 0.** `GameMatch::update` publishes the shield structures' bubble spheres
(the merge group's sphere where merged, from `ForceEmitter::getBubbleBounds`), so a unit that walks
into a far base's field ticks and is pushed with no player near. They stay set until set again (the
Game refreshes them every 0.25 s).

Both are consumed by the selection job (`buildSelectSpheres` at kick): one sphere list with a
query radius and a radius per tier. **The tier stamps are the job's, not the cull job's** — see
the selection section below.

**NO focus and no zone, paused (`dt == 0`), or disabled = LOD INACTIVE**: every root and every child
is visited, and the pass scales with the entity count.

## Active: the visit set

The pass is **DETACHED from the entity count.** It is:

0. **THE VISIBLE SET, every frame** — the cull job's Main frustum pass hands over the handles of
   every visible entity (`SpatialIndex::setVisibleCollect(SpatialLayer_Entity)`, no second
   traversal); `update()` walks each live one to its root (`selectUpdateRoot`, ancestors stamped).
   Dedupe is by STAMP, no sort: the `VisibleRoot` pass (a new generation every pass) records a
   root once among the visible walk, and a root whose `UpdateRoot` stamp is current is in the
   periodic result already and is skipped, so a root is visited once. **This is the frame-sensitive
   part**: what the player sees is at full rate no
   matter how stale the periodic selection is. A visible entity the periodic job has not stamped
   yet takes its tier from the direct distance (see Tiers).
1. The **`Global` roots** (`m_globalRoots`).
2. **The PENDING roots** (`m_pendingRoots`, `PendingRoot` = entity + the pass it was added in),
   visited unconditionally every pass until a selection job that could have found them has run —
   a fresh entry links at the next commit at the latest, so the job kicked at the end of a LATER
   pass finds it. A fresh result retires every pending root added before its kick pass, plus any
   it contains anyway (its `UpdateRoot` stamp is current). A pending root the visible walk queued
   this pass (`VisibleRoot` current) is skipped too.
3. **The selection job's roots**: one `queryUpdateTiers` per focus point at `radius[2] +
   queryMargin` — **and one per zone** at its radius + `zoneMargin` + `zoneTier2Band` +
   `queryMargin` — on the Entity layer, **at ANY depth**: `selectUpdateRoot` walks each hit up to
   its root, **stamping every ancestor so the descent passes through them**, and records the root
   if its atomic `stampCurrentOnce(UpdateRoot)` wins — overlapping balls reach one root from
   several jobs, exactly one records it. No sort, no merge: the per-sphere lists concatenate.

**Descent.** `submitEntityBatches` copies only SELECTED children into the arena (`simLodSelected`:
any UpdateTier or Main stamp on the child's OWN entry; never-stamped fresh entries count as stamped).

> So a visited organisational root at the origin **neither hides its far-flung children nor drags all
> of them in** — selection is per entity, and the parent is only the transform carrier.

Parent-before-child order is untouched: children are still emitted by their parent's batch.

## Tiers

`simLodTiers` returns `{ dist, tick }`.

**`dist` is the DISTANCE tier** — from the entity's own UpdateTier stamps once the periodic job has
placed it (`SpatialIndex::hasStamp`; an old generation = the last job saw it outside every ball =
3), else from `simLodDistanceTier`, the same tier by direct distance to the focus points and zones
(a fresh entity: unlinked on its spawn frame, or linked since the last job — the link gives the
tier passes the `SpatialStamp_Linked` sentinel, never a current generation). **The bubble gate
and the dormant edge go by `dist` alone.**

> The top-down camera sees the whole tier-1/2 area, so visibility must never override distance there.
> It did: every on-screen unit read tier 0 and kept its bubble.

**`tick` is `dist` floored** for an in-view entity (Main stamp) inside the outer radius by
`visibleMaxTier`:

| tick tier | Cadence |
|---|---|
| 0 | Every frame. |
| 1 / 2 | TIME-based: a tick every `intervalSec[t]`, but never closer than `minFrames[t]` × this frame's delta — **the frame gap is the floor for low frame rates**, and at a high frame rate the interval rules. |
| 3 (dormant) | The margin band. `intervalSec[2]` 0 = never; the entity AND its subtree are skipped entirely. |

**The per-entity clock** (Entity must stay ≤ 64 bytes): `schedTick` is the sim time of the last
tick in **1/64 s, 14 bits, wrapping every 256 s** — the two bytes the header had left. Elapsed is the
wrap-safe difference, and since both ends sit on the same grid the deltas of consecutive ticks sum to
exactly the grid time. **It replaced a pass counter against a 256-entry time ring**, which could
measure at most 255 passes back: at 1000 fps that is 0.25 s, so a tier-2 tick could never see its
1 s and never fired, and tier-1 ticks with a jittered threshold above 0.25 s never fired either.

`intervalJitter` is a per-entity ± factor **so a wave that entered a tier together drifts apart**
instead of ticking in lockstep.

**The far tick (`NpcSystem::service`) must therefore key on the TIER stamps, never on Main:** a
visible unit beyond the outer radius is walked from the visible set but its cadence is dormant, so
nothing ticks it; the far tick used to skip on Main too and froze every far unit the camera could
see (waves arrived in blobs released as they scrolled out of view). Since the camera is top-down,
**a visible entity beyond the outer radius is accepted as not
selected** — there is no frustum query. (The user's call.)

## Per-entity kinds

A **following** kind makes the entity a candidate for throttling. A **NON-following** sim kind
(`m_simLodPinMask`) **PINS the entity to full rate WHILE SELECTED.**

> Pinning does not make a far structure selected: a barracks beyond the outer radius is not visited at
> all. **Author `Global true` on prefabs that must always run.**

## Unstamped visits

A fresh entry carries no tier generation: the spawn-guard 0 while unlinked (so `getPassMask` says
"every pass" — that is what lets a fresh CHILD pass `simLodSelected` on its spawn frame), then the
link-time `SpatialStamp_Linked` sentinel until the periodic job stamps it. `simLodTiers` treats both
as "not placed" and takes the tier from the distance, so a fresh entity is scheduled correctly from
its first visit — dormant if far, full rate if near — with no full-rate grace frames. `simLodSelected`
additionally selects a linked-never-stamped child of a NON-Global parent (its tier comes from the
distance too); a Global root's children need a real stamp, or every rock under the terrain root
would be visited every frame.

## Force bubbles

**Bubbles spawn DARK** (`ForceComponent::spawn` ends with `setActive(false)`), and `simLodDelta`
is the ONE place that switches them — so a bubble never holds a GPU slot before its entity has a
tier, and a far spawn stays dark for as long as it stays far (it is never visited):

* **Outside the LOD** — inactive, Global (every building), no entry — the visit switches it ON.
* **Selected**: `setActive(dist <= forceMaxTier)` by DISTANCE TIER on EVERY visited entity with a
  ForceComponent, throttled or not. Refreshed every visit, so a tweak change applies at once. A
  fresh entity's tier comes from the direct distance (see Tiers), so a shot next to the player has
  its field from its first visit.
* One that leaves the selection keeps its last state, and **the query-margin visit (no stamp = tier 3)
  switches it off on the way out.**

> The link-time spawn-guard stamp covers the RENDER passes only. It used to cover the tiers too,
> which read as "tier 0" until the next selection once the tiers went periodic — every far spawn
> got its bubble switched on by its pending visits and was never visited again to switch it off
> (a 7.6k-emitter map with every bubble active).

**This gate is what keeps the force system inside its GPU budget.** A gated-off emitter HANDS ITS
RENDERER SLOT BACK (`MAX_FORCE_EMITTERS` 8192, versus `MAX_FORCE_INSTANCES` 32768 CPU instances), so
a 25k-unit map only pays GPU slots for the bubbles near a player. See
[`Docs/Context/Force/CONTEXT.md`](../Force/CONTEXT.md).

## Buoyancy gate

The same shape as the bubble gate, for a DYNAMIC `PhysicsComponent` body: `simLodDelta` writes
`PhysicsComponent::buoyant = dist <= buoyancyMaxTier` on every visited entity with one (always
`true` outside the LOD). The component runs its own probe math on the pass while the flag is set
(see Buoyancy per component in [`Docs/Context/Physics/CONTEXT.md`](../Physics/CONTEXT.md)), so the cost
follows the near tiers: a body spawns with the flag off, a never-visited far spawn stays off, and
the query-margin visit clears it on the way out. A static body never pays the tier lookup.

## The delta

`updateSelf` gets:

| Value | Meaning |
|---|---|
| the frame delta | full rate |
| the accumulated catch-up | a throttled entity's tick frame, capped at `maxCatchUpSec` (never below one frame delta) |
| **0** | **skipped frame: no sim step.** Game components, script and animator are not called; Network correction, the Physics pose read and the placement tail (compose / render / spatial / audio / particle / force / light / children) still run. |
| **< 0** | DORMANT: do not visit the entity or its subtree at all. |

`simLodDelta` is three pieces: `simLodTiers`, `simLodTransition` (the `schedTier` edges) and
`simLodCadence` (the time-based tick).

## Dormant and wake edges

Any throttled entity with a PhysicsComponent. Both ride the body-command queue, **so the pass never
writes box3d.**

* **DORMANT EDGE** — `PhysicsComponent::park(disable)`: zero the linear and angular velocity, then
  either `SetEnabled 0` (`b3Body_Disable` — out of the broadphase AND solver, pose kept, **nothing can
  wake or push it**) or merely `SetAwake 0` (asleep; a contact wakes it).
* **WAKE EDGE** — `PhysicsComponent::unpark()`: zero the velocities (**a body parked inside a crowd may
  still hold a contact push-out**) and queue `SetEnabled 1` unconditionally — a no-op on an enabled
  body, so a tweak flipped mid-dormancy never strands a disabled body. **Skipped while `suspended`**:
  an Enabled-off subtree owns its own disable.
* **A FRESH entity starts at `schedTier` 3, so its first visit inside a tier IS a wake edge** —
  the hook a spawner uses to park a body at spawn and have it enabled once a player is near.
* The query margin exists so a unit LEAVING the outer tier is still visited once with no stamp and
  takes the dormant edge.

Unselected units with orders are moved by the Game's FAR TICK instead
(`GameUnitComponent::updateFar` from `NpcSystem::service`, before the pass);
`World::simLodActive()` gates it.

## Known gaps

* An unselected unit never runs its death check; damage banked into it settles when it is selected
  again.
* With the disable OFF, a parked body woken by a contact moves without its entity following —
  disabled bodies cannot.
* Unselected entities are not rendered (no shadow or GI either).
* A networked client entity outside the selection runs no correction:
  `NetworkManager::handleSnapshot` applies its snapshot pose directly instead, so it re-enters the
  selection where the server has it. `World::simLodSelected` is public for exactly that.

"Game/Sim LOD/Stats" shows live per-tier counts (per-worker staging counters summed after the join);
**The selection is a FIRE-AND-FORGET job** (`computeSelection`, `"Update selection query"`),
submitted by `update()` once `selectionIntervalSec` of sim time has passed since the last kick — and
not on a physics-step frame that a step-free frame follows (`JobSystem::deferFromPhysicsFrame`:
the workers carry the solver tasks or this job in a frame, never both) — the moment the pass's
wait returns — so
it has the whole rest of the frame, not just the present window — and joined by main in
`World::joinSelection` right before the next frame's spatial kick (the commit inside that kick would
mutate the index under a running query; normally a no-op). **The batches therefore kick without
waiting on any query.** Nothing destroys entities between the pass and that join (the destroy
windows sit after the frame's joins), so the root walk is safe, and registrations take the index's
exclusive lock against the query. The job runs between commits — it sees positions one commit older
than an inline query would; the query margin has to cover `selectionIntervalSec` of motion, and
roots spawned meanwhile arrive through the pending list.

**The job owns the tier stamps.** It opens a new `UpdateTier` generation
(`SpatialIndex::advanceUpdateTiers`) and then, sphere by sphere, runs ONE PARALLEL traversal
(`queryUpdateTiers` rides the index's frontier fan-out: it stamps each hit's tiers by distance band
and returns the hits as a list per traversal chunk) followed by a parallelFor over those chunks
(`"Update selection roots"`, a roots slot per chunk) whose root walk stamps the ancestors with the
same generation. Sequential spheres because the index has one fan-out scratch; each is parallel
inside, so one player's ball no longer costs a single worker milliseconds.
Nothing else stamps the tiers, so the stamps stay current across the passes that reuse the result —
the cull job stamps Main and Near only, nothing on the frame-critical path. Readers that run before
the join (the far tick, a client's snapshot apply) may see a stamp mid-transition; a torn decision
there is one coarse tick, never a crash (aligned 32-bit stores).

Its `SelectResult` carries SUBMIT-READY nodes (deduped by the `UpdateRoot` stamp, Global roots
skipped, order free) WITH their spatial handles, and is REUSED until the next job replaces it. What
`update()` does on main (`"Update selection"`), touching NO entity — only the contiguous handle
array and the two short lists: retire the pending roots a fresh result covers, swap-remove roots
whose handle is no longer alive (`SpatialIndex::isAlive` — a slot reuse fails its generation),
open the `VisibleRoot` generation and gather the Global roots plus the pending roots the result
does not hold (a pending root is claimed with `stampCurrentOnce` so the visible walk skips it).
The first LOD frame has no result and computes inline.

**The per-root work runs on ROOT SLICE JOBS** (`"Update batch submit"` only kicks them, so the
first batches start within microseconds and the workers are not idle while main walks lists):
`submitRootSlices` hands the result's nodes (and, LOD off, every root) to the workers in slices of
`rootSliceSize` (256), each running `submitEntityBatches` — the selection filter, the cost
partition, the arena copy and the batch submits — on its range; `submitVisibleRootSlices` slices
the cull job's `visibleHandles()` the same way, each slice walking its handles to their roots
into its own scratch slot (`VisibleRootSlice`, owner-sliced) before submitting them. The sources
stay disjoint by the stamps (`UpdateRoot` for the result, `VisibleRoot` for the walk and the
pending claim), so slices never need to see each other.
