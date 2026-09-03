# Physics

> Library documentation for `Code/Physics`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

box3d (Erin Catto's 3D engine, C API) wrapped in `module Physics`. box3d links PRIVATE and never
leaks — ids are stored as integer bits.

## Multithreaded solver (`Physics:TaskScheduler`)

`TaskScheduler.ixx` / `.cpp`. box3d's per-step fork/join is driven by the ENGINE job system, not by
the private thread pool box3d spawns for itself when `workerCount > 1` without task callbacks — that
pool would fight the engine's workers for the same cores.

* `PhysicsTaskScheduler` is an OBJECT owned by PhysicsWorld and handed over as
  `b3WorldDef::userTaskContext`. It comes back through the `userContext` parameter of both callbacks,
  so all scheduler state is per-world and none of it is global (`enqueueTask` / `finishTask` are its
  two statics).
* box3d has ALREADY sliced each phase into `workerCount` pieces and baked the slice index into every
  `taskContext`, so a task is an opaque closure: run it exactly once, on any thread, with no
  worker-index mapping. (This is why the callback takes no worker index, unlike box2d.)
* Each enqueue is one `EJobPriority::High` job — the step blocks on it, so it is on the frame's
  critical path. `finish` is `jobSystem.wait(counter)`.
* Task slots are a FIXED 256-entry (`B3_MAX_TASKS`) ring claimed with one `fetch_add` — no free list,
  no lock. box3d never enqueues more than `B3_MAX_TASKS` per step and finishes all of them inside
  that step, so any wrap reuses a slot at least a whole step old, and the slot addresses handed back
  to box3d stay stable.

### Why the step runs on main

`finishTask` MUST BLOCK — `b3World_Step` holds its stack across every fork/join — and box3d's rule is
"DO NOT call `b3World_Step` and park that fiber". So the step must run somewhere whose blocking wait
HELPS rather than suspends. `PhysicsWorld::update` is main-thread-only, and `JobSystem::wait()` helps
on the main thread, draining the High queue these tasks just landed in first. Driving the step from a
job fiber would satisfy the JobSystem but violate box3d's rule.

### Partition boundary

The partition deliberately does NOT include box3d. The callbacks are spelled in plain C types
(`void(*)(void*)`), identical to `b3TaskCallback*` once the typedef expands, so `b3WorldDef` takes
them without a cast and box3d still never reaches a Physics interface. The two facts the interface
therefore cannot check — `c_maxTasks == B3_MAX_TASKS`, and power-of-two — are `static_assert`ed
inside `enqueue`.

### Lifetime and worker count

* The job system is assumed ALWAYS LIVE (it has no `isInitialized` — see Threading).
  `Globals::physics` sits in init_seg `OC_SEG_PHYSICS` (XCU51), destructing after `~World` (bodies
  free into the live world) but BEFORE `~JobSystem` (XCU5), so even `b3DestroyWorld`'s tasks land on
  live workers. There is no inline fallback path.
* Tweak "Physics/World" → `Worker count`, live through `b3World_SetWorkerCount`; 1 = single-threaded
  A/B. It defaults to `jobSystem.getNumWorkers() + 1` — the `+1` is the thread DRIVING the step,
  which box3d counts as a worker and slices for. box3d's own guidance is to size this by PHYSICAL
  core count, which the job system itself now defaults to, so the default matches; the tweak remains
  the knob for trimming further (efficiency cores).

## Physics job markers

Always on, `EProfileCategory::Physics`.

* Each task runs under a `ProfileScope` named by the LITERAL `"Physics Job"`, opened by the JobSystem
  itself from the submit's `JobProfile` — the same string is the job name, so stats and flame graph
  agree. A step's fan-out reads as named spans on the worker tracks.
* box3d's own `taskName` is DELIBERATELY IGNORED. `ProfileRecord` stores names BY POINTER and the
  panel reads them back frames later, but box3d builds `taskName` into a TRANSIENT buffer, so the
  pointer is dangling by draw time and every physics scope rendered as a row of `?????`. Copying the
  text into the task slot does not fix it either — the slot is reused a step later and the copy is
  overwritten, so names on already-recorded blocks visibly keep changing. A literal is the only name
  that still means what it said, and box3d's per-phase detail is not worth a string store on this
  path.
* Unlike the per-entity component updates these are NOT rate-limited or gated: the record volume is
  bounded by the STEP rate, not the frame rate (`B3_MAX_TASKS` per step × the 120 Hz `Step Hz`
  ceiling ≈ 15.6K records/sec spread across every worker — tens of seconds of ring history per thread
  against a 512-frame panel window).
* The BLOCKING side gets one `"Physics fork/join"` marker per `finishTask`, on the thread driving the
  step. The row of them under "Physics step" IS the step's parallel structure, each one's width the
  cost of that phase. Because `wait()` helps rather than sleeps, unrelated jobs the main thread picks
  up to burn the stall nest inside that scope with their own markers — the same honest-but-broad
  attribution "UI prepare wait" has, and it also lands their allocations under the physics path in
  the Memory panel.

## `Globals::physics` (`PhysicsWorld`)

* `initialize()` once.
* `update(deltaSec)` per frame — a fixed-step accumulator stepping AT MOST ONCE per update. This is
  deliberate: box3d buffers each step's contact events until the NEXT step, so one step keeps them
  valid for the DEFERRED `dispatchContactEvents(contactCb)`, which main calls after the
  spatial/begin-frame joins (`update()` itself touches neither the index nor the renderer, letting
  those jobs overlap the step). Below `stepHz` the sim runs slower than real time instead of catching
  up, with the residual clamped to one owed step; a double fire is guarded by the step count.
  Gravity, paused, interpolate, timescale, substeps and Hz live under the Physics/World tweaks.
* `createBody(PhysicsBodyDesc, span<PhysicsShape>)` — Box / Sphere / Capsule / Hull / Mesh; the desc
  scale is baked into the shape dims.
* `castRayClosest()` with an optional layer mask.
* **Debug draw** — App registers a line sink and a view-position source once through
  `setDebugDrawCallback`; `update` emits wireframes while "Physics/Debug/Draw colliders" is on (also
  Draw joints / contacts / bounds + Range). Not gated on paused: inspecting a stopped sim is the
  point.

## Threading

* **`teleportBody(body, pos, rot)` is the ONLY way to move a body** — mutex-queued, applied before
  the first step and before the paused check, handle revalidated through `b3Body_IsValid`.
  `PhysicsBody` deliberately has NO `setTransform`.
* The queue generalizes to a BODY-COMMAND queue: `queueBodyCommand(body, EBodyCommand, a, b, c)` plus
  `queueSetGravity`. It covers velocities, impulses, forces, torques, gravity scale, damping,
  `setAwake`, `SetEnabled` (a.x 0 = `b3Body_Disable`, else Enable — both no-ops when already in that
  state; the pass-safe twin of `PhysicsBody::setEnabled`, used by the World's SIM LOD to disable
  dormant bodies), `NudgeVelocity` (bounded network correction), and world gravity.
* EVERY script physics-write thunk queues — scripts tick on workers, and even a velocity setter wakes
  the body into shared solver sets.
* All other box3d writes are MAIN THREAD ONLY; `PhysicsBody`'s direct setters remain as main-thread
  API. One frame of latency; reads return pre-write values.
* The DSL's `self.physics.teleport(position, eulerDeg)` rides the same queue and works on any body
  type.

## Buoyancy

`setWaterSurface(fn, activeFn)` plus per-body probes ("Physics/Buoyancy" tweaks); the App wires in
the ocean's `sampleWaterHeight` and `hasWater`.

The optional `activeFn` is the GLOBAL gate. The pass iterates bodies through a whole-world broadphase
overlap every step — box3d has no body-list API — so with no water anywhere
(`OceanGenerator::hasWater()` false: disabled, or the readback unprimed) the pass skips outright
instead of sweeping every shape to early-out one at a time.

## Handles

`PhysicsBody` / `PhysicsJoint` / `PhysicsMesh` are RAII movable handles.

* Joints (`createDistance/Revolute/Spherical/WeldJoint`) take world-space anchors; `staticBody()`
  anchors to the world.
* `PhysicsMesh` (triangle BVH) must outlive Mesh shapes; Hull shapes clone.
* Mesh colliders only collide on static bodies.

## `PhysicsComponent`

`Component Physics` in a `.pre`:

* `Body` Static / Kinematic / Dynamic; `Shape` Box / Sphere / Capsule / Hull / Mesh.
* `HalfExtents` / `Radius` / `HalfHeight` / `Density` / `Friction` / `Restitution` /
  `MaxHullVertices` / `Sensor` / `ContactEvents` / `LockRotation`.
* `LockRotation` is a solver-level lock on all angular axes — upright character capsules, see
  `Entities/Debug/playerCapsule.pre`. A locked dynamic body writes back only its POSITION, leaving
  `entity.rot` to scripts (player facing yaw).
* Demo: `Entities/Debug/physicsCube.pre`.

Hull and Mesh geometry come from the sibling render mesh through `CollisionCache` — a World member,
and THE asset→physics bridge: it snapshots a container's collision geometry from the same
`ISceneData` one import serves, then derives hulls, BVHs and occluder sets per container|node and
caches each.

`Col_*` meshes and nodes are collision proxies: they replace the same-named render mesh for physics
and are never rendered.

## The body owns the pose

* A body is created at spawn from the entity's world transform; physics never reads the entity's
  position again.
* Dynamic bodies write the simulated pose back each update, interpolated between fixed steps.
  Non-dynamic bodies stay put.
* Gizmo-dragging or writing `self.pos` moves only the mesh — `teleportBody` is the only way to move a
  body.
* The static-mesh occluder is baked once at spawn, and re-baked at the body's pose on
  resume-from-disabled. Sponza has a static mesh collider.

## Contact and sensor events

Shapes opt in through `ContactEvents` / `Sensor`, and events are delivered per step through
`update`'s contact callback → `World::handleContactEvent` → `PhysicsComponent::onContact` plus the
script's `OnPhysicsEvent` with the other entity, the begin/sensor bools and a `contactId` (sensor
events have contactId 0).

`getContactPoint(contactId, ...)` resolves the world hit position and normal, valid until the next
update.

## Layers

`PhysicsLayers::bit(name)` allocates one of 64 bits ("Default" = 0); shapes carry category, mask and
group.

In a `.pre`: `Layer Debris` + `CollidesWith Default, Player` (or `All` / `None`) + optional
`Group <int>`. Demo: `physicsSphere.pre`.
