# Entity

> Library documentation for `Code/Entity`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The ECS, and the widest library in the engine: it links RendererVK, File, Script, Physics, Audio,
Particle, Force, Spatial, Threading, Network and Nav.

## The three invariants everything else follows from

1. **`sizeof(Entity) <= 64`** — one cache line, static_asserted. Components append at
   `alignUp(sizeof(Entity), 16)` = 64. This is why the name lives outside the entity and why the
   World's scheduling state is two bytes.
2. **An entity and its components live in ONE allocation**, and a spawned prefab TREE is one
   allocator block.
3. **The entity only ever sees the delta it is handed.** All scheduling — SIM LOD tiers, catch-up,
   dormancy — is decided in World; `updateSelf` just gets a float.

## `Entity`

[EntityP.ixx:70](Private/EntityP.ixx#L70). Fields: `pos` / `scale` / `rot`, `parent`,
`spawnTemplate`, `spatialEntry`, `refCount`, `typeBits`, `flags`, `updateCost`, `schedTier`,
`schedSkipped`.

### Names

**There is NO name field.** `getName` / `setName` / `hasName` forward to `Globals::entityNames`
(`EntityNameRegistry`), a pointer-keyed 64-shard mutexed map to an INTERNED name
(`Profiler::internName` — permanent and deduped).

So the same pointer names the entity's opt-in `ProfileScope` in the parallel pass **and stays valid
in the profiler ring after the entity dies**. `Entity::destroy` erases the entry; `set`/`erase` are
parallel-spawn/destroy safe, and `get` is called from the entity pass.

> The 8 bytes freed by moving the name out now hold the `SpatialEntry` handle.

### Flags (`EEntityFlags`)

| Flag | Meaning |
|---|---|
| `PrefabInstance` | Root of a locked prefab instance; cleared by "unpack". |
| `Enabled` | Off prunes the subtree from update and suspends its physics bodies. Serialized as entity-level `Enabled false`. |
| `Frozen` | Scripts, animator and physics do not tick. **A SUBTREE state** written down the whole tree by `setFrozen`; `Entity::create` inherits it from the spawn parent and `reparentEntity` adopts the new parent's. Every check is a plain self `isFrozen()`, including entry points outside update (global script events, physics contacts). **Script `OnSpawn` runs even frozen.** What the Entity Editor sets on an open document. |
| `RootAllocation` / `ContiguousAllocation` | See allocation below. |
| `PhysicsSuspended` | Latch so the disable walk is not repeated every frame. **`reparentEntity` clears it up the new ancestor chain** — otherwise a body joining a disabled subtree stays live and collides invisibly. |
| `Profiled` | Opt-in per-entity `ProfileScope`, one-way latch. |
| `Global` | **Root only, never inherited.** The World visits it every frame from its own list instead of finding it through the spatial index — for organisational roots whose children spread across the world (the co-op terrain root). It still registers a spatial entry; **its children are selected individually like everything else.** |

Per-component `enabled` bools are independent of all of these.

### Components

`typeBits` masks 13 types in this fixed order:

```
Scene(0) Render(1) Animator(2) Physics(3) Audio(4) Particle(5) Force(6) Light(7)
Network(8) GameUnit(9) GameStructure(10) GameProjectile(11) Script(12)
```

**Script is LAST so every other component is available when it spawns.**
`getComponent<T>` / `hasComponent<T>` compute byte offsets from compile-time sizes;
`MaxInlineComponentTypes` is 13 and `ComponentAlignment` 16.

Each type is a partition under `Private/Components/` — struct plus `get*SpawnInfo` /
`write*SpawnInfo` in the `.ixx`, bodies in the same-named `.cpp`. `Component.ixx` re-exports them all
and holds the shared machinery.

> Per-component `.ixx`s must NOT import `:Component` (cycle). Cross-component code goes in the `.cpp`
> implementation units.

#### Adding a component type

1. New `.ixx`/`.cpp` pair with a `getId()` struct.
2. `export import` from Component.ixx.
3. Entries in `EComponentID`, `inlineSizes`, `componentTypeName`.
4. Create/destroy switches in Entity.cpp.
5. Parse branch in `World::buildTemplate`.
6. Write branch in Prefab.cpp.
7. **Require-slot fill branch in `ScriptComponent::syncScriptData`** — the host reserves one pointer
   slot per required component at the FRONT of ScriptData in ascending bit order, so **a missing
   branch shifts every later slot. This crashed once.** See
   [`Code/Script/CONTEXT.md`](../Script/CONTEXT.md).
8. Entity.natvis offset entry.
9. Bump `MaxInlineComponentTypes`.
10. An Entity Editor section, if authorable.

## Allocation

`EntityPtr` is an intrusive refcounted handle. The count is a plain `uint16` field accessed through
`oc::atomic_ref`, which is what keeps `Entity` inside its cache line.

`Entity::create(template, transform, initialFlags)` placement-news into `Globals::entityAllocator` —
lock-free: an atomic chunk bump plus tagged per-size-class Treiber free lists, blocks ≤ 64 KB.
`initialFlags` applies **before components spawn**, so e.g. Frozen is already visible to a
component's `spawn()`.

The tree is `SceneComponent::children` plus `Entity::parent`; `reparentEntity` moves ownership.
`parent` is linked at construction, **before components spawn, so spawn-time logic never sees a
contiguous member with a null parent.**

### Contiguous tree allocations

A tree spawn is ONE allocator block, sized from the template's cached `treeAllocSize` and carved by
the spawn recursion through a `treeCursor`.

* While intact (`RootAllocation` + `ContiguousAllocation`) the root frees the whole block in one
  call, after a refcount pre-walk (`contiguousTreeSolelyOwned`), and members skip their own free.
* **A structural break SPLITS the allocation** (`breakContiguousAllocation`): path ancestors revert to
  per-entity freeing, and every off-path subtree — plus the departing member — is promoted to its own
  `RootAllocation` over its contiguous DFS range and keeps one-chunk freeing.
* `breakContiguousAllocationFromRoot` is the same split entered at the root, for `Entity::destroy`'s
  not-solely-owned fallback.
* Each entity otherwise frees its own exact-size slice, so **tree members are independent**: any of
  them can outlive the others or be reparented away.
* Moving within the same allocation, or grafting a whole allocation root, never breaks anything.

## The per-entity visit

`Entity::update` is a thin serial recursive wrapper around **`Entity::updateSelf`, the ONE hot
per-entity visit**:

```
Script → Network → Animator → Physics
  → compose world transform → refresh the SpatialIndex entry
  → push the RenderNode (gated on getPassMask when Spatial culling mode >= Cull)
```

`updateSelf` emits its children into a caller-supplied vector rather than recursing, which is what
lets the parallel pass slice them.

## Spatial entries

**EVERY entity registers at the end of `Entity::create`** ([EntityP.cpp:261](Private/EntityP.cpp#L261))
— parallel-spawn safe, since the index locks.

* Layer `SpatialLayer_Entity` always, plus `SpatialLayer_Render` when it has a render node.
* Bounds come from `RenderNode::getWorldBounds`, skinned inflated by the culling config's
  `skinnedRadiusScale`; otherwise a point at the spawn position. **For a tree CHILD that is its LOCAL
  position** — the entry links at the next commit, and the child's first visit re-places it in world
  space before any query can see it.
* Headless has no render nodes, so every entry there is a point.
* Gameplay queries use the Render layer; **the Entity layer is the World's update-selection layer.**
* Headless still registers and main.cpp calls `commitFrame()` in place of the cull job, so radius
  queries work server-side.

## The parallel update pass

`World::update(renderer, dt)` ([World.cpp:222](Private/World.cpp#L222)). Always on.

### Continuation batches — no level barrier

A batch job processes a node range from the frame arena, then **immediately slices the children it
emitted into new batch jobs** on ONE pass-wide `JobCounter` that main waits on (helping).

**The only real dependency is parent-before-CHILD**: a child reads its parent solely through the
`parentWorld` baked into its `EntityUpdateNode`, and the child's batch is submitted only after its
parent's batch body ran. Subtrees therefore descend independently.

> The old breadth-first level walk stalled every depth on the slowest batch of the previous one at a
> "Level merge" barrier.

Worker-submitted continuations land on that worker's LIFO deque, so a deep subtree descends
cache-warm on one core while wide fan-outs get stolen. Batches submit at **`EJobPriority::High`**
under the name `"Entity Update"` — the pass is the frame's critical path, every batch is bounded, and
High is what the window-thread helper serves between pumps.

### Cost budgeting

Every entity carries `updateCost`, a **u8 in 250 ns units, MEASURED**: 0 = unmeasured, the entity's
FIRST update is timed and becomes the cost — **no guessed initial value, the first update IS the
guess** — then a low-chance random re-measure keeps it honest. Unmeasured counts as 1 so it still
partitions.

A batch fills until the summed cost reaches ~25 µs (`m_updateBudget`, clamped to 1..4096).
`m_updateCost`'s EMA is fed COST UNITS as its item count, **so `nsPerItem()` self-calibrates to "ns
per cost unit".** There is no even-split term any more: **parallelism comes from the fan-out itself.**

### The frame arena

`m_updateArena` is claimed with an atomic bump that is **NEVER rolled back**, is pointer-stable during
the pass, and is resized **only between passes** (jobs hold pointers into it) from last frame's use
plus overflow, doubled.

A claim past the end runs those subtrees serially through the recursive `Entity::update` — correct,
just not parallel — and the overflow grows the arena next frame, so it is a one-frame hiccup.

`updateSelf` emits children into `PerWorker` staging, and `submitEntityBatches` copies them into the
arena in **ONE claim BEFORE the first submit.**

> **Load-bearing, not an optimization:** a submit can execute its job INLINE on queue exhaustion, and
> an inline child batch reuses the caller's staging slot — so nothing may read the staging pointer
> once the first submit goes out.

**Neither the batch job nor `updateSelf` ever fiber-waits**, which is what makes one `PerWorker`
staging slot per worker sufficient.

### What is safe inline

A parent always finishes before its children, and all other writes land in the entity's own
components:

* scripts — DSL cross-entity access is parent → child writes and child → parent reads; **world radius
  queries are the known open hole**
* animator plus `setSkinningPalette` (disjoint regions)
* dynamic-body pose reads
* `RenderNode::setTransform` / `renderNode`
* `SpatialIndex::updateEntry`
* audio follow, particle and force emitter slots

**The pass NEVER writes to box3d.** The gizmo entity uses the serial path.

---

# SIM LOD

`SimLodConfig` in [World.ixx:44](Private/World.ixx#L44). "Game/Sim LOD" tweaks — **NOT Saved**, so the
code defaults rule every run and a stale tweaks.cfg never overrides a tuning change.

**Decided ENTIRELY in World.** The entity only sees its delta.

| Tweak | Default |
|---|---|
| `enabled` | true |
| `horizontal` | **true — XZ distance** (top-down game); off = full 3D |
| `radius[3]` | **25 / 50 / 100 m** |
| `intervalSec[3]` | 0.25 / 1.0 / 0.0 (dormant 0 = never) |
| `minFrames[3]` | 4 / 16 / 8 |
| `intervalJitter` | 0.25 |
| `dormantDisableBody` | true |
| `forceMaxTier` | 1 |
| `visibleMaxTier` | 2 (= distance rules everything) |
| `queryMargin` | 10 m |
| `maxCatchUp` | 8 frames |
| Follows: units / structures / projectiles / scripts / animators | **on / off / off / off / on** |

## Focus

`World::setSimLodFocus(points, count)` — at most `MaxSimLodFocus` 16 — every frame BEFORE
`world.update`. `GameMatch::update` publishes every player capsule (own plus the server's client
twins); main.cpp publishes the camera in the plain testbed.

It forwards to `SpatialIndex::setUpdateLod`, which stamps the three `UpdateTier` passes in the NEXT
cull job. **One frame of latency vs the focus.**

**NO focus, paused (`dt == 0`), or disabled = LOD INACTIVE**: every root and every child is visited,
and the pass scales with the entity count.

## Active: the visit set

The pass is **DETACHED from the entity count.** It is:

1. The **`Global` roots** (`m_globalRoots`).
2. **Roots added since the last pass** (`m_pendingRoots`), visited ONCE unconditionally — a fresh
   entry links only at the next commit, so no query can find it on its spawn frame.
3. **One `querySphere` per focus point** at `radius[2] + queryMargin` on the Entity layer, **at ANY
   depth**: `selectUpdateRoot` walks each hit up to its root, **stamping every ancestor so the descent
   passes through them**, and queues the root. Sort + unique dedupes overlapping balls.

**Descent.** `submitEntityBatches` copies only SELECTED children into the arena (`simLodSelected`:
any UpdateTier or Main stamp on the child's OWN entry; never-stamped fresh entries count as stamped).

> So a visited organisational root at the origin **neither hides its far-flung children nor drags all
> of them in** — selection is per entity, and the parent is only the transform carrier.

Parent-before-child order is untouched: children are still emitted by their parent's batch.

## Tiers

`simLodTiers` returns `{ dist, tick, placed }`.

**`dist` is the DISTANCE tier** from the entity's own UpdateTier stamps (none = 3). **The bubble gate
and the dormant edge go by `dist` alone.**

> The top-down camera sees the whole tier-1/2 area, so visibility must never override distance there.
> It did: every on-screen unit read tier 0 and kept its bubble.

**`tick` is `dist` floored** for an in-view entity (Main stamp) inside the outer radius by
`visibleMaxTier`:

| tick tier | Cadence |
|---|---|
| 0 | Every frame. |
| 1 / 2 | TIME-based: a tick every `intervalSec[t]`, but never closer than `minFrames[t]` — **the frame gap is the floor for low frame rates.** |
| 3 (dormant) | The margin band. `intervalSec[2]` 0 = never; the entity AND its subtree are skipped entirely. |

**Exact elapsed time without a per-entity clock** (Entity must stay ≤ 64 bytes):
`World::m_frameTimeRing` holds the cumulative sim time at the end of each of the last **256** passes,
so elapsed = now − ring[pass of the last tick], found from `schedSkipped` alone (saturating at 254).

`intervalJitter` is a per-entity ± factor **so a wave that entered a tier together drifts apart**
instead of ticking in lockstep.

Since the camera is top-down, **a visible entity beyond the outer radius is accepted as not
selected** — there is no frustum query. (The user's call.)

## Per-entity kinds

A **following** kind makes the entity a candidate for throttling. A **NON-following** sim kind
(`m_simLodPinMask`) **PINS the entity to full rate WHILE SELECTED.**

> Pinning does not make a far structure selected: a barracks beyond the outer radius is not visited at
> all. **Author `Global true` on prefabs that must always run.**

## Unstamped visits

The spawn-frame visit from the pending list sees only the spawn GUARD — the entry is unlinked, so
`getPassMask` says "every pass". `simLodDelta` therefore checks the stamps **EXACTLY**
(`getPassMaskExact`): no real tier stamp = tier unknown = full-rate visit, nothing decided.

## Force bubbles

Bubbles spawn ON. The World only ever gates them **BY DISTANCE TIER on a placed, selected entity**:
`ForceComponent::setActive(dist <= forceMaxTier)` on EVERY selected entity with a ForceComponent,
throttled or not — so a structure's emitter beyond tier 1 projects no field either, and the tier is
read for such entities even though their tick rate stays full. Refreshed every visit, so a tweak
change applies at once.

* A spawner that places a unit far from every player parks its bubble along with its body
  (`NpcSystem::spawnLooseUnits`) — it is never visited, so nothing would gate it.
* One that leaves the selection keeps its last state, and **the query-margin visit (no stamp = tier 3)
  switches it off on the way out.**
* Where the LOD does not apply — inactive, Global, no entry — the World never touches a bubble.

## The delta

`updateSelf` gets:

| Value | Meaning |
|---|---|
| the frame delta | full rate |
| the accumulated catch-up | a throttled entity's tick frame, capped at `maxCatchUp` × the frame delta |
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
* **A FRESH entity starts at `schedTier` 3 (unplaced), so its first stamped visit IS a wake edge** —
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
**The selection QUERY is a FIRE-AND-FORGET job** (`computeSelection`, `"Update selection query"`),
submitted by `update()` for the NEXT pass the moment this pass's wait returns — so it has the whole
rest of the frame, not just the present window — and joined by main in `World::joinSelection` right
before the next frame's spatial kick (the commit inside that kick would mutate the index under a
running query; normally a no-op). **The batches therefore kick without waiting on any query.**
Nothing destroys entities between the pass and that join (the destroy windows sit after the frame's
joins), so the root walk is safe, and registrations take the index's exclusive lock against the
query. The job runs between commits — it sees positions one commit older than an inline query
would; the query margin covers a frame of motion, and roots spawned meanwhile arrive through the
pending list. Its `SelectResult` carries SUBMIT-READY nodes (deduped,
Global roots skipped) WITH their spatial handles and the ancestor entries between each hit and its
root. What `update()` still does on main (`"Update selection"`), all O(roots) and **no sort, no
copy**: stamp those ancestors with the CURRENT generation (the cull re-stamps the tiers every
frame, so a stamp made inside the job would be stale), swap-remove roots whose handle is no longer
alive (`SpatialIndex::isAlive` — a slot reuse fails its generation), swap the job's list in as the
level and append the Global and pending roots — **dedupe-free because the job skips Global roots and
a pending root's entry is unlinked until the commit after the job ran.** The first LOD frame has no
result and computes inline.

---

# Parallel entity spawning and destruction

`Entity::create` / `Entity::destroy` are callable from worker jobs **CONCURRENTLY WITH EACH OTHER**,
in the same frame window where main-thread spawning is legal today — the entity-change drains and
`game.update`, after the spatial/begin-frame joins. **Never during the parallel entity pass, the
physics step, or present.**

## `World::spawnBatch(span<SpawnRequest>, addRoots = true)`

The sanctioned entry. **Templates resolve on MAIN** — the template, container, clip and audio caches
are not job-safe — then a cost-grained parallelFor runs only the `Entity::create` calls, and roots
attach serially after the join.

A request name **with an extension** resolves like `spawnAssetFile` with
`overrideDefaultTransform = true` (position replaced, rotation composed onto the authored default,
authored scale kept); a plain name resolves like `spawn()`. Results stay index-aligned with the
requests, with a null entry for an unknown prefab.

## `World::releaseBatch(vector<EntityPtr>&&)`

Fans the releases — and so the `Entity::destroy` of every last reference — over a parallelFor.

**A handle that is not the entity's LAST reference just decrements**, so callers drop every OTHER
owner they mean to (the root list through `removeRootEntity`, so its roster callback stays serial;
then rosters) **BEFORE this, on main** — only the teardown itself runs on workers.

Users: the EntityChange Delete drain in `handleEntityChanges` (which moves the Delete handles out
after the serial pass, so the teardown is one parallel batch) and `NpcSystem::clear`.

## What makes it safe, per resource

Each seam locks ONLY create/destroy — **the parallel-pass hot paths stay lock-free.**

| Resource | Protection |
|---|---|
| EntityAllocator, `treeAllocSize` / `treeNetworkCount` caches | Already lock-free / benign-race |
| Spatial `registerEntry` / `unregisterEntry` | `m_registerMutex` EXCLUSIVE (pool growth reallocates the SoA); `query*` take it SHARED, since a spawn job's script `OnSpawn` may query while another worker registers. The `markVisible*` traversals stay lock-free — the cull kick/join window forbids registration. |
| `OcclusionBuffer` add/remove | Mutex (PhysicsComponent spawn and resume) |
| box3d body create/destroy | The Physics-module `g_bodyLifecycleMutex` |
| Renderer slot/registry allocators — transform slots, mesh/material/instance-offset registries, LOD state, skinned bundles/palettes/job ranges, solid-colour materials, force and particle slots | ONE RECURSIVE `Renderer::m_spawnMutex`, held whole-body by `spawnNodeForIdx` / `spawnSkinnedNode` |
| `MeshDataManager` range alloc/free, `TextureManager` upload/free | Mutex each |
| ForceSystem create/destroy | Mutex, plus `m_emitters` / `m_queries` RESERVED to the renderer caps at initialize so growth never reallocates under a concurrent handle resolve |
| ParticleSystem effect create/destroy/cache | Mutex held only for the map and vector work — the `.pfx` load, texture loads and renderer slot creation run outside it |
| `ScriptHost::getOrLoad` | Mutex (cacheKey canonicalization outside) |
| ScriptEventManager listeners | A plain mutex; `fireEvent` SNAPSHOTS the dispatch list under it and invokes the scripts after releasing |
| NetworkManager registration | A recursive `m_registerMutex`. See below. |
| AudioSystem source list | `m_sourceMutex` over create/release/detach; the miniaudio teardown runs outside it |

**NetworkManager detail.** A replicated tree's netIds must stay CONTIGUOUS, since clients adopt
`base + cursor` in DFS order. So a SERVER tree spawn whose template carries **more than one**
NetworkComponent (the lazy `treeNetworkCount` cache) holds that mutex across the WHOLE tree through
`begin`/`endTreeRegistration` in `Entity::create`'s root overload; `registerEntity` re-locks
recursively from inside. **Single-component trees — every unit and projectile prefab — mint
atomically and spawn fully parallel even on the server.** Lock order is `m_registerMutex` BEFORE
`m_entityMutex`.

## Event safety

`Entity::destroy` FIRST calls `ScriptComponent::detachListener`, whose unregister **WAITS for
`ScriptEventManager::m_dispatching`** — `fireEvent`'s in-flight snapshot count — to drain before any
component is torn down.

A script `OnDestroy` firing a global event on one worker can therefore never reach a sibling another
worker is mid-teardown. **The wait is only ever taken on the destroy path**, never from inside a
dispatch: destroys are deferred requests, and `syncScriptDataLive`'s re-registration passes
`waitForDispatches = false`.

---

# `World` (`Globals::world`)

## Spawning and roots

* `spawn(name, transform)`, `spawnAssetFile(path, transform, overrideDefaultTransform = true)`,
  `createEmptyEntity(name)` — **archetype 0, NO components, so it cannot hold children.** A grouping
  root needs a `Component Scene` prefab, e.g. `Entities/Game/terrainroot.pre`.
* `addRootEntity` also queues the root for ONE unconditional visit and, if Global, adds it to the
  always-visited list.
* **`removeRootEntity` notifies `m_onRootEntityRemoved` FIRST, while the entity is still alive.** The
  Game layer's rosters deregister through it, **so EVERY removal path — editor delete, script destroy
  request, network despawn — reaches them without any world-wide query.** The callback must not call
  `removeRootEntity` itself (reentrant `erase_if`); a remover that already deregistered just sees a
  no-op.

## Caches

Templates (`EntitySpawnTemplate` = archetype plus per-component `SpawnInfo` recipes), ObjectContainers,
`CollisionCache`, retargeted clip sets keyed by skeleton + animator name, and audio buffers keyed by
path — **each source file imported once.** A failed audio load is cached as an invalid buffer so a bad
path does not retry and re-log every spawn.

`reloadPrefabs` / `invalidatePrefab` for editing; retired templates stay alive for live entities
(`m_retiredTemplates`, plus `keepTemplateAlive` for ad-hoc ones). `m_buildingTemplates` is the
prefab-recursion cycle guard.

**`findLoadedContainer` is lookup-only and never imports**, which is what makes it safe from the
off-main UI pass — an import creates renderer resources.

## `EntityChange`

A variant queue applied by `handleEntityChange(change, camera, viewportRect)`:

`CreateHierarchy` · `CreateViewport` · `AddSceneEntity` · `SpawnAtPosition` · `Delete` · `Reparent` ·
`SavePrefab` · `OpenPrefabForEdit` · `NewPrefab` · `RespawnEntity` · `SetEnabled`

* **`SpawnAtPosition`** is the script spawn (`ctx->spawnEntity`), deferred rather than spawned in the
  thunk: spawning touches the World caches, box3d and the renderer, **none of which tolerate the
  parallel entity pass running around them.**
* **`SetEnabled`** rides the queue rather than calling `Entity::setEnabled` directly, because the
  widget pass runs OFF the main thread and `setEnabled` walks the subtree suspending physics bodies —
  box3d writes, main-thread-only.

UI notifications route back through `setOnPrefabOpened` / `setOnEntityRespawned`.

## Headless

`setHeadless(true)` BEFORE any spawn makes `buildTemplate` emit only **Scene / Physics / Script /
Network** components. Everything renderer-touching (Render, Animator, Light, Particle, Force) and
Audio is dropped at build time, **so `updateSelf` never dereferences the uninitialized renderer and no
GPU resource is ever created.**

Hull and Mesh collision still works: the Render node's container NAME is parsed textually and the
geometry comes from the renderer-free `ensureCollisionSource` import.

## Contact dispatch

`World::handleContactEvent` is the physics contact callback, passed to `dispatchContactEvents` from
main AFTER the spatial and begin-frame joins — **contact scripts query the index and can touch
renderer state.** It fires, for each side:

1. `PhysicsComponent::onContact`
2. `GameProjectileComponent::onContact` — the shared game-layer behaviour: a projectile spends itself
   on first touch and damages an enemy-team victim
3. the script's `OnPhysicsEvent`

---

# Assets, prefabs and tint

* **`AssetRegistry`** (`Globals::assetRegistry`) — `scanDirectory()` (cwd = `Assets/`) registers
  ObjectContainers (`.oc`), clips (`.anm`), animator graphs (`.apl`) and prefabs (`.pre`) by name.
* **`savePrefab(root, path)`** serializes through the per-component `write*SpawnInfo`s — **the spawn
  recipe IS the serialization.** Spawned prefabs are locked instances, unpackable in the Scene panel;
  `prefabWouldCycle` guards recursion.
* **Per-entity tint:** `Component Render` takes `Color r g b`. `RenderComponent::spawn` overrides
  every mesh instance's material through `RenderNode::setMaterialOverride` plus
  `Renderer::createSolidColorMaterial`, cached per RGB8 — a 1×1 texture and a MaterialInfo minted once
  per colour. Instances carry `materialIdx` per record, **so there are no shader or layout changes.**
  All Game prefabs author unique colours this way.

# The components

| Component | Notes |
|---|---|
| `SceneComponent` | Children. |
| `RenderComponent` | RenderNode + local transform, static or skinned, plus `Color`. |
| `AnimatorComponent` | AnimationPlayer + AnimStateMachine from `.apl`; gameplay through `stateMachine.setFloat/Bool/Trigger`, clip events through `onEvent`. |
| `ScriptComponent` | See [`Code/Script/CONTEXT.md`](../Script/CONTEXT.md). |
| `PhysicsComponent` | See [`Code/Physics/CONTEXT.md`](../Physics/CONTEXT.md). |
| `AudioComponent` | See [`Code/Audio/CONTEXT.md`](../Audio/CONTEXT.md). |
| `ParticleComponent` | See [`Code/Particle/CONTEXT.md`](../Particle/CONTEXT.md). |
| `ForceComponent` | See [`Code/Force/CONTEXT.md`](../Force/CONTEXT.md). |
| `LightComponent` | Below. |
| `NetworkComponent` | Multiplayer, below. |
| Game components (9/10/11) | Below. |

## `LightComponent` (ID 7)

A **LIST** of lights. `Component Light` with `Light Point/Spot/Area/Tube` children: `Color` /
`Intensity` / `Range`, entity-space `Offset` / `Direction`, `Enabled`, plus `ConeAngle` /
`EdgeSoftness` (Spot), `Width` / `Height` / `Rotation` (Area), `Radius` / `Length` (Tube). Component
`Debug true` plus the "Lights/Debug geometry" tweak draw wireframes. Demo:
`Entities/Debug/lightRig.pre`.

**Lights are PER-FRAME RECORDS**: `update` calls `renderer.addLightInfo` per enabled light (lock-free,
rides the parallel pass), and spawn/destroy own nothing. Not frozen-gated — placement, not simulation.
**Range and size are WORLD units, not scaled by the entity.**

> **`Direction` always means where the light POINTS.** The component reconciles the renderer's
> encoding (`LightFrame`): area lights encode a height-axis plus roll, and `TubeLight` stores FULL
> length while the renderer halves it. **Do not "fix" either side alone.**

Script surface: a `Light` DSL struct plus the writable `self.light.lights` sequence and
`self.light.count`. The ABI is only `entityGetLightComponent` / `lightGetCount` / `lightGetAt` /
`lightSetAt`, and `lightSetAt` clamps and normalizes host-side.

## The Game components

ID 9/10/11 in one partition, `Components/GameComponents.ixx`.

* **`GameUnitComponent`** — team, health, shield battery, plus C++ steering / targeting / melee /
  ranged stance. DSL sets orders through `self.unit.setTarget`.
* **`GameStructureComponent`** — team, health, blueprint, invulnerable, meleeRadius, its own
  ForceQuery territory damage, atomic `damage()` / `addLoad()` intake, the three stores and bands, the
  derived LINKS, and a machine-state UNION.
* **`GameProjectileComponent`** — team-tagged lifetime, deflection and contact damage.

**Contract:** `update()` simulates only when NOT a network client (mirrors write state on clients).
Cross-entity writes are atomic CAS; cross-entity lookup is spatial queries — **no entity lists**;
physics writes ride the body-command queue.

Tuning statics (`GameUnitComponent::params` etc.) are set by the Game layer's tweaks — the full
parameter tables and the steering are in [`Code/Nav/CONTEXT.md`](../Nav/CONTEXT.md) and
[`Code/Game/CONTEXT.md`](../Game/CONTEXT.md).

### Authoring

**`Component GameUnit`** — `ShortName` (the 3–5 char HUD tag, interned; the world labels use it on
every instance, **so replicated units need no type on the wire**), then `Team`, `HealthMax`,
`EnergyMax`, `ShieldOutput`, `MoveSpeed`, `Accel`, `AttackRange`, `AttackDps`, `PlayerDps`,
`EmitterDrain`, `Ranged`, `StandoffRange`, `FireInterval`, `ShotKind`, `AlwaysDisplayHealth`,
`HeightLimit`.

`ShotKind` (ranged) is what the game does per shot: **0** the direct `enemyShot`, **1** the splash
`enemyLob`, **2** spawns a loose Swarm body beside the unit.

> **HEIGHT LIMIT.** The physics can launch a body (bubble shoves, stacked bodies, contact impulses),
> so every actor is held under a world-Y ceiling: above it the body is teleported back AT the ceiling
> with its climb cancelled — vy clamped ≤ 0, planar velocity kept, per the teleport contract.
> Shared default `Game/Actors/Height limit (m)` = 10; per prefab **0 = shared, > 0 = own ceiling,
> < 0 = NONE** (the hook for flying units). Applied at the top of `GameUnitComponent::update`
> **BEFORE the puppet gate**, so the server's twins of client capsules are held too, and owner-side in
> `GamePlayer::tickShieldAndHealth` — both land at the same height, so the owner's next claim
> re-anchors instead of being rejected.

**`Component GameStructure`** — Team / HealthMax / Invulnerable / MeleeRadius / AlwaysDisplayHealth /
**AlwaysShowResources** (its overhead STORE bars show even unselected — storage, emitters, barracks;
everything else shows them only while selected. The health bar is separate: damage always shows one).

**`Component GameProjectile`** — Team / UnitDamage / StructureDamage / Lifetime / EmitterDrain /
EmitterDrainRadius / SplashRadius (**> 0 damages EVERY enemy unit and structure within that radius of
the impact** through a spatial query — the lobber shell).

All three are skipped headless.

### Structure links

`GameStructureLink` is mirrored on both endpoints, and **each link is processed EXACTLY ONCE per tick
by its OWNER side** — pushing when it is the source, pulling when the far side is — in the entity
pass with atomic reserve/add/return transfers.

> Both endpoints processing, each deciding independently, moved every link **twice per frame in
> opposite directions**, which made balancing links slosh visibly.

The owner gathers its OUTGOING links first and splits the store as a per-medium **FAIR SHARE** across
receivers; a full receiver's share flows to the rest. Pulls are unbudgeted — the far side's links
belong to their own owners — but atomically clamped.

**Fairness runs on the RECEIVING end too:** a push is capped at the destination's headroom DIVIDED by
the links that can feed it, so a full-but-draining destination trickles in from all its feeders
steadily instead of handing each tick's scraps to whichever worker ran first.

`link` / `unlink` / `unlinkAll` are main-thread bookkeeping — **a structure is ALWAYS unlinked before
destruction, so links never dangle.**

**NO id-keyed maps anywhere** — cooldowns, tallies and boosts die with their structure. Machine state
lives in a UNION discriminated by the game's structure type (the NetEntityState pattern): only the
prefab's variant is ever touched, while `strainable` stays common because units probe it on arbitrary
structures.

---

# Script glue

Lives here, not in Script.

* **`Globals::scriptContext`** — the ABI singleton plus the host thunks.
* **`ScriptEventManager`** (`Globals::scriptEvents`) — global named events. Names map to a `uint32`
  `EventKey` through a shared_mutex-guarded registry (`getEventKeyForName` interns, `findEventKey`
  looks up), so **a fire is one key lookup plus an `LPMultiMap` dispatch**, not a string compare per
  listener. Its `initialize()` must run from main before any scripted entity spawns.
* Deferred script-driven mutations — destroy, reparent, spawn requests — are a mutex-guarded
  `EntityChange` queue drained by the main loop through `takeEntityChanges()`.

## Thunk thread-safety

Scripts tick on workers.

* `ctx->spawnEntity` queues `EntityChange::SpawnAtPosition` and **returns null** — the DSL types
  `world.spawn` as `Entity?`, so scripts take the miss branch on the spawning frame.
* `internString` is shared_mutex; script RNG is `thread_local`; `spatialQueryRadius`'s buffer is
  `thread_local`.
* The DSL array registry is CHUNKED (`g_arrayChunks`, 64 × 1024, handle = 16 bits), **so storage never
  moves and resolve is lock-free.**

---

# Multiplayer

`Entity:NetworkManager` (`Globals::networkManager`) plus `NetworkComponent` (ID 8).

## The ownership model

**Every networked entity is SERVER-OWNED, and the server mints every netId.**

A component registering on the server — scene load or runtime alike — gets an id plus a spawn record
replicated to clients. **Late joiners get every live record replayed after their Welcome, and that
replay IS how the networked world arrives.**

A client registering OUTSIDE an incoming Spawn stays **LOCAL-INERT** (netId 0, never synced). So
client-only and server-only entities are just entities the other side does not have, **the two scenes
can differ arbitrarily, and id conflicts are structurally impossible.**

> **Only STATE is networked.** Static scenery that never moves needs no NetworkComponent.

`NetworkComponent::authority()` derives Local / ServerOwned / LocalOwner / RemoteOwner (worker-safe).
`localClientId()` is 0 on the server and before the Welcome, **which is what makes
`ownerClientId == localClientId()` false everywhere except on the actual owner.**

## State layout

The inline component is **identity only** — netId, ownerClientId, and a `unique_ptr<NetEntityState>`,
16 bytes. All sync state lives in `NetEntityState`, allocated only inside a session.

**Role-exclusive state is a UNION of `ServerState` / `ClientState`** — a process is one role for life,
and both are trivially destructible. Only `input`, `transferredOwnership` and `sleepDirty` are
dual-role. **Cross-role reads are bugs.**

The manager never null-checks `state`; the component's own `update()` and outside readers
(EntityEditor, InputControls) must.

## Roles and the thread contract

```
--server [--port N] [--headless] [--tickrate N]
--connect <ip[:port]>          --no-encrypt
(no flags)  = single player (ENetRole::None; fireNetworkEvent degrades to a local fireEvent)
```

`initialize()` registers the "Network" tweak block **regardless of role**, so the section exists in
single player too. Hard cap `MaxClients` 32.

**Thread contract:** every NetHost call is main-thread — `receive()` before the entity and physics
updates, `send()` after `world.update`. Snapshot decode writes component target state on the main
thread too, **so `NetworkComponent::update` only ever reads it from workers.** Client physics
corrections go through the body-command queue.

## Headless server

`headlessServer` branching inside main() — **one init sequence and ONE main loop for all modes.** No
window, renderer, UI or input.

**Loop:** entity-change drains → receive → scriptContext → physics → `world.update` → send →
Sleep-based tick limiter. Ctrl+C is a clean shutdown; there is a 5 s status line.

> Component thunks do NOT null-check their handle. **`//@@require`'s live `requirementsMet` gate is
> the guarantee** that a script never reaches an absent component, so a script using a stripped
> component simply never runs its entry points headless. Cross-entity thunks still
> `getComponent`-check, and `Renderer::isInitialized()` gates the few thunks that touch renderer
> state.

**Deliberately skipped headless:** terrain, ocean, scatter and the terrain collider (camera-centered
streaming — the test scene carries its own ground), buoyancy, and audio. Spatial entries DO register.

## The wire protocol

`GameProtocolId` (`0x4F43534B`) bumps on ANY format change — **the transport handshake denies
mismatched ids, which IS the version gate.** `GameNetVersion` (15) rides in Hello/Welcome.
[NetworkManager.cpp:14](Private/NetworkManager.cpp#L14). NetFuzz mirrors both by hand.

| Channel | Messages |
|---|---|
| **ch0 Unreliable** | Snapshot |
| **ch1 Reliable** | Event |
| **ch2 Reliable** | Hello / Welcome / Deny / Spawn / Despawn / OwnerChange |
| **ch3 Unreliable** | Claim |

**Spawn and Despawn ride the session channel so they order after Welcome.**

**Event** carries `senderClientId` **server→client ONLY**: a client sends no client id, because *its
identity is the connection, and an ignored client-writable one is a refactor away from being
trusted*. The NET id names the firing entity and travels both ways; being client-supplied, the server
resolves it only after an ownership check (`findOwnedEntity`).

## Snapshots (server → clients, per peer)

Records carry the entity's LOCAL pos/rot, or **the BODY's world pose** when `NetRecFlag_Physics`.
Rotation is smallest-three quantized behind a live `Quantize` toggle; velocities are 3×u16 over live
`Max vel` / `Max ang vel` ranges. **Those ranges are live tweaks, so every message states them** and
the decoder is told per message.

Chunked under `Snapshot max bytes`; **every chunk repeats the tick and staleness is dropped PER
ENTITY**, so chunk reordering is harmless — UnreliableSequenced would drop sibling chunks instead.

### Two phases per tick

**1 — OBSERVE** (`observeSnapshotTick`, every entity once). Sample the wire record — pose, velocities,
flags, game blob, with the claim passthrough advancing here — into `ServerState::obs*` and stamp
`changedTick` on any difference.

* An AWAKE body changes every tick; a sleeping one only on the sleep edge or a blob/flag change; a
  non-physics entity when it moved past the epsilon tweaks (`Send pos epsilon` 0.001,
  `Send rot epsilon (deg)` 0.1).
* It also rebuilds the flat `m_tickEntities` array and the players' positions by clientId.

**2 — SEND** (`sendSnapshotTo`, per ready peer). Each peer has a SLOT, and every entity is TIERED by
distance **to THAT peer's own player(s)**:

| Tier | Radius | Cadence |
|---|---|---|
| NEAR | < `Near radius` 40 | every tick |
| MID | < `Mid radius` 100 (= the sim LOD outer tier) | every `Mid every ticks` 2 |
| FAR | beyond | every `Far every ticks` 4 |

A peer with no player yet sees everything as far.

An entity is DUE when `changedTick > sentTick[slot]` and its tier's cadence is up, **or on its
KEYFRAME rotation** (`serverTick % K == netId % K`): near/mid `Keyframe every ticks` 20, far
`Far keyframe every ticks` 200, and a SLEEPING body at least `Asleep keyframe every ticks` 100.
**Keyframes are the loss repair — nothing else re-sends an unchanged entity.**

Each tier is walked round-robin from the peer's own cursor, near first under `Max entities per tick`
300, with far additionally capped by `Far max per tick` 100. `Snapshot Hz` is 20 — **matching the
physics fixed step.**

Far units DO move (the Game's far tick walks a wave in from the spawn ring); the client only needs
them roughly right until they cross its player's radius.

### The client's unselected-root path

An UNSELECTED root — beyond every player's outer radius, so never visited and
`NetworkComponent::update` never runs — gets its snapshot pose applied **DIRECTLY in
`handleSnapshot`**, under the teleport contract.

> The LOD selects by the entity's LOCAL position, so without this a far unit sat at its spawn point
> forever while the server's walked into range — the client saw a wave only as the few units that
> spawned near a player.

## The spawn stream

**Spawn and Despawn are NEVER sent directly.** Every ready peer has a `PeerStream` of OWED baseIds —
the join replay queued at Hello, plus each frame's announces and despawns.

`drainSpawnStreams()` packs them **many per message** (`[u16 count]` + records, one message per
reliable window slot), only while `getQueuedReliable(peer, ch2)` is under `Queue target` 64 and at
most `Records per frame` 128 per peer per frame.

> The transport DISCONNECTS a peer whose reliable queue passes 1024, **which a 100-per-frame wave
> trickle or a 20k-record join replay hit immediately before this.**

Ids never recycle, **so an owed spawn whose record is gone was despawned unsent and is skipped**;
despawns drain before spawns, so a despawn for an unsent spawn is ignored client-side and a spawn can
never follow its own despawn. Ownership that diverged from the record rides as OwnerChange right
behind the batch. The server title bar shows `spawn backlog N`.

**The Spawn message carries the root body's live velocities**, sampled at send — i.e. AFTER same-frame
launch impulses — and the receiver caps and applies them, **so a replicated projectile's local twin
flies instead of dropping from rest.** Corrections near the receiver's own capsule are
grace-suppressed, so that initial velocity is the only thing moving it there.

Clients adopt server ids **in tree-DFS order**. `setOwner(root, clientId)` must run in the spawn's own
frame, before `send()` announces; later transfers use OwnerChange. **Never store NetPeerIds on
entities** — they recycle; clients get a stable `clientId` at Hello/Welcome.

An occupied id is REPLACED with a warning (the Entity Editor respawns before destroying, so a
collision there is the stale twin), and **unregister erases only when the component pointer still
matches** — otherwise that stale twin's destroy would take the new registration down with it.
Despawn is all-or-nothing at the root.

## Claims (owner → server)

The owner simulates freely and streams input plus claimed pose and velocities, **PHASE-LOCKED to the
physics step**: `send()` fires them only on frames where `getStepCount()` changed.

`Max update Hz` (20) is only a thinning limit, counted in **WHOLE STEPS**
(`stepCount - lastClaimStep >= round(stepHz / maxUpdateHz)`), never against the wall clock.

> A step boundary falls partway through a frame while the net clock advances in whole frames, so a
> time comparison at the step rate measures just-under-interval and **DROPS that step's claim**, which
> the passthrough extrapolates over and double-jumps: remote-client pulsing that vanished only when
> the limit was raised until it stopped binding. Paused physics = no claims, correct.

Each packet carries the last `Claim redundancy` claims (ring cap 8) — **redundancy beats a reliable
channel under loss: no head-of-line stall** — and the server dedups by seq. **The record count comes
from `validCount`, never seq**, so a re-acquired entity cannot replay a previous owner's poses.

One trailing `look` vector applies to every record in the window, since it barely changes across it.
Claims follow the sleep policy: every awake tick, one rest-pose claim on the sleep edge, then silence.

## Validation ("Network/Validation")

* A movement token bucket in metres, refilled at `Max speed` (60) per second of **SERVER WALL
  CLOCK** — never per claim, since the packet rate is attacker-controlled.
* `Max velocity` / `Max ang vel` (50 each) on **EVERY accept path**.
* A trajectory raycast (`castRayClosest` with `ignoreBody` + `staticOnly` — see Physics).
* A hard teleport cap (10 m) regardless of elapsed time.
* The first claim seeds the anchor but is still checked.
* **`Claim reanchor radius` (2 m): a claim this close to the twin's CURRENT state ALWAYS accepts.**
  That is the guaranteed recovery from a rejection spiral.
* Every float off the wire is finite-checked and quaternions re-normalized — **NaN passes comparisons
  and poisons the solver.**

**Accepted** → the twin FOLLOWS the claim through the solver (`Twin follow gain` 10, a bounded
`NudgeVelocity` = claimed velocity plus a corrective velocity toward the claimed pose); a hard
teleport only past `Twin resync (m)` 2.

> It is deliberately NOT teleport-pinned per claim. That collapsed the render interpolation — prev and
> curr both became the claim pose — and, since claims are sampled on the OWNER's step clock while the
> server steps on its own, **yanked the twin backwards whenever two server steps fell between two
> claims**: pulsing visible on the SERVER's own view. Other clients are unaffected either way, because
> the passthrough sends them the owner's stream, not the twin.

`lastAcceptedClaimPos` anchors the next budget and the re-emit through **CLAIM PASSTHROUGH**: the
snapshot re-emits the newest accepted claim state instead of sampling the wobbling twin, extrapolating
by claim velocity for a bounded gap. `Owner predict (ticks)` (0.5) adds a constant forward shift.

**Rejected** → `violations`++ plus `NetRecFlag_Forced` for `Forced ticks`. The owner ignores
corrections UNLESS Forced, an accepted claim clears it immediately, and `updatePlayerControl` yields
input while Forced — **steering against the correction is a tug-of-war.**

## Client correction (`NetSyncParams`, "Network/Correction")

**The DEFAULT for dynamic bodies is the PHYSICAL PUSH**: error × gain becomes corrective velocity on
top of the server's, delivered as bounded impulses, **so contacts and gameplay impulses COMPOSE.** No
teleport on this path, and bodies still sleep inside the deadzones.

| Band | Behaviour |
|---|---|
| Inside `Pos deadzone` (0.05) / `Rot deadzone` (0.5°) | Local state free-runs — **but the velocity still settles onto the server's** (a NudgeVelocity toward the target velocities whenever they differ, never for a body already matching, so rest stays asleep). |
| Past `Pos snap threshold` (2 m) / `Rot snap` (45°) | CATCH-UP: gains and caps × `Push catch-up boost` 4, **still through the sim** — teleporting into an occupied space would depenetration-fling both bodies into fresh desync. |
| Past `Pos teleport threshold` (10 m) | The hard `teleportBody` resync — **position error only**, since a wrong orientation cannot materialize inside anything. |

> Why the deadzone still settles velocity: the push leaves its corrective term in the body, and a
> **FRICTIONLESS body (units) kept it forever**, coasting out the far side of the deadzone and back —
> the client-side swinging.

**MASS SCALING.** Gains, velocity caps and acceleration limits — the deadzone settle included —
multiply by `clamp(mass / Push mass reference (30 kg), Push mass scale min (0.15), 1)`.

> A light body's velocity answers every contact impulse strongly, so in a packed crowd its correction
> and its neighbours' pushes compound into swinging (the swarm). Bodies at or above the reference
> correct at full strength.

Gains: `Push pos gain` 0.5 /s per metre, `Push rot gain` 5 /s per radian, `Push max vel` 10 m/s,
`Push max ang vel` 10 rad/s, `Push accel limit` 10 m/s² (**keep it above gravity**),
`Push ang accel limit` 60 rad/s².

**NON-physics entities** blend the entity transform at `Blend rate` 10, with optional `Extrapolate`
(dead-reckon by `linVel × timeSinceSnapshot`).

**INTERACTION GRACE** (`Interaction radius` 1.5, `linger` 0.5 s): a server-owned body near one of OUR
claim-driven bodies suspends its corrections — **they would fight the shove the player is applying
with the server's RTT-old pre-push state.** The twin gets the same push an RTT later. Proxy positions
are published in `receive()` and read from workers.

**ARBITRATED OWNER** (player-vs-player contact) uses `Arbitrate deadzone` 0.4 and halved gains:
*the local feel comes from the local contact with the opponent's replica, so the server correction
should only reconcile REAL divergence* — a tight deadzone would micro-correct the pipeline lag and
drag against the player's input for the whole window.

## Remote-owned entities

Other players skip the chase entirely and use **BUFFERED SNAPSHOT INTERPOLATION**: a `NetSnapshotRing`
per entity (manager-owned, main-write / worker-read) replays the owner's recorded trajectory at
`newestTick − Remote interp (ticks)` **in the SERVER's tick units** (`serverSnapshotHz` comes from the
Welcome).

> Minimum and default 2 — **the cursor clamps to `newest - 1`, so 1 leaves no headroom and playback
> advances only on snapshot arrival. Buy smoothness with snapshot RATE, not depth.** Loss gaps fall
> through to the push for a frame.

Teleport-following with matched velocities and prev/curr stomps. Asleep records hard-sync once then
queue `SetAwake 0`. Kinematic and static bodies use the entity-transform path.

## The teleport contract

**EVERY teleport must also stomp `PhysicsComponent::prevPos` / `currPos` / `prevRot` / `currRot` AND
claim the step (`physics->lastStep = getStepCount()`).**

`NetworkComponent::update` runs BEFORE `PhysicsComponent::update` on the same entity, so on any frame
the sim stepped it would otherwise overwrite `curr` with `body.getPosition()` — which still holds LAST
frame's teleport, since teleports apply at the next `physics.update` — and render
`mix(thisFramePose, lastFramePose)`, i.e. **BACKWARD on stepping frames and correctly on the
others. That alternation was the remote-entity pulsing.**

## Transfer, steal and arbitration ("Network/Ownership")

`Transfer enabled` is the kill switch, mirrored by `setOwnershipTransfers`.

* Awake server-owned dynamics within `Transfer radius` (2.5) of a client's PRIMARY hand over through a
  per-entity reliable OwnerChange; the server resets that entity's claim and validation state. They
  revert after `Release delay` (1 s) outside `Release radius` (4).
* **Primaries only as SOURCES** — chained transfer through a pushed cube would deadlock release on
  piles; the client-side interaction grace covers second-order contact instead.
* **The SERVER's own player body registers as a primary too** (`setServerPrimary`, clientId 0). It is
  never handed to a client, and it RE-CLAIMS transferred objects by proximity — only ever transferred
  ones, never a client's primary. Its snapshot records carry `NetRecFlag_ServerPlayer`, so observers
  route it into the remote-player INTERP ring and **skip the interaction grace**: it is an
  actively-steered body, and grace-freeing it for local shoves only fabricates divergence.
* **Contact STEALING** (`stealOwnershipOnContact`, wired as the primary's `onContact` in main.cpp;
  needs `ContactEvents true`): **last collider owns**, even inside the previous owner's transfer
  bubble — but never another client's PRIMARY.
* **Primary-vs-primary contact ARBITRATES** both for `Arbitrate window` (1 s, refreshed per contact):
  twins go solver-owned, claims apply as bounded NudgeVelocity, records carry `NetRecFlag_Arbitrated`,
  and input keeps steering. An object touched by two DISTINCT clients within `Contest window` (1.5 s)
  is **CONTESTED → server-owned** until the window decays.
* Late joiners get OwnerChange replays after the Spawn replay; a disconnect reverts transfers before
  tearing down primaries.

`setOnClientJoined` / `Left` (main.cpp) spawn and tear down `Entities/Debug/netPlayerCapsule.pre` per
client. **`joined` runs AFTER the Welcome and world replay are queued**, so anything spawned inside
arrives in the same session stream.

`setOnServerLost` fires on the main thread inside `receive()`, **BEFORE the automatic reconnect
attempt**. The App decides what a lost server means. **Do not shut the host down from inside it** —
`receive()` is still walking its events.

## Events

`fireNetworkEvent(name, data, sender)` = a local fire plus Reliable ch1; the server relays to other
clients. Role None = local fire only.

* **Callable from job workers** (script thunks tick in the parallel pass): the network half queues
  under a mutex and `send()` drains it on the main thread, since NetHost is single-threaded. The
  payload is copied.
* `MaxEventDataBytes` 1024, **static_asserted against the transport's `netMaxSinglePacketMessage()`**
  so a max-size event still rides in ONE packet.
* `currentEventSender()` / `currentEventData()` are readable during dispatch. **The sender clientId is
  server-stamped from the receiving peer**, so a client cannot claim to be someone else. The data is a
  view into the receive buffer — copy anything you keep.
* **`setEventFilter`** gates client-originated events: false drops it, neither fired nor relayed.
  `sender` is non-null ONLY when that client really owns the supplied netId. Server-originated events
  bypass. **No filter = everything allowed, so install one before any event grants state.**
* **`setOnGameEvent`** is the C++ listener for every dispatched event, local and received — the
  counterpart of the script listeners. It runs synchronously inside the dispatch. **Main thread for
  received events, but a locally fired event dispatches on the FIRING thread** (worker script thunks
  included), so keep the handler cheap and thread-tolerant.

## Session

The 4-way transport handshake gates on `GameProtocolId`, then Hello → Welcome on ch2 marks the peer
ready. **Per-peer state MUST clear on Disconnected.**

**Events (ch1) have NO ordering against the ch2 Welcome**, so a client PARKS events that arrive before
its Welcome is processed (`m_preWelcomeEvents`, bounded, replayed in order right after).

> The server's join-time burst — game world replay, tweak sync — is fired in the same frame as the
> Welcome and was **silently lost** before this (already acked = never resent). **Anything
> join-critical sent as an event relies on this parking.**

Encryption is on by default at this layer (`NetHostConfig::encrypt`'s own default is false). It
**cannot change on a live host**, so call `setEncryption()` before start; `--no-encrypt` drives it, and
both ends must match or the handshake denies — a clean failure. Without it a peer is identified only
by source address, trivially forged on a LAN.

## Testing

Server key U throws `Entities/Debug/netPhysCube.pre`; client key C toggles player control; key K fires
"NetPing". Demo `Entities/Debug/networkTest.pre` spawns at a **FIXED transform — never
camera-relative, since both worlds must match.** Link-sim tweaks bind into `m_host.config()`, and the
window title shows role plus RTT / loss / KB/s (`getStatusText`).

## Player control

Key C on a client, `InputControls::updatePlayerControl`. WASD and Space velocity-steer every
locally-owned dynamic body camera-relative ("Network/Player" tweaks — **keep Move speed under
Validation's `Max speed`**), with the jump gated on a downward raycast and fly-camera movement
released while active.

The owning client simulates its body directly — main thread, before `physics.update`, where direct
setters are sanctioned — writes intent into `comp->input`, and the claim stream carries the result.
