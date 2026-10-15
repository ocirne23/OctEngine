# The components

> Part of the Entity documentation ([`Docs/Context/Entity/CONTEXT.md`](CONTEXT.md)) — read that first.

| Component | Notes |
|---|---|
| `SceneComponent` | Children. The list is mutated through `addChild` / `removeChild` / `replaceChild` / `adoptChildren` (plain mutators: the one place to hook a future "children changed"); `spawn` appends directly. |
| `RenderComponent` | RenderNode + local transform, static or skinned, plus `Color`. `place` sets the node transform; the spatial refresh and the culled submit are `Entity::updateSelf`'s (see CullMode ([CONTEXT.md](CONTEXT.md))). |
| `AnimatorComponent` | AnimationPlayer + AnimStateMachine from `.apl`; gameplay through `stateMachine.setFloat/Bool/Trigger`, clip events through `onEvent`. Skinned meshes only. |
| `SceneAnimatorComponent` | **PARKED — no component id, no type bit, unknown to the Entity class.** A rigid-part model as BONES inside one entity + the GENERAL procedural animation (layers / tracks). Kept compiling; below. |
| `HumanoidAnimatorComponent` | The same bone model with the BARE-MINIMUM animation: four limbs about local Z, nothing else (CubeGuy, every game unit). Below. |
| `ScriptComponent` | See [`Docs/Context/Script/CONTEXT.md`](../Script/CONTEXT.md). |
| `PhysicsComponent` | See [`Docs/Context/Physics/CONTEXT.md`](../Physics/CONTEXT.md). |
| `AudioComponent` | See [`Docs/Context/Audio/CONTEXT.md`](../Audio/CONTEXT.md). |
| `ParticleComponent` | See [`Docs/Context/Particle/CONTEXT.md`](../Particle/CONTEXT.md). |
| `ForceComponent` | See [`Docs/Context/Force/CONTEXT.md`](../Force/CONTEXT.md). |
| `LightComponent` | Below. |
| `NetworkComponent` | Multiplayer, below ([Multiplayer.md](Multiplayer.md)). |
| Game components (9/10/11) | Below. |

## `SceneAnimatorComponent` — PARKED (no ID)

> **Nothing uses it now, so it holds NO component id and NO type bit, and the Entity class does not
> know it**: no create / destroy case, no `update` / `place` call in `updateSelf`, no RootOnly
> bounds, no prefab write, no Entity Editor pass-through, no natvis entry, no SIM LOD kind, and
> `GameUnitComponent` (tint, `tickModel`) handles the Humanoid only. A `.pre` that authors
> `Component SceneAnimator` gets a warning and no component. **The code is kept compiling so it does
> not rot** (`Component.ixx` still imports the partition; `World::buildSceneAnimatorSpawnInfo`
> still exists, with no caller). To bring it back, follow the list in the comment above the struct
> in `SceneAnimatorComponent.ixx` — it is the "Adding a component type" ([CONTEXT.md](CONTEXT.md)) checklist plus the
> bone-model call sites. **Everything below describes the code as it is, not something that runs.**

A rigid-part model INSIDE ONE ENTITY and its GENERAL procedural animation (layers and tracks). It
shares nothing with `AnimatorComponent` — no skeleton, no clips, no `.apl`, no string parameters,
no keys, no sampled pose. The fixed four-limb walk is a SEPARATE component,
`HumanoidAnimatorComponent` (below); what the two have in common is `:BoneModel` + `BoneMath`.

### What the two bone-model animators share — with NO overhead

Everything shared is an inline function, a template, or a plain struct; **there is no virtual
call, no function pointer and no base-class dispatch**, so each animator compiles its own copy and
the sharing costs nothing at run time (non-Debug: `/Ob2` + `/GL`; a Debug build does not inline
and keeps the calls).

* **`Animation:BoneMath`** (Animation library, Core-only — pure numerics): `polyTrig`,
  `polyTrig4` (SSE), `phaseTrig`, and `ParentCompose` — `composeTransform(parent, local)` for many
  locals under one parent, the parent prepared once. The `Transform` layout static_asserts live
  there.
