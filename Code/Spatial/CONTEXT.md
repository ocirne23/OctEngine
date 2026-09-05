# Spatial

> Library documentation for `Code/Spatial`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

The spatial index: culling and position lookup, **independent of the entity parent/child
hierarchy**. Links Threading only.

## The one call the App makes

`SpatialIndex::update(camera, frustum, viewProjRelCamera)`
([Query.cpp:597](Private/Query.cpp#L597)) is the whole per-frame spatial step, and it runs **as a
High job**:

```cpp
Globals::spatialIndex.kickUpdateJob(Globals::rendererVK.getCullView(camera, viewportRect));
...
Globals::spatialIndex.joinUpdateJob();
```

In order, `update` does:

1. **`commitFrame()`** — applies the cell moves queued during last frame's entity updates.
2. **SIM LOD tier stamps**, if a focus is set. Deliberately BEFORE the culling-mode gate: update
   selection is not culling and must not stop with it.
3. **`setCullMaxDist(camera.far)`** — the main-pass cull distance tracks the render camera's far
   plane, so terrain and entities stream to exactly the view distance rather than a fixed cap.
4. Returns early if the mode is `Off` or `freeze` is set.
5. **Occlusion raster**, when the CPU occlusion buffer is enabled.
6. **The Main stamp** — the margin-inflated camera frustum over `SpatialLayer_Render |
   SpatialLayer_Terrain`.
7. **The Near stamp** — a camera ball over `SpatialLayer_Render` only, behind requery hysteresis.

### The kick/join window

main.cpp kicks right BEFORE `physics.update`, next to the renderer's "Begin frame" job, and joins
after the physics step, `audio.update` and the nav publish
([main.cpp:757-782](../App/main.cpp#L757)).

> **The index must stay QUIESCENT between kick and join. NOTHING added there may touch it** — no
> registers, commits, queries or traversals.

That holds today because the entity-change drains, net receive and `game.update` above the kick are
the last pre-pass writers, and contact scripts fire only AFTER the join
(`physics.dispatchContactEvents`). Two reasons it matters: `commitFrame` relinks cells, and
`registerEntry`'s pool growth reallocates the SoA the traversals read.

**An INVALID view skips the whole update for that frame** — the first VR frame, where no head view
exists yet. Stamps stay a frame stale (the spawn guard keeps fresh entries visible) and the commit's
pending ops wait one frame.

**Headless** has no cull job, so main.cpp calls `commitFrame()` directly
([main.cpp:784](../App/main.cpp#L784)) — every entity still registers, so radius queries (script
queries, unit targeting) work server-side.

## Structure

An implicit 64-ary hierarchy over per-level hashed grids.

* **An entry lives at exactly ONE level** — the smallest whose cell size fits its bounding sphere
  (`cellSize >= 2*radius`, `Morton::levelForRadius`). So an entity extends at most half a cell beyond
  its own, and queries only need half-a-cell loose bounds.
* **11 levels** (`Morton::MaxLevels`), cell size ×4 per level. The finest is the compile-time
  `SPATIAL_FINEST_CELL_SIZE`, default 2 m; 21 bits per axis puts the world bound at ±2097 km, with
  the origin mid-lattice.
* **BMI2 Morton keys** (`Spatial:Morton`): `pdep` / `pext`. A level-L key is the fine key `>> 6*L`, a
  parent is `key >> 6`, and the low 6 bits select one of the parent's 4×4×4 children.
* **Every `CellRecord` holds a 64-bit child-occupancy mask**, so queries descend with bit scans and
  never visit empty space.
* The API takes `dvec3`; per-entry data is SoA with **cell-relative float positions**, and query math
  is reference-relative float — exact at planet scale. Frustums rebase to camera-relative in double
  (`rebaseFrustum`) so the planes stay exact.

## `Globals::spatialIndex` API

```cpp
registerEntry(pos, radius, userData, layerMask = 1, spawnVisible = true) -> SpatialHandle
unregisterEntry(handle)      // neutralized immediately, unlinked at commit
updateEntry(handle, pos, radius)
setLayerMask(handle, mask)
commitFrame()
```

`SpatialEntry` ([Types.ixx:18](Private/Types.ixx#L18)) is the move-only RAII wrapper.

**Queries**, read-only and valid between commits:
`querySphere` · `queryAABB` · `queryFrustum` · `queryRay` (a **broadphase** — entries whose bounds
cross the segment) · `queryNearest`, all filling a caller's `oc::vector<uint64>`; plus the
CALLBACK forms `forEachInSphere` / `forEachInFrustum`, which hand each hit to a functor straight
out of the traversal — **the form to use from job code**: no result buffer, so nothing
`thread_local` rides a fiber that may resume on another thread (zero-allocation type erasure; do
not wait inside the callback — the index's shared lock is held). Every game/entity probe uses them.

### `spawnVisible` — the spawn guard

A never-stamped entry counts as visible **in every pass** until its first real stamp, so a fresh
spawn does not flash invisible during its link + stamp latency.

**Streamed geometry passes `false`** (`RecordFlag_NoSpawnGuard`). Appearing one frame late is
invisible for something that did not exist before, while the guard would leak never-stamped
off-screen entries into the main pass — permanently while culling is frozen. Terrain chunks, ocean
sectors and scatter groups all pass false.

### Threading contract

| Operation | Rule |
|---|---|
| `updateEntry` | **Callable from any job during the parallel entity pass.** A same-cell update writes only that entry's SoA slots (~4 stores); a cell change stages into a `PerWorker` pending list. One visitor per entry. |
| `registerEntry` / `unregisterEntry` | Callable from any thread **in the spawn window** (parallel entity spawning). Both take `m_registerMutex` EXCLUSIVE, because pool growth reallocates the SoA. |
| `query*` | Take `m_registerMutex` SHARED — a spawning worker's script `OnSpawn` may query while another worker registers. |
| `setLayerMask` / `commitFrame` | Single-threaded, main, outside the pass. |
| `markVisible*` traversals | Lock-free. Safe only because the kick/join window forbids registration **by contract**. |

## Layers

| Layer | Bit | Contents |
|---|---|---|
| `SpatialLayer_Render` | 0 | Entities that have a RenderComponent. **This is the gameplay-query layer.** |
| `SpatialLayer_Stress` | 1 | Synthetic stress-test entries. |
| `SpatialLayer_Terrain` | 2 | Procedural terrain chunks, ocean sectors, scatter groups. |
| `SpatialLayer_Entity` | 3 | EVERY entity. The World's update-selection layer. |

> **`SpatialLayer_Terrain` entries carry `userData = 0`, not an `Entity*`. Gameplay queries must
> never include that layer** ([Types.ixx:58](Private/Types.ixx#L58)).

## Visibility stamps

Five passes (`ESpatialPass`), each with its own stamp generation. **Each `markVisible*` call
invalidates that pass's previous generation — one consumer per pass by design.**

| Pass | Meaning |
|---|---|
| `Main` | The camera frustum, occlusion-testable. |
| `Near` | A camera ball that keeps off-screen shadow casters and ray-traced geometry alive. |
| `UpdateTier0/1/2` | The World's SIM LOD selection. **Not rendering.** |
| `UpdateRoot` / `VisibleRoot` | The World's ROOT-DEDUPE stamps: "this root is in the current periodic selection result" (advances with the selection job) / "already queued from this frame's visible set" (advances every pass). No visibility meaning — read with the exact accessors only. |

**Stamps are 16-bit** (`SpatialStamp`): a generation counts 1..65534, and `advanceStamp` sweeps the
pass's pool row back to `SpatialStamp_Linked` when it wraps, so a stale value can never read as
current again. The pool's other narrow rows: `layerMask` is a byte (4 layer bits, static_assert),
`lastMoveFrame` a modular uint16 (the promotion age compares as `uint16(frame - last)`); `next`/`prev`/
`storeIdx` stay 32-bit (indices up to the capacity) and `gen` stays 32-bit so a stale handle can never
match a reused slot.

**The link-time spawn-guard stamp covers Main and Near ONLY.** The tier and root passes are left at
0 on link: their generations advance with the World's periodic selection, so a "current" stamp made
here would read as a real tier 0 (or "root already held") for frames.

Read back with `getPassMask` / `isVisible` (both apply the spawn guard), or with the exact-compare
variants below. `SpatialPassBit_*` are the bits; `SpatialPassBits_UpdateTiers` is the tier mask.

### Multithreaded traversal

Both `markVisibleSet` and `markVisibleSphere` run `traverseParallel`
([Query.cpp:376](Private/Query.cpp#L376)):

1. The top level is a handful of huge cells, so a **serial frontier expansion** splits the tree until
   it holds `numWorkers * 4` independent subtree roots — **4× over-partitioned so the shared
   parallelFor cursor can rebalance around expensive roots**, since the cells around the camera hold
   most of the visible world. It classifies cells and emits the upper cells' own few entries as it
   goes.
2. A **grain-1 High `parallelFor`** runs `traverseCell` per root. `"Spatial mark visible"` spans show
   on the worker tracks. High because the render gate waits on these stamps.

**Thread-safe by structure:** an entry lives in exactly ONE cell, so no two roots ever stamp the same
slot; the cell maps only mutate in `commitFrame`; and the stamp is a pure store. Per-traversal
counters (`TraverseStats`) accumulate locally and merge once — which also fixed a pre-existing stats
race from concurrent script-worker queries. `OcclusionBuffer`'s hidden-cell counter went atomic with
a plain-int mirror for the tweak panel, which binds a raw `int*`.

### The SIM LOD tiers are NOT stamped by the cull job

`update()` stamps Main and Near only. The `UpdateTier` passes are stamped by the World's selection
job through `advanceUpdateTiers` + `queryUpdateTiers` (below), off the frame-critical path.

### The visible set hand-over (`setVisibleCollect`)

The Main frustum stamp also COLLECTS: with `setVisibleCollect(layerMask)` set (the World passes
`SpatialLayer_Entity`), the stamp lambda appends the `SpatialHandle` of every hit carrying one of
those layers to an owner-sliced per-chunk list (`traverseParallel` hands a chunk-aware emit — `(idx,
pos, chunk)` — slot 0 for its serial expansion, 1 + the chunk's first frontier index for the
fan-out), merged once inside the job. `visibleHandles()` is valid from `joinUpdateJob` until the
next kick; `userData(handle)` resolves one and reads 0 for an entry that died in between (the
destroy windows sit between the join and the World's pass). **This is how the World selects what is
on screen every frame without a traversal of its own.**

### The Near ball's hysteresis

The Near ball barely changes frame to frame, so it is inflated by `nearSlack` (16 m) and requeried
only once the camera has moved that far — **or every 30 frames**, which keeps off-screen MOVERS from
staying unstamped: they can enter the ball without the camera moving
([Query.cpp:646](Private/Query.cpp#L646)).

Terrain rides the Main stamp but **skips the Near ball**: main-culled terrain keeps its shadow and GI
passes unconditionally.

### Culling config (`Spatial/Culling` tweaks)

| Field | Default | Notes |
|---|---|---|
| `mode` | `Cull` | `Off` / `StatsOnly` / `Cull` / `MainOnly` (debug — visibly breaks off-screen shadows and GI). |
| `freeze` | false | Stop re-stamping and fly around to inspect the culled set. |
| `margin` | 4 m | Frustum inflation masking the one-frame stamp latency. |
| `nearRadius` | **0** | Shadow-caster + ray-tracing relevance range. |
| `nearSlack` | 16 m | Near-ball inflation and requery threshold; 0 = every frame. |
| `maxDist` | overwritten | Set from the camera far plane every update. |
| `skinnedRadiusScale` | 1.5 | Animation can exceed the bind-pose bounds sphere. |

## Entity integration

**Every entity registers at the end of `Entity::create`**
([EntityP.cpp:261](../Entity/Private/EntityP.cpp#L261)) — parallel-spawn safe, since the index locks.

* Layer `SpatialLayer_Entity` always, plus `SpatialLayer_Render` when the entity has a render node.
* Bounds come from `RenderNode::getWorldBounds`, skinned inflated by `skinnedRadiusScale`; otherwise
  a point at the spawn position. For a tree CHILD that is its LOCAL position — the entry links at the
  next commit, and the child's first visit re-places it in world space before any query can see it.
* Headless has no render nodes, so every entry there is a point.
* `updateSelf` refreshes it every visit, and the render gate reads its pass mask.
* Update pushes per-pass masks: Main-visible entities push `PASS_ALL`, Near-only push
  `PASS_SHADOW|PASS_GI` so shadows and GI keep off-screen entities.

### SIM LOD hooks

`advanceUpdateTiers()` opens a new stamp generation for the three `UpdateTier` passes — the World's
selection job calls it once per selection, and NOTHING else stamps those passes, so the stamps stay
current until the next selection (which may be several frames later).

`queryUpdateTiers(center, queryRadius, tierRadius[3], horizontal, layerMask, outUserData)` is ONE
serial `traverse` of the ball that both emits every hit AND stamps it in each tier whose radius
exceeds its distance to the center (nested balls; a `tierRadius` <= 0 is never stamped by that
ball; `horizontal` = XZ distance). The World runs one per sphere on a parallelFor — **the stamps are
pure stores, so overlapping balls stamping the same entry concurrently is fine.** The distance test
is center-to-center (the entry radius only widens the query ball).

Three accessors exist purely for the World's selection logic:

| Accessor | Difference from the normal one |
|---|---|
| `getPassMaskExact` | **No spawn guard** — a never-stamped entry reads as in no pass. |
| `hasStamp` | Whether a real generation was EVER written in a pass (neither the spawn-guard 0 nor `SpatialStamp_Linked`). For the tier passes: "the selection job placed this entry at some point" — the World derives a fresh entry's tier from the distance instead. |
| `isStampedCurrent` | Single-entry exact compare. |
| `stampCurrent` | Single-entry stamp (a pure store, job-safe); the selection job marks the ancestors of every hit with it. |
| `stampCurrentOnce` | Stamp + "was it not current before" as ONE atomic exchange: of several jobs reaching the same root exactly one gets true. The World's root dedupe. |
| `advanceStamp` | Opens a new generation for one pass (the World: `VisibleRoot`, every pass). |

See the SIM LOD section in [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md).

## CPU software occlusion

`Spatial:Occlusion`, `Globals::occlusionBuffer`, Spatial/Occlusion tweaks, **default off**.

The largest triangles by area of static mesh colliders — extracted by
`OcclusionBuffer::extractOccluders`, registered by PhysicsComponent through the RAII
`SpatialOccluder` — rasterize camera-relative into a **256×144** CPU depth buffer plus **8×8**
max-depth blocks. Default budget 2048 triangles.

It plugs into the Main stamp as `IOcclusionTester`, so occluded cells drop from the MAIN pass only.

**Everything is conservative toward "visible":** pixel-center coverage under-rasterizes occluders,
the block mip keeps the FARTHEST depth, and any near-plane crossing reports visible.

> The renderer's projection is REVERSED-Z, so `update` flips the z row back to standard orientation
> (`z' = w - z`) before handing the matrix to the rasterizer — for the rasterizer ONLY
> ([Query.cpp:624](Private/Query.cpp#L624)).

`addOccluder` / `removeOccluder` take a mutex: they run concurrently from spawn jobs
(PhysicsComponent spawn and resume), while `render()` runs in the cull window where no spawn is
legal.

## Static tier

Entries unchanged for `promoteAfterFrames` (60) promote into per-level **Morton-key-sorted SoA
ranges** (`StaticStore`), so queries iterate them linearly and the 8-wide AVX2 testers (`test8`) get
transpose-free loads.

* Promotion is budgeted: `staticScanBudget` (65536) pool slots inspected per commit, and
  `staticRebuildBatch` (1024) pending promotions force a level rebuild — **one level per commit**.
* A promoted entry stays in its dynamic list until a rebuild consumes it.
* **Demotion tombstones** — a negative radius fails every test in place; rebuilds drop tombstones and
  merge pending promotions back into sorted order.
* Sphere, AABB and frustum testers all test static ranges 8-wide.

## Stress harness

`Globals::spatialStress`, Spatial/Stress tweaks: synthetic entries on `SpatialLayer_Stress`, churn,
timed queries, brute-force verification. Stats under Spatial/Stats.

## Not yet built

**Threading-parallel QUERIES.** The `markVisible*` stamps already fan out, but the `query*` entry
points still traverse serially: their emit appends to one out-vector, so they need per-task
collection before they can ride `traverseParallel`.
