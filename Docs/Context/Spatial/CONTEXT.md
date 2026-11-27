# Spatial

> Library documentation for `Code/Spatial`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The spatial index: culling and position lookup, **independent of the entity parent/child
hierarchy**. Links Threading (and Settings, for its tweaks).

## Source layout: the public API split

The same split as RendererVK's ("The public API split" in [RendererVK CONTEXT](../RendererVK/CONTEXT.md)) and Entity's (which
adds the MSVC rules it follows: each state's `self()` is a member template in the state partition, no state member defined in
another partition). The primary interface `Public/SpatialModule.ixx` exports `:SpatialTypes`, `:SpatialIndex` and `:Occlusion` only.

| API (`Public/`) | State / internals (`Private/`) | Friends of the state |
|---|---|---|
| `SpatialTypes.ixx`: `SpatialHandle`, `SpatialEntry`, the layers + `SpatialTerrainTag_Ocean`, `ESpatialPass` + `SpatialPassBit_*`, `rebaseFrustum`, `SpatialIndexDesc`; re-exports `Settings.Spatial` | `Types.ixx`: `SpatialStamp`, `IOcclusionTester`, `inflateFrustum` (and it re-exports `:SpatialTypes` to every internal partition) | |
| `SpatialIndex.ixx` | `SpatialIndexState.ixx`: storage, commit, traversals, the cull job (`update`), `markVisibleSet` / `markVisibleSphere`, `unregisterEntry`, the hand-over buffers | SpatialEntry, SpatialStressTest |
| `Occlusion.ixx`: `OccluderData`, `SpatialOccluder`, `OcclusionBuffer` | `OcclusionState.ixx`: the raster, `render`, `removeOccluder`, the `IOcclusionTester` answer | SpatialOccluder, SpatialIndexState (the cull job) |
| | `Morton.ixx`, `CellMap.ixx`, `RecordPool.ixx`, `BlockStore.ixx`, `Stress.ixx` | |

A nested state type the API needs is re-published with a type ALIAS (`using ECollect = SpatialIndexState::ECollect;`), not a
using-declaration: MSVC keeps the base's protected access on a nested enum named through a using-declaration.

## The one call the App makes