* **`Entity:BoneModel`** (it needs `RenderNode`, so it cannot be in Animation): `BoneModelRig` (the
  authored bones; `SceneAnimRig` and `HumanoidRig` DERIVE from it, plain inheritance) and the
  templates over the component's own bone type — it needs `nodeLocal` and `node` —
  `spawnBoneNodes`, `placeFlatBones`, `submitBones`, `addBoneRestBounds`.
  `World::appendSceneAnimBones` parses `Bone` blocks into a `BoneModelRig` for both.
* **NOT shared, on purpose: the per-instance state.** Each component holds its own plain fields
  (`drivenDistance` / `drivenDelta` / `lastPos` / `hasLastPos` for the distance source,
  `placedWorld` / `placeDirty` for the compose-again test, `atRest`) and its own copy of the two
  small functions that use them (`feed`, the private `beginTick`; the test is inline in `place`).
  There is no wrapper struct around them. `GameUnitComponent::tickModel` uses the Humanoid's
  `feed` + `atRest` directly. (The parked SceneAnimator's `atRest` is a `static constexpr false` —
  a timed layer moves a standing model, so it has no rest pose.)

**The motion is closed-form.** Shared per template in `SceneAnimRig` (`layers`, `tracks`, `parts`);
per instance only `layers` — an `oc::small_vector<LayerState, 2>`, inline:

```
value = layerWeight * amplitude * sin(2 pi * (harmonic * layerPhase + trackPhase))
```

* **`SceneAnimRig::Layer`** + the instance's **`LayerState`** — ONE phase + ONE weight. A **stride**
  layer (`cyclesPerMetre > 0`) advances its phase by the DISTANCE moved (feet do not slide) and
  takes its weight from the speed (1 at `fullSpeed`); a **timed** layer runs at `cyclesPerSecond`
  with a constant `weight`. Weights move at `fadeRate` per second. **A blend IS the weight.**
  `manualWeight >= 0` is the gameplay override. A layer with weight 0 does not advance its phase.
* **`Track`** — a rotation about a fixed bone-local axis AROUND THE BIND rotation, or a translation
  along a fixed offset. `harmonic` is 1 or 2 only: harmonic 2 comes from the double-angle
  identities and the track phase is stored as sin/cos, so **a tick costs one `sin`/`cos` pair per
  ACTIVE LAYER** (kept in `LayerState::s` / `c`), never per track. The quaternion's half-angle is
  a polynomial + normalize (swing capped at 180°).
