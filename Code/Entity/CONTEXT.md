# Entity

> Library documentation for `Code/Entity`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The ECS. An `Entity` (pos / scale / rot, parent, spawn template, refcount, `typeBits`, `flags`) and
its components live in ONE allocation.

## Names

The entity has NO name field. `getName` / `setName` / `hasName` forward to `Globals::entityNames`
(`EntityNameRegistry`, `Private/EntityNames.ixx`) — a pointer-keyed, 64-shard mutexed map from entity
to the INTERNED name (`Profiler::internName`, permanent and deduped).

That way the same pointer names the entity's opt-in `ProfileScope` in the parallel pass and stays
valid in the profiler ring after the entity dies.

`Entity::destroy` erases the entry. `set` / `erase` are parallel-spawn/destroy safe; `get` is called
from the entity pass (script thunks, the profiled scope).

## Components

`typeBits` masks the 13 component types, in this fixed order:

```
Scene, Render, Animator, Physics, Audio, Particle, Force, Light, Network,
GameUnit, GameStructure, GameProjectile, Script          (Script last)
```

`EComponentID` in `EntityP.ixx`; `MaxInlineComponentTypes` 13. `getComponent<T>` / `hasComponent<T>`
compute byte offsets from compile-time sizes (`Component.ixx`).

Each component is a partition under `Private/Components/` — struct plus `get*SpawnInfo` /
`write*SpawnInfo` in the `.ixx`, bodies in the same-named `.cpp`. `Component.ixx` re-exports them and
holds the shared machinery.

> Per-component `.ixx`s must NOT import `:Component` (cycle). Cross-component code goes in the `.cpp`
> implementation units.

### Adding a component type

1. New `.ixx` / `.cpp` pair with a `getId()` struct.
2. `export import` from Component.ixx.
3. Entries in `EComponentID`, `inlineSizes` and `componentTypeName`.
4. Create/destroy switches in Entity.cpp.
5. Parse branch in `World::buildTemplate`.
6. Write branch in Prefab.cpp.
7. Require-slot fill branch in `ScriptComponent::syncScriptData` — **a missing branch shifts every
   later slot; this crashed once.**
8. Entity.natvis offset entry.
9. Bump `MaxInlineComponentTypes`.
10. An Entity Editor section, if authorable.

## Allocation

* `EntityPtr` is an intrusive refcounted handle (atomic).
* `Entity::create(template, transform)` placement-news into `Globals::entityAllocator` — lock-free:
  atomic chunk bump plus tagged per-size-class Treiber free lists, blocks ≤ 64 KB.
* The tree is `SceneComponent::children` plus `Entity::parent`; `reparentEntity` moves ownership.

### Contiguous tree allocations

