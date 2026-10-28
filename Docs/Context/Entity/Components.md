# The components

> Part of the Entity documentation ([`Docs/Context/Entity/CONTEXT.md`](CONTEXT.md)) — read that first.

| Component | Notes |
|---|---|
| `SceneComponent` | Children. The list is mutated through `addChild` / `removeChild` / `replaceChild` / `adoptChildren` (plain mutators: the one place to hook a future "children changed"); `spawn` appends directly. |
| `RenderComponent` | RenderNode + local transform, static or skinned, plus `Color`. `place` sets the node transform; the spatial refresh and the culled submit are `Entity::updateSelf`'s (see CullMode ([CONTEXT.md](CONTEXT.md))). |
| `AnimatorComponent` | AnimationPlayer + AnimStateMachine from `.apl`; gameplay through `stateMachine.setFloat/Bool/Trigger`, clip events through `onEvent`. Skinned meshes only. |
| `HumanoidAnimatorComponent` | A rigid-part bone model with the BARE-MINIMUM animation: four limbs about local Z, nothing else (CubeGuy, every game unit). Below. |
| `SceneAnimatorComponent` | **PARKED — no component id, no type bit, unknown to the Entity class.** The same bone model with general procedural animation (layers / tracks). Kept compiling; below. |
| `ScriptComponent` | See [`Docs/Context/Script/CONTEXT.md`](../Script/CONTEXT.md). |
| `PhysicsComponent` | See [`Docs/Context/Physics/CONTEXT.md`](../Physics/CONTEXT.md). |
| `AudioComponent` | See [`Docs/Context/Audio/CONTEXT.md`](../Audio/CONTEXT.md). |
| `ParticleComponent` | See [`Docs/Context/Particle/CONTEXT.md`](../Particle/CONTEXT.md). |
| `ForceComponent` | See [`Docs/Context/Force/CONTEXT.md`](../Force/CONTEXT.md). |
| `LightComponent` | Below. |
| `NetworkComponent` | Multiplayer ([Multiplayer.md](Multiplayer.md)). |
| Game components (9/10/11) | Below. |

## Bone models

A rigid-part model INSIDE ONE ENTITY. It shares nothing with `AnimatorComponent` — no skeleton, no clips, no
`.apl`, no string parameters, no keys, no sampled pose.

**A BONE is what an entity with only a transform and a RenderComponent would be, minus the entity**: a
`nodeLocal` transform and a `RenderNode` in the component's `bones` — an `oc::small_vector<Bone, 6>`, so up to 6
bones sit INSIDE the component, in the entity's own allocation (no allocation per spawn, and the visit stays in the
entity's memory); more bones spill to the heap. No entity allocation, no spatial entry, no visit, no child list,
no name — CubeGuy is 1 entity, not 7 (a unit: 2, not 8). A bone with no `Component Render` is a pure joint.

### The shared code — with NO overhead

Everything shared is an inline function, a template, or a plain struct; **there is no virtual call, no function
pointer and no base-class dispatch**, so each animator compiles its own copy.

* **`Animation:BoneMath`** (Animation library, Core-only — pure numerics): `polyTrig`, `polyTrig4` (SSE),
  `phaseTrig`, `phaseSin`, and `ParentCompose` — `composeTransform(parent, local)` for many locals under one
  parent, the parent prepared once. The `Transform` layout static_asserts live there (32 bytes:
  `(pos.xyz, scale)` + `quat (x, y, z, w)`, two SSE registers).
* **`Entity:BoneModel`** (it needs `RenderNode`, so it cannot be in Animation): `BoneModelRig` (the authored bones:
  name, parent, bind, the render recipe — a plain `RenderComponent::SpawnInfo` from `World::buildRenderSpawnInfo`,
  static nodes only — and `flat`; `HumanoidRig` and `SceneAnimRig` DERIVE from it) and the templates over the
  component's own bone type: `spawnBoneNodes`, `placeFlatBones`, `submitBones`, `addBoneRestBounds`.
  `World::appendSceneAnimBones` parses `Bone` blocks into a `BoneModelRig` for both.
