# Spatial

> Library documentation for `Code/Spatial`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

## Structure

High-performance spatial index for culling and position lookup, independent of the entity hierarchy.

* Implicit 64-ary hierarchy over per-level hashed grids: 11 levels, cell size ×4 per level (finest =
  compile-time `SPATIAL_FINEST_CELL_SIZE`, default 2 m → ±2097 km).
* BMI2 Morton keys (`Spatial:Morton`). Each `CellRecord` holds a 64-bit child-occupancy mask, so
  queries descend with bit scans.
* The API takes `dvec3` (quantized to an int64 lattice); per-entry data is SoA with cell-relative
  float positions; query math is reference-relative float — exact at planet scale.

## `Globals::spatialIndex`

* `initialize()` before world spawns.
* `registerEntry(pos, radius, userData, layerMask)` → RAII `SpatialHandle` / `SpatialEntry`.
* `updateEntry` per frame: a same-cell rewrite is ~4 stores; cell changes stage into `PerWorker`
  lists, so it is callable from any job, one visitor per entry.
  `register` / `unregister` / `commit` / `setLayerMask` are main-thread.
* `commitFrame()` once per frame drains and sweeps.
* Queries are read-only between commits: `querySphere` / `queryAABB` / `queryFrustum` / `queryRay` /
  `queryNearest`.

### Visibility stamps

Two stamp sets — `markVisibleSet(Main, frustum)` and `markVisibleSphere(Near, ball)`, read back
through `getPassMask` / `isVisible`. Both are MULTITHREADED (`traverseParallel` in Query.cpp):

* The top level is a handful of huge cells, so a serial frontier expansion first splits the tree into
  at least *workers × 4* subtree roots — classifying cells and emitting the upper cells' own few
  entries as it goes — then a grain-1 High `parallelFor` runs `traverseCell` per root
  ("Spatial mark visible" spans on the worker tracks).
* Thread-safe by structure: an entry lives in exactly ONE cell, so no two roots stamp the same slot;
  cell maps only mutate in `commitFrame`; per-traversal counters (`TraverseStats`) accumulate locally
  and merge once — which also fixed the pre-existing stats race from concurrent script-worker
  queries. `OcclusionBuffer::isVisible`'s hidden-cell counter went atomic with a plain-int mirror for
  its tweak, since the tweak panel binds a raw `int*`.

### The one driving call

The App drives all of it through `SpatialIndex::update(camera, frustum, viewProjRelCamera)` — commit,
cull distance, mode/freeze gate, margin-inflated Main stamp with occlusion, and the Near ball with
requery hysteresis — AS A JOB in every mode:

* `kickUpdateJob(Renderer::getCullView(camera, viewportRect))` / `joinUpdateJob()`. The `CullView` is
  a `Core.Frustum` type, since Spatial and RendererVK do not link each other, and is copied into
  members. One "Spatial cull" High job.
* main.cpp kicks right BEFORE `physics.update`, overlapping `audio.update` and the renderer's
  "Begin frame job" (see RendererVK), and joins before `world.update`.
* An INVALID view skips that frame's whole update; the spawn guards cover the stale stamps.

**The kick/join window is the frame's only index-QUIESCENT stretch.** The entity-change drains, net
receive and `game.update` are the last pre-pass writers and the last registers; contact scripts fire
only AFTER the join (`physics.dispatchContactEvents`). Commit relinks cells and register's pool
growth reallocates the SoA that the traversals read, so neither may overlap a query.
**NOTHING added between the kick and the join may touch the index.**

### The frustum

Comes from `Renderer::computeCullFrustum(camera, viewportRect)` BEFORE `beginFrame` — camera and
viewport are final by then, TAA jitter is never baked into the mvp, and it applies the viewport rect
and publishes `getCenterViewProj` early. It is bit-identical to the UBO's by construction: both go
through `computeCenterViewProj`, the ONE place the center view-projection is built.

**VR** kicks the SAME job on LAST frame's head view — `getCullView` returns the camera + frustum
stored at the end of each VR `beginFrame`. That is one frame of cull latency, absorbed by the culling
margin and the near-ball slack; the fresh pose only exists after `xrWaitFrame` inside `beginFrame`,
so the job overlaps that stall. The first VR frame's view is invalid, so culling is skipped.
`computeCullFrustum` still asserts `!VR`.

## Entity integration

Every entity registers at the end of `Entity::create` (`Entity::spatialEntry`):

* Layer `SpatialLayer_Entity` always, plus `SpatialLayer_Render` when it has a render node — bounds
  from `RenderNode::getWorldBounds`, skinned × "Skinned radius scale"; otherwise a point at the
  entity position.
* Gameplay queries stay on the Render layer. The Entity layer is the World's update-selection layer.
* Update pushes per-pass masks: Main-visible entities push `PASS_ALL`; Near-only entities push
  `PASS_SHADOW|PASS_GI`, so shadows and GI keep off-screen entities.

### Update tier passes

`ESpatialPass::UpdateTier0/1/2`, bits `SpatialPassBit_UpdateTier*` / `SpatialPassBits_UpdateTiers`.
These are the World's SIM LOD, not rendering.

* `setUpdateLod(focus, count, radii)` (at most `MaxUpdateLodFocus` 16 points) makes `update()` stamp
  the three passes with `markVisibleSpheres` — ONE stamp generation over the union of the balls,
  since `markVisibleSphere` per call would leave only the last ball — BEFORE the culling-mode/freeze
  gate, so selection never stops with culling. `visiblePerPass` counts overlapping balls twice.
* `getPassMaskExact` is `getPassMask` WITHOUT the spawn guard: a never-stamped entry reads as in no
  pass, which is how the World tells a fresh unlinked entry from a placed one.
* `isStampedCurrent` / `stampCurrent` are MAIN-THREAD single-entry stamp accessors (exact compare),
  used by the World to mark the ancestors of a selected entity between the join and the pass.

See the SIM LOD section in [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md).

### Culling mode tweaks

`Spatial/Culling/Mode`: Off / Stats only / Cull (default) / Main only (debug — visibly kills
off-screen shadows and GI). `Freeze` stops re-stamping so the culled set can be inspected.

## CPU software occlusion

`Spatial:Occlusion`, `Globals::occlusionBuffer`, Spatial/Occlusion tweaks, default off.

The largest triangles of static mesh colliders — from CollisionCache occluder sets, registered by
PhysicsComponent, RAII `SpatialOccluder` — rasterize camera-relative into a 256×144 CPU depth buffer
plus 8×8 max-depth blocks. Plugged into the Main stamp as `IOcclusionTester`; occluded cells drop
from the MAIN pass only. Conservative toward visible.

## Static tier

Entries unchanged for N frames (Spatial/Static tweaks) promote into per-level Morton-sorted SoA
ranges (`StaticStore`), budgeted one level per commit; any change demotes. Sphere, AABB and frustum
testers all test static ranges 8-wide (AVX2 `test8`).

## Other

* **Stress harness** (`Globals::spatialStress`, Spatial/Stress tweaks) — synthetic entries on
  `SpatialLayer_Stress`, churn, timed queries, brute-force verify. Stats under Spatial/Stats.
* **Scripts** — `ctx->spatialQueryRadius` / `ctx->spatialGetNearestEntity`; NodeEditor
  "Get Nearest Entity" and "For Each Entity In Radius".
* **Planned, not yet built** — Threading-parallel QUERIES. The `markVisible*` stamps already fan out
  (above); the `query*` entry points still traverse serially, because their emit appends to one
  out-vector, so they need per-task collection before they can ride `traverseParallel`.
