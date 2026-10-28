# Entity

> Library documentation for `Code/Entity`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The ECS, and the widest library in the engine: it links RendererVK, File, Script, Physics, Audio,
Particle, Force, Spatial, Threading, Network and Nav.

## The documentation files

| File | Covers |
|---|---|
| [SimLod.md](SimLod.md) | SIM LOD: the tweaks, focus + zones, the visit set, tiers, per-entity kinds, unstamped visits, force-bubble and buoyancy gates, the delta, dormant and wake edges, known gaps, the selection job |
| [Components.md](Components.md) | The components: the component table, the bone models (HumanoidAnimator, the parked SceneAnimator), Light, the Game components (authoring, structure stores and the transport) |
| [Multiplayer.md](Multiplayer.md) | Multiplayer: ownership model, state layout, roles + thread contract, headless server, wire protocol, snapshots, spawn stream, claims, validation, client correction, remote-owned entities, teleport contract, transfer / steal / arbitration, events, session, testing, player control |

## The three invariants everything else follows from

1. **`sizeof(Entity) <= 64`** — one cache line, static_asserted. Components append at
   `alignUp(sizeof(Entity), 16)` = 64. This is why the name lives outside the entity and why the
   World's scheduling state is two bytes.
2. **An entity and its components live in ONE allocation**, and a spawned prefab TREE is one
   allocator block.
3. **The entity only ever sees the delta it is handed.** All scheduling — SIM LOD tiers, catch-up,
   dormancy — is decided in World; `updateSelf` just gets a float.

## Source layout: the public API split

The same split as RendererVK's ("The public API split" in [RendererVK CONTEXT](../RendererVK/CONTEXT.md)): each class used outside the
library is an API file in `Public/` over a `<Class>State` base in `Private/` with every data member and private helper, all `protected`.
The primary interface `Public/EntityModule.ixx` exports `:Entity`, `:Component`, `:World`, `:NetworkManager`, `:AssetRegistry`,
`:ScriptEventManager`, `:ScriptContext`, `:Prefab` - NOT `:EntityNames` / `:Allocator` (internal).

| API (`Public/`) | State (`Private/`) | Friends of the state |
|---|---|---|
| `Entity.ixx`: `Entity`, `EntityPtr`, `EntitySpawnTemplate`, `EntityChange`, `EComponentID`, `EEntityCullMode` | `EntityState.ixx`: the fields, `EEntityFlags`, `EntityUpdateNode`, the per-entity visit, the tree-allocation helpers | World(State), NetworkManager(State), EntityPtr, SceneComponent, Animator / Physics / GameUnit / GameStructure components, EngineScriptContext |
| `Component.ixx`: the component re-exports, `hasComponent` / `getComponent` / `getSpawnInfo<T>` | `ComponentLayout.ixx`: the inline layout (`inlineSizes`, offsets, `componentTypeName`) | - |
| `World.ixx` | `WorldState.ixx`: the caches, the template builders, the update pass, the SIM LOD selection | NetworkManager |
| `NetworkManager.ixx` (+ `ENetRole`) | `NetworkManagerState.ixx`: the session, the wire protocol (described there), the registry, the component-facing calls | Entity, NetworkComponent |
| `AssetRegistry.ixx` | `AssetRegistryState.ixx`: the tables, the scan, the World-only lookups | World(State) |
| `ScriptEventManager.ixx` | `ScriptEventManagerState.ixx`: the keys, the listener registry, the change queue | ScriptComponent |
| `ScriptContext.ixx`, `Prefab.ixx` | (no state) | |

Rules this library adds to the RendererVK ones (each was an MSVC failure):

* **A State member is never DEFINED in another partition.** `self()` is a member TEMPLATE in the state partition itself
  (`template<typename E = Entity> E& self()`, dependent, so it compiles where the derived class is complete); defining it in the API
  partition made every importer hit an internal compiler error on the IFC import ("base class undefined").
* **An API inline body that takes a `Renderer&` goes in a `.cpp`**: `Renderer` is the unattached `RendererVK.fwd` declaration in the
  Entity partitions, so an inline body there mangles against a different `Renderer` than its callers (an LNK2019 - `updateDetached`).