Tree spawns are ONE allocator block (the template's cached `treeAllocSize`, bump-carved).

* While intact (`EEntityFlag_RootAllocation` / `ContiguousAllocation`) the root frees the whole
  block, after a refcount pre-walk (`contiguousTreeSolelyOwned`).
* Reparenting or deleting a member out of its birth tree SPLITS the allocation
  (`breakContiguousAllocation`): path ancestors demote to per-entity freeing, and off-path subtrees
  promote to their own RootAllocations.
* Moving within the same allocation, or grafting a whole allocation root, never breaks anything.

## The per-entity visit

`Entity::update` is a thin serial recursive wrapper around `Entity::updateSelf`, the ONE hot
per-entity visit:

```
Script → Network → Animator → Physics
  → compose world transform → refresh the SpatialIndex entry
  → push the RenderNode (gated on spatialIndex.getPassMask when Spatial culling mode ≥ Cull)
```

## Spatial entries

EVERY entity owns a `SpatialEntry` (`Entity::spatialEntry`), registered at the end of
`Entity::create` — parallel-spawn safe, since the index locks.

* Layer `SpatialLayer_Entity` always, plus `SpatialLayer_Render` when it has a render node (bounds
  from the node, skinned inflated by the culling config; otherwise a point at the position).
* `updateSelf` refreshes it every visit, and the render gate reads its pass mask.
* Gameplay queries keep using the Render layer, so they still see only rendered things. The Entity
  layer is the World's UPDATE SELECTION layer.
* **Headless:** every entity registers too (as points), and main.cpp calls `commitFrame()` in place
  of the cull job, so radius queries work server-side.

## SIM LOD and update selection

"Game/Sim LOD" tweaks — NOT Saved, so the code defaults in `SimLodConfig` rule every run and a stale
tweaks.cfg never overrides a tuning change. `SimLodConfig` lives in World.ixx.

**Decided ENTIRELY in World.** The entity only ever sees the delta it is handed.

### Focus

`World::setSimLodFocus(points, count)` runs every frame BEFORE `world.update`. The Game layer
publishes every player capsule (its own plus the server's client twins, in `GameMatch::update`);
main.cpp publishes the camera in the plain testbed.

It forwards to `SpatialIndex::setUpdateLod`, which stamps the three
`ESpatialPass::UpdateTier0/1/2` passes in the NEXT cull job — balls at "Full rate within" /
"Tier 1 within" / "Tier 2 within" around every focus point, `markVisibleSpheres` being one stamp
generation over the union. It runs in every culling mode, with one frame of latency vs the focus.

**NO focus, paused, or disabled = LOD INACTIVE:** every root and every child is visited, so the pass
scales with the entity count.

### Active: the visit set

The pass is DETACHED from the entity count. The visit set is:

1. The `Global` roots — `Global true` in the `.pre`, root only, never inherited
   (`EEntityFlag_Global`, `World::m_globalRoots`). Organisational roots whose children spread across
   the world, e.g. `terrainroot.pre`.
2. Roots added since the last pass (`m_pendingRoots`), visited ONCE unconditionally — a fresh entry
   links only at the next commit, so no query can find it on its spawn frame.
3. One `querySphere` per focus point at `radius[2]` + "Query margin (m)" on the Entity layer, at ANY
   depth. `selectUpdateRoot` walks each hit up to its root, stamping every ancestor UpdateTier2 —
   stopping at a Global ancestor or at one already stamped this frame — and queues the root. Sort +
   unique dedupes.

**Descent.** `submitEntityBatches` copies only SELECTED children into the arena (`simLodSelected`:
any UpdateTier or Main stamp on the child's own entry; never-stamped fresh entries count as
stamped). So a visited organisational parent neither hides its far children nor drags all of them
in — selection is per entity, and the parent is only the transform carrier. Parent-before-child order
is untouched, since children are still emitted by their parent's batch.

### Tiers

**TIER = the entity's OWN pass mask** (`World::simLodDelta` per node). The DISTANCE tier (UpdateTier0/
1/2 stamps; none = 3) is what the bubble gate and the dormant edge go by.

> The top-down camera sees the whole tier-1/2 area, so visibility must never override distance there.
> It did: every on-screen unit read tier 0 and kept its bubble.

The TICK tier is the distance tier, floored for an in-view entity (Main stamp) inside the outer
radius by "Visible max tier" (default 2 = no effect; 0 = everything on screen ticks every frame):

| Distance tier | Tick |
|---|---|
| UpdateTier0 | Every frame |
| UpdateTier1 / 2 | TIME-based: a tick every "Tier N interval (s)" (0.25 / 1.0), but never closer than "Tier N min frames" (4 / 16 — the frame gap is the floor for low frame rates) |
| 3 | DORMANT whatever the visibility (the margin band). "Dormant interval (s)" 0 = never, else the same time + min-frames rule. The entity and its subtree are skipped this frame. |

**Exact elapsed time without a per-entity clock** (Entity stays ≤ 64 bytes): `World::m_frameTimeRing`
holds the cumulative sim time at the end of each of the last 256 passes, so elapsed = now −
ring[pass of the last tick], found from `schedSkipped` (saturating at 254) alone.

"Interval jitter" (±25 %) is a per-entity factor on the interval, so a wave that entered a tier
together drifts apart.

The camera is top-down, so a visible entity beyond the outer radius is accepted as not selected —
no frustum query. (The user's call.)

### Per-entity component kinds

"Game/Sim LOD/Follows": Units (default on) / Structures / Projectiles / Scripts (off) /
Animators (on).

* A **following** kind makes the entity a candidate.
* A **NON-following** sim kind PINS the entity to full rate WHILE SELECTED. It does not make a far
  structure selected — a barracks beyond the outer radius is not visited at all, so author
  `Global true` on prefabs that must always run.

### Unstamped visits

The spawn-frame visit from the pending list sees only the spawn GUARD — the entry is unlinked, so
`getPassMask` says "every pass". `simLodDelta` therefore checks the tier stamps EXACTLY
(`getPassMaskExact`): no real tier stamp = tier unknown = full-rate visit, nothing decided.

### Force bubbles

Bubbles spawn ON. The World only ever gates them BY DISTANCE TIER on a placed, selected entity:
"Force bubbles max tier (3 = always)", default 1.

* `ForceComponent::setActive(tier <= it)` on EVERY selected entity with a ForceComponent, throttled
  or not — so a structure's emitter beyond tier 1 projects no field either, and the tier is read for
  such entities even though their tick rate stays full. Refreshed every visit, so a tweak change
  applies at once.
* A spawner that places a unit far from every player parks its bubble along with its body
  (`NpcSystem::spawnLooseUnits` — never visited, so nothing would gate it). One that leaves the
  selection keeps its last state, and the query-margin visit (no stamp = tier 3) switches it off on
  the way out.
* Where the LOD does not apply — inactive, Global, no entry — the World never touches a bubble.

### The entity only sees its delta

`updateSelf` gets 0 on a skipped frame. **Zero delta = no sim step:** game components, script and
animator are not called, while Network correction, the Physics pose read, and the placement tail
(compose / render / spatial / audio / particle / force / light / children) still run.

On a tick frame it gets the accumulated CATCH-UP: `Entity::schedSkipped` (World-owned scheduling
bytes next to `updateCost`, with `schedTier`) counts skipped passes, and the tick receives the exact
covered sim time (see the ring above), capped at "Max catch-up (frames)" × the frame delta. Waking
from dormant resets the counter.

`simLodDelta` is three pieces: `simLodTiers` (the tiers from the stamps plus placed),
`simLodTransition` (the schedTier edges), and `simLodCadence` (the time-based tick).

### Dormant and wake edges

Any throttled entity with a PhysicsComponent.

* **DORMANT EDGE** — `PhysicsComponent::park(disable)`, through the body-command queue; the pass
  never writes box3d. Zero linear and angular velocity, then either
  "Dormant disables physics body" (default on) = `EBodyCommand::SetEnabled 0` (`b3Body_Disable`: out
  of the broadphase and solver, pose kept, NOTHING can wake or push it), or off = `SetAwake 0`
  (asleep, and a contact wakes it).
* **WAKE EDGE** — `PhysicsComponent::unpark()`: zero the velocities and queue `SetEnabled 1`
  unconditionally, a no-op on an enabled body, so a tweak flipped mid-dormancy never strands a
  disabled body. Unless `PhysicsComponent::suspended`, where an Enabled-off subtree owns its own
  disable. The next tick's velocity write wakes a sleeping one.
* The Game's `spawnLooseUnits` uses the same `park(true)` to park loose units at spawn.
* A FRESH entity starts at `schedTier` 3 (unplaced), so its first stamped visit is a wake edge —
  that is the hook a spawner uses to park a body at spawn (the Game's loose units) and have it
  enabled once a player is near.
* The query margin exists so a unit LEAVING the outer tier is still visited once with no stamp and
  takes the dormant edge.

Unselected units with orders are moved by the Game's FAR TICK instead
(`GameUnitComponent::updateFar` from `NpcSystem::service`, before the pass — see
[`Code/Game/CONTEXT.md`](../Game/CONTEXT.md); `World::simLodActive()` gates it).

### Known gaps

* An unselected unit never runs its death check; damage banked into it settles when it is selected
  again.
* With the disable OFF, a parked body woken by a contact moves without its entity following —
  disabled bodies cannot.
* Unselected entities are not rendered (no shadow or GI either).
* A networked client entity outside the selection runs no correction:
  `NetworkManager::handleSnapshot` applies its snapshot pose directly instead (see the Multiplayer
  SNAPSHOTS section), so it re-enters the selection where the server has it.

"Game/Sim LOD/Stats" shows live per-tier counts (per-worker staging counters summed after the join).
"Update selection" is the main-thread scope.

## Entity flags (`EEntityFlags`)

| Flag | Meaning |
|---|---|
| `EEntityFlag_Enabled` | Off prunes the subtree from update and suspends physics bodies. Serialized as entity-level `Enabled false`; default true. |
| `EEntityFlag_Frozen` | Script Update, animator and physics do not tick, and script events do not fire. What the Entity Editor sets on an open document. |
| `EEntityFlag_PhysicsSuspended` | Latches the disable walk. Cleared up the new ancestor chain by `reparentEntity` — otherwise a body joining a disabled subtree stays live and collides invisibly. |

Frozen is a SUBTREE state written down the whole tree by `setFrozen`. `Entity::create` inherits it
from the spawn parent, and `reparentEntity` adopts the new parent's state. Every check is a plain
self `isFrozen()`, including entry points outside update (global script events, physics contacts).
Script `OnSpawn` runs even frozen.

Per-component `enabled` bools are independent of all of these.

## Parallel entity update

`World::update(renderer, dt)`, always on.

### Continuation batches

No level barrier. A batch job processes a node range, then immediately slices the children it emitted
into new batch jobs (`submitEntityBatches`) on ONE pass-wide `JobCounter` that main waits on
(helping).

The only real dependency is parent-before-CHILD: a child reads its parent solely through the
`parentWorld` baked into its `EntityUpdateNode`, and the child's batch is submitted only after its
parent's batch body ran. Subtrees therefore descend independently. (The old breadth-first level walk
stalled every depth on the slowest batch of the previous one, at a "Level merge" barrier.)

Worker-submitted continuations land on that worker's LIFO deque, so a deep subtree descends
cache-warm on one core while wide fan-outs get stolen.

### Cost budgeting

Batches are COST-BUDGETED, not uniform-grain.

* Every entity carries `updateCost` — a u8 in 250 ns units, MEASURED: 0 = unmeasured, the entity's
  FIRST update is timed and becomes the cost (no guessed initial value), then a ~1/1024-per-frame
  random re-measure, salted by `m_updateFrame` and the entity pointer, keeps it honest.
  `sizeof(Entity) == 64` is static_asserted — one cache line.
* A batch fills to about 25 µs worth. `m_updateCost`'s EMA is fed cost units as its item count, so
  the budget self-calibrates to real ns-per-cost-unit. The old even-split term is gone — parallelism
  now comes from the fan-out itself.

### The frame arena

Batch nodes live in `m_updateArena`: claimed with an atomic bump (NEVER rolled back), pointer-stable
during the pass, and resized only between frames from last use plus overflow. A claim past the end
runs those subtrees serially through the recursive `Entity::update` and grows the arena next frame.

`updateSelf` emits children into `PerWorker` staging, and `submitEntityBatches` copies them into the
arena in ONE claim BEFORE the first submit. **Load-bearing:** a submit can execute its job INLINE on
queue exhaustion, and an inline child batch reuses the caller's staging slot, so nothing may read the
staging pointer after the first submit.

Neither the batch job nor `updateSelf` ever fiber-waits — that is what makes the per-worker staging
slot exclusive for the job body.

### What is safe inline

A parent always finishes before its children, and all other writes land in the entity's own
components. Thread-safe inline:

* scripts — DSL cross-entity access is parent → child writes and child → parent reads; world radius
  queries are the known open hole
* animator plus `setSkinningPalette` (disjoint regions)
* dynamic-body pose reads
* `RenderNode::setTransform` / `renderNode`
* `SpatialIndex::updateEntry`
* audio follow, particle and force emitter slots

**The pass NEVER writes to box3d.** The gizmo entity uses the serial path.

## Parallel entity spawning

`Entity::create` / `Entity::destroy` are callable from worker jobs CONCURRENTLY WITH EACH OTHER, in
the same frame window where main-thread spawning is legal today — the entity-change drains and
`game.update`, after the spatial/begin-frame joins. Never during the parallel entity pass, the
physics step, or present.

`World::spawnBatch(span<SpawnRequest>, addRoots)` is the sanctioned entry: main resolves templates
(the template, container, clip and audio caches stay main-thread-only; a request name WITH an
extension resolves like `spawnAssetFile` with `overrideDefaultTransform`, a plain name like
`spawn()`), a cost-grained parallelFor runs the `Entity::create` calls, and roots attach serially
after the join.

### What makes it safe, per resource

Each seam locks ONLY create/destroy — the parallel-pass hot paths stay lock-free.

| Resource | Protection |
|---|---|
| EntityAllocator, `treeAllocSize` / `treeTypeBits` caches | Already lock-free / benign-race |
| Spatial `registerEntry` / `unregisterEntry` | `m_registerMutex` EXCLUSIVE (pool growth reallocates the SoA), with `query*` taking it SHARED — a spawn job's script `OnSpawn` queries while another registers. The `markVisible*` traversals stay lock-free: the cull kick/join window forbids registration. |
| `OcclusionBuffer` add/remove | Mutex |
| box3d body create/destroy | The Physics-module `g_bodyLifecycleMutex` (Body.ixx) |
| Renderer slot/registry allocators — transform slots, mesh/material/instance-offset registries, LOD state, skinned bundles/palettes/job ranges, solid-colour materials, force and particle slots | ONE RECURSIVE `Renderer::m_spawnMutex`, held whole-body by `ObjectContainer::spawnNodeForIdx` / `spawnSkinnedNode` |
| `MeshDataManager` range alloc/free | Mutex |
| `TextureManager` upload/free | Mutex |
| ForceSystem create/destroy | Mutex, plus `m_emitters` / `m_queries` RESERVED to the renderer caps at initialize so growth never reallocates under a concurrent handle resolve |
| ParticleSystem effect create/destroy/cache | Mutex held only for the map and vector work — the `.pfx` load, texture loads and renderer slot creation run outside it. Spawn-time `setEmitting` locks too; pass-safe `setTransform` / `setVelocity` do not. |
| `ScriptHost::getOrLoad` | Mutex (cacheKey canonicalization outside) |
| ScriptEventManager listener structures | A plain mutex. `fireEvent` SNAPSHOTS the dispatch list under it and invokes the scripts after releasing; nested fires and re-registration re-lock freshly. |
| NetworkManager registration | A recursive `m_registerMutex`. See below. |

**NetworkManager detail.** A replicated tree's netIds must stay CONTIGUOUS, since clients adopt base +
cursor in DFS order. So a SERVER tree spawn whose template tree carries MORE THAN ONE
NetworkComponent (lazy `treeNetworkCount` cache) holds that mutex across the WHOLE tree, through
`begin`/`endTreeRegistration` in `Entity::create`'s root overload. Single-component trees — every
unit and projectile prefab — mint atomically inside `registerEntity` and spawn fully parallel even on
the server.

### Parallel destruction

Rides the same seams, plus the audio source list (`AudioSystem::m_sourceMutex` over create / release /
detach; the miniaudio and Steam Audio teardown itself runs outside it).

`World::releaseBatch(vector<EntityPtr>&&)` fans the releases — and so the `Entity::destroy` of every
last reference — over a parallelFor. Callers drop every OTHER owner FIRST on main (the root list
through `removeRootEntity`, so its roster callback stays serial; then rosters), then hand the batch
over.

Users: the EntityChange Delete drain (`handleEntityChanges` moves the Delete handles out after the
serial pass — unit deaths) and `NpcSystem::clear` (load and teardown).

### Event safety

`Entity::destroy` FIRST calls `ScriptComponent::detachListener`, whose unregister WAITS for
`ScriptEventManager::m_dispatching` — `fireEvent`'s in-flight snapshot count — to drain before any
component is torn down.

A script `OnDestroy` firing a global event on one worker can therefore never reach a sibling that
another worker is mid-teardown. The wait is only ever taken on the destroy path, never from inside a
dispatch: destroys are deferred requests, and `syncScriptDataLive`'s re-registration passes
`waitForDispatches = false`.

## `World` (`Globals::world`)

* `spawn("prefabName", transform)`, `spawnAssetFile("Entities/x.pre", ...)`, `createEmptyEntity`
  (archetype 0 — NO components, so it cannot hold children; a grouping root needs a `Component Scene`
  prefab, e.g. `Entities/Game/terrainroot.pre`).
* Builds and caches `EntitySpawnTemplate`s (archetype plus per-component `SpawnInfo` recipes), and
  caches ObjectContainers, collision sources and meshes, and retargeted clip sets — each source file
  imported once.
* `reloadPrefabs` / `invalidatePrefab` for editing; retired templates stay alive for live entities.
* OWNS the root entity list: `addRootEntity` / `removeRootEntity` / `rootEntities` (const-only) /
  `clearRootEntities`. Roots die in `~World`, ordered by the init_seg teardown scheme — see Style.
* Applies queued `EntityChange`s through `handleEntityChange(change, camera, viewportRect)`. UI
  notifications route back through `setOnPrefabOpened` / `setOnEntityRespawned` callbacks.
* `World::handleContactEvent` is the physics contact callback, passed to
  `physics.dispatchContactEvents` from main AFTER the spatial/begin-frame joins — contact scripts
  query the index and can touch renderer state. It invokes `PhysicsComponent::onContact` plus the
  script's `OnPhysicsEvent`.

## Assets and prefabs

* `AssetRegistry` (`Globals::assetRegistry`) — `scanDirectory()` (cwd = `Assets/`) registers
  ObjectContainers (`.oc`), clips (`.anm`), animator graphs (`.apl`) and prefabs (`.pre`) by name.
* `savePrefab(root, path)` serializes through the per-component `write*SpawnInfo`s — **the spawn
  recipe IS the serialization.**
* Spawned prefabs are locked instances (`EEntityFlag_PrefabInstance`, unpackable in the Scene panel).
  `prefabWouldCycle` guards recursion.

### Per-entity tint

`Component Render` takes `Color r g b`. `RenderComponent::spawn` overrides every mesh instance's
material through `RenderNode::setMaterialOverride` plus `Renderer::createSolidColorMaterial(color)`,
cached per RGB8 — a 1×1 texture and a MaterialInfo minted once per colour. Instances carry
`materialIdx` per record, so there are no shader or layout changes. All Game prefabs author unique
colours this way.

## The components

| Component | Notes |
|---|---|
| `SceneComponent` | Children. |
| `RenderComponent` | RenderNode + local transform, static or skinned. |
| `AnimatorComponent` | AnimationPlayer + AnimStateMachine from `.apl`; gameplay through `stateMachine.setFloat/Bool/Trigger`, clip events through `onEvent`. |
| `ScriptComponent`, `PhysicsComponent`, `AudioComponent`, `ParticleComponent`, `ForceComponent`, `LightComponent`, `NetworkComponent` | See their own sections / libraries. |
| **Game components** (ID 9/10/11, `Components/GameComponents.ixx` — one partition for all three) | `GameUnitComponent`, `GameStructureComponent`, `GameProjectileComponent`. |

### The Game components

* **`GameUnitComponent`** — team, health, shield battery, plus C++ steering / targeting / melee /
  spitter stance. DSL sets orders through `self.unit.setTarget`.
* **`GameStructureComponent`** — team, health, blueprint, invulnerable, meleeRadius, its own
  ForceQuery territory damage, and atomic `damage()` / `addLoad()` intake.
* **`GameProjectileComponent`** — team-tagged lifetime, deflection and contact damage, routed from
  `World::handleContactEvent`.

**Contract.** `update()` simulates only when NOT a network client (mirrors write state on clients).
Cross-entity writes are atomic CAS (`damage` / `addLoad`); cross-entity lookup is spatial queries —
no entity lists; physics writes ride the body-command queue (parallel pass). Tuning statics
(`GameUnitComponent::params` etc.) are set by the Game layer's tweaks.

**DSL surface** — `self.unit.*` / `self.structure.*` / `self.projectile.*`, plus the
`ctx->gameUnit*` / `gameStructure*` / `gameProjectile*` thunks.

#### `Component GameUnit` authoring

`ShortName` — the 3–5 char HUD tag, interned through `Profiler::internName` and read back by
`getShortName()`. The world labels use it on every instance, so replicated units need no type on the
wire.

Then: `Team`, `HealthMax`, `EnergyMax`, `ShieldOutput`, `MoveSpeed`, `Accel`, `AttackRange`,
`AttackDps`, `PlayerDps`, `EmitterDrain`, `Ranged`, `StandoffRange`, `FireInterval`, `ShotKind`,
`AlwaysDisplayHealth`, `HeightLimit`.

`ShotKind` (ranged) is what the game does per "shot": 0 the direct `enemyShot`, 1 the splash
`enemyLob`, 2 spawns a loose Swarm body beside the unit. It rides the FireRequest.

**HEIGHT LIMIT.** The physics can launch a body (bubble shoves, stacked bodies), so every actor is
held under a world-Y ceiling: above it the body is teleported back AT the ceiling with its climb
cancelled — vy clamped ≤ 0, planar velocity kept, per the teleport contract (prev/curr/lastStep
stomp).

* The shared default is the "Game/Actors/Height limit (m)" tweak (`GameUnitParams::heightLimit`, 10).
* `HeightLimit` per prefab: 0 = shared, > 0 = own ceiling, < 0 = NONE — the hook for flying units.
  `effectiveHeightLimit()` resolves it.
* Applied at the top of `GameUnitComponent::update` BEFORE the puppet gate, so the server's twins of
  client capsules are held too, and owner-side in `GamePlayer::tickShieldAndHealth`. Both land at the
  same height, so the owner's next claim re-anchors instead of being rejected.

#### The other two

* `Component GameStructure` — Team / HealthMax / Invulnerable / MeleeRadius / AlwaysDisplayHealth.
* `Component GameProjectile` — Team / UnitDamage / StructureDamage / Lifetime / EmitterDrain /
  EmitterDrainRadius / SplashRadius. `SplashRadius` > 0 makes the contact damage EVERY enemy unit and
  structure within that radius of the impact, through a spatial query — the lobber shell.

All three are skipped headless.

#### Ownership of state

ADOPTED by the Game layer: unit, projectile and structure prefabs author them, per-type unit stats
live in the `.pre` files (`Component GameUnit` blocks), and Code/Game holds NO entity lists.

`NpcSystem` is production plus servicing over its unit and projectile ROSTERS — owning `EntityPtr`s.
**NO world-wide spatial queries anywhere in the game layer:** rosters register at spawn and
deregister through `World::setOnRootEntityRemoved`, the ONE notification every removal path funnels
into (destroy requests, editor deletes, network despawns). The callback must never call
`removeRootEntity` back, and the game's own removers deregister first so it no-ops for them.

StructureSystem's roster is `m_frame`. `GameStructureComponent` additionally carries:

* the stable `structureId`
* the three stores, capacities and bands
* the LINKS — `GameStructureLink`, mirrored on both endpoints
* machine state: `powered` and `flowUtil` are common, and the TYPE-SPECIFIC rest lives in a UNION
  discriminated by the game's structure type (`barracks` spawnTimer / population / popCap / unitType
  / spawnPop / houses / route; `turret` fireTimer; `emitter` outputFrac / outputFracTarget / down /
  unitLoad). The NetEntityState pattern: only the prefab's variant is ever touched. `strainable` stays
  common, because units probe it on arbitrary structures.

**Link processing.** Each link is processed EXACTLY ONCE per tick by its OWNER side — pushing when it
is the source, pulling when the far side is — in the entity pass, with atomic reserve/add/return
transfers.

> Both endpoints processing, each deciding independently, moved every link twice per frame in
> opposite directions, which made balancing links slosh visibly.

The owner gathers its OUTGOING links first and splits the store as a per-medium FAIR SHARE across
receivers; a full receiver's share flows to the rest. Pulls are unbudgeted (the far side's links
belong to their own owners) but atomically clamped.

