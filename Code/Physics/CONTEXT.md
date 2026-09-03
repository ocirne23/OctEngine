# Physics

> Library documentation for `Code/Physics`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

box3d (Erin Catto's 3D engine, C API) wrapped in `module Physics`. box3d links PRIVATE and **never
leaks** — ids are stored as integer bits (`b3BodyId` in a `uint64`, `b3WorldId` in a `uint32`).

## The two rules

1. **`teleportBody` is the ONLY way to move a body.** `PhysicsBody` deliberately has no
   `setTransform`. See *The body owns the pose*.
2. **Every box3d WRITE that can run off the main thread must go through the body-command queue.**
   Any body write can wake the body, which mutates the world's shared solver sets.

## `Globals::physics` (`PhysicsWorld`)

[PhysicsWorld.ixx:13](Private/PhysicsWorld.ixx#L13).

### The frame

```cpp
physics.update(simDeltaSec);                       // main.cpp:775 — overlapped by the cull + begin-frame jobs
...  spatial/begin-frame joins, game.update  ...
physics.dispatchContactEvents(callback);           // main.cpp:796
```

`update(deltaSec)`:

1. **`applyQueuedCommands()` — before the paused check.** Placing a body or setting its state is
   authoring, not simulation.
2. `stepSimulation` unless paused.
3. Collider debug draw, **not** gated on paused: inspecting colliders with the simulation stopped is
   the point. It runs after the step so the wireframes match the poses the entities render from.

**`stepSimulation` takes AT MOST ONE step per update.** Deliberate:

* box3d buffers each step's contact and sensor events until the NEXT step, so one step keeps them
  valid for the deferred `dispatchContactEvents`.
* It bounds the step cost per frame.
* Below `stepHz` the sim runs **slower than real time instead of catching up**; the residual is
  clamped to one owed step so a hitch never spirals.

**`dispatchContactEvents` is deferred out of `update()`** because contact scripts query the spatial
index and can touch renderer state (light/sun thunks) — so `update()` itself touches neither, and the
main loop overlaps it with the "Spatial cull" and "Begin frame" jobs. It no-ops unless a step ran
since the last dispatch (`m_lastDispatchedStep`): the buffers still hold the OLD events, and refiring
would duplicate them.

### Creation and queries

| Call | Notes |
|---|---|
| `createBody(PhysicsBodyDesc, span<PhysicsShape>)` | `desc.transform.scale` is baked into the shape dimensions. |
| `createCollisionMesh(vertices, indices)` | Shared triangle BVH. **Standalone: does not need the world and may be built before `initialize()`** — which is what lets tile builds run on jobs. |
| `staticBody()` | A shapeless static body owned by the world, for anchoring joints to the world itself. |
| `createDistance/Revolute/Spherical/WeldJoint` | Anchors and axes are WORLD space; local frames derive from the bodies' current poses, **so create joints after the bodies are at their intended placement**. |
| `castRayClosest(origin, translation, mask, ignoreBody, staticOnly)` | See below. |
| `getContactPoint(contactId, ...)` | Resolves a `ContactEvent::contactId` to its first manifold point. Valid only before the next `update()`. False if the contact is stale or has no manifold point (a purely speculative contact). |
| `getStepCount()` / `getInterpolationAlpha()` / `getStepHz()` | `getStepHz` exists for code that must pace itself in **whole steps** — the network claim stream does. |

**`castRayClosest`'s two extra parameters exist for network claim validation:**

* `ignoreBody` skips every shape of that body — needed when the ray travels between two poses of the
  body itself, because box3d's closest-cast only ignores overlap AT the origin, so the body's own
  surface a step ahead would count as a hit.
* `staticOnly` restricts hits to static bodies: a DYNAMIC body in a movement claim's path is
  something the claimant can legitimately push, never a wall to validate against.

### Tweaks

| Tweak | Default | Range |
|---|---|---|
| `Physics/World → Gravity` | (0, −9.81, 0) | |
| `Physics/World → Paused` / `Interpolate` | false / true | |
| `Physics/World → Time Scale` | 1 | 0..4 |
| `Physics/World → Sub Steps` | 4 | 1..16 |
| `Physics/World → Step Hz` | **20** | 5..120 |
| `Physics/World → Worker count` | see below | 1..`B3_MAX_WORKERS` |
| `Physics/Buoyancy → Density (kg/m3)` | 1000 | 0..3000 |
| `Physics/Buoyancy → Linear drag` | 3 | 0..20 |
| `Physics/Debug → Draw colliders / joints / contacts / bounds` | off | |
| `Physics/Debug → Range` | 64 m | 4..1024 |

## The multithreaded solver (`Physics:TaskScheduler`)

box3d fans each solver phase out through two callbacks instead of spawning threads of its own —
which is what `b3WorldDef::workerCount > 1` WITHOUT callbacks does, a second private thread pool
competing with the engine's for the same cores.

* `PhysicsTaskScheduler` is an OBJECT owned by PhysicsWorld and handed over as
  `b3WorldDef::userTaskContext`. It comes back through the `userContext` parameter of both callbacks,
  **so every bit of scheduler state is per-world and none of it is global.**
* **box3d has already sliced each phase into `workerCount` pieces and baked the slice index into
  every `taskContext`**, so a task is an opaque closure: run it exactly once, on any thread, with the
  pointer box3d gave. That is why the callback carries no worker index, unlike box2d.
* Each enqueue is one `EJobPriority::High` job — the step blocks in `finish()` until it lands, so it
  is on the frame's critical path. `finish` is `jobSystem.wait(counter)`.

### The task ring

A FIXED 256-entry array (`c_maxTasks`, static_asserted against `B3_MAX_TASKS` and against being a
power of two), claimed with **one `fetch_add` masked into the ring — no free list, no lock**.

box3d never enqueues more than `B3_MAX_TASKS` per step and finishes every one inside that same step,
so any reuse is at least a whole step old, and the slot addresses handed back to box3d stay stable
for the world's life. An assert catches a wrap onto a live task.

### Why the step runs on main

`finishTask` MUST BLOCK — `b3World_Step` holds its stack across every fork/join — and box3d's own
rule is **"do not call `b3World_Step` and park that fiber"**. So the step has to run somewhere whose
blocking wait HELPS rather than suspends.

`PhysicsWorld::update` is main-thread-only, and `JobSystem::wait()` helps on the main thread, running
ready jobs and reaching the High queue these tasks just landed in first. Driving the step from a job
fiber would satisfy the JobSystem but violate box3d's rule.

### The partition boundary

`:TaskScheduler` deliberately does not include box3d at all. The callbacks are spelled in plain C
(`void(*)(void*)`), identical to `b3TaskCallback*` once the typedef expands, so `b3WorldDef` takes
them without a cast. The two facts the interface therefore cannot check are `static_assert`ed inside
`enqueue` in the .cpp, which does see box3d.

### Worker count

`defaultWorkerCount()` = `clamp(jobSystem.getNumWorkers(), 1, B3_MAX_WORKERS)`
([TaskScheduler.cpp](Private/TaskScheduler.cpp)). box3d counts the thread driving the step as a
worker too and hands it a slice, so the engine's worker count already covers main.

The `Worker count` tweak is live (`b3World_SetWorkerCount` on the next step). **1 = single-threaded,
which is the A/B toggle for measuring what the fan-out actually buys on a scene.** box3d's own
guidance is to size this by PHYSICAL core count — which the job system already defaults to above 16
cores — so the tweak is the knob for trimming further on efficiency cores.

### Lifetime

`Globals::physics` sits in init_seg `OC_SEG_PHYSICS`, destructing **after `~World`** (XCU8; entities
free their bodies into the still-live world) but **before `~JobSystem`** (XCU5), so even
`b3DestroyWorld`'s teardown tasks land on live workers. **This is what lets the task scheduler assume
the job system is always up — there is no inline fallback path.**

## Profile markers

Always on, `EProfileCategory::Physics`. Scope tree: `"Physics"` → `"Physics step"` →
`"Buoyancy"`, with `"Collider debug draw"` and `"Physics contacts"` as siblings.

* **Each task runs under a `ProfileScope` named by the LITERAL `"Physics Job"`**, opened by the
  JobSystem itself from the submit's `JobProfile` — the same string is the job name, so stats and
  flame graph agree.
* **box3d's own `taskName` is DELIBERATELY IGNORED.** `ProfileRecord` stores names BY POINTER and the
  panel reads them back frames later, but box3d builds `taskName` into a TRANSIENT buffer, so the
  pointer is dangling by draw time — every physics scope rendered as a row of `?????`. Copying the
  text into the task slot does not fix it either: the slot is reused a step later and the copy is
  overwritten, so names on already-recorded blocks visibly keep changing.
* **Not rate-limited or gated**, unlike the per-entity component updates: the record volume is bounded
  by the STEP rate, not the frame rate — `B3_MAX_TASKS` per step × the 120 Hz `Step Hz` ceiling
  ≈ 15.6K records/sec spread across every worker, which is tens of seconds of ring history per thread
  against a 512-frame panel window.
* **One `"Physics fork/join"` marker per `finishTask`**, on the thread driving the step. The row of
  them under "Physics step" IS the step's parallel structure, each one's width the cost of that
  phase. Because `wait()` helps rather than sleeps, unrelated jobs the main thread picks up nest
  inside that scope with their own markers — the same honest-but-broad attribution "UI prepare wait"
  has, and it also lands their allocations under the physics path in the Memory panel.

## Threading

### The body-command queue

`queueBodyCommand(body, EBodyCommand, a, b, c)` plus `queueSetGravity`. Queued under a mutex, applied
in call order at the start of the next `update()`, **dropped if the body died before the drain**.
Reads meanwhile see pre-write values: one frame of latency.

| Command | `a` / `b` / `c` |
|---|---|
| `Teleport` | pos (+ the separate `rot` field) |
| `SetLinearVelocity`, `SetAngularVelocity` | the velocity |
| `ApplyImpulse`, `ApplyAngularImpulse`, `ApplyForce`, `ApplyTorque` | the vector |
| `ApplyImpulseAtPoint`, `ApplyForceAtPoint` | `a` = vector, `b` = world point |
| `SetGravityScale`, `SetLinearDamping`, `SetAngularDamping`, `SetAwake` | scalar in `a.x` |
| `SetEnabled` | `a.x` 0 = `b3Body_Disable` (out of the broadphase AND solver entirely, pose kept), else Enable. **The pass-safe form of `PhysicsBody::setEnabled`.** |
| `SetWorldGravity` | `a` (no body) |
| `NudgeVelocity` | `a` = target linear velocity, `b` = target angular velocity, `c.x`/`c.y` = max linear/angular change THIS drain |

**`NudgeVelocity` is the network correction "push".** The linear half lands as a centre impulse of at
most `mass * c.x`, so it composes with the sim like any other hit. The angular half is the exact
result the ideal angular impulse `I·dw` would produce, applied as a capped velocity add — the inertia
tensor cancels out of the math.

The queue swaps `m_commands` with `m_commandScratch` at drain, so neither vector reallocates.

### What is main-thread-only

Everything else. `PhysicsBody`'s direct setters remain as main-thread API, and are sanctioned in
`InputControls::updatePlayerControl` (before `physics.update`).

**EVERY script physics-write thunk queues** — scripts tick on workers. The DSL's
`self.physics.teleport(position, eulerDeg)` rides the same queue and works on any body type.

### Body create/destroy

box3d's body create and destroy mutate shared world arrays (body pools, broadphase), so concurrent
spawn and despawn jobs serialize on ONE module-wide mutex, `g_bodyLifecycleMutex`
([Body.ixx:12](Private/Body.ixx#L12)). It is at namespace scope per the `/Zc:threadSafeInit-` rule;
a `std::mutex` is constant-initialized, so static init is safe.

## `PhysicsBody`

Move-only RAII, like `RenderNode`. Destroying the handle destroys the body.

* Reads: `getPosition`, `getRotation`, `getLinearVelocity`, `getAngularVelocity` (**radians/second**),
  `getMass` (0 for static/kinematic), `getCenterOfMass` (world), `getPointVelocity` (includes spin),
  `getGravityScale`, damping, `isAwake`, `isEnabled`.
* **IMPULSES are instantaneous** (a hit, a jump). **FORCES accumulate over the step and box3d clears
  them every step**, so a continuous push must be re-applied each frame. Both wake the body.
* The `AtPoint` variants take a WORLD-space point and therefore also impart spin; the plain forms
  apply at the centre of mass and do not.
* `setEnabled` removes and re-adds the body entirely — no collision, queries or events.
  **Expensive in box3d; callers should track state and avoid redundant toggles.**
* `setAwake` is what makes a settled body react to a changed velocity or force.
* Per-body `gravityScale`: 0 floats, 1 normal, negative falls upward.

## Shapes, bodies and layers

`PhysicsShape` ([Types.ixx:26](Private/Types.ixx#L26)): `Box` / `Sphere` / `Capsule` / `Hull` /
`Mesh`, with `halfExtents`, `radius`, `halfHeight` (centre to hemisphere centre, along local Y),
`offset`, `density` (1000 kg/m³), `friction` (0.6), `restitution`, `categoryBits`, `maskBits`,
`groupIndex`, `isSensor`, `contactEvents`.

* **Hull** reduces a point cloud to a convex hull at body creation, budget `maxHullVertices`
  (32, clamped to 4..64).
* **Mesh** points at a shared `PhysicsMesh` that **must outlive the body**, and works on **static
  bodies only**.
* `PhysicsBodyDesc::lockRotation` is a solver-level lock on all angular axes — upright character
  capsules. Contacts apply no torque.
* `userData` is reported back through `ContactEvent`; the engine stores `Entity*` there.

**Layers** (`PhysicsLayers::bit(name)`) allocate one of 64 category bits on first use; `"Default"` is
bit 0. Two shapes collide when **each one's mask contains the other's category**, unless a non-zero
matching `groupIndex` overrides (negative = never, positive = always).

In a `.pre`: `Layer Debris` + `CollidesWith Default, Player` (or `All` / `None`) + optional
`Group <int>`. Demo: `physicsSphere.pre`.

## Buoyancy

`setWaterSurface(fn, activeFn)`. `fn(x, z)` returns the water surface world Y at that column, or
`-FLT_MAX` where there is no water; the App wires in the ocean's `sampleWaterHeight`.

Every dynamic body gets probe-based buoyancy and drag **before each fixed step** (box3d clears
applied forces every step). Bodies denser than water sink, lighter ones float and bob in the swell,
and tilted floaters right themselves — per-probe forces torque the body for free.

**`activeFn` is the cheap global gate.** The pass iterates bodies through a WHOLE-WORLD broadphase
overlap every step, because box3d has no body-list API, which is pure waste while no water exists
anywhere. `OceanGenerator::hasWater()` false — disabled, or the readback unprimed — skips the pass
outright. An empty `activeFn` means always active; an empty `fn` disables the pass entirely.

## Debug draw

The App registers a line sink and a view-position source ONCE through `setDebugDrawCallback`, which
keeps Physics free of any renderer dependency. The view position is a **callback rather than an
`update()` argument, because the simulation has no notion of a viewer and should not carry one
through its signature.**

Every shape type decomposes into world-space segments, plus optional joints, contacts and AABBs.
Drawing is bounded to `Range` around the view position, and mesh triangles are additionally CPU-culled
per triangle so a huge static mesh collider does not emit its whole wireframe. Colour is packed RGBA8
with R in the low byte, matching GLSL `unpackUnorm4x8`.

## `PhysicsComponent` (lives in Entity)

`Component Physics` in a `.pre`: `Body` Static/Kinematic/Dynamic, `Shape` Box/Sphere/Capsule/Hull/Mesh,
`HalfExtents` / `Radius` / `HalfHeight` / `Density` / `Friction` / `Restitution` /
`MaxHullVertices` / `Sensor` / `ContactEvents` / `LockRotation`, plus `Layer` / `CollidesWith` /
`Group`. Demo: `Entities/Debug/physicsCube.pre`.

Hull and Mesh geometry come from the sibling render mesh through `CollisionCache`, a World member and
**THE asset→physics bridge**: it snapshots a container's collision geometry from the same `ISceneData`
one import serves, then derives hulls, BVHs and occluder sets per container|node and caches each.

`Col_*` meshes and nodes are collision proxies: they replace the same-named render mesh for physics
and are never rendered.

### The body owns the pose

* Created at spawn from the entity's composed world transform. **Physics never reads the entity's
  position again**, and the world scale is baked into the shape.
* Dynamic bodies write the simulated pose back into the entity each update, interpolated between
  fixed steps from `prevPos`/`currPos`/`prevRot`/`currRot`. Non-dynamic bodies stay put.
* **A `lockRotation` body writes back only its POSITION**, so `entity.rot` stays free for
  script-driven facing (player yaw).
* Gizmo-dragging or writing `self.pos` moves only the mesh.
* The static-mesh occluder is baked once at spawn, and re-baked at the body's pose on
  resume-from-disabled.

### Suspend / park

| Call | Meaning |
|---|---|
| `suspendBody()` / `suspendPhysicsTree` | The entity was DISABLED (`EEntityFlag_Enabled`). `updateTree` stops reaching it, so the body would otherwise keep colliding invisibly. Drops the occluder too; the next update after re-enable re-adds and resyncs. |
| `park(disable)` | SIM LOD dormancy. Zero the velocities, then either DISABLE or merely sleep. |
| `unpark()` | Zero the velocities — a body parked inside a crowd may still hold a contact push-out — and enable. **Skipped while `suspended`**: an Enabled-off subtree owns its own disable. |

Both park calls ride the body-command queue, so **the entity pass never writes box3d.** See the SIM
LOD section in [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md).

## Contact and sensor events

Shapes opt in through `ContactEvents` or `Sensor`. `ContactEvent` carries `userDataA/B` (`Entity*`),
`begin`, `sensor` and `contactId` (**0 for sensor events, which have no manifold**).

Delivery: `dispatchContactEvents` → `World::handleContactEvent` → `PhysicsComponent::onContact` plus
the script's `OnPhysicsEvent` with the other entity, the begin/sensor bools and the contactId.

**Do not store the returned `ContactEvent` references** — the dispatch reads box3d's buffers with no
copy.