* **`Part`** — a bone that tracks move (bone index; the bind is the bone's). Its tracks are two
  runs, `numRotations` then `numTranslations`, so `evaluate()` has no branch per track and writes IN
  PLACE only the channels that have tracks.
* **`SwingBatch`** — the usual limb (ONE rotation track, no translation, identity bind rotation)
  does not become a `Part`: `finalize` packs up to four into one SoA batch, and
  `evaluateSwings` makes the four quaternions in ONE SSE pass (value → half angle → polynomial →
  `rsqrt` + one Newton step → transpose → four stores). `uniformLayer` (every lane on the same
  layer + harmonic — the limb walk) broadcasts the layer term instead of gathering it. CubeGuy's
  four limbs are one batch, and its `parts` list is empty.
* **`advance(dt, distance)`** returns FALSE when the pose is the same as after the last tick. The
  layer's sin / cos is `phaseTrig`, not the CRT: the phase is folded onto the nearest quadrant axis,
  so the polynomial sees |x| ≤ π/4 (error < 4e-7).
* **SSE in `place()`** (flat rigs): `Transform` is 32 bytes — `(pos.xyz, scale)` + `quat (x, y, z,
  w)`, static_asserted — so a transform is two registers. `world` is prepared ONCE per call (its
  rotation as three columns × scale, its quaternion as four sign-folded broadcast rows); each bone
  is then two loads, six shuffles, seven FMAs and two stores. FMA is baseline (`/arch:AVX2`).
* `refreshNodeLocal`: a render recipe whose local transform is a pure offset
  (`Bone::renderOffsetOnly` — the usual mesh offset) needs no quaternion product.
* **Authoring side** (World's parse): `addLayer` / `swing` / `bob` / `walkCycle(WalkCycle)` name
  their bone; `finalize(ownerName)` resolves the names (an unknown bone is a warning) and groups
  the tracks into `parts`. The rig is immutable after.

**A BONE is what an entity with only a transform and a RenderComponent would be, minus the
entity**: `Bone { Transform local; Transform nodeLocal; RenderNode node; }` in the component's
`bones` — an `oc::small_vector<Bone, 6>`, so up to 6 bones sit INSIDE the component, in the
entity's own allocation (no allocation per spawn, and the visit stays in the entity's memory); more
bones spill to the heap. `nodeLocal` = `local` × the render recipe's local transform, refreshed only
when `local` changes. No entity allocation, no spatial entry, no visit, no child
list, no name — CubeGuy is 1 entity, not 7 (a unit: 2, not 8). Bones can nest (`parent`, parent
before child); a bone with no `Component Render` is a pure joint. The limbs were full child
entities before; that design, its cached child pointers and `SceneComponent::childrenChanged`
are gone.

**The design rule is MINIMUM MUTATION:**

* A tick that does not change the pose (`advance` returns false — a unit that stands still)
  **writes no bone**: no evaluation, no write.
* A moved part gets `local.quat` only, `local.pos` only, or both — whatever its tracks move.
* **`place(renderer, world, passMask)`** is the placement tail of `Entity::updateSelf` (every visit,
  frozen or not, right after the entity's own render submit): when NEITHER a bone's local changed
  (`placeDirty`) NOR the entity's world transform (`placedWorld`), it composes nothing and only
  submits the nodes (`renderNode`, lock-free); else, on a FLAT rig (`SceneAnimRig::flat`: no bone
  has a parent bone — CubeGuy), ONE compose per bone (`world` × `nodeLocal`) + `setTransform`; a
  nested rig runs the world chain in a stack scratch, two composes per bone.
  `passMask` 0 = culled: nothing is submitted.
* **Open cost, in RendererVK:** the submit is one `Renderer::renderNode` per bone — per call an
  atomic claim on the shared instance cursor, per-mesh atomic counts and the texture-use note (a
  `log2` + three `noteUse`). A batched submit for one model (one claim, one texture note) is the
  next step; a bone spawn (`spawnNodeForIdx`) takes no renderer lock after warm-up.
* **The walk drives itself**: the component measures the planar (XZ) distance its OWN entity moved
  since the last tick, so no script or gameplay code sets a speed. It runs AFTER
  `PhysicsComponent::update` in `updateSelf`, so the distance has this frame's pose. The distance is
  in the parent's space — the world for a root entity, which a unit is; **an animator on a child
  that only moves with its parent sees no distance.**
* Catch-up ticks (SIM LOD) stay correct: the distance covers the same span as the delta.
* **A MODEL CHILD is driven by its parent**: `feed(metres, deltaSeconds)` (a parent → child
  write, before the child's visit) switches the animator to driven mode — it stops measuring itself,
  and on its own visit it consumes and zeroes the fed distance AND the fed delta.
  `GameUnitComponent::tickModel` is the user, once per unit tick, also at 0 m (a standing unit's
  swing still has to fade out).
* **SIM LOD.** A driven animator ticks ONLY on a frame with a feed, with the PARENT's delta
  (catch-up included), so it follows the unit's cadence exactly. This is load-bearing: the model
  child is covered by the unit's RootOnly entry, has no entry of its own, and the World therefore
  hands it the full frame delta every frame (`alwaysVisited`) — its own delta is not a schedule.
  A SELF-driven animator (an entity with its own entry) is a following sim kind under the
  "Follows: animators" tweak, like `AnimatorComponent`. `place()` is placement, not simulation: it
  runs every visit, and does nothing but submit while no pose and no transform changes.
* **`Stride` / `FullSpeed` are in the animator entity's LOCAL units** (the distance is divided by
  `entity.scale`), so one model prefab keeps its gait at every reference-site `Scale` — CubeGuy at
  0.25 walks `Stride 6` = 1.5 m per cycle, the Titan's at 1.12 walks 6.7 m.

**`SceneAnimRig`** (shared per template, in the SpawnInfo): `bones` (name, parent, authored bind,
and the bone's render recipe — a plain `RenderComponent::SpawnInfo` from
`World::buildRenderSpawnInfo`, so a bone authors `Component Render` exactly like an entity: container,
`Node`, local `Position`, `Color`; static nodes only), `flat`, and the motion (`layers`, `tracks`,
`parts` — above). Tracks find their bone by NAME at `finalize`.

**CULLING is the entity's.** The bones ride the entity's pass mask. `CullMode RootOnly` measures
them into the template's tree bounds (`addRestBounds`, called from `gatherTreeCullBounds`; the REST
pose, a swing is not added) and gives the entry the Render layer. **A bone model on a `PerEntity`
entity with no RenderComponent of its own has a point entry on the Entity layer only: it is never
culled and gameplay queries on the Render layer do not find it — author `RootOnly`, or put it under
a RootOnly root** (the unit prefabs).

* `GameUnitComponent`'s `tintSubtree` colours the bones too (it reads `bones` + the rig's render
  recipes).