Fairness runs on the RECEIVING end too: a push is capped at the destination's headroom DIVIDED by the
links that can feed it, so a full-but-draining destination trickles in from all its feeders steadily
instead of handing each tick's scraps to whichever worker ran first (which made the inflow hop
between links).

`link` / `unlink` / `unlinkAll` are the main-thread bookkeeping — a structure is ALWAYS unlinked
before destruction, so links never dangle.

**NO id-keyed maps anywhere** — cooldowns, tallies and boosts die with their structure.

StructureSystem works on `m_frame`, its persistent ROSTER of owning Refs (type / nodeIndex / id
recorded at `spawnStructure`; removals through `removeStructureBookkeeping`, shared by
`destroyStructureAt` and the world-removal callback). `refresh()` at the top of `tickAuthority` /
`tickMirror` only `stampTuning`-re-stamps capacities, bands and throughputs so tweaks stay live — no
query. It keeps only placement, requests, production, sweeps, mirrors and save/load.

### `AudioComponent`

Named triggerable sounds. `Component Audio` with `Sound <alias>` entries, each holding one or more
`Path` clips with per-Path settings (`Volume` / `Pitch` / `Loop` / `Relative` / `ReferenceDistance` /
`MaxDistance` / `Rolloff`), plus an optional `Select` mode `Single` / `Random` / `RandomNoRepeat` /
`Cycle` / `CycleStartRandom`.

