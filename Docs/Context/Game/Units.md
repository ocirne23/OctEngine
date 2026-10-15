# Units and the player

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.

## Units

`Game:Npc`. **Barracks-produced, plus the co-op loose spawns.**

**Own-team units are TINTED GREEN** (`GameUnitComponent::applyTeamTint`: each render node's own
authored colour mixed 55 % toward the HUD's own-team green, children included — a tint, so the
unit types stay told apart): the game stamps the viewer's team into
`GameUnitParams::localTeam` (ctor, the server's lobby team, a client's adopted team), and the tint
applies at spawn and whenever the replicated team lands through the network blob — so clients tint
their own side too, and a team change re-tints as the next snapshot arrives. Other teams keep their
authored colours (restored per node if a unit ever leaves the local team).

### The split

* **The SIM is `GameUnitComponent`** in the entity pass: steering by the Nav flow fields (see Nav),
  shield battery on player rules minus regen with permanent collapse, exposure damage, field push,
  melee, the ranged stance, and death / void self-despawn.
* **`NpcSystem` is PRODUCTION + SERVICING.** It holds no entity lists at all — units and shots are
  World roots, walked when needed (see The architecture rule ([CONTEXT.md](CONTEXT.md))).
  `service(structures)` drains the components' static queues on the main thread: FireRequests, deaths
  (freeing the spawner's roster slot), SeedRequests, TurretFireRequests and SpawnRequests.
  A FireRequest spawns its shell from a CACHED template (`shellTemplate`, re-resolved when
  `World::templateGeneration()` changes) through `World::spawnTemplate`, and the shot keeps its
  prefab name — the per-shot path resolve and rename were the "Npc fire requests" allocations;
  what remains is the entity tree block plus the component spawns themselves.
* **Shield, health and team need NO publish step** — they live on the component and ride the entity
  snapshot's game blob.

**BARRACKS and TURRETS are per-entity too.** `GameStructureComponent::update`'s machine block —
discriminated by `machineKind`, stamped with the union variant — runs the spawn clock or target query
ON the structure, pays from its own stores, claims the roster slot, and queues a request. `service()`
only performs the main-thread entity spawn, **refunding a failed one.**

Roster counts are spawn/death **EDGES** (`++aliveUnits` at the decision, `--` on the death event;
re-seeded by `loadUnits`).

### Targets

`GameMatch::gatherNavFeed` (a post-update job that also calls the Nav setters) supplies obstacles (the border ring plus structure footprints) and per-team sources (live
non-invulnerable structures, player capsules, and **live units from the World's root list — CULLED
to units with another team's unit or player within "Nav unit source reach" 64 m** via a coarse cell
hash, so thousands of far ambient enemies no longer tile the map with the AI team's field). That hash
is `NavCellTeamMap` (Match.ixx), a flat open-addressing table whose `clear()` only resets slots: the
EASTL map it replaced freed and re-allocated every node each cycle, most of the feed's memory churn.

**UNIT-VS-UNIT COMBAT:** non-ranged units melee ONE victim — the nearest enemy unit or player
capsule inside `attackRange + victim.bodyRadius` (a capsule gets a flat 0.8 allowance) — holding
at that ring.

**MELEE IS DISCRETE.** A unit swings once every `AttackInterval` seconds for `AttackDamage` health
on a unit or structure, `PlayerDamage` on a player capsule (all authored per prefab; the timer runs
down whether or not a victim is in reach, clamped at 0, so the first swing on arrival lands at
once, and it starts at a random phase so a barracks batch never swings in lockstep). One swing
lands on ONE victim: the melee enemy unit or player first, else the structure it is biting. Each landed swing is reported as a `GameUnitComponent::HitRecord`
(striker + victim position), drained by `NpcSystem::service` into a `MeleeHit` beam — a line
striker → victim over "Melee hit lifetime" 0.25 s; the victim lights itself through the hurt
light (see the turret section ([Hotbar.md](Hotbar.md)) for beams, flashes and the hurt light). Ranged units never swing.

**There is NO published player list.** Units find enemy players — puppet GameUnitComponents,
spatially registered like everything else — in the SAME queries as structures: the auto-target sweep
takes the nearest enemy puppet as fallback, and the short melee probe chews any puppet in reach.

### Damage

**PLAYER DAMAGE IS UNIFIED under `GameUnitComponent::damage()`** — the ONE entry point for every
victim:

* On a unit it CAS-subtracts health.
* On a PUPPET it atomically banks into `pendingDamage` — **health is owner-computed, so a direct write
  would be stomped.**

Unit melee and enemy projectile contacts both just call `damage()` on whatever they hit. The inbox
drains main-thread through `takePendingDamage()`: `GamePlayer::tickShieldAndHealth` drains its OWN
capsule, and the server's GDm flush drains the client twins' inboxes directly. **The component
accumulates between flushes, so there is no clientId-keyed damage map.**

`GamePlayer::applyDamage` ABSORBS into the shield battery first ("Damage absorb", energy per hp);
overflow, or a hit while collapsed, reaches health. Spawn grace applies.

### Far tick

`NpcSystem::service`, authority only. "Far tick interval (s)" 0.5, readout "Far ticked". **Not on a
physics-step frame that a step-free frame follows** (`JobSystem::deferFromPhysicsFrame`): the
accumulated time carries over, so the deferred tick covers a little more.

Units the SIM LOD did not select — **no tier stamp, body disabled, never visited by the entity pass** —
still walk their ROUTE or MOVE ORDER through `GameUnitComponent::updateFar`, **a parallelFor over the
World's root list** (non-units skipped per element) every interval of sim time:

* Straight at the target where the raster shows a clear line, else along the enemy team field's
  descent (geodesic, around rocks).
* **By TELEPORT**: the entity pos is the far truth; the body pose and prev/curr are stomped, and the
  spatial entry is refreshed so the selection query finds it as it approaches.
* Waypoints advance and the order clears with the full sim's radius rule. A step into rock holds.
* No combat, bubble, strain or health death check while far. **The ONE exception: a unit below
  `voidY` −3 — fallen through the floor — is killed by the far tick too**, so no unit escapes it.

> **THE VOID KILL RUNS BEFORE BOTH SKIPS**, on every unit, in `NpcSystem::service`'s walk —
> not only inside `updateFar`. A unit that falls off the world edge keeps sinking while its body is
> live and ends up somewhere nothing visits: OUT of the spatial index (the `spatialEntry.isValid`
> skip) or holding a stale tier stamp (the selected skip) while the entity pass no longer reaches
> it. `updateFar`'s own `voidY` test was then unreachable and the body sank forever — deep under
> the map, still a World root. **That parallelFor is the one thing that sees every unit, so the
> check belongs there.** `updateFar` keeps its copy for the units it does run on.

### Loose units spawn PARKED

`spawnLooseUnits` calls `PhysicsComponent::park(true)` right after the batch. **The body command
applies before the next step, so the body never simulates one.** The bubble needs nothing here:
every bubble spawns dark, and only the World's SIM LOD gate switches it on (see the Entity
CONTEXT's "Force bubbles").

> Wave blobs overlap, and a live unselected body took box3d's push-out and — frictionless and never
> steered — coasted away: the "flung" waves.

**The World's wake edge** (a fresh entity starts at `schedTier` 3, so its first stamped visit IS a
wake) zeroes the velocities and enables it once a player is near.

This is what makes a WAVE — spawned outside the barrier, far from everyone — reach the Base. A unit
that arrives at a shielded base enters its ZONE (see SIM LOD focus ([CONTEXT.md](CONTEXT.md))) and ticks at tier 1 there, so
the field pushes it. **One that arrives at an UNSHIELDED target while no player is near stands
frozen there** (known).

### `Friction 0` on every unit body

Every `enemy*.pre`; the player keeps 0.3. **Units are SPHERE bodies** (`Shape Sphere`, radius per
prefab, `LockRotation`); only the player is still an upright capsule. `GameUnitComponent` reads the
body's radius and top from the physics spawn info, so the shape switch needs no code.

The steering SETS the body velocity each tick, and the SIM LOD ticks tier-1/2 units only every 0.25 /
1 s, **so ground friction (unit 0.3 × ground 0.6, ~4 m/s² of deceleration) bled the commanded speed
away between ticks — tier-2 units stopped dead inside their interval.**

> **Velocity-driven bodies need no friction; do not put it back.**

The flip side: **STOPPING is an explicit command too.** `GameUnitComponent::update` BRAKES — planar
velocity to zero at the steering accel — whenever it has no walk target or has arrived. Without it a
coasting unit never stopped, kept splatting its velocity into the crowd lane, and the pack followed the
ghost trail. **No lane splat while braking.**

## Player

`Entities/Game/player.pre` = playerCapsule plus a `Component Force` shield.

* **MOUSE-ONLY movement** — RTS right-click move orders on the testbed's velocity-steering model,
  LShift sprint, Space jump off a ground raycast. **NO WASD** (those keys are grid hotkeys), and
  `tickMovement` ignores its camera-forward argument.
* `Accel` is **deliberately soft (30)**: steering force must lose the shoving match against bubble
  push.
* **NO player RANGED combat** — the old shot/projectile code is deleted. There IS a **melee AURA**:
  `GameMatch::tickPlayerMelee` (authority) grinds every adjacent enemy unit for "Melee dps" within
  "Melee radius" from every player capsule.

### The shield — a BATTERY model

While the Energy battery holds ANY charge the shield runs at constant "Max output": **its radius
squishes with PRESSURE alone (field equilibrium), never with the battery level**, so regen refills the
bar without regrowing the bubble.

* Pressure drains the battery; at empty the shield **COLLAPSES (latched)** and restarts only once regen
  reaches "Reboot energy".
* **Sprint burns the battery too** ("Sprint energy/s"), and emptying it collapses the shield — **sprint
  stays locked out until the battery refills to "Reboot energy".**
* The opposing field physically pushes the capsule (applied-force readback → impulse). **The push
  normalizes by the OUTPUT that produced it** (`m_outputHistory`), since the readback is a few
  frames latent and scales with output. The readback and the territory/density readout both come
  from the CPU pressure bake (`bakedReadback` / `sampleBakedField`) — the player holds no GPU
  query slot.
* **SURFACE TENSION**: push AND drain scale by `(1 + tension × pressure)` on BOTH sides
  ("Game/Player/Surface tension", "Game/Enemies/Push tension", "Game/Structures/Pressure draw
  tension") — **leaning deep into a bubble stiffens superlinearly and burns both batteries.**
* "Cover drain reduction" cuts the drain by the FRIENDLY field surplus over the own output — standing
  inside a team emitter's bubble.
* **Health drains only while the equilibrium shield radius squishes below "Damage radius" AND enemy
  pressure is actually present**, and it REGENERATES at "Base heal/s" within "Base heal radius" of an
  own-team Base. `tickBaseHealing` is **own player only** — health is owner-computed, so every instance
  heals its own capsule against its local structure mirror, with no sync.
* "Spawn grace" gives post-respawn immunity, **because the (bake) readbacks still carry the death
  position for a few frames.**
* The player capsule (`player.pre`, also every client twin) authors `Global true` — always visited by the entity pass. It MUST be: the SIM LOD selects roots by their SPATIAL entry, which the pass refreshes from `entity.pos`, which the PhysicsComponent copies from the body — a body teleported far beyond the outer radius (the death respawn from the far map) left the entry at the death spot, so the capsule was never selected again and its entity/spatial state froze there while the body stood at the Base.
* Death teleport-respawns per the teleport contract — onto a FREE CELL near the anchor: `GamePlayer::setRespawnResolver` (wired in `spawnWorld`) probes 1x1 cells in rings of 2 m out to 16 m via `cellsFree` (cables walk-through), so a building placed on the spawn spot never swallows the capsule; a fully built-over area falls back to the anchor.

**Problem bubbles.** Every own-team BUILT structure's label can carry a `warning` badge — a bordered
box above its title, in the problem's colour — from `structureWarning`, the most pressing state the
bars do not explain. One badge, first hit wins:

| Badge | When | Colour |
|---|---|---|
| **No power cable / No pipeline / No conveyor** | a medium the structure MOVES has no link of that medium — an input that can never arrive, an output nothing takes, or a store nothing reaches | red / orange / amber |
| **Pop full** | a barracks whose next unit would exceed its population cap (build houses) | amber |

The media a structure moves: **energy** for everything that burns, banks or makes it (emitters,
barracks, extractor, fabricator, constructor, medic, turret, generator, solar, battery), **fuel** for
generators, fabricators, tanks and a FUEL-node extractor, **minerals** for constructors, silos,
fabricators and a MINERAL-node extractor (`structureNodeIndex` + `nodeType` pick the extractor's
side — its buffer capacities do not). **The Base is exempt**: it is the hub, self-generates, and
starts every match bare.

The check is STRUCTURAL — it does not gate on current stock. A cabled-but-starved or
cabled-but-backed-up structure gets NO badge (its bars say that, the red unpowered ring in
`drawDebug` marks it, and it resolves itself); an uncabled one never resolves.

**Blueprints never warn** (inert by design), and other teams' structures never show theirs.

**Checked on a per-structure JITTERED ~1 s timer**, not per frame: the check scans a structure's
links and every state it reports moves on the timescale of a player's actions. The jitter is a
STABLE per-id phase (a hash of the structure id, 0.75–1.25×), so a batch placed or loaded together
spreads over the interval instead of re-checking in lockstep forever. The result rides the roster
entry (`Ref::warning`), so it dies with its structure — no id-keyed map.

**The STORE bars are opt-in.** A structure's energy/fuel/mineral bars show unselected only when its
prefab authors **`AlwaysShowResources true`** — the Base, Battery, Fuel tank, Mineral silo, Emitter,
Bastion, Lance, Barracks and Turret, the stores a player watches at a glance. Every other structure shows
them while SELECTED. The HEALTH bar is separate and unchanged: damage (or blueprint progress, or
selection, or `AlwaysDisplayHealth`) always draws one.

**World labels** (`buildWorldLabels`: health/store bars over structures and units, the selected
info block, the barracks popup) are a JOB: `updateWindowed` captures this frame's final camera +
viewport, `update` submits the job at ITS END (structures settled; only the entity pass overlaps
it, which changes field values, never rosters or the structure list), and main joins it right
before `ui.update` queues the widget pass that paints them (`joinWorldLabels`). GameHud writes are
mutexed; `~GameMatch` joins it too. The label list and the popup are the job's KEPT scratch
(`m_labelsScratch`, `m_labelsPopup`), swapped into GameHud at the end so last frame's list comes
back with its capacity — no per-frame list allocation, only the selected info string outgrows SSO.

**HUD** through `Globals::gameHud`: bars Health / Shield / Materials, counters Minerals / Fuel / Power
(co-op authority adds "Time" — the match clock as h:mm:ss of authority sim time, `m_matchTime`, saved
as `MatchTime` — then "Next wave (s)", "Next wave power" — the coming wave's budget points before
the alive cap, `nextWaveBudget` — and "Enemies alive"),
and hotbar slot counts = affordable.

**"Enemies alive"** is `aiAliveCount()` — `GameUnitComponent::liveCount`, the component's own
count kept at its spawn / destroy edges (O(1), any thread, nothing walked), which is also what the
alive cap compares against "Max enemy units" in `queueWave`. **It counts every unit**, so
player-team barracks units count in it too, and a unit at 0 hp stays until its queued destroy
drains.