* **NOT shared, on purpose: the per-instance state.** Each component holds its own plain fields
  (`drivenDistance` / `drivenDelta` / `lastPos` / `hasLastPos` for the distance source, `placedWorld` /
  `placeDirty` for the compose-again test, `atRest`) and its own copy of `feed` and the private `beginTick`.

### The rules both follow

* **MINIMUM MUTATION.** A tick that does not change the pose writes no bone.
* **`place(renderer, world, passMask)`** is the placement tail of `Entity::updateSelf` (every visit, frozen or
  not, right after the entity's own render submit): when NEITHER a bone changed (`placeDirty`) NOR the entity's
  world transform (`placedWorld`), it composes nothing and only submits the nodes (`renderNode`, lock-free); else,
  on a FLAT rig, ONE compose per bone (`world` × `nodeLocal`) + `setTransform`. `passMask` 0 = culled: nothing
  is submitted.
* **Open cost, in RendererVK:** the submit is one `Renderer::renderNode` per bone. A batched submit for one model
  (one instance claim, one texture note) is the next step.
* **The walk drives itself**: a SELF-driven animator measures the planar (XZ) distance its OWN entity moved since
  the last tick, so no script or gameplay code sets a speed. It runs AFTER `PhysicsComponent::update` in
  `updateSelf`, so the distance has this frame's pose. The distance is in the parent's space — **an animator on a
  child that only moves with its parent sees no distance.**
* **A MODEL CHILD is driven by its parent**: `feed(metres, deltaSeconds)` (a parent → child write, before the
  child's visit) switches the animator to driven mode — it stops measuring itself, and on its own visit it consumes
  and zeroes the fed distance AND the fed delta. `GameUnitComponent::tickModel` is the user.
* **SIM LOD.** A driven animator ticks ONLY on a frame with a feed, with the PARENT's delta (catch-up included).
  This is load-bearing: the model child is covered by the unit's RootOnly entry, has no entry of its own, and the
  World therefore hands it the full frame delta every frame (`alwaysVisited`) — its own delta is not a schedule. A
  SELF-driven animator (an entity with its own entry) is a following sim kind under "Game/Sim LOD/Follows/
  Animators", like `AnimatorComponent`.
* **`Stride` / `FullSpeed` are in the animator entity's LOCAL units** (the distance is divided by `entity.scale`),
  so one model prefab keeps its gait at every reference-site `Scale` — CubeGuy at 0.25 walks `Stride 6` = 1.5 m per
  cycle, the Titan's at 1.12 walks 6.7 m.
* **CULLING is the entity's.** The bones ride the entity's pass mask. `CullMode RootOnly` measures them into the
  template's tree bounds (`addRestBounds` from `gatherTreeCullBounds`; the REST pose, a swing is not added) and
  gives the entry the Render layer. **A bone model on a `PerEntity` entity with no RenderComponent of its own has a
  point entry on the Entity layer only: it is never culled and Render-layer gameplay queries do not find it —
  author `RootOnly`, or put it under a RootOnly root** (the unit prefabs).
* `GameUnitComponent`'s `tintSubtree` colours the bones too (it reads `bones` + the rig's render recipes).
* **Not headless** (render nodes), and **no script surface** — nothing registers bit 12 with the DSL bindings, so
  no script can require it and `syncScriptData` needs no slot branch for it.
* **The recipe IS the serialization**: the SpawnInfo keeps the authored `AssetNode`, so a save never bakes a live
  pose — and **the bones are not editable in the Entity Editor** (they are not entities). Edit the `.pre` text.

## `HumanoidAnimatorComponent` (ID 12)

**THE BARE-MINIMUM ANIMATOR, for exactly one setup (CubeGuy): four limb bones that swing about their local Z,
driven by the distance moved — and nothing else, on purpose.** No layers, no tracks, no gameplay weight override.

* **`HumanoidRig::configure` checks the setup ONCE**, at template build: flat rig, the four bones exist and are
  distinct, identity bind rotation, and a limb `Component Render` with NO local transform — **the limb mesh's
  origin must BE the pivot** (baseshapes' `Limb3`, not `Pillar3` with a `Position` offset). **A setup that does not
  fit is a warning and NO component** (`World::buildHumanoidAnimatorSpawnInfo` returns null — the model is absent,
  which is hard to miss). **So `update` has no check and no loop over data.**
* **At rest = no work.** `update` returns right after `beginTick` when the distance is 0 and `walkWeight` is 0 —
  before the division and the fade. It reports the rest pose in its `atRest` field; `GameUnitComponent::tickModel`
  then stops feeding a standing unit (`m_modelResting`: it returns at its top, before the model child is
  touched), so a driven model of an idle unit costs nothing. A distance > 0 starts it again. Past that test the
  pose ALWAYS changes (moving, or fading out), so there is no second early-out.
* **The tick:** one weight, one phase (`walkPhase` / `walkWeight` are the whole state), one `BoneMath::phaseSin` —
  the sine alone and with NO branch (`phaseTrig`'s quadrant switch depends on the data, and a crowd's phases are
  random, so it mispredicts) — then **the four limbs as ONE SSE pass** (`polyTrig4`) over the rig's `halfAngle`
  lanes. A Z rotation is the quaternion `(0, 0, sin h, cos h)`: no product, no axis multiply. The limbs differ only
  in the SIGN of their half angle (+leg, −leg, −arm, +arm), and the polynomial carries it (sin is odd, cos is
  even). The half angle is clamped to 45° (a 90° swing), where the un-normalized polynomial is within 3e-7, so
  there is no `rsqrt`.
* **ROTATION ONLY.** A `RenderNode` turns a mesh about the MESH origin, so a mesh that hangs off its bone by a
  render offset has to swing on an arc — a position write per limb per tick. With the origin at the pivot the node
  position never changes: the tick's whole output is an unpack, four shuffles and **four quaternion stores** into
  `nodeLocal.quat`; `pos` and `scale` keep what spawn set from the bind. That is why `configure` refuses a render
  offset.
* **Lane i IS bone i:** `configure` REORDERS the rig's bones so the limbs are bones 0..3 in `Limb` order (LeftLeg,
  RightLeg, LeftArm, RightArm); the rest follow in authored order. No index table — and the bone order is NOT the
  `.pre` order (CubeGuy: limbs, then Head, Body).
* **`Bone` is `{ nodeLocal, node }`** — no separate pose: the tick writes `nodeLocal` directly and `place()`
  (always the flat SSE path) reads nothing else.

```
Component HumanoidAnimator
    Stride / FullSpeed                        the entity's LOCAL units (defaults 6 / 4)
    Fade <sec>    LegAngle / ArmAngle <deg>   at most 90
    LeftLeg / RightLeg / LeftArm / RightArm   bone names, default = the key
    Enabled true|false
    Bone <name>                               Position / Rotation / Scale like an entity;
        Component Render ...                  optional; nested `Bone`s are its children (refused here: flat only)
```

Demo: `Entities/Debug/CubeGuy.pre` — the body of every game unit.

## `SceneAnimatorComponent` — PARKED (no ID)

> **Nothing uses it, so it holds NO component id and NO type bit, and the Entity class does not know it**: no
> create / destroy case, no `update` / `place` call in `updateSelf`, no RootOnly bounds, no prefab write, no Entity
> Editor pass-through, no natvis entry, no SIM LOD kind, and `GameUnitComponent` (tint, `tickModel`) handles the
> Humanoid only. A `.pre` that authors `Component SceneAnimator` gets a warning and no component. **The code is
> kept compiling so it does not rot** (`Public/Component.ixx` still imports the partition;
> `World::buildSceneAnimatorSpawnInfo` still exists, with no caller). To bring it back, follow the list in the
> comment above the struct in `SceneAnimatorComponent.ixx` — the "Adding a component type"
> ([CONTEXT.md](CONTEXT.md)) checklist plus the bone-model call sites.

What it is: the bone model with GENERAL closed-form animation. Shared per template in `SceneAnimRig` (`layers`,
`tracks`, `parts`, `SwingBatch`es); per instance one phase + one weight per layer:

```
value = layerWeight * amplitude * sin(2 pi * (harmonic * layerPhase + trackPhase))
```

A **stride** layer advances its phase by the distance moved and takes its weight from the speed; a **timed** layer
runs at a fixed rate and weight. A track is a rotation about a fixed bone-local axis around the bind, or a
translation along a fixed offset; `harmonic` is 1 or 2. Bones may nest (parent before child). Its bones also keep a
`local` pose besides `nodeLocal`.

Authoring (angles in degrees, phases in 0..1 cycles):

```
Component SceneAnimator
    Bone <name> ...                           as in HumanoidAnimator, nesting allowed
    Layer <name>
        Stride <m per cycle>  FullSpeed <m/s>     stride layer: phase by distance, weight by speed
        Duration <sec per cycle>  Weight <0..1>   timed layer (no Stride)
        Fade <sec>    Axis x y z                  the layer's default swing axis, bone-local
        Walk                                      preset: LegAngle / ArmAngle / LeftLeg / RightLeg /
                                                  LeftArm / RightArm / BobBone / BobHeight
        Swing <bone> <angleDeg> [phase] [harmonic 1|2]     child Axis
        Bob <bone> <height> [phase] [harmonic 1|2]         child Direction
```

The equivalent `Layer Walk` block for CubeGuy is the comment at the top of `Entities/Debug/CubeGuy.pre`.

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

**`GameUnitComponent::update`** first runs `tickHurtLight` and `tickModel` (every role), then returns on a network
client. The simulation is a step sequence over a per-tick `Tick` context (private, declared in the `.ixx`):
`applyHeightLimit` → the puppet gate → `applyDamageAndHeal` (damage, death, heal) → `resolveWalkTarget` (route /
order / `resolveNavTarget` / `searchLocalTarget`) → `tickCombat` (the one probe) → `tickSteering` (`steerHeading`
= the context steering, brake, the hard cap) → `tickField` (shield battery + push, or the baked-field stand-in —
both BANK the push into `Tick::impulse`) → `applyPush` (**the tick's ONE `ApplyImpulse`**). The body's velocity is
read ONCE into the context and every queued command builds on it. **The push speed limiter is on the impulse
itself:** the steering's `SetLinearVelocity` lands first, the impulse on top, so `applyPush` scales the impulse by
the largest `s` in [0, 1] keeping `|vel + s * impulse / mass|` under `params.maxSpeed` (the positive root of the
quadratic) — **one ABSOLUTE cap** ("Game/Enemies/Unit max speed (m/s)", 12), the same one `tickSteering` holds the
body to; there is no per-unit multiplier. A body already over the cap only takes a push that slows it.

> Do not move the cap back into a trailing `SetLinearVelocity` tested against the PRE-impulse velocity: it never
> caps the tick's own shove and, when it fires a tick later, it overwrites the push — a launch / snap-back
> oscillation.

* **`GameUnitComponent`** — team, health, shield battery, plus C++ steering / targeting / melee /
  ranged stance. DSL sets orders through `self.unit.setTarget`. **`liveCount()`** is the number
  of non-puppet instances alive — a relaxed atomic incremented in `spawn` and decremented in
  `destroy` (both worker-side in the batch paths), so the game's "units alive" is a load, never a
  walk.
* **THE HURT LIGHT** (`tickHurtLight`, at the top of `update` on every role, before the client gate): a health
  drop since the last tick pushes a point light over the collider's top (`addLightInfo` with a `PointLight`, a per-frame record) at
  full brightness, decaying over `params.hurtLightDecay` after the last drop, off whatever wrote `health` (the sim
  or the blob). **Its colour is the ATTACKER's team** (the Force team colours): `damage(amount, sourceTeam)` tags
  the victim (melee, shells, the turret, the player aura pass their team), field exposure tags the burning field's
  owner off the bake sample or, where the sample names no team, the next team over (`opposingTeamGuess`);
  untagged damage (the script thunk) keeps the neutral red. Clients never receive the tag, so a replicated unit's
  flash is red there. **AREA-BUDGETED** (a lock-free hashed slot table in the `.cpp`, world XZ bucketed into
  `params.lightArea` squares): a hurt unit flashes only when its area's flash slot has been quiet for
  `1 / hurtFlashRate` seconds (CAS on a timestamp — a lone hit unit always flashes, a swarm in a field shows random
  flashes across it). Tweaks under "Game/Combat". **The shield GLOW is not here:** it is the Force system's bubble
  light (see [`Docs/Context/Force/CONTEXT.md`](../Force/CONTEXT.md)) — a collapsed shield has no bubble and so no
  light, and a merged crowd is one light.
* **THE MODEL** (`tickModel`, right after the hurt light, every role): the unit's body is a CHILD — the first child
  with a `HumanoidAnimatorComponent`, a shared model prefab (`Debug/CubeGuy.pre`). The collider has `LockRotation`
  and the root's `rot` is the physics pose, so the FACING goes on the model: a yaw about Y that turns the model's
  +X to the heading, at 12 rad/s, only above 0.25 m/s (a slower move is a shove, not a heading). The same distance
  feeds the model's walk (`feed` carries this tick's delta too, so the model follows the unit's SIM LOD cadence).
  Both come from the entity POSITION delta, never from the sim, so a replicated unit on a client turns and walks
  too. A unit that stands still writes nothing. The model's authored rotation is replaced by the yaw.
* **COLOUR:** `Component GameUnit` `Color r g b` is the unit's colour on EVERY render node of its subtree (the
  shared model has none): `spawn` applies it (Scene, bit 0, has spawned the model by then;
  `RenderMaterial::getOrCreateSolidColor` is safe on workers) and `applyTeamTint` mixes the friendly green onto it, over
  the whole subtree (`tintSubtree`). With no `Color`, each node keeps its own authored Render `Color`.
* **`GameStructureComponent`** — team, health, blueprint, invulnerable, meleeRadius, `bubbleRadius`, atomic
  `damage()` / `addLoad()` intake, the three float stores (the game's cable transport moves whole cells in and out
  of them at its tick boundary — see [`Docs/Context/Game/CONTEXT.md`](../Game/CONTEXT.md)), and a machine-state
  UNION. Its flag bitfields are MAIN-THREAD-ONLY (a bitfield write is a read-modify-write of the byte).
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
every instance, **so replicated units need no type on the wire**), then `Team`, `Puppet`, `HealthMax`,
`EnergyMax`, `ShieldOutput`, `MoveSpeed`, `Accel`, `AttackRange`, `AttackInterval` + `AttackDamage`
+ `PlayerDamage` (melee is DISCRETE: one hit every `AttackInterval` seconds on one victim — the
nearest enemy unit or player capsule first, for `AttackDamage` / `PlayerDamage`, else the bitten
structure for `AttackDamage` — each reported as a `HitRecord` for the game's visual),
`EmitterDrain`, `Ranged`, `StandoffRange`, `FireInterval`, `ShotKind`, `AlwaysDisplayHealth`,
`HeightLimit`, `Color`.

**A unit prefab's shape:** root = `CullMode RootOnly` + Physics + Network + GameUnit (+ Force), and
`Component Scene` with ONE `Prefab CubeGuy` child that carries the reference-site `Position` /
`Scale` (1.38 × / 0.56 × the collider radius: the model is TWICE as tall as the sphere, feet at its
bottom — visual only, the collider and every gameplay radius stay the sphere). **The root needs `RootOnly`**: it
has no render node of its own, and gameplay queries run on the Render layer — RootOnly registers the root over the
subtree's render bounds, so melee, turrets and shells still find the unit. See `Entities/Game/enemyGrunt.pre`.

`ShotKind` (ranged) is what the game does per shot: **0** the direct `enemyShot`, **1** the splash
`enemyLob`, **2** spawns a loose Swarm body beside the unit (`enemySpawner.pre`).

> **HEIGHT LIMIT.** The physics can launch a body (bubble shoves, stacked bodies, contact impulses),
> so every actor is held under a world-Y ceiling: above it the body is teleported back AT the ceiling
> with its climb cancelled — vy clamped ≤ 0, planar velocity kept, per the teleport contract.
> Shared default "Game/Nav/Height limit (m)" = 5; per prefab **0 = shared, > 0 = own ceiling,
> < 0 = NONE** (the hook for flying units). Applied at the top of the simulation in `GameUnitComponent::update`
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
by the game. The structure's own consumption (machines, emitters) spends from its store in the pass and the
production tick.

**NO id-keyed maps anywhere** — cooldowns, tallies and boosts die with their structure. Machine state
lives in a UNION (`barracks` / `turret` / `emitter`, tagged by `machineKind`, game-stamped from the structure's
type — the NetEntityState pattern): only the prefab's variant is ever touched, while `strainable` stays common
because units probe it on arbitrary structures.