`trigger(entity, alias, overrides)`. Playing spatial sounds follow the entity unless position-pinned.
Demo: `physicsCube.pre`.

### `LightComponent` (ID 7)

A LIST of lights. `Component Light` with `Light Point/Spot/Area/Tube` children:

* Common: `Color` / `Intensity` / `Range`, plus entity-space `Offset` / `Direction`, and `Enabled`.
* Spot: `ConeAngle` / `EdgeSoftness`. Area: `Width` / `Height` / `Rotation`. Tube: `Radius` /
  `Length`.
* Component `Debug true` plus the "Lights/Debug geometry" tweak draw wireframes. Demo:
  `Entities/Debug/lightRig.pre`.

Lights are PER-FRAME RECORDS: `update` calls `renderer.addLightInfo` per enabled light (lock-free,
riding the parallel pass), and spawn/destroy own nothing. Not frozen-gated — placement, not
simulation. Range and size are WORLD units, not scaled by the entity.

`Direction` always means where the light POINTS. The component reconciles the renderer's encoding
(`LightFrame` in LightComponent.cpp): area lights encode height-axis plus roll, and `TubeLight`
stores FULL length while the renderer halves it. **Do not "fix" either side alone.**

**Script surface** — a `Light` DSL struct (mirroring `OcLight` in ScriptAPI.h) plus the writable
`self.light.lights` sequence (`foreach [ref] Light l in ...` / `ifexist ... at i`,
`self.light.count`). The ABI is only `entityGetLightComponent` / `lightGetCount` / `lightGetAt` /
`lightSetAt`, and `lightSetAt` clamps and normalizes host-side. The NodeEditor Get/Set Light nodes
and the Entity Editor Light section go through the same surface.