* **Not headless** (render nodes), and **no script surface** — nothing registers bit 12 with the DSL
  bindings, so no script can require it and `syncScriptData` needs no slot branch for it.
* Gameplay C++: `findLayer(name)` + `setLayerWeight(layer, w)` overrides a layer's weight; `< 0`
  hands it back.
* The Entity Editor has no section for it; `commitRespawn` carries the authored recipe over as it is.
  The recipe IS the serialization: the SpawnInfo keeps the authored `AssetNode`.
  So a save never bakes a live pose (the old child-entity hazard is gone) — and **the bones are not
  editable in the Entity Editor**: they are not entities. Edit the `.pre` text.

Authoring (`Component SceneAnimator`, angles in degrees, phases in 0..1 cycles):

```
Bone <name>                                   Position / Rotation / Scale like an entity;
    Component Render ...                      optional; nested `Bone`s are its children
Layer <name>
    Stride <m per cycle>  FullSpeed <m/s>     stride layer: phase by distance, weight by speed
    Duration <sec per cycle>  Weight <0..1>   timed layer (no Stride)
    Fade <sec>    Axis x y z                  the layer's default swing axis, bone-local
    Walk                                      preset: LegAngle / ArmAngle / LeftLeg / RightLeg /
                                              LeftArm / RightArm / BobBone / BobHeight
    Swing <bone> <angleDeg> [phase] [harmonic 1|2]     child Axis
    Bob <bone> <height> [phase] [harmonic 1|2]         child Direction
```

No demo prefab uses it now (CubeGuy is a `HumanoidAnimator`); the equivalent `Layer Walk` block is
the comment at the top of `Entities/Debug/CubeGuy.pre`.

## `HumanoidAnimatorComponent` (ID 12)

**THE BARE-MINIMUM ANIMATOR, for exactly one setup (CubeGuy): four limb bones that swing about
their local Z, driven by the distance moved — and nothing else, on purpose.** No layers, no tracks,
no parts, no batches, no gameplay weight override. It is a bone model like the SceneAnimator's
(same `Bone` blocks, same culling and SIM LOD rules, same `feed` from
`GameUnitComponent::tickModel`, same unit tint) — see the shared-code list above.

* **`HumanoidRig::configure` checks the setup ONCE**, at template build: flat rig, the four bones
  exist and are distinct, identity bind rotation, and a limb `Component Render` with NO local
  transform — **the limb mesh's origin must BE the pivot** (baseshapes' `Limb3`, not `Pillar3`
  with a `Position` offset). **A setup that does not fit is a warning and NO component**
  (`World::buildHumanoidAnimatorSpawnInfo` returns null — the model is absent, which is hard to
  miss). **So `update` has no check and no loop over data.**