* A public enum the state needs is opaque-declared there (`export enum EComponentID : uint16;`, `ENetRole`) and defined in the API file.
* `getComponent` / `hasComponent` take `EntityState*` (so the state's own members pass `this`) and read the bits through
  `Entity::getTypeBits()` - no friend templates.

## `Entity`

[Entity.ixx](../../../Code/Entity/Public/Entity.ixx). Fields (in `EntityState`): `pos` / `scale` / `rot` (the only public data), `parent`,
`spawnTemplate`, `spatialEntry`, `refCount`, `typeBits`, `flags`, `updateCost`, `schedTier` (2 bits), `schedTick` (14 bits). Outside the
library they are read through `getParent()`, `getSpawnTemplate()`, `getTypeBits()`, `getSpatialEntry()`. A tree not in the World (the
gizmo entity) is updated by its owner with `updateDetached(renderer, dt)`; `detachFromOwner()` takes an entity out of its parent's
children (the deletion path).

### Names

**There is NO name field.** `getName` / `setName` / `hasName` forward to `Globals::entityNames`
(`EntityNameRegistry`), a pointer-keyed 64-shard mutexed map that owns each name buffer — **but
only for a name SET after the spawn.** A spawned entity's name is its template's `displayName`,
read through `spawnTemplate` (which outlives the entity: World's caches, retired lists and
`keepTemplateAlive`), so `Entity::create` makes no name allocation. A `setName` overrides it.

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

`typeBits` masks 14 types in this fixed order:

```
Scene(0) Render(1) Animator(2) Physics(3) Audio(4) Particle(5) Force(6) Light(7)
Network(8) GameUnit(9) GameStructure(10) GameProjectile(11) HumanoidAnimator(12) Script(13)
```

**Script is LAST so every other component is available when it spawns.**
`getComponent<T>` / `hasComponent<T>` compute byte offsets from compile-time sizes;
`MaxInlineComponentTypes` is 14 and `ComponentAlignment` 16.

Each type is a partition under `Private/Components/` — struct plus `get*SpawnInfo` /
`write*SpawnInfo` in the `.ixx`, bodies in the same-named `.cpp`. `Public/Component.ixx` re-exports them all
and holds the accessors; the layout is `Private/ComponentLayout.ixx`. Every `get*SpawnInfo` is `getSpawnInfo<T>(entity)`: the
template's `spawnInfos` run in component-bit order, so T's recipe is at the popcount of the bits below it.

> Per-component `.ixx`s must NOT import `:Component` (cycle). Cross-component code goes in the `.cpp`
> implementation units.

#### Adding a component type

1. New `.ixx`/`.cpp` pair with a `getId()` struct.
2. `export import` from `Public/Component.ixx`, and an `import` in `Private/ComponentLayout.ixx`.
3. Entries in `EComponentID` (Entity.ixx), `inlineSizes` and `componentTypeName` (ComponentLayout.ixx).
4. Create/destroy switches in Entity.cpp.
5. Parse branch in `World::buildTemplate`.
6. Write branch in Prefab.cpp.
7. **Require-slot fill branch in `ScriptComponent::syncScriptData`** if the DSL can require it — the host reserves
   one pointer slot per required component at the FRONT of ScriptData in ascending bit order, so **a missing
   branch shifts every later slot (a crash).** See [`Docs/Context/Script/CONTEXT.md`](../Script/CONTEXT.md).
8. Entity.natvis offset entry.
9. Bump `MaxInlineComponentTypes`.
10. An Entity Editor section, if authorable.

## Allocation

`EntityPtr` is an intrusive refcounted handle. The count is a plain `uint16` field accessed through
`oc::atomic_ref`, which is what keeps `Entity` inside its cache line.

`Entity::create(template, transform, initialFlags)` placement-news into `Globals::entityAllocator` —
an atomic chunk bump plus tagged per-size-class Treiber free lists, blocks ≤ 64 KB (only the rare 1 MB chunk add
takes a spinlock). `initialFlags` applies **before components spawn**, so e.g. Frozen is already visible to a
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
not frozen / paused:  [sim step] GameUnit → GameStructure → GameProjectile → Script
                      → Network → [sim step] Animator → Physics → [sim step] HumanoidAnimator
  ([sim step] = only when delta > 0)
  → compose world transform
  → RenderComponent::place, then the culling tail: refresh the SpatialIndex entry (RootOnly root:
    cached subtree bounds; else the node bounds; no node: the entity position, radius 0),
    push the RenderNode under the pass mask (own getPassMask when Spatial culling mode >= Cull,
    or the mask inherited from a RootOnly ancestor — see CullMode)
  → HumanoidAnimator::place → Audio → Particle (not frozen) → Force → Light → emit children
```

A disabled entity returns at the top (after suspending its subtree's physics once). `updateSelf` emits its
children into a caller-supplied vector rather than recursing, which is what lets the parallel pass slice them.

## Spatial entries

**EVERY entity registers at the end of `Entity::create`** — parallel-spawn safe, since registration is
lock-free — **unless its `CullMode` says otherwise (below).**

* Layer `SpatialLayer_Entity` always, plus `SpatialLayer_Render` when it has a render node.
* Bounds come from `RenderNode::getWorldBounds`, skinned inflated by the culling config's
  `skinnedRadiusScale`; otherwise a point at the spawn position. **For a tree CHILD that is its LOCAL
  position** — the entry links at the next commit, and the child's first visit re-places it in world
  space before any query can see it.
* Headless has no render nodes, so every entry there is a point.
* Gameplay queries use the Render layer; **the Entity layer is the World's update-selection layer.**
* Headless still registers and main.cpp calls `commitFrame()` in place of the cull job, so radius
  queries work server-side.

### `CullMode`

Entity-level `CullMode PerEntity|RootOnly|None` in the `.pre` (`EEntityCullMode`). **It lives on
`EntitySpawnTemplate::cullMode`, not on the entity** (no room in the cache line); the entity reads it
through `spawnTemplate` (`getCullMode`).

| Mode | Registration | Render gate |
|---|---|---|
| `PerEntity` (default) | Its own entry, as above. | Its own pass mask. |
| `RootOnly` | This entity registers ONE entry over the render bounds of its whole spawned subtree. **Everything spawned under it registers nothing** (`hasRootOnlyAncestor`, by template mode — the ancestor registers after its children). | The root reads its pass mask once and hands it down in `EntityUpdateNode::cullPassMask`; a covered descendant does NO spatial work. |
| `None` | No entry. Not inherited by children. | Never culled (`PASS_ALL`). |

* **The subtree bounds are a lazy per-TEMPLATE cache** (`treeCullBounds`, root-LOCAL space, skinned
  nodes inflated; state stored last, benign race like `treeAllocSize`): measured once from the first
  spawned tree (`gatherTreeCullBounds` composes the authored local transforms — child render nodes sit
  in parent-local space at spawn), then placed by the root's world transform every visit. They are
  the REST pose: a limb swing is not added. **HumanoidAnimator BONES count as render nodes
  here** (`HumanoidAnimatorComponent::addRestBounds`). `refreshTreeCullBounds` re-measures (the
  RespawnEntity change calls it after the child splice).
* An entity WITH an entry always culls itself, also under a RootOnly ancestor (a PerEntity entity
  reparented in). An entity with no entry and no covering root renders unculled.
* **SIM LOD:** an entry-less child is always `simLodSelected` and gets the full delta, so a covered
  subtree is visited exactly when its root is. **A ROOT without an entry joins `m_globalRoots`**
  (`addRootEntity`) — no query can find it, so it is visited every frame. `World::alwaysVisited`
  (Global, or no entry) is the ONE definition of "outside the SIM LOD".
* **`Global` is not `None`:** Global is scheduling only — the entity keeps its entry, so it is still
  culled and still found by gameplay queries (every structure). `terrainroot.pre` carries both: None
  drops its useless point entry, Global keeps the children's real-stamp selection rule.
* Gameplay queries see only the root of a RootOnly tree (Render layer when any subtree node renders),
  and never a `None` entity. `renderIsVisible` goes through `Entity::getCullOwner`.
* Teleports outside the visit (`GameUnitComponent::updateFar`, the client snapshot apply) re-place
  the entry through `Entity::placeSpatialEntry`.
* Entity Editor: the "Cull Mode" combo respawns the selected entity only; children change their
  registration on the next load of the prefab.

Demo: `Entities/Debug/CubeGuy.pre` (RootOnly — one entry for all body parts).

## The parallel update pass

`World::update(renderer, dt)` ([World.cpp:328](../../../Code/Entity/Private/World.cpp#L328)). Always on.

### Continuation batches — no level barrier

A batch job processes a node range from the frame arena, then **immediately slices the children it
emitted into new batch jobs** on ONE pass-wide `JobCounter` that main waits on (helping).

**The only real dependency is parent-before-CHILD**: a child reads its parent solely through the
`parentWorld` baked into its `EntityUpdateNode`, and the child's batch is submitted only after its
parent's batch body ran. Subtrees therefore descend independently — never reintroduce a per-depth barrier, which
stalls every level on the slowest batch of the previous one.

**The first budget of a batch's children runs on the SAME fiber**: `submitEntityBatches` hands the
first batch back to the calling `updateBatchJob`, which loops on it, and only the remaining batches
become jobs. A unit's few child parts are therefore updated by the job that updated the unit — one
job per subtree BUDGET, not one per level per parent (which at 40k units exhausts the job pool). The root slices
still submit every batch (they are the fan-out). The other continuations land on that worker's LIFO deque, so wide
fan-outs get stolen while the warm subtree stays put. Batches submit at **`EJobPriority::High`**
under the name `"Entity Update"` — the pass is the frame's critical path, every batch is bounded, and
High is what the window-thread helper serves between pumps.

### Cost budgeting

Every entity carries `updateCost`, a **u8 in 250 ns units, MEASURED**: 0 = unmeasured, the entity's
FIRST update is timed and becomes the cost — **no guessed initial value, the first update IS the
guess** — then a ~1/1024-per-frame random re-measure keeps it honest. Unmeasured counts as 1 so it still
partitions.

A batch fills until the summed cost reaches the budget `m_updateBudget` = `m_batchTimeUs` (200 µs) over
`m_updateCost.nsPerItem()`, clamped to 1..4096. `m_updateCost`'s EMA is fed COST UNITS as its item count, **so
`nsPerItem()` self-calibrates to "ns per cost unit".** There is no even-split term: **parallelism comes from the
fan-out itself.**

### The frame arena

`m_updateArena` is claimed with an atomic bump that is **NEVER rolled back**, is pointer-stable during
the pass, and is resized **only between passes** (jobs hold pointers into it) from last frame's use
plus overflow, doubled (minimum 4096).

A claim past the end **spills into a block of its own** (`m_updateArenaSpill`, allocated under a
mutex — the rare path, since a burst such as a wave spawn or a camera swing is what gets there),
pointer-stable like the arena and freed between passes once the arena has grown to cover it. The
pass stays parallel; do not fall back to running the overflow serially on the submitting thread (on main that
cost 20 ms on a burst frame). Batch jobs address nodes by POINTER, never by arena index, so arena and spill
batches are the same to them.

`updateSelf` emits children into `PerWorker` staging, and `submitEntityBatches` copies them into the
arena in **ONE claim BEFORE the first submit.**

> **Load-bearing, not an optimization:** a submit can execute its job INLINE on queue exhaustion, and
> an inline child batch reuses the caller's staging slot — so nothing may read the staging pointer
> once the first submit goes out.

**Neither the batch job nor `updateSelf` ever fiber-waits**, which is what makes one `PerWorker`
staging slot per worker sufficient; `updateBatchJob` holds a `ThreadLocalScope` to assert it.

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

# Parallel entity spawning and destruction

`Entity::create` / `Entity::destroy` are callable from worker jobs **CONCURRENTLY WITH EACH OTHER**,
in the same frame window where main-thread spawning is legal — the entity-change drains and
`game.update`, after the spatial/begin-frame joins. **Never during the parallel entity pass, the
physics step, or present.**

## `World::spawnBatch(span<SpawnRequest>, addRoots = true)`

The sanctioned entry. **Templates resolve on MAIN** — the template, container, clip and audio caches
are not job-safe — then a cost-grained parallelFor runs only the `Entity::create` calls (each under
a per-prefab `ProfileScope`, nested in "Spawn batch"), and roots attach serially after the join.

A request name **with an extension** resolves like `spawnAssetFile` with
`overrideDefaultTransform = true` (position replaced, rotation composed onto the authored default,
authored scale kept); a plain name resolves like `spawn()`. Results stay index-aligned with the
requests, with a null entry for an unknown prefab.

## `World::releaseBatch(vector<EntityPtr>&&)`

Fans the releases — and so the `Entity::destroy` of every last reference — over a parallelFor ("Destroy batch").

**A handle that is not the entity's LAST reference just decrements**, so callers drop every OTHER
owner they mean to (the root list through `removeRootEntity`, so its roster callback stays serial;
then rosters) **BEFORE this, on main** — only the teardown itself runs on workers.

Users: the EntityChange Delete drain in `handleEntityChanges` (which moves the Delete handles out
after the serial pass, so the teardown is one parallel batch), the Game load path's unit + shot
despawn (`NpcSystem::despawnUnitsAndShots`), and **`World::clearRootEntities`** — the whole-world
wipe (game teardown, exit-to-menu): it moves the root list out, clears the pending / global lists,
fires NO removal notifications, and releases every root through this batch. A root some other
holder still references survives it, so the teardowns drop their own `EntityPtr`s first.

## What makes it safe, per resource

Each seam locks ONLY create/destroy — **the parallel-pass hot paths stay lock-free.**

| Resource | Protection |
|---|---|
| EntityAllocator, `treeAllocSize` / `treeNetworkCount` caches | Already lock-free / benign-race |
| Spatial `registerEntry` / `unregisterEntry` | LOCK-FREE, and so are the `query*` beside them: the record pool's rows are `VirtualArray`s that never move (see "The record pool" in Spatial). The cull kick/join window still forbids registration. |
| `OcclusionBuffer` add/remove | `m_occluderMutex` (PhysicsComponent spawn and resume) |
| box3d body create/destroy | The Physics-module `g_bodyLifecycleMutex` |
| Renderer transform slots, LOD state ranges, force / particle slots, solid-colour tint HITS, skinned despawns (queued, parked at beginFrame) | LOCK-FREE (RendererVK `Util/SlotAlloc.ixx`; see "The per-entity spawn allocators" in [RendererVK Scene.md](../RendererVK/Scene.md)) |
| Renderer registries — mesh/material/instance-offset tables (adds AND releases), LOD groups, skinned bundles/palettes/job ranges | ONE RECURSIVE `Renderer::m_spawnMutex`; `spawnSkinnedNode` holds it over its bundle work |
| A NEW solid-colour material | Its own `Renderer::m_solidColorMutex` (the texture upload must not run under the spawn mutex; order `m_solidColorMutex` → `m_spawnMutex`) |
| `MeshDataManager` range alloc/free, `TextureManager` upload/free | Mutex each |
| ForceSystem create/destroy | `m_createMutex`, plus `m_emitters` / `m_queries` RESERVED at initialize so growth never reallocates under a concurrent handle resolve |
| ParticleSystem effect create/destroy/cache | Mutex held only for the map and vector work — the `.pfx` load, texture loads and renderer slot creation run outside it |
| `ScriptHost::getOrLoad` | `m_loadMutex` (cacheKey canonicalization outside) |
| ScriptEventManager listeners | A plain mutex; `fireEvent` SNAPSHOTS the dispatch list under it and invokes the scripts after releasing |
| NetworkManager registration | A recursive `m_registerMutex`, taken only for a registration that gets an id: a LOCAL-INERT one (single player, client-local content: role None, or Client outside a replicated spawn) returns before it. See below. |
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
`waitForDispatches = false` (the default).

---

# `World` (`Globals::world`)

## Spawning and roots

* `spawn(name, transform)`, `spawnAssetFile(path, transform, overrideDefaultTransform = true)`,
  `createEmptyEntity(name)` — **archetype 0, NO components, so it cannot hold children.** A grouping
  root needs a `Component Scene` prefab, e.g. `Entities/Game/terrainroot.pre`.
* `spawnAssetFile` = `resolveAssetTemplate(path)` + `spawnTemplate(tmpl, transform, override)`.
  **A caller that spawns the same file every frame holds the template** (NpcSystem's projectile
  shells): a resolve is a lexical path normalization plus two keyed lookups — several heap strings
  per call. `templateGeneration()` changes on `reloadPrefabs` / `invalidatePrefab`; a holder
  re-resolves when it sees a new value. Every spawn runs inside a `ProfileScope` named by the
  template's `displayName` (the single route and the batch job alike), so the memory panel
  attributes spawn allocations per prefab.
* `addRootEntity` puts an `alwaysVisited` root (Global, or no spatial entry) on the always-visited list, and every
  other root on the pending list, visited unconditionally until a selection job could have found it (see
  [SimLod.md](SimLod.md)).
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
Network** components. Everything renderer-touching (Render, Animator, HumanoidAnimator, Light, Particle, Force),
Audio, and the Game components (they create Force queries) are dropped at build time, **so `updateSelf` never
dereferences the uninitialized renderer and no GPU resource is ever created.**

Hull and Mesh collision still works: the Render node's container NAME is parsed textually and the
geometry comes from the renderer-free `ensureCollisionSource` import.

## Contact dispatch

`World::handleContactEvent` is the physics contact callback, passed to `dispatchContactEvents` from
main AFTER the spatial and begin-frame joins — **contact scripts query the index and can touch
renderer state.** It fires, for each side:

1. `PhysicsComponent::onContact`
2. `GameProjectileComponent::onContact` — the shared game-layer behaviour: a projectile spends itself
   on first touch and damages an enemy-team victim
3. the script's `OnPhysicsEvent` (`ScriptComponent::firePhysicsEvent`)

---

# Assets, prefabs and tint

* **`AssetRegistry`** (`Globals::assetRegistry`) — `scanDirectory()` (cwd = `Assets/`) registers
  ObjectContainers (`.oc`), clips (`.anm`), animator graphs (`.apl`) and prefabs (`.pre`) by name.
* **`savePrefab(root, path)`** serializes through the per-component `write*SpawnInfo`s — **the spawn
  recipe IS the serialization.** Spawned prefabs are locked instances, unpackable in the Scene panel;
  `prefabWouldCycle` guards recursion.
* **Per-entity tint:** `Component Render` takes `Color r g b`. `RenderComponent::spawn` overrides
  every mesh instance's material through `RenderNode::setMaterialOverride` plus
  `RenderMaterial::getOrCreateSolidColor`, cached per RGB8 — a 1×1 texture and a MaterialInfo minted once
  per colour. Instances carry `materialIdx` per record, **so there are no shader or layout changes.**
  All Game prefabs author unique colours this way.

---

# Script glue

Lives here, not in Script.

* **`Globals::scriptContext`** — the ABI singleton plus the host thunks.
* **`ScriptEventManager`** (`Globals::scriptEvents`) — global named events. Names map to a `uint32`
  `EventKey` through a shared_mutex-guarded registry (`getEventKeyForName` interns, `findEventKey`
  looks up), so **a fire is one key lookup plus an `LPMultiMap` dispatch**, not a string compare per
  listener. Its `initialize()` must run from main before any scripted entity spawns.
* Deferred script-driven mutations — destroy, reparent, spawn requests — are a mutex-guarded
  `EntityChange` queue drained by the main loop through `takeEntityChanges()`. **At exit `~World`
  drains it** (a Delete queued after the last frame's drain): `Entity::destroy` reads the entity's
  `spawnTemplate`, which World's template maps own, and `scriptEvents` (XCU7) must outlive `~World`
  (XCU8) because dying ScriptComponents unregister from it — so the queue cannot simply die with
  its owner.

## Thunk thread-safety

Scripts tick on workers, every entry point under a `ThreadLocalScope`: **no thunk may wait.**

* `ctx->spawnEntity` queues `EntityChange::SpawnAtPosition` (`ScriptEventManager::addSpawnRequest`) and
  **returns null** — the DSL types `world.spawn` as `Entity?`, so scripts take the miss branch on the spawning frame.
* `internString` is shared_mutex; script RNG is a `thread_local` `mt19937`.
* `spatialQueryRadius` writes straight into the caller's array (no scratch). `worldQueryRadius` hands out a
  generation-tagged handle into a `thread_local` ring of 8 result buffers (queries nest inside `foreach`); a handle
  older than 8 further queries reads as empty, never as another query's results.
* The DSL array registry is CHUNKED (`g_arrayChunks`, 64 × 1024, handle index = 16 bits), **so storage never
  moves and resolve is lock-free.** Only allocation and the free list take a lock.