## Script glue

Lives here, not in Script.

* `Globals::scriptContext` — the ABI singleton plus the host thunks.
* `ScriptEventManager` (`Globals::scriptEvents`) — global named events (`fireEvent("W Down")`);
  scripts subscribe through On Event entries, and the mapping is rebuilt on reload. Its
  `initialize()` must run from main before any scripted entity spawns.
* Deferred script-driven mutations (destroy, reparent, spawn requests) are a mutex-guarded
  `EntityChange` queue in ScriptEventManager, drained by the main loop.

### Thunk thread-safety

Scripts tick on workers.

* `ctx->spawnEntity` queues `EntityChange::SpawnAtPosition` and returns null — the DSL types
  `world.spawn` as `Entity?`, so scripts take the miss branch on the spawning frame.
* `internString` is shared_mutex. Script RNG is thread_local. `spatialQueryRadius`'s buffer is
  thread_local.
* The DSL array registry is CHUNKED (`g_arrayChunks`, 64×1024, handle = 16 bits), so storage never
  moves and resolve is lock-free.

---

# Multiplayer

`Entity:NetworkManager` (`Globals::networkManager`) plus `NetworkComponent` (ID 8).

## State layout

The inline component is identity only: netId, ownerClientId, and `unique_ptr<NetEntityState>` — 16
bytes. All sync state lives in `NetEntityState` (NetworkComponent.ixx), allocated only inside a
session.