* **At rest = no work.** `update` returns right after `beginTick` when the distance is 0 and
  `walkWeight` is 0 — before the division and the fade. It reports the rest pose in its `atRest`
  field; `GameUnitComponent::tickModel` then stops feeding a standing unit
  (`m_modelResting`: it returns at its top, before the model child is touched), so a driven model
  of an idle unit costs one failed `beginTick` test. A distance > 0 starts it again. Past that test the
  pose ALWAYS changes (moving, or fading out), so there is no second early-out.
* **The tick:** one weight, one phase (`walkPhase` / `walkWeight` are the whole state), one
  `BoneMath::phaseSin` — the sine alone and with NO branch (`phaseTrig`'s quadrant switch depends
  on the data, and a crowd's phases are random, so it mispredicts) — then **the four limbs as ONE SSE pass** over the rig's one lane set, `halfAngle`.
  A Z rotation is the quaternion `(0, 0, sin h, cos h)`: no product, no axis multiply. The limbs
  differ only in the SIGN of their half angle (+leg, −leg, −arm, +arm — the same signs as
  `SceneAnimRig::walkCycle`), and the polynomial carries it (sin is odd, cos is even). The half
  angle is clamped to 45° (a 90° swing), where the un-normalized polynomial is within 3e-7, so
  there is no `rsqrt`.
* **ROTATION ONLY.** A `RenderNode` turns a mesh about the MESH origin, so a mesh that hangs off
  its bone by a render offset has to swing on an arc — a position write per limb per tick. With
  the origin at the pivot the node position never changes: the tick's whole output is an unpack,
  four shuffles and **four quaternion stores** into `nodeLocal.quat`; `pos` and `scale` keep what
  spawn set from the bind. That is why `configure` refuses a render offset.
* **Lane i IS bone i:** `configure` REORDERS the rig's bones so the limbs are bones 0..3 in `Limb`
  order (LeftLeg, RightLeg, LeftArm, RightArm); the rest follow in authored order. No index
  table — and the bone order is NOT the `.pre` order (CubeGuy: limbs, then Head, Body).
* **`Bone` is `{ nodeLocal, node }`** — no separate pose: the tick writes `nodeLocal` directly and
  `place()` (always the flat SSE path) reads nothing else.
* A unit that stands still returns after the weight test and writes nothing.

```
Component HumanoidAnimator
    Stride / FullSpeed                        the entity's LOCAL units, like a stride Layer
    Fade <sec>    LegAngle / ArmAngle <deg>   at most 90
    LeftLeg / RightLeg / LeftArm / RightArm   bone names, default = the key
    Bone <name> ...                           as in SceneAnimator
```

Demo: `Entities/Debug/CubeGuy.pre` — the body of every game unit.

## `LightComponent` (ID 7)

A **LIST** of lights. `Component Light` with `Light Point/Spot/Area/Tube` children: `Color` /
`Intensity` / `Range`, entity-space `Offset` / `Direction`, `Enabled`, plus `ConeAngle` /
`EdgeSoftness` (Spot), `Width` / `Height` / `Rotation` (Area), `Radius` / `Length` (Tube). Component
`Debug true` plus the "Editor/Light debug geometry" tweak draw wireframes. Demo:
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

ID 9/10/11, one partition each under `Components/Game/`: `GameUnitComponent`,
`GameStructureComponent`, `GameProjectileComponent` (`.ixx` + `.cpp`). Each `.cpp` carries its own
copies of the small helpers (the client early-out, the atomic CAS adds, the spawn-info readback),
like every other component.

**`GameUnitComponent::update` is a step sequence over a per-tick `Tick` context** (private, declared
in the `.ixx`): `applyHeightLimit` → the puppet gate → `applyDamageAndHeal` (damage, death, heal) →
`resolveWalkTarget` (route / order / `resolveNavTarget` / `searchLocalTarget`) → `tickCombat` (the
one probe) → `tickSteering` (`steerHeading` = the context steering, brake, the hard cap) →
`tickField` (shield battery + push, or the baked-field stand-in — both BANK the push into
`Tick::impulse`) → `applyPush` (**the tick's ONE `ApplyImpulse`**). The body's velocity is read ONCE
into the context and every queued command builds on it. **The push speed limiter is on the impulse
itself:** the steering's `SetLinearVelocity` lands first, the impulse on top, so applyPush scales
the impulse by the largest `s` in [0, 1] keeping `|vel + s * impulse / mass|` under
`params.maxSpeed` (the positive root of the quadratic) — **one ABSOLUTE cap** ("Unit max speed
(m/s)", 12 ≈ 1.5x the runner's 7.6, the fastest unit), the same one `tickSteering` holds the
body to; there is no per-unit multiplier. A body already over the cap only takes a push that
slows it. Before this, the clamp was a trailing `SetLinearVelocity` tested against
the PRE-impulse velocity: it never capped the tick's own shove and, when it fired a tick later,
it overwrote the push entirely — a launch / snap-back oscillation.

* **`GameUnitComponent`** — team, health, shield battery, plus C++ steering / targeting / melee /
  ranged stance. DSL sets orders through `self.unit.setTarget`. **`liveCount()`** is the number
  of non-puppet instances alive — a relaxed atomic incremented in `spawn` and decremented in
  `destroy` (both worker-side in the batch paths), so the game's "units alive" is a load, never a
  walk. **The HURT LIGHT**
  (`tickHurtLight`, at the top of `update` on every role, before the client gate): a health drop
  since the last tick pushes a red point light over the collider's top (`addPointLight`, a
  per-frame record) at full brightness, decaying over `params.hurtLightDecay` after the last
  drop, off whatever wrote `health` (the sim or the blob). **Its colour is the ATTACKER's team**
  (the Force shell colours): `damage(amount, sourceTeam)` tags the victim (melee, shells, the
  turret, the player aura pass their team), field exposure tags the burning field's owner off the
  bake sample or, where the sample names no team, the next team over (`opposingTeamGuess`);
  untagged damage (the script thunk) keeps the neutral red. Clients never receive the tag, so a
  replicated unit's flash is red there. **AREA-BUDGETED** (the Nav seed
  limiter's idea, as a lock-free hashed slot table in the `.cpp`, world XZ bucketed into
  `params.lightArea` squares): a hurt unit flashes only when its area's flash slot has been quiet
  for `1 / hurtFlashRate` seconds (CAS on a timestamp — a lone hit unit always flashes, a swarm
  in a field shows random flashes across it). **The shield GLOW is not here:** it is the Force
  system's bubble light (see [`Docs/Context/Force/CONTEXT.md`](../Force/CONTEXT.md)) — a collapsed
  shield has no bubble and so no light, and a merged crowd is one light. **The MODEL**
  (`tickModel`, right after the hurt light, every role): the unit's body is a CHILD — the first
  child with a `HumanoidAnimatorComponent`, a
  shared model prefab (`Debug/CubeGuy.pre`). The collider
  has `LockRotation` and the root's `rot` is the physics pose, so the FACING goes on the model: a
  yaw about Y that turns the model's +X to the heading, at 12 rad/s, only above 0.25 m/s (a slower
  move is a shove, not a heading). The same distance feeds the model's walk
  (the animator's `feed` — a child does not move in its parent's space; the feed carries this tick's delta too,
  so the model follows the unit's SIM LOD cadence). Both come from the entity
  POSITION delta, never from the sim, so a replicated unit on a client turns and walks too. A unit
  that stands still writes nothing. The model's authored rotation is replaced by the yaw.
  **COLOUR:** `Component GameUnit` `Color r g b` is the unit's colour on EVERY render node of its
  subtree (the shared model has none): `spawn` applies it (Scene, bit 0, has spawned the model by
  then; `getOrCreateSolidColorMaterial` is spawn-mutex safe on workers) and `applyTeamTint` mixes the
  friendly green onto it, over the whole subtree (`tintSubtree`). With no `Color`, each node
  keeps its own authored Render `Color`, as before.
* **`GameStructureComponent`** — team, health, blueprint, invulnerable, meleeRadius, its bake-tap
  territory damage, atomic `damage()` / `addLoad()` intake, the three float stores (the game's cable
  transport moves whole cells in and out of them at its tick boundary — see
  [`Docs/Context/Game/CONTEXT.md`](../Game/CONTEXT.md)), and a machine-state UNION.
* **`GameProjectileComponent`** — team-tagged lifetime, deflection and contact damage.

**Contract:** `update()` simulates only when NOT a network client (mirrors write state on clients).
Cross-entity writes are atomic CAS; cross-entity lookup is spatial queries — **no entity lists**;
physics writes ride the body-command queue.

Tuning statics (`GameUnitComponent::params` / `GameStructureComponent::params`) are references to
`Globals::settings.game.unitParams` / `structureParams` (types in `Settings.Game`, tweaks registered by
`Settings::registerGame`; GameMatch writes `localTeam` / `huntSeedTeam`) — the full
parameter tables and the steering are in [`Docs/Context/Nav/CONTEXT.md`](../Nav/CONTEXT.md) and
[`Docs/Context/Game/CONTEXT.md`](../Game/CONTEXT.md).

#### Authoring

**`Component GameUnit`** — `ShortName` (the 3–5 char HUD tag, owned by each component; the world labels use it on
every instance, **so replicated units need no type on the wire**), then `Team`, `HealthMax`,
`EnergyMax`, `ShieldOutput`, `MoveSpeed`, `Accel`, `AttackRange`, `AttackInterval` + `AttackDamage`
+ `PlayerDamage` (melee is DISCRETE: one hit every `AttackInterval` seconds on one victim — the
nearest enemy unit or player capsule first, for `AttackDamage` / `PlayerDamage`, else the bitten
structure for `AttackDamage` — each reported as a `HitRecord` for the game's visual),
`EmitterDrain`, `Ranged`, `StandoffRange`, `FireInterval`, `ShotKind`, `AlwaysDisplayHealth`,
`HeightLimit`, `Color`.

**A unit prefab's shape:** root = `CullMode RootOnly` + Physics + Network + GameUnit (+ Force), and
`Component Scene` with ONE `Prefab CubeGuy` child that carries the reference-site `Position` /
`Scale` (1.38 × / 0.56 × the collider radius: the model is TWICE as tall as the sphere, feet at its
bottom — visual only, the collider and every gameplay radius stay the sphere). **The root needs `RootOnly`**: it has no render node of its own, and gameplay queries run
on the Render layer — RootOnly registers the root over the subtree's render bounds, so melee,
turrets and shells still find the unit. See `Entities/Game/enemyGrunt.pre`.

`ShotKind` (ranged) is what the game does per shot: **0** the direct `enemyShot`, **1** the splash
`enemyLob`, **2** spawns a loose Swarm body beside the unit.

> **HEIGHT LIMIT.** The physics can launch a body (bubble shoves, stacked bodies, contact impulses),
> so every actor is held under a world-Y ceiling: above it the body is teleported back AT the ceiling
> with its climb cancelled — vy clamped ≤ 0, planar velocity kept, per the teleport contract.
> Shared default `Game/Nav/Height limit (m)` = 10; per prefab **0 = shared, > 0 = own ceiling,
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

#### Structure stores and the transport

The component holds FLOAT stores only. **It moves nothing in the entity pass**: the game's cable
transport (StructureSystem, `Transport.cpp`) reserves whole cells out of a producer's store at its
tick's inject and adds delivered cells at the join — both main-thread, outside the pass — so there
are no cross-entity store writes and no atomics on stores at all. `attachedMask` (a network of
medium m touches this structure) and `flowUtil` (the served fraction of the port's ask) are stamped
by the game. The structure's own consumption (machines, emitters) keeps spending from its store in
the pass and the production tick as before.

**NO id-keyed maps anywhere** — cooldowns, tallies and boosts die with their structure. Machine state
lives in a UNION discriminated by the game's structure type (the NetEntityState pattern): only the
prefab's variant is ever touched, while `strainable` stays common because units probe it on arbitrary
structures.