`SpatialIndex::update(camera, frustum, viewProjRelCamera, sunDirection)`
([Query.cpp:1167](../../../Code/Spatial/Private/Query.cpp#L1167)) is the whole per-frame spatial step, and it runs **as the
High job "Spatial cull"**:

```cpp
Globals::spatialIndex.kickUpdateJob(Globals::rendererVK.setFrameView(camera, viewportRect));
...
Globals::spatialIndex.joinUpdateJob();
```

In order, `update` does:

1. **`commitFrame()`** — applies the cell moves queued during last frame's entity updates.
2. **`m_culling.maxDist = camera.far`** — the main-pass cull distance tracks the render camera's far
   plane, so terrain and entities stream to exactly the view distance rather than a fixed cap.
3. Returns early if the mode is `Off` or `freeze` is set.
4. **Occlusion raster**, when the CPU occlusion buffer is enabled.
5. **The Main and Shadow stamps, in ONE traversal** — Main: the margin-inflated camera frustum over `SpatialLayer_Render |
   SpatialLayer_Terrain` (it also fills the visible-set hand-over lists). Shadow: that frustum swept toward the sun, over
   `SpatialLayer_Render` (see "The Shadow pass").
6. **The Near stamp** — a camera ball over `SpatialLayer_Render` only, behind requery hysteresis.

The SIM LOD tier stamps are NOT made here — see "The SIM LOD tiers are NOT stamped by the cull job".

### The kick/join window

main.cpp kicks right BEFORE `physics.update`, next to the renderer's "Begin frame" job, and joins
after the physics step, `audio.update`, `navSystem.update` and the begin-frame join
([main.cpp:298-307](../../../Code/App/main.cpp#L298)).

> **The index must stay QUIESCENT between kick and join. NOTHING added there may touch it** — no
> registers, commits, queries or traversals.

That holds because the entity-change drains, net receive, `game.updatePlayer` and the script context
update above the kick are the last pre-pass writers, and `game.update` and the contact scripts
(`physics.dispatchContactEvents`) run only AFTER the join. The reason it matters: `commitFrame`
relinks cells.

**An INVALID view skips the whole update for that frame** — the first VR frame, where no head view
exists yet. Stamps stay a frame stale (the spawn guard keeps fresh entries visible) and the commit's
pending ops wait one frame.

**Headless** has no cull job, so main.cpp calls `commitFrame()` directly
([main.cpp:310](../../../Code/App/main.cpp#L310)) — every entity still registers, so radius queries (script
queries, unit targeting) work server-side.

## Structure

An implicit 64-ary hierarchy over per-level hashed grids.

* **An entry lives at exactly ONE level** — the smallest whose cell size fits its bounding sphere
  (`cellSize >= 2*radius`, `Morton::levelForRadius`, clamped by `entryLevel`). So an entity extends
  at most half a cell beyond its own, and queries only need half-a-cell loose bounds.
* **11 levels** (`Morton::MaxLevels`), cell size ×4 per level. The finest is the compile-time
  `SPATIAL_FINEST_CELL_SIZE`, default 8 m; 21 bits per axis puts the world bound at ±8389 km, with
  the origin mid-lattice. **8 m is a density choice:** at game density unit-sized entries share a
  cell ~10 to one. At 2 m they sat one per cell — the traversal tested ~2 cells per hit and the
  8-lane blocks ran 2 lanes full; 8 m cut cells tested 6× for the same visible set. 32 m measured the
  same, so the finer of the two won.
* **BMI2 Morton keys** (`Spatial:Morton`): `pdep` / `pext`. A level-L key is the fine key `>> 6*L`, a
  parent is `key >> 6`, and the low 6 bits select one of the parent's 4×4×4 children.
* **`CellMap`** (one per level): open addressing, linear probing, backward-shift deletion. A slot is the key next to its
  `CellRecord` (32 bytes, two per cache line, never split), so a hit reads ONE line; the home slot is a Fibonacci hash (one
  multiply, the top bits). It grows at half full, so a miss - the direct cell path probes empty cells - stays ~2.5 slots.
* **Every `CellRecord` holds a 64-bit child-occupancy mask**, so queries descend with bit scans and
  never visit empty space. **A cell in the map always has its bit set in its parent**: `setOccupancyBits` sets it (and
  walks up through every ancestor it creates), and only `sweepEmptyCells` clears it, in the same step as it erases the
  cell. So `insertLane` walks up only when `getOrCreate` created the cell; a lane into a cell already in the map (a
  cell waiting for the sweep included) skips the parent probe.
* **Children are classified 8 at a time BEFORE their hash probe** (`forEachChild`, testers with
  `classify8`: sphere and frustum). The batch centers come from the parent's float min plus the
  4×4×4 grid offset (`g_childOffsets`), tested with a box inflated by a bound on the float rounding,
  so "outside" and "fully inside" stay conservative; only survivors pay the `CellMap` probe and the
  exact double `cellMinWorld`. AABB and ray fall back to a probe + scalar classify per child.
* **Plane masking.** A traversal carries an `active` mask down the tree: the bounds a cell still
  STRADDLES (frustum: bits 0–5 the planes, bit 6 the distance band; the others one bit), 0 = fully
  inside. A child's loose box sits inside its parent's, so it tests only the parent's active bounds,
  and `test8` does too — deep in a frustum that is usually one or two planes, not six.
* **Small bounded queries skip the descent** (`traverse`, testers with `boundHalf()`: sphere and
  AABB): per level holding any entry (`m_levelEntityCount`), the cells the volume overlaps — the
  bound plus that level's loose half cell — are addressed straight from the key math (`quantize`,
  shift, `encode`) and probed directly; empty levels cost nothing. A few-metre ball is 1–8 probes
  at level 0. Past `DirectCellLimit` (96) cells over all levels the query descends from the top
  instead. Same loose bounds, so the result set is identical either way. **Built for the gameplay
  case: many small-radius queries per frame.**
* The API takes `dvec3`; per-entry data is SoA with **cell-relative float positions**, and query math
  is reference-relative float — exact at planet scale. Frustums rebase to camera-relative in double
  (`rebaseFrustum`) so the planes stay exact.

## `Globals::spatialIndex` API

```cpp
registerEntry(pos, radius, userData, layerMask = 1, spawnVisible = true) -> SpatialHandle
updateEntry(handle, pos, radius)
commitFrame()
```

`SpatialEntry` ([SpatialTypes.ixx](../../../Code/Spatial/Public/SpatialTypes.ixx)) is the move-only RAII wrapper: its `reset` (and its
destructor) is the only unregister - neutralized immediately, unindexed at commit.

**Indexed** = the entry has a lane in its cell's block chain, so a query can find it. `registerEntry` gives a slot and a
handle at once but the entry stays `Unindexed` (`RecordFlag_Unindexed`, no lane, `storeIdx` UINT32_MAX) until the next
`commitFrame` indexes it: the cell maps and the blocks change only there.

**Queries**, read-only and valid between commits:
`querySphere` · `queryAABB` · `queryFrustum` · `queryRay` (a **broadphase** — entries whose bounds
cross the segment) · `queryNearest`, all filling a caller's `oc::vector<uint64>` (`queryNearest`
returns one userData); plus the CALLBACK forms `forEachInSphere` / `forEachInFrustum`, which hand each
hit to a functor straight out of the traversal — **the form to use from job code**: no result buffer,
so nothing `thread_local` rides a fiber that may resume on another thread (zero-allocation type
erasure). **Do not wait inside the callback.** Every game/entity probe uses them.

### `spawnVisible` — the spawn guard

A never-stamped entry counts as visible **in every pass** until its first real stamp, so a fresh
spawn does not flash invisible during its index + stamp latency.

**Streamed geometry passes `false`** (`RecordFlag_NoSpawnGuard`). Appearing one frame late is
invisible for something that did not exist before, while the guard would leak never-stamped
off-screen entries into the main pass — permanently while culling is frozen. Terrain chunks and ocean
sectors both pass false.

### Threading contract

| Operation | Rule |
|---|---|
| `updateEntry` | **Callable from any job during the parallel entity pass.** A same-cell update of an indexed entry writes only its block lane (4 stores, two lines; none when nothing changed); a cell change, or any update of an Unindexed entry, stages a Move into a `PerWorker` pending list. One visitor per entry. |
| `registerEntry` / `SpatialEntry::reset` | Callable from any thread **in the spawn window** (parallel entity spawning). **LOCK-FREE** (see "The record pool" below). |
| `query*` | **Lock-free**, also beside concurrent registrations — a spawning worker's script `OnSpawn` may query while another worker registers: the pool rows never move and the cell maps change only at commit. |
| `commitFrame` | Single-threaded, main, outside the pass. |
| `markVisible*` traversals | Lock-free. The kick/join window forbids registration **by contract**. |

The pending ops are deliberately a `PerWorker` (not owner-sliced or a shared queue): the pushes come
from ANY job with no chunk identity to slice by, the per-frame volume is unbounded, and thousands of
pushes on one shared cursor would contend.

### The record pool

`RecordPool` (the per-entry SoA rows) is what makes registration lock-free:

* **Every row is a `Core.VirtualArray`**: `MAX_ENTRIES` (16M) reserved as ADDRESS SPACE at initialize (~740 MB
  over all rows, no memory), pages committed as the pool grows (doubling, under a commit mutex - rare). A
  growth never moves a row, so nothing needs to exclude the readers, and a row access stays a plain base +
  index - no page table on the traversal / `getPassMask` paths. The committed memory shows in the Memory
  panel as "Spatial record pool" (VirtualArray reports its commits to the tracker).
* **The row base pointers sit together in ONE cache line** (`RecordPool::cellKey` ... `flags`: raw pointers set at
  initialize; the `VirtualArray`s are private). A VirtualArray is ~640 B, mostly its commit-step table, so read through
  the arrays every row of one `updateEntry` was a different line.
* **Slots** come from a tagged lock-free free stack (the head's high 32 bits bump on every push and pop:
  ABA), else a CAS bump of the high-water mark that commits FIRST - so `capacity()` (the high-water mark)
  never passes the committed rows, and the stamp wrap sweep can walk `[0, capacity())` while spawns
  register. `release` runs in `commitFrame` only.
* A registered slot is the registering call's alone until its Index op at commit (no traversal reaches an
  unindexed slot); an unregister writes only its own rows and lane tombstone.

## Layers

| Layer | Bit | Contents |
|---|---|---|
| `SpatialLayer_Render` | 0 | Entities that have a RenderComponent. **This is the gameplay-query layer.** |
| `SpatialLayer_Stress` | 1 | Synthetic stress-test entries. |
| `SpatialLayer_Terrain` | 2 | Procedural terrain chunks, ocean clipmap sectors. |
| `SpatialLayer_Entity` | 3 | EVERY registered entity. The World's update-selection layer. |

> **`SpatialLayer_Terrain` entries never carry an `Entity*`** — a terrain chunk's userData is its
> streamer `Resident*` (its node AND its vegetation, AND the river water over it: Procedural RiverSystem's tiles ride
> the chunks), an ocean sector's is its `RenderNode*` **with
> `SpatialTerrainTag_Ocean` (bit 63, above the 47-bit user address range) set**. The streamer and the ocean share ONE
> hand-over list (collect slot 1), walked ONCE by `TerrainStreamer::render`'s job, which tells the two apart by the tag
> and pushes every node with its own pass mask; the tag also keeps the sectors out of the terrain's shadow ball. The
> owners keep a dead node's memory until `visibleCollectGeneration` moves on (see the hand-over below). **Gameplay
> queries must never include that layer** ([SpatialTypes.ixx](../../../Code/Spatial/Public/SpatialTypes.ixx)).

## Visibility stamps

Eight stamp passes (`ESpatialPass`), each with its own stamp generation. **Each `markVisible*` call
invalidates that pass's previous generation — one consumer per pass by design.**

| Pass | Meaning |
|---|---|
| `Main` | The camera frustum, occlusion-testable. |
| `Near` | A camera ball that keeps off-screen ray-traced geometry (and its shadows) alive. |
| `Shadow` | The Main frustum swept toward the sun by `shadowReach`: off-screen sun shadow casters. |
| `UpdateTier0/1/2` | The World's SIM LOD selection. **Not rendering.** |
| `UpdateRoot` / `VisibleRoot` | The World's ROOT-DEDUPE stamps: "this root is in the current periodic selection result" (advances with the selection job) / "already queued from this frame's visible set" (advances every pass). No visibility meaning — read with the exact accessors only. |

**Stamps are 16-bit** (`SpatialStamp`): a generation counts 1..65534, and `advanceStamp` sweeps the
pass's pool column back to `SpatialStamp_Indexed` when it wraps, so a stale value can never read as
current again. **The 8 pass stamps of an entry sit together** (`RecordPool::stamps`, one 16-byte
`PassStamps` per entry, not one array per pass): `getPassMask` / `getPassMaskExact` are one
`__m128i` load compared against the 8 current generations (`m_visibleQueryId`, 16-aligned) with
`_mm_cmpeq_epi16`, the spawn guard one more compare against 0 — one cache line per call instead of
one per pass. The pool's other rows: `storeIdx` stays 32-bit (block * 8 + lane) and `gen` stays 32-bit so a stale handle can never match
a reused slot.

**The index-time spawn-guard stamp covers Main, Near and Shadow ONLY.** The tier passes get `SpatialStamp_Indexed`
and the root passes stay 0 when the entry is indexed: their generations advance with the World's periodic selection, so a "current" stamp made
here would read as a real tier 0 (or "root already held") for frames.

Read back with `getPassMask` / `isVisible` (both apply the spawn guard), or with the exact-compare
variants below. `SpatialPassBit_*` are the bits; `SpatialPassBits_UpdateTiers` is the tier mask.

### Multithreaded traversal

`markVisibleSet`, `markVisibleSphere` and `queryUpdateTiers` run `traverseParallel`
([Query.cpp:784](../../../Code/Spatial/Private/Query.cpp#L784)):

1. The top level is a handful of huge cells, so a **serial frontier expansion** splits the tree until
   it holds `numWorkers * 4` independent subtree roots — **4× over-partitioned so the shared
   parallelFor cursor can rebalance around expensive roots**, since the cells around the camera hold
   most of the visible world. It classifies cells and emits the upper cells' own few entries as it
   goes.
2. A **grain-1 High `parallelFor`** runs `traverseCell` per root. `"Spatial traverse chunk"` spans
   show on the worker tracks. High because the render gate waits on these stamps.

Every emit gets the hit's lane, `(block, lane, cellMin, passes)`, and reads only what it needs from it: the userData
(queries), the pool slot (stamps), the layer byte (the collect lists), the position relative to the reference (`cellMin` + the
lane's position: `queryNearest`, the tiers). `passes` is bit 0 for a one-pass tester; the Main + Shadow tester sets bit 0 for
Main and bit 1 for Shadow. A chunk-aware emit `(block, lane, cellMin, passes, chunk)` gets `prepareChunks(n)`
before any emit with `chunk < n`, so a caller can keep an owner-sliced list per chunk (slot 0 = the
serial expansion, `1 + ` the chunk's first frontier index = the fan-out). A caller outside the
kick/join window (`queryUpdateTiers`) needs no lock either: registrations never move the pool rows
(see "The record pool"). The `m_frontier` scratch allows ONE traverseParallel at a time: the cull
job's, or the post-update selection's, never both in flight (the selection is joined before the
spatial kick).

**Thread-safe by structure:** an entry lives in exactly ONE cell, so no two roots ever stamp the same
slot; the cell maps only mutate in `commitFrame`; and the stamp is a pure store. Per-traversal
counters (`TraverseStats`) accumulate locally and merge once, since traversals run concurrently
(script queries on workers, the fan-out). `OcclusionBuffer`'s hidden-cell counter is atomic with a
plain-int mirror for the tweak panel, which binds a raw `int*`.

### The SIM LOD tiers are NOT stamped by the cull job

`update()` stamps Main, Near and Shadow only. The `UpdateTier` passes are stamped by the World's selection
job through `advanceUpdateTiers` + `queryUpdateTiers` (below), off the frame-critical path.

### The visible set hand-over (`setVisibleCollect`)

The Main frustum stamp also COLLECTS: with `setVisibleCollect(layerMask, slot, what)` set, the
stamp lambda appends every hit carrying one of that slot's layers to the slot's owner-sliced
per-chunk list (list 0 for the serial expansion, 1 + the chunk's first frontier index for the
fan-out), merged once inside the job. A slot collects either **`ECollect::Handles`**
(`visibleHandles(slot)`) or **`ECollect::UserData`** (`visibleUserData(slot)`: the value is read from
the lane IN THE JOB and zeros are dropped, so neither the job nor the consumer's walk touches a pool row for it — but it
gets a value, not a live check, and must tolerate an entry that changed or died since the stamp).
**Two collect slots** (`NumVisibleCollectSlots`):
* slot 0 — the World's (`SpatialLayer_Entity`, Handles, the default arguments): entities die in the
  destroy windows between the join and the World's pass, and `userData(handle)` reads 0 for those.
* slot 1 — the terrain streamer's (`SpatialLayer_Terrain`, UserData): the `Resident*` of its chunks
  and the tagged `RenderNode*` of the ocean's sectors, pushed in ONE walk by `TerrainStreamer::render`'s
  job (a 0 userData never enters the list) — see Procedural.

The lists are valid from `joinUpdateJob` until the next kick, and a FROZEN cull keeps the last list,
like the stamps. **`visibleCollectGeneration()`** counts the list rebuilds (real Main stamps only —
not while culling is Off, frozen or the view invalid). An owner whose userData is an object ADDRESS
retires a dying object instead of freeing it: everything that draws goes at once, the memory stays
until the generation moves past the value at retirement — from then on no list can name it. The
terrain residents and the ocean grids work this way.

`forEachInSphere(..., skipStampedIn)` drops hits currently stamped in any of the given passes (exact
compare, read in the traversal): the terrain's shadow ball skips what the Main set already pushed.
**This is how the World selects what is on screen every frame without a traversal of its own**, and
how the streamer and the ocean push their on-screen geometry without walking it all.

### The Near ball's hysteresis

The Near ball barely changes frame to frame, so it is inflated by `nearSlack` (16 m) and requeried
only once the camera has moved that far — **or every 30 frames**, which keeps off-screen MOVERS from
staying unstamped: they can enter the ball without the camera moving
([Query.cpp:1231](../../../Code/Spatial/Private/Query.cpp#L1231)).

Terrain rides the Main stamp but **skips the Near ball**: main-culled terrain keeps its shadow and GI
passes unconditionally.

### The Shadow pass

A caster shadows the visible ground when it sits UP-SUN of it, so the Near ball (centred on a camera
that may hang far above the scene) is the wrong shape for shadow relevance. `update()` therefore
stamps a third render pass every frame: **the inflated Main frustum swept toward the sun on the
horizontal plane by `shadowReach`** — the Minkowski sum with that segment, which keeps the face
normals and moves only the planes facing against the sweep (by reach × −(n·s)). The sun direction
rides in `CullView::sunDirection` (the renderer fills it in `setFrameView`); a near-vertical sun skips
the sweep (and the pass: its stamps stay as they were). No occlusion test: a caster hidden behind a wall still casts.
`Shadow`-only entries push `PASS_SHADOW` alone (Entity.cpp); Near-only still pushes `PASS_SHADOW | PASS_GI`.

**The swept frustum CONTAINS the Main one** (same planes, some moved out, a longer band), so both stamp in ONE traversal
(`DualFrustumTester`, `markVisibleSet` with a shadow frustum): one descent, one hash probe and one classify per cell instead of
two. Its active mask holds both: bits 0–6 Main, bit 7 "Main ruled out" (outside Main, or occluded - the subtree still runs for
Shadow), bits 8–14 Shadow; outside Shadow is outside both. A plane the sweep did not move gives both passes the same answer,
so `test8` and `classify8` test it once. Occlusion prunes Main only.

### Culling config (`Spatial/Culling` tweaks)

All Spatial tweaks are `SpatialSettings` = `Globals::settings.spatial` (`Settings.Spatial`, registered in
`Code/Settings`): `culling`, `stats` (the readouts the index writes, `SPATIAL_STATS` builds only - see Stats), `occlusion`, `stress`. `Spatial:SpatialTypes`
re-exports `Settings.Spatial`, so `ESpatialCullMode` / `SpatialCullingConfig` / `SpatialStats` reach every
importer of Spatial. The index binds `m_culling` / `m_stats` as references to them.

| Field | Tweak | Default | Notes |
|---|---|---|---|
| `mode` | `Mode` | `Cull` | `Off` / `StatsOnly` / `Cull` / `MainOnly` (debug — visibly breaks off-screen shadows and GI). |
| `freeze` | `Freeze` | false | Stop re-stamping and fly around to inspect the culled set. |
| `margin` | `Margin` | 4 m | Frustum inflation masking the one-frame stamp latency. |
| `nearRadius` | `Near radius` | **0** | Ray-tracing (and RT shadow) relevance range around the camera. |
| `shadowReach` | `Shadow reach (m)` | 60 m | Shadow pass: how far the view frustum is swept toward the sun; 0 = off. |
| `nearSlack` | `Near requery slack` | 16 m | Near-ball inflation and requery threshold; 0 = every frame. |
| `maxDist` | — (not a tweak) | overwritten | Set from the camera far plane every update. |
| `skinnedRadiusScale` | `Skinned radius scale` | 1.5 | Animation can exceed the bind-pose bounds sphere. |

## Entity integration

**Every entity registers at the end of `Entity::create`** — parallel-spawn safe and lock-free (see
"The record pool"). The exception is the entity `CullMode` (see [`Docs/Context/Entity/CONTEXT.md`](../Entity/CONTEXT.md)):
a `RootOnly` root registers one entry over its whole subtree and the subtree registers nothing;
`None` registers nothing.

* Layer `SpatialLayer_Entity` always, plus `SpatialLayer_Render` when the entity has a render node.
* Bounds come from `RenderNode::getWorldBounds`, skinned inflated by `skinnedRadiusScale`; otherwise
  a point at the spawn position. A tree CHILD's bounds go through its ancestors' transforms, so its
  entry is in WORLD space from the start (see Entity integration in the Entity CONTEXT).
* Headless has no render nodes, so every entry there is a point.
* `updateSelf` refreshes it every visit, and the render gate reads its pass mask.
* Update pushes per-pass masks: Main-visible entities push `PASS_ALL`, Near-only push
  `PASS_SHADOW|PASS_GI` so shadows and GI keep off-screen entities, Shadow-only push `PASS_SHADOW`.

### SIM LOD hooks

`advanceUpdateTiers()` opens a new stamp generation for the three `UpdateTier` passes and `UpdateRoot`
— the World's selection job calls it once per selection, and NOTHING else stamps those passes, so the
stamps stay current until the next selection (which may be several frames later).

`queryUpdateTiers(center, queryRadius, tierRadius[3], horizontal, layerMask)` is ONE PARALLEL
traversal of the ball (`traverseParallel`) that stamps every hit in each tier
whose radius exceeds its distance to the center (nested balls; a `tierRadius` <= 0 is never stamped
by that ball; `horizontal` = XZ distance) and returns the hits' userData as **one owner-sliced list per
traversal chunk** (`m_tierHitChunks`, some empty, valid until the next call). The World runs its
spheres IN SEQUENCE (the frontier scratch allows one traversal at a time) — **the stamps are pure
stores, so overlapping balls stamping the same entry is fine.** The distance test is
center-to-center (the entry radius only widens the query ball).

The accessors that exist purely for the World's selection logic:

| Accessor | Difference from the normal one |
|---|---|
| `getPassMaskExact` | **No spawn guard** — a never-stamped entry reads as in no pass. |
| `hasStamp` | Whether a real generation was EVER written in a pass (neither the spawn-guard 0 nor `SpatialStamp_Indexed`). For the tier passes: "the selection job placed this entry at some point" — the World derives a fresh entry's tier from the distance instead. |
| `isStampedCurrent` | Single-entry exact compare. |
| `stampCurrent` | Single-entry stamp (a pure store, job-safe); the selection job marks the ancestors of every hit with it. |
| `stampCurrentOnce` | Stamp + "was it not current before" as ONE atomic exchange: of several jobs reaching the same root exactly one gets true. The World's root dedupe. |
| `advanceStamp` | Opens a new generation for one pass (the World: `VisibleRoot`, every pass). |
| `isAlive` | Whether a root the post-update selection found still exists when the next pass uses it. |

See [`Docs/Context/Entity/SimLod.md`](../Entity/SimLod.md).

## CPU software occlusion

`Spatial:Occlusion` (+ `:OcclusionState`), `Globals::occlusionBuffer`, Spatial/Occlusion tweaks, **default off**.

The largest triangles by area of static mesh colliders — extracted by
`OcclusionBuffer::extractOccluders`, registered by PhysicsComponent through the RAII
`SpatialOccluder` — rasterize camera-relative into a **256×144** CPU depth buffer plus **8×8**
max-depth blocks. Default budget 2048 triangles (`Max occluder tris`).

It plugs into the Main stamp as `IOcclusionTester`, so occluded cells drop from the MAIN pass only.

**Everything is conservative toward "visible":** pixel-center coverage under-rasterizes occluders,
the block mip keeps the FARTHEST depth, and any near-plane crossing reports visible.

> The renderer's projection is REVERSED-Z, so `update` flips the z row back to standard orientation
> (`z' = w - z`) before handing the matrix to the rasterizer — for the rasterizer ONLY
> ([Query.cpp:1190](../../../Code/Spatial/Private/Query.cpp#L1190)).

`addOccluder` / `removeOccluder` take a mutex: they run concurrently from spawn jobs
(PhysicsComponent spawn and resume), while `render()` runs in the cull window where no spawn is
legal.

## Cell blocks

Every indexed entry lives in an **8-lane SoA block chained per cell** (`BlockStore` / `CellBlock`,
`CellRecord.head`), so the 8-wide AVX2 testers (`test8`) load a block transpose-free and **every
mutation costs the size of its cell, never the level**. A `CellBlock` is `alignas(64)`, 256 bytes,
so no 32-byte lane row splits a cache line (the `oc::vector` holding them gets the alignment from
`oc::allocator<T>` — see Core). The fully-inside emit masks live lanes with one AVX2 compare
(`liveLanes`). There is no separate dynamic list and no static tier: a settled entry and a moving one
are the same lane, the difference is only whether `updateEntry` writes it this frame.

* **Layout.** Four lines: `posX` + `posY` | `posZ` + `radius` | `userData` | `poolIdx` + `layer` (one byte per lane: every
  layer bit is below 256) + the header. `storeIdx` = `block * 8 + lane` (UINT32_MAX while `Unindexed`). **An entry's position,
  radius and layer mask live ONLY in its lane** — the pool has no such rows. Before the entry is indexed
  they ride the pending ops: the Index op carries the registration's (layer mask included), and every
  `updateEntry` of an `Unindexed` entry queues a Move. **The lane also holds a COPY of the userData** (the pool row is
  written once, at registration, so the copy cannot go stale; `insertLane` reads it from the pool): a query hit and the
  UserData collect read no pool row. The pool keeps what a handle needs (`gen`, `flags`, `cellKey`, `level`, `storeIdx`,
  `userData`) and the stamps. Unused lanes past a block's `count` hold a tombstone radius, so `test8` always runs all 8 lanes.
  The layer test is one 8-byte `and` + `cmpeq_epi8` + `movemask`, ANDed into the hit mask.
* **Same-cell update** of an indexed entry = compare against the lane, then the lane stores (two cache
  lines; none when unchanged), from any job. Its code path has no call: the body (`updateEntryAt`) and the
  key math are `OC_FORCEINLINE`, and the cold parts are `OC_NOINLINE` (`updateEntryTop` for a top-level
  entry, clamped and maybe oversize; `queueOp`, the op push and its vector growth), so their register needs
  do not land in `updateEntry`'s prologue. The key and the cell-relative position come from `locate`,
  x / y / z 4-wide: the lattice offset added in double (no integer offset, no constant held in a register),
  one floor, one clamp with memory bounds, one convert; the cell min is the fine coordinate with its low
  `2 * level` bits cleared (one `vpand`); only the key's `pdep` encode is per axis. The lane compare and
  stores stay scalar: tried one 4-wide `cmpneq` + `movmsk`, dropped because gathering the lane's four rows
  (32 bytes apart) and splitting the result for the stores cost more instructions than it saved.
* **Cell change** = a Move op: at commit `retireLane` the old lane (it returns the lane's layer mask), set
  the pool's `cellKey` / `level`, `insertLane` the op's position with that mask into the new cell's head
  block (a new head when that is full). `Cell moves` in the stats counts them per commit.
* **Indexing** (`indexEntry`): `commitFrame` walks the ops ONCE. The Index (the spawning thread) and the
  first Move of the same entry (the visiting worker) may sit in different `PerWorker` slots, so a Move that
  finds its entry still `Unindexed` goes to `m_deferredMoves` (in order) and is replayed after the walk, once
  every Index has run. Every Move is newer than the Index, so the last Move's position wins. Tried two
  passes (every Index first), dropped because they walk every op twice for a rare case.
* **Tombstones** — a negative radius fails every test in place. `unregisterEntry` writes only the
  lane's tombstone and the pool flags (queries read lock-free); the counters and blocks settle in `retireLane` at
  commit (the Unindex op, or a Move).
* **Blocks free as soon as they empty**, so `count == 0` <=> no chain (the sweep relies on it). A
  cell whose dead lanes would fill a whole block compacts itself (`compactCell`: gather the OWNED
  lanes into the kept `m_compactScratch`, refill from the head, free the tail) — at least one block
  freed per pass, so one cell walk per 8 retirements. **Owned** = the record's `level` is this level
  AND its `storeIdx` names the lane: a tombstoned lane whose Unindex op is still queued this commit is
  still owned (dropping it would strand that op's `storeIdx`), and slots are numbered per level, so a
  record that moved levels can hold the same slot number elsewhere. The chain's block count for that
  test lives in the HEAD block (`CellBlock::chainBlocks`, in the alignment padding, so `CellRecord`
  stays 16 bytes); `insert`, the block unlink in `retireLane` and `compactCell` keep it, and a Debug
  assert re-walks the chain to check it.
* Sphere, AABB and frustum testers test blocks 8-wide; the ray tester goes lane by lane. Stats:
  `Blocks` (entries / blocks = the lane fill) and `Cell moves`.

## Stats

**Spatial/Stats is filled only in a build with `SPATIAL_STATS=1`** (`SpatialStatsEnabled` in `Types.ixx`; the commented
line in `Code/Spatial/CMakeLists.txt` sets it). Otherwise every counter, the per-level loop at commit and the `Clock` reads
compile out (`if constexpr`), and the rows read 0: the counters sat on the traversal's hottest paths.

## Stress harness

`Globals::spatialStress`, Spatial/Stress tweaks: synthetic entries on `SpatialLayer_Stress`, churn,
timed queries, brute-force verification. Stats under Spatial/Stats. **Nothing calls its `update()`**
— the tweaks show in the panel but do nothing until the App drives it.

## Not yet built

* **Parallel LARGE script queries.** Only the `markVisible*` stamps and `queryUpdateTiers` fan out;
  the other `query*` / `forEachIn*` entry points traverse serially (their emit appends to one
  out-vector or runs the caller's callback, and they may run concurrently from script workers, so
  they cannot share the single `m_frontier` scratch). Small ones take the direct cell path; only a
  large ball from a script would gain.
* **Leaf-cell slicing in the fan-out.** The expansion stops at leaves: a narrow view over a few big
  cells yields fewer roots than `numWorkers * 4`, and one root can hold most of the work. Splitting a
  leaf's block chain (indexable by block) into slices would fill the workers for that case.