Role-exclusive state is a UNION of `ServerState` / `ClientState` — a process is one role for life,
and both are trivially destructible. Only `input`, `transferredOwnership` and `sleepDirty` are
dual-role. **Cross-role reads are bugs.**

The manager never null-checks `state`; the component's own `update()` and outside readers
(EntityEditor, InputControls) must.

## Roles and the thread contract

One App.exe for all roles:

```
--server [--port N] [--headless] [--tickrate N]
--connect <ip[:port]>
--no-encrypt
(no flags)  = single player (ENetRole::None, everything inert; registerTweaks() still runs)
```

Hard cap `MaxClients` 32.

**Thread contract.** The manager writes on the main thread pre and post the parallel pass —
`receive()` before, `send()` after. `NetworkComponent::update` reads and writes only its own entity's
block, on workers. Client physics corrections go through the thread-safe body-command queue
(`teleportBody` / `queueBodyCommand`) — never direct box3d writes from workers.

## Headless server

`headlessServer` branching inside main() — one init sequence and ONE main loop for all modes. No
window, renderer, UI or input.

`World::setHeadless(true)` BEFORE any spawn makes `buildTemplate` emit only Scene / Physics / Script /
Network components.

> Component thunks do NOT null-check their handle. `//@@require`'s live `requirementsMet` gate is the
> guarantee that a script never reaches an absent component, so a script using a stripped component
> simply never runs its entry points headless. Cross-entity thunks still `getComponent`-check, and
> `Renderer::isInitialized()` gates the few thunks that touch renderer state.

Hull and Mesh colliders still build — the container name is parsed textually, so the import is
renderer-free.

**Loop:** entity-change drains → receive → scriptContext → physics → `world.update` → send →
Sleep-based tick limiter. Ctrl+C is a clean shutdown; there is a 5 s status line.

**Deliberately skipped headless:** terrain / ocean / scatter plus the terrain collider
(camera-centered streaming — the test scene carries its own ground), buoyancy, and audio.

Spatial entries DO register headless (every entity, as points — see the SpatialEntry section) and
main.cpp commits them each frame, so radius queries work server-side.

## Snapshots (server → clients, Unreliable ch0, per peer)

The server streams registered entities' LOCAL pos and rot at "Network/Snapshot Hz", chunked under
"Snapshot max bytes". Per-record staleness is dropped per entity by tick — chunk reordering is
harmless, which is why this is deliberately not UnreliableSequenced.

Two phases per tick (`sendSnapshotTick`):

**1 — OBSERVE** (`observeSnapshotTick`, every entity once). Sample the wire record (pose, velocities,
NetRecFlags, game blob — the claim passthrough advances here) into `ServerState::obs*` and stamp
`changedTick` on any difference.

* An AWAKE body changes every tick. A sleeping one changes only on the sleep edge, or on a blob or
  flag change. A non-physics entity changes when it moved past the epsilon tweaks.
* It also rebuilds the flat `m_tickEntities` array and the players' positions by clientId
  (`m_playerFocus`).

**2 — SEND** (`sendSnapshotTo`, per ready peer). The peer has a SLOT (`PeerStream::slot`,
`m_peerSlotMask`, zeroed across every entity's `sentTick[slot]` at Hello), and every entity is TIERED
by distance to THAT peer's own player(s):

| Tier | Radius | Cadence |
|---|---|---|
| NEAR | < "Network/Relevance/Near radius" 40 | every tick |
| MID | < "Mid radius" 100 (= the sim LOD outer tier) | every "Mid every ticks" (2) |
| FAR | beyond | every "Far every ticks" (4) |

A peer with no player yet sees everything as far.

An entity is DUE when `changedTick > sentTick[slot]` and its tier's cadence tick is up, or on its
KEYFRAME rotation (`serverTick % K == netId % K`): near/mid "Keyframe every ticks" 20, far
"Far keyframe every ticks" 200, and a SLEEPING body at least "Asleep keyframe every ticks" 100.
**Keyframes are the loss repair — nothing else re-sends an unchanged entity.**

Each tier is walked round-robin from the peer's own cursor, near first under
"Max entities per tick" (300, per peer), with far additionally capped by "Far max per tick" (100).

Far units DO move — the Game's far tick walks a wave in from the spawn ring — and the client only
needs them roughly right until they cross its player's radius.

**Client side.** An UNSELECTED root (sim LOD: beyond every player's outer radius, never visited, so
`NetworkComponent::update` never runs) gets its snapshot pose applied DIRECTLY in `handleSnapshot`,
under the teleport contract: body + prev/curr stomp + step claim + entity pos + spatial entry,
velocities zeroed. The LOD selects by the entity's local position, so without this a far unit sat at
its spawn point forever while the server's walked into range — the client saw a wave only as the few
units that spawned near a player.

One shared stream (`sendToAll`), so relevance is near-ANY-player, not per peer: the change-detection
state is per entity. Rotation is smallest-three quantized behind a live "Quantize" toggle carried in
the header.

Clients simulate locally and correct in `NetworkComponent::update` per the "Network/Correction"
tweaks; targets are published by `receive()` pre-pass, and the component only reads.

## Ownership

Everything is server-owned by default. `netId` is minted only by the server, never authored —
`Component Network` in a `.pre` is pure presence, with no children.

`registerEntity` on the server creates or extends the root's spawn record (prefab, id base,
transform), announced as a reliable Spawn and REPLAYED to late joiners. **That replay is how the
world arrives at connect.**

### Spawn stream (for unbounded unit counts)

Spawn and Despawn are NEVER sent directly.

* Every ready peer has a `PeerStream` of OWED baseIds — the join replay is queued at Hello, and each
  frame's announces and despawns are appended in `send()`.
* `drainSpawnStreams()` packs them many-per-message (`[u16 count]` + records, one message per reliable
  window slot) only while `NetHost::getQueuedReliable(peer, ch2)` is under
  "Network/Spawn stream/Queue target" (64), and at most "Records per frame" (128) per peer per frame.
* The transport DISCONNECTS a peer whose reliable queue passes 1024
  (`maxQueuedReliablePerChannel`), which a 100-per-frame wave trickle or a 20k-record join replay hit
  immediately before this.
* Ids never recycle, so an owed spawn whose record is gone was despawned unsent and is skipped.
  Despawns drain before spawns — a despawn for an unsent spawn is ignored client-side, and a spawn
  can never follow its own despawn.
* Ownership that diverged from a record (`transferredOwnership`) rides as an OwnerChange right behind
  the batch that carried the spawn.
* The server title bar shows `spawn backlog N` while a peer is owed anything.

The Spawn message also carries the root body's live velocities, sampled at send — i.e. AFTER
same-frame launch impulses — and the receiver caps and applies them. That way a replicated
projectile's local twin flies instead of dropping from rest: corrections near the receiver's own
capsule are grace-suppressed, so the initial velocity is the only thing moving it there.

### Ids

Clients adopt server ids in tree-DFS order. Any other registration returns netId 0 = local-inert,
never synced — so client-only and server-only entities are just entities without a NetworkComponent,
and id conflicts are structurally impossible.

Static scenery that never moves should carry NO NetworkComponent. Unregistering the base id sends
Despawn.

**Never store NetPeerIds on entities** — they recycle. Clients get a stable `clientId` at
Hello/Welcome.

`setOwner(root, clientId)` must run in the spawn's own frame (the record rides `m_dynamicRootIds`,
purged at `send()`); later transfers use the per-entity OwnerChange message.
`NetworkComponent::authority()` derives Local / ServerOwned / LocalOwner / RemoteOwner (worker-safe).

## Claims (owner → server, Unreliable ch3)

The owner simulates freely and streams input plus claimed pose and velocities, PHASE-LOCKED to the
physics step: `send()` fires them only on frames where `getStepCount()` changed.

"Max update Hz" is only a thinning limit, counted in WHOLE STEPS
(`stepCount - lastClaimStep >= round(stepHz / maxUpdateHz)`) and never against the wall clock.

> A step boundary falls partway through a frame while the net clock advances in whole frames, so a
> time comparison at the step rate measures just-under-interval and DROPS that step's claim, which
> the passthrough extrapolates over and double-jumps — remote-client pulsing that vanished only when
> the limit was raised until it stopped binding. Paused physics = no claims, correct.

Each packet carries the last "Claim redundancy" claims (default 4, ring cap 8) — redundancy beats
reliable under loss. The packet record count comes from `validCount`, never seq, so a re-acquired
entity cannot replay a previous owner's poses.

Quantized like snapshots, with live ranges in the header. Claims follow the sleep policy
(`sleepDirty`): every awake tick, one rest-pose claim on the sleep edge, then silence.

## Validation ("Network/Validation" tweaks)

* A movement token bucket in metres, refilled at "Max speed" per second of SERVER WALL CLOCK — never
  per claim, since the packet rate is attacker-controlled.
* Linear and angular velocity caps on EVERY accept path.
* A trajectory raycast ("Path raycast" toggle), and a hard teleport cap.
* The first claim seeds the anchor but is still checked.
* "Max speed" (default 60) must exceed the fastest legitimate motion, free fall included.
* Every float off the wire is finite-checked and quaternions re-normalized — NaN passes comparisons
  and poisons the solver.

**Accepted** → the twin FOLLOWS the claim through the solver: "Twin follow gain", a bounded
`NudgeVelocity` = claimed velocity plus a corrective velocity toward the claimed pose; a hard
teleport only past "Twin resync (m)".

> It is deliberately NOT teleport-pinned per claim. That collapsed the render interpolation (prev and
> curr both became the claim pose) and, since claims are sampled on the OWNER's step clock while the
> server steps on its own, yanked the twin backwards whenever two server steps fell between two
> claims — pulsing visible on the SERVER's own view. Other clients are unaffected either way, because
> the passthrough sends them the owner's stream, not the twin.

`lastAcceptedClaimPos` anchors the next budget and the re-emit to other clients through CLAIM
PASSTHROUGH: the snapshot re-emits the newest accepted claim state instead of sampling the wobbling
twin, extrapolating by claim velocity for a bounded gap. "Owner predict (ticks)" adds a constant
forward shift at emit.

**Rejected** → `violations`++ plus `NetRecFlag_Forced` on snapshots for "Forced ticks". The owner
ignores corrections UNLESS Forced, an accepted claim clears Forced immediately, and
`updatePlayerControl` yields input while Forced — steering against the correction is a tug-of-war.

## Interaction grace

"Interaction radius" / "linger". Server-owned bodies near OUR claim-driven bodies suspend
corrections: they would fight the push the player is applying, and the twin gets the same push an RTT
later. Proxy positions are published in `receive()` and read from workers.

## Transfer, steal and arbitration

"Network/Ownership" tweaks, with a "Transfer enabled" kill switch.

* Awake server-owned dynamics within "Transfer radius" of a client's PRIMARY hand over through a
  per-entity reliable OwnerChange; the server resets that entity's claim and validation state. They
  revert after "Release delay" outside "Release radius".
* Primaries only as transfer sources (`transferredOwnership` distinguishes; client-side it means
  "owned but not the player" — `updatePlayerControl` skips it).
* **The SERVER's own player body registers as a primary too** (`setServerPrimary`, source clientId 0;
  the Game layer marks it at spawn). It is never handed to a client, and it RE-CLAIMS transferred
  objects by proximity — only ever transferred ones, never a client's primary. Its snapshot records
  carry `NetRecFlag_ServerPlayer`, so observers route it into the remote-player INTERP ring and skip
  the interaction grace: it is an actively-steered body, and grace-freeing it for local shoves only
  fabricates divergence — the shove reaches it through the server sim, like pushing another client's
  player.
* **Contact-driven STEALING** (`stealOwnershipOnContact`, wired as the primary's `onContact` in
  main.cpp; needs `ContactEvents true`): last collider owns, and it never steals another client's
  primary.
* **Primary-vs-primary contact ARBITRATES** both for a window: twins go solver-owned, claims apply as
  bounded `NudgeVelocity`, records carry `NetRecFlag_Arbitrated`, and input keeps steering. An object
  touched by two distinct clients within "Contest window" is CONTESTED → server-owned until the
  window decays.
* Late joiners get OwnerChange replays after the Spawn replay. A disconnect reverts transfers before
  tearing down primaries.
* CLIENT side, `setOnServerLost` fires (main thread, inside `receive()`, BEFORE the auto-reconnect
  attempt) when the server connection drops — the App decides: menu sessions return to the menu next
  frame, CLI clients keep reconnecting. **Never shut the host down inside it.**
* `setOnClientJoined` / `Left` (main.cpp) spawns and tears down
  `Entities/Debug/netPlayerCapsule.pre` per client — an upright `LockRotation` capsule, with the same
  movement and camera as the local player capsule.

## Physics sync

A dynamic-body entity syncs the BODY: world pose plus velocities, `NetRecFlag_Physics`; velocities
are 3×u16 over live "Max vel" / "Max ang vel" ranges carried in the header. The server sends every
awake tick, one sleep-edge record, then keyframes.

**DEFAULT client correction is the PHYSICAL PUSH.** Error × `Push pos/rot gain` becomes a corrective
velocity on top of the server's (capped; delivered as bounded impulses), so contacts and gameplay
impulses COMPOSE. No teleport on this path, and bodies still sleep inside the deadzones.

INSIDE the deadzone the velocity still settles onto the server's — a `NudgeVelocity` toward
`targetLinVel` / `AngVel` whenever they differ, never for a body already matching, so rest stays
asleep.

> The push leaves its corrective term in the body, and a FRICTIONLESS body (units) kept it forever,
> coasting out the far side of the deadzone and back — the client-side swinging.

Past the snap thresholds it enters CATCH-UP (boosted gains, still through the sim); a hard
`teleportBody` resync happens only at "Pos teleport threshold".

**MASS SCALING.** Gains, velocity caps and acceleration limits — the deadzone settle included —
multiply by `clamp(mass / "Push mass reference (kg)", "Push mass scale min", 1)`. A light body answers
every contact impulse strongly, so in a packed crowd a full-strength correction compounds with the
neighbours' pushes into swinging (the swarm). Bodies at or above the reference correct at full
strength.

### The teleport contract

EVERY teleport must also stomp `PhysicsComponent::prevPos` / `currPos` / `prevRot` / `currRot` AND
claim the step (`physics->lastStep = getStepCount()`).

`NetworkComponent::update` runs BEFORE `PhysicsComponent::update` on the same entity, so on any frame
the sim stepped it would otherwise overwrite `curr` with `body.getPosition()` — which still holds
LAST frame's teleport, since teleports apply at the next `physics.update` — and render
`mix(thisFramePose, lastFramePose)`, i.e. BACKWARD, on stepping frames and correctly on the others.
That alternation was the remote-entity pulsing.

### Remote-owned entities

Other players use BUFFERED SNAPSHOT INTERPOLATION instead. "Remote interp (ticks)", min and default
2 — it must exceed 1 or playback degenerates to snapshot arrival. **Buy smoothness with snapshot
RATE, not depth.**

A `NetSnapshotRing` per entity (manager-owned, main-write / worker-read) plays back at
`newestTick − interpTicks` in server tick units, teleport-following with matched velocities and
prev/curr stomps. Asleep records hard-sync once, then queue `SetAwake 0`.

Kinematic and static bodies use the entity-transform path.

## Events

`fireNetworkEvent(name[, data])` = a local fire plus Reliable ch1; the server relays to other
clients.

* Payload ≤ `MaxEventDataBytes` 1024, static_asserted against the transport's
  `netMaxSinglePacketMessage()` so a max-size event still rides in ONE packet.
* Readable during dispatch through `currentEventData()` / `currentEventSender()` (script:
  `networkEventSender()`).
* The sender clientId is SERVER → CLIENT only: the server derives it from the receiving peer and
  re-serializes the relay — **never trust a client-writable identity.** A client-supplied sender netId
  resolves through `findOwnedEntity`, and is null unless that client owns it.
* `setEventFilter(clientId, name, data, senderEntity)` gates client-originated events; return false =
  dropped, not relayed. No filter = allow all — **install one before any event grants state.**
* Script event sends queue under a mutex, drained by `send()` (NetHost is main-thread-only).

## Session

A 4-way transport handshake gates on `GameProtocolId` — **bump it on ANY wire change**; NetFuzz's
game mode mirrors it. Then Hello → Welcome on Reliable ch2 marks the peer ready. Per-peer state MUST
clear on Disconnected.

Events (ch1) have NO ordering vs the ch2 Welcome, so a client PARKS events that arrive before its
Welcome is processed (`m_preWelcomeEvents`, bounded, replayed in order right after). The server's
join-time burst — game world replay, tweak sync — is fired in the same frame as the Welcome and was
silently lost before this (already acked = never resent). **Anything join-critical sent as an event
relies on this parking.**

Encryption is on by default at the NetworkManager layer (`s_encrypt = true` →
`NetHostConfig::encrypt`, whose own default is false). It cannot change on a live host, so call
`setEncryption()` before start; it is driven by `--no-encrypt`, and both ends must match. Without it
a peer is identified only by source address, which is trivially forged on a LAN.

## Testing

* Server key U throws `Entities/Debug/netPhysCube.pre`.
* Client key C toggles player control (see Testbed keys). Key K fires "NetPing".
* Demo `Entities/Debug/networkTest.pre` spawns at a FIXED transform — never camera-relative, since
  both worlds must match.
* Link-sim tweaks (loss / latency / jitter) bind into `m_host.config()`. The window title shows role
  plus RTT / loss / KB/s.

## Player control

Key C on a client, `InputControls::updatePlayerControl`.

WASD and Space velocity-steer every locally-owned dynamic body camera-relative ("Network/Player"
tweaks: Move speed / Accel / Jump speed — **keep Move speed under Validation's "Max speed"**). The
Space jump is gated on a downward raycast, and fly-camera movement is released while active.

The owning client simulates its body directly — main thread, before `physics.update`, where direct
setters are sanctioned — writes intent into `comp->input`, and the claim stream carries the resulting
state.
