# Game

> Library documentation for `Code/Game`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction.
>
> The game COMPONENTS (`GameUnitComponent` / `GameStructureComponent` / `GameProjectileComponent`)
> live in Entity — see [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md). Unit steering over the flow
> fields is in [`Code/Nav/CONTEXT.md`](../Nav/CONTEXT.md).

A static lib linked by App; links Entity, Force, Physics and Input. Two modes on one codebase: a
top-down tactical **PvP** whitebox, and a **co-op PvE** survival mode.

```
App.exe --game                     single player sandbox
App.exe --game --server            WINDOWED listen server  (--headless refuses --game: GPU field
App.exe --game --connect <ip>      client                   readbacks drive the authority sim)
App.exe --game --coop [...]        PvE; clients pass --coop too — the world layout is built locally
```

Without `--game` the testbed is untouched.

## The architecture rule

**Code/Game holds NO entity lists and runs NO world-wide spatial queries.**

* `StructureSystem` keeps **a ROSTER of owning `EntityPtr`s**, added at spawn and deregistered
  through `World::setOnRootEntityRemoved` — **the ONE notification every removal path funnels
  into** (death destroy request, network despawn, editor delete). No per-frame query revalidates
  it.
* **UNITS and PROJECTILES have NO roster.** The World's root list owns them, and every "all units"
  consumer walks `World::rootEntities()` — a root with a `GameUnitComponent` that is not a puppet
  IS a unit, a root with a `GameProjectileComponent` IS a shot (`NpcSystem::queryAllUnits`, the
  far tick, the nav feed sweep, the SIM LOD clusters, the ambient wander probes, the route push,
  the load path's despawn). The list only mutates on main, after the post-update jobs join, so
  those jobs read it like the roster they replaced. **The unit COUNT is no walk:**
  `GameUnitComponent::liveCount` is a relaxed atomic kept at the component's spawn / destroy
  edges (non-puppets only), read through `NpcSystem::countUnits` / `aiAliveCount` from any
  thread — nothing on main touches it.
* **Teardown wipes the WHOLE World.** `~GameMatch` has every holder drop its `EntityPtr`s
  (structures, player, client twins, selection, terrain root, ground), then `NpcSystem::clear`
  calls `World::clearRootEntities` — every root, released as one parallel batch — and discards the
  component queues. The load path (`loadUnits`) despawns only units + shots by root walk, since
  `loadFrom` has just rebuilt the structures.
* The per-entity SIMULATION lives in the Entity components, inside the parallel pass. A unit
  **REPORTS** what the game needs — shots to spawn, its death, damage — through the components' static
  event queues, and `NpcSystem::service` drains them on the main thread.
* Machine state lives ON the structure (a union in `GameStructureComponent`), **so there are no
  id-keyed maps and state dies with its structure.**

## Partitions

`Game:Match` (the orchestrator), `Game:Player`, `Game:Structures`, `Game:StructureTypes`
(`EStructureType` + the constexpr type predicates, `ENodeType`, `GameMaxTeams`,
`GameNumUnitTypes` — `export import`ed by `Game:Structures`, so importers see one surface),
`Game:Npc`, `Game:GameCamera`; barrel `Public/Game.ixx`.

**`StructureSystem`'s bodies are split over six implementation units by topic** (all
`module Game;`, the list is repeated at the end of `Structures.ixx`): `Structures.cpp` (the
type tables behind `structureTypeName` / `spawnHeightOf`, `describeType`, `registerTweaks`,
`refresh` / `stampTuning`, `clear`, the nodes, request queueing, `tickAuthority` / `tickMirror`),
`StructuresPlacement.cpp` (the grid: snap, footprints, the cell hash, `cellsFree` /
`planCrossing`; `spawnStructure` / destroy / `placeStructure` / `spawnBase` / demolish — the
prefab table lives here), `StructuresNetwork.cpp` (`rebuildNetworks` + `updateArms`),
`StructuresEconomy.cpp` (`tickProduction`, the death sweep, constructors, materials / repairs,
the tint, `drawDebug`), `StructuresSync.cpp` (the `mirror*` appliers, `saveTo` / `loadFrom` /
`clearAllStructures`) and `Transport.cpp` (the cable transport tick).

**`GameMatch`'s bodies are split over seven implementation units by topic** (all `module Game;`,
the list is repeated at the end of `Match.ixx`): `Match.cpp` (construction, `spawnWorld`, the three
frame entry points, the nav feed, the player ticks, the HUD), `MatchMap.cpp` (the generated
terrain + the per-team Base anchors), `MatchCoop.cpp` (waves, the spawn trickle, the ambient
wander, the trickle save state), `MatchNet.cpp` (teams, the join replay, every game event, the
request seams, the pause), `MatchSave.cpp` (F9/F10 + the scenario), `MatchInput.cpp` (hotbar,
modes, placement, selection, orders), `MatchLabels.cpp` (the world-label job). What more than one
of them reads — the shared constants and the structure ghost — is declared UNEXPORTED at the end
of `Match.ixx` (module linkage). `packColor` / `drawCircle` are declared the same way at the end
of `Structures.ixx` and serve every Game implementation unit (Structures, Npc, the Match files).

> **`GameMatch` MUST be a stack local in `main()`** — it holds EntityPtrs and Force handles, so a
> global would need an InitSeg slot.

---

# The frame

Three entry points, all main thread, at three different points of main.cpp's loop.

| Call | Where | What |
|---|---|---|
| `updatePlayer(dt)` | After `networkManager.receive`, **PRE-PHYSICS** ([main.cpp:747](../App/main.cpp#L747) region) | **The PLAYER/CAMERA hot path and nothing else**: capsule adoption on a client, velocity steering, and the shield's body push — the direct body setters that must land BEFORE this frame's physics step. Deliberately minimal so main reaches the spatial and begin-frame kicks as early as possible. |
| `update(dt)` | AFTER the spatial + begin-frame joins ([main.cpp:791](../App/main.cpp#L791)) | The whole rest of the tick: structures authority/mirror, materials, unit production, base healing, nav staging, net flushes. **Spawns, destroys and spatial queries are legal again here.** Becomes the server tick in MP. |
| `updateWindowed(camera, dt)` | Right after `controls.applyPlayerCamera` | Camera overwrite, aim and placement input (`updateGameInput`), ghost + debug draw, HUD. **SKIPPED while the main menu or lobby is active**, so a lobby client's constructed GameMatch simulates but gets no input, camera or HUD. With the "Detach camera" tweak on, the camera overwrite and the input half are skipped (see the Multiplayer section). |

**Consequences of `update`'s placement:** freshly spawned actors link into the spatial index at the
NEXT commit (the spawn guard keeps them visible), new bodies' velocities integrate on the NEXT step,
and the nav feed's staging feeds the NEXT frame's `NavSystem::update` — **the gather
(`gatherNavFeed`: structure roster, the World's root list for the units, player bodies) AND the Nav
setter calls are ONE POST-UPDATE job** submitted by `update` and joined at the top of the next
frame, ahead of that frame's `NavSystem::update` (see Nav's thread contract for why the setters are
legal there). **The unit sweep is SLICED over Nav's "Rebuild interval"** — ceil(roots × dt /
interval) World roots a frame (non-units skipped), a constant slice, publishing when the cursor
wraps — since Nav consumes sources only at that cadence;
the cull hash a cycle tests against is the previous cycle's (one interval stale). Both post-update
jobs are Normal and carry **pre-emption points** (see Threading) so High work gets through: the nav
feed every 256 swept units, after the sweep and before the publish; the ambient wander every 16
strolls. A unit or a stroll is always handled whole before a point.

In game mode `InputControls::setGameMode(true)` mutes the testbed spawn and possess keys — script
event fires, hotbar routing, F5/F6 and T/R/G stay — and the sponza spawn is skipped.

## SIM LOD focus

`GameMatch::update` publishes the focus every frame through `World::setSimLodFocus`: its own capsule
plus every client twin, **so no unit throttles near any player** — **plus FRIENDLY UNIT CLUSTERS**:
combat only runs inside the selection, so an army fighting far from every player (and the enemies
around it) would otherwise be dormant. Greedy clusters over the non-AI units, refreshed every
0.25 s: a unit farther than "Unit cluster focus radius" (40 m) from every focus seeds a new one, up
to the 16-slot cap (players first, then clusters in World root order).

**PLUS THE BASE FIELDS AS ZONES** (`World::setSimLodZones`, refreshed on the same 0.25 s timer):
`StructureSystem::collectShieldBubbles` gathers every shield structure's bubble sphere — ONE sphere
per merge group where they merged (friendly units in the group ride along), else the structure's own
bubble; blueprints and unpowered emitters have no bubble and add none — up to the 64-zone cap in
roster order. A zone stamps **tier 1 + a tier 2 band, never tier 0**. The structures themselves are
`Global` (always ticking, fields always projected); what was missing was the ENEMY in their field:
beyond every focus point it was unselected, and the far tick teleported it straight through the
barrier. Inside a zone it ticks at tier 1 with its body live, so the field pushes it.

With the LOD active the World visits ONLY entities within the outer tier radius of a focus point
or a zone's band, plus `Global true` roots. A structure beyond that radius **does not tick at all** — no production, no
flows, no turret fire.

> **EVERY BUILDING prefab authors `Global true`** (emitter, generator, extractor, battery, fuel tank,
> solar, fabricator, bastion, lance, barracks, wall, turret, silo, constructor, base, house), so
> player structures are never LOD-culled. **Only the CABLE segments and the crossings stay
> LOD-selected** — they carry no stores, and hundreds of them would sit in the always-visited list.

`terrainroot.pre` is Global too, so the rocks, markers and barrier children are selected individually
by their own position.

---

# The world

A flat 600 m plane (`Assets/Entities/Game/*.pre`, baseshapes solid meshes with per-prefab `Color`
tints), plus generated rock terrain and per-mode resource nodes.

**NO win condition yet.** The Force AMBIENT FIELD (the old PvE world field) is fully REMOVED.

## The shared terrain grid

`CoopMap` serves BOTH modes — it carries its own `cellsX` / `cellsZ` / `cellSize` / `origin`, and
everything downstream (cell math, flood fill, rect merging, rock spawn, `clampToOpenGround`) reads
those.

| Mode | Grid |
|---|---|
| Co-op | `c_coopCells` 54² at 10 m — **exactly `2 × c_coopHalfSize` (270 m)**, static_asserted |
| PvP | a per-arena rectangle at `c_pvpCellSize` 5 m (so `rock.pre`, a 10 m cube, spawns at HALF scale) plus a `c_pvpBorderCells` (2) rock border ring = 10 m |

Shared machinery: `finishGrid(seedCells)` does the BFS from the seeds, **seals cut-off pockets by
turning unreachable open cells into rock**, and merges the blocked runs into `m_terrainRects`. Those
rects are simultaneously the Nav obstacles, the `StructureSystem::setTerrainBlocked` rects and the
placement bounds.

Blocked cells spawn `rock.pre` (sunk to a 3.5–6 m exposed height) plus `terrainmark.pre` — a dark-red
ground plane, render-only — under ONE `m_terrainRoot` through `spawnBatch`.

**A move order or route waypoint clicked into rock is unreachable** (the A* and every unit plan to it
fail), so `clampToOpenGround` snaps it to the nearest open cell centre.

## Determinism and the GMp event

Generation is **pure seeded math** (`MapRng` / `mapHash` — **NEVER `glm::linearRand`**), so every
instance derives the identical map.

The AUTHORITY broadcasts the inputs as the **"GMp"** event, **FIRST in `onClientJoined`, before the
GPl replay** (reliable ch1 order). GMp carries a MODE BYTE: **0 = the co-op inputs (seed, fill,
lanes), 1 = the PvP arena index.**

**Clients defer terrain and nodes until it arrives**, so a client has no terrain until its join
replay lands — which is also what makes extractor node indices agree by construction. `rebuildCoopMap`
/ `rebuildPvpMap` no-op on repeats.

The same inputs ride the F9 save (`MapSeed` / `MapFill` / `MapLanes`, or `PvpMap`; co-op saves also
carry the wave clock — `WaveIndex` waves launched, which sizes the next one, and `WaveTimer` seconds
to it — so a load resumes the escalation; a wave mid-trickle is not resumed; plus `MatchTime`, the
HUD clock), **because a
joiner's tweak sync lands after the replay — too late.** `loadGame` regenerates the exact map after
clearing structures.

---

# Co-op PvE (`--coop`)

## The map

A big square — `c_coopHalfSize` 270 m on the 600 m ground plane — of RANDOM IMPASSABLE TERRAIN.
`rebuildCoopMap(seed, fill, lanes)`:

1. Two-octave value noise thresholded to EXACTLY "Terrain fill" (a sorted-copy threshold).
2. A rock-free Base zone, `c_coopBaseClearRadius` 26 m.
3. "Terrain lanes" carves wobbling attack lanes from the Base ring out past the edge.
4. `finishGrid` flood-fills from the Base. **`CoopMap::depth` is the geodesic BFS distance**, which
   the ambient scatter gates on.

**Edge barrier at ±270:** a `barrier.pre` ring (20 m segments, E/W yaw'd 90°) whose collider is
`Layer Barrier, CollidesWith Player`. **It blocks ONLY player capsules** (`player.pre` carries
`Layer Player`, mask All) — units and shots walk through. Visuals are a post per segment plus
`drawCoopBarrier`'s pulsing energy lines on the real clock.

**Edge WALL at ±299:** an `edgewall.pre` ring just inside the 600 m ground rim — **invisible** (no
Render component) and, unlike the barrier, on the Default layer, so it is **solid for everyone**
exactly like `rock.pre`. Waves spawn in the band BETWEEN the barrier and the rim, where crowd
pressure used to shove bodies off the world; a unit de-selected mid-fall then hung under the map
(see **Far tick**). 100 m segments — nothing sees it, so long boxes hold the ring to 6 static
bodies a side, each side spanning the WHOLE rim (±300) so the four seal the corners between them.
Co-op only: a PvP arena's rock border is already solid.

> **Spawn clearance is a `static_assert`.** A wave spawn point is clamped to ±`c_coopGroundEdge`
> (296) on BOTH axes — the push-out writes 272–282, also inside — so no spawn CENTRE can land in
> the wall. The body around it still has to fit: `c_edgeWallClearance` = wall − half-thickness −
> clamp = **2.5 m**, against the largest unit radius (Titan, 2.0 m in `enemyTitan.pre`) — 0.5 m
> spare. The assert fails the build if anyone moves `c_coopGroundEdge` out or the wall in;
> `c_edgeWallHalfThick` must be kept in step with `edgewall.pre`'s `HalfExtents`.

**Nodes:** a STARTER mineral + fuel pair ~10 m from the Base, then golden-angle-spiral candidates
SNAPPED to reachable open cells with a 13 m spacing floor.

## Teams

Every player is on **team 0** around ONE central Base (`allocateClientTeam` returns 0, and
`teamStartPos` is the shared start). The AI team is `GameMatch::CoopAiTeam` = **1, NOT a high slot**,
and fields UNITS ONLY.

> Co-op runs the Force system at **TWO LIVE TEAMS** (`ForceSystem::setNumTeams(2)` in the ctor), so
> every force shader recompiles with `NUM_FORCE_TEAMS=2` and the bake volume and buffers shrink to
> fit. **Every co-op team index must stay < 2.**

## The Swarm unit and shield-less bodies

`ENpcType::Swarm`, `Entities/Game/swarmUnit.pre` — **the cheap horde body: health only, NO
`Component Force`**, so no bubble, battery or GPU readbacks. Built for many-thousand counts.

**Shield-less units interact with emitters through the BAKED PRESSURE FIELD**
(`ForceSystem::sampleBakedField` in the entity pass — a CPU bilinear tap, no per-unit GPU slot,
unlimited counts):

* `opposing` / `opposingGradient` reproduce the SHIELDED units' exact push chain —
  `-grad × 0.35` (the 13-sample integral's mean self-weight) `× forceGain × pushGain × pressure ×
  tension`, speed-clamped — **so ONE "Field push gain" tweak rules both paths.**
* Exposure damage is GRADED by field depth (`smoothstep(0, iso, opposing) × fieldDps × mult`),
  **because the push equilibrium parks pressing units AT the surface** (`opposing ≈ iso`), where a
  binary inside test read false and units ground on shells unharmed. The ramp starts at zero field —
  anywhere the opposing field is present does some damage.
* The push itself also ramps from "Field push starts (x iso)" 0.7 — **below it the field does NOT
  shove, so units walk into the damage band instead of being stopped out in the weak fringe.**
* ~3 frames latent.

**FALLBACK** with the bake disabled: `GameStructureComponent::bubbleRadius` (stamped next to
`strainable` in the death sweep as `reach × 0.5 × outputFrac`) gives the same exposure damage
CPU-side, with no push.

A shield-less body spawns with a **ZERO battery** (`energy = energyMax = 0` when `shieldOutput <= 0`),
so the label pass's shield-vs-health branch and the damage absorb never mistake it for shielded, and
damaged swarm show health bars like any unit. They still deposit `addLoad` strain.

## Ambient scatter

"Ambient budget" (30000) POINTS of units placed at world start, in small **GROUPS**: `AmbientSpawn`
holds one archetype and a remaining count, so the trickle fills the current group — a disc around a
reachable anchor — before rolling the next. **Loose blobs, not a lattice.**

`ambientPointNear` rejection-samples open ground and slides bodies 1.2 m off rock edges, **so nothing
lines up on the cell lattice.**

**The archetype roll is gated by GEODESIC spawn depth**, the same `minWave` gate driven by depth
instead of time, as a **WINDOW**: only recipes whose gate lies within "Ambient recipe window" (3)
bands below the cell's depth band roll. The band is the cell's depth over `maxDepth` × "Ambient depth
scale" (0.9), clamped to 1 — **so the top band starts before the deepest cell and titans are not
confined to the corners** (the corners are the geodesic maximum; at 0.9 the mid-edges qualify too).

> So the near ring is swarm-grade and **the deep map holds ONLY the elite tier** — giants, titans and
> lobbers never sit near the Base.

The scatter keeps clear of the planar "Ambient safe radius" (45 m) around the Base.

**Wander** (`tickAmbientWander`, co-op authority, **a POST-UPDATE job** — it only writes idle units'
order fields and reads the World's root list + the immutable map, so it runs during present, never in front of
the entity batch submit; its orders land in the next pass): an IDLE AI unit — not hunting, locked or routing —
now and then strolls 0.4–1× "Ambient wander distance" (12 m) with its heading = a random unit vector
+ "Ambient wander base bias" (0.5) × toward the Base, clamped to open ground. **Cheap and SMOOTH by
design:** the expected strolls per frame = SELECTED units / "Ambient wander interval" (90 s) × dt
(the unit count is `GameUnitComponent::liveCount`, and the selected fraction is a smoothed estimate from the random
probes — sizing from every unit dumped every far unit's strolls on the few near a player), carried
as a fractional budget (`m_wanderBudget`) so every frame issues that many on average, each costing
at most 32 random probes into the root list (a non-unit root is a wasted probe) to find an idle
unit — a steady trickle, no per-unit timer, no burst.
The order is a **wander order** (`GameUnitComponent::orderWander`): it never seeds a lane (not even
on the hunt-seed AI team — a stroll is a direction, not a route), **walks at
"Wander speed mult" (0.25) × the unit's move speed, capped at "Wander speed max" (2 m/s) so runners
stroll too** (the far tick's teleport as well), and self-expires
after "Ambient wander timeout" (12 s — ticked by the far tick as well), so a target behind rock cannot pin
the unit. **Only SELECTED units (inside some player's outer SIM LOD tier) are candidates**: a far
unit is invisible, would walk the order by the far tick's teleport and then arrive in view
mid-stroll — a whole patch visibly "starting to wander" as a player approached. Tier 2 and closer
behave alike.

## Waves

They-are-Billions style. `tickWaves` arms the clock ("First wave delay" 30 s, then "Wave interval"
90 s).

**`queueWave`:**

1. Picks a RANDOM compass direction projected onto the barrier square and stepped OUTSIDE it — **the
   swarm spawns in the open ring between barrier and ground edge and walks in THROUGH the
   player-only barrier.** Cluster points that drift inside push back out along the dominant axis,
   clamped to `c_coopGroundEdge` 296.
2. **Sizes the wave in BUDGET POINTS**, not unit counts: "Wave budget" (20) plus "Wave budget growth"
   (40) per wave, where the growth itself climbs by "Wave growth growth" (10) every wave — wave i =
   base + growth·i + growthGrowth·i(i−1)/2. Each type spends its "Cost <type>" tweak, **so a brute-heavy archetype fields far
   fewer bodies than a swarm flood of the same budget.** The "Max enemy units" cap (15000) converts at
   the cheapest cost, and an unaffordable roll downgrades to the mix's cheapest type.
3. Seeds ONE lane and fires "GWv" (u16 index) at clients.

**`tickCoopSpawns` TRICKLES the actual spawns** ("Spawns per frame" 100 — wave first, then ambient),
so a huge wave enters over seconds rather than one hitch. **The trickle is THREADED:** the frame's
rolls (budget math plus glm RNG, main-thread — `linearRand` is not thread-safe) collect into
`NpcSystem::spawnLooseUnits`, one `World::spawnBatch` fan-out per frame, with team/order/roster fixup
serial after the join.

## The spawn blob

Bodies land in a disc around `m_waveOrigin` (the ring point), **sized ONCE per wave in `queueWave`
by `waveSpawnRadius`: the AREA scales with the wave's expected body count, so the areal density is
the same in wave 1 and wave 20.** Body count = the queued points / the MIX's weighted mean cost, and
`r = sqrt(bodies · "Wave spawn area per unit" / π)`, clamped to [8 m, the ground edge]. It rides the
save (`WaveTrickle/Radius`), so a mid-wave load keeps the same blob.

> The old formula grew the radius LINEARLY off the REMAINING budget and capped it at 30 m. Two
> faults: a big wave stacked hundreds of bodies on one 30 m disc — and parked bodies never push each
> other apart, so the pile only resolved as box3d shoved them out once a player came near — and the
> disc SHRANK as the wave trickled, packing the tail tightest of all.

The blob is clamped to the ground plane and any point inside the barrier square pushes back out
along the wave's dominant axis, **so an oversized disc becomes a wide BAND along the barrier face** —
the only direction with room, since the spawn band is just `c_coopGroundEdge − c_coopHalfSize` deep.

Spawn points then roll up to 6 times against the last `c_waveRecentSpawns` (32) spawns at 2 m
spacing, **so parked bodies rarely overlap in the first place.**

Each unit is `orderMove`d to the Base's near face on the incoming side; the lock releases on arrival
OR at the first enemy structure inside "Order break radius" (14 m — `GameUnitParams::orderBreakRadius`,
checked by the unit's combat probe), and the AI takes over, hunting the NEAREST structure — without
the break the whole wave marched past everything to the Base and only bit what stood in its way. All spawns go through `spawnLooseUnit` — **sourceId 0: no route, no death
accounting.** The authority HUD shows "Next wave (s)".

### Unit types and costs

`ENpcType`: Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant, Titan, Lobber, Spawner, Warrior.
**SAVE FILES store the type as an int — APPEND only.**

| Type | Cost | Notes |
|---|---|---|
| Grunt / Brute / Runner / Spitter / Swarm | 3 / 10 / 2 / 5 / 1 | The barracks tier. Spitter is enemy-only. Grunt, Runner and Swarm are HEALTH-ONLY bodies (no Component Force). |
| Warrior | 5 | `enemyWarrior.pre`: the SHIELDED grunt (bubble + battery, 80 hp, harder hits, a bit bigger and yellow) — a BARRACKS option too (12 energy, 3 pop); "warrior line" from wave 4, "shield wall" from 6. |
| Elite / Giant / Titan / Lobber / Spawner | 8 / 25 / 60 / 12 / 30 | **Enemy-only elite tier**, `enemyElite/Giant/Titan/Lobber.pre` + `enemySpawner.pre`. |

* **The Lobber** is `Ranged` with `ShotKind 1` = the slow SPLASH shell `enemyLob.pre`
  (`SplashRadius 3`, 12 unit / 18 structure damage); shot speed 14.
* **The Spawner** is a tough ranged-stance hive: `ShotKind 2` makes each "shot" a LOOSE Swarm body
  born beside it toward its target every `FireInterval` while it holds at `StandoffRange`. **The
  births are loose units — no wave budget, no roster cap — so the hive itself is the thing to kill.**
* `isBarracksUnitType` allows only Grunt / Brute / Runner / Swarm anywhere; **an old Spitter-barracks
  save loads as a Grunt barracks.**

### Archetypes (`c_waveArchetypes`, [MatchCoop.cpp](Private/MatchCoop.cpp))

ONE archetype per wave: a named recipe of at most 4 weighted types, **gated by a minimum wave index**,
weights jittered, and **never the same recipe twice in a row** when a choice exists.

| Min wave | Recipes |
|---|---|
| 1–3 | swarm · swarm + runners · grunt push · runner rush |
| 4–6 | spitter siege · brute hammer · combined arms |
| 7–8 | brute wall · **elite guard** · the works · **lobber barrage** |
| 9–10 | **giant push** · **hive** · siege column |
| 11–13 | **titan** · **hive siege** · endgame (all four elites) |

`rollWaveType` samples the wave's stored jittered copy per spawn; the ambient scatter samples the
archetype directly per spawn. The wave log names the recipe.

## Targeting range gates

EVERY unit's Nav team-field targeting is RANGE-GATED by "Target search radius" (geodesic). The field
rebuilds ~every 0.25 s with LIVE source positions, **so in-range units track a moving player tightly
while out-of-range units hold their patch instead of marching across the map.** This replaced the old
per-unit `ambient` flag.

Out of range a non-locked `hasTarget` from an earlier tick is **DROPPED at once** — kept, the unit
marched to the last known spot for up to a whole retarget interval and looked as if it ignored the
follow radius. The "Nav follow radius" band may still walk an existing crowd lane.

Wave units stay order-driven until arrival or the first enemy structure inside "Order break radius", then the same gated AI takes over.

**That standing order is therefore the ONLY thing that marks a wave unit**, so the F9 save carries it
per unit (`Order`, see Save / load): without it a loaded wave stopped where it stood and held its
patch like ambient scatter.

## Known gaps

* **The Base takes damage but the death sweep never destroys it — no lose condition yet.**
* Thousands of dynamic bodies is the AIM, not yet a measured budget. Knobs: Max enemy units, Spawns
  per frame, `Physics/World/Worker count`.

---

# PvP

Place Force emitter pylons plus economy to siege the other team.

## The arenas (`EPvpMap`)

The lobby's "Map" pick calls `setPvpMap` before `spawnWorld`; the AUTHORITY builds it, CLIENTS build
whatever GMp names.

| Arena | Layout |
|---|---|
| **Lane** | The original corridor, x ±65 / z ±20. |
| **Wide lane** | z ±40. |
| **Chokepoints** | The wide lane plus a 10 m middle wall at \|x\| < 5, pierced by three 10 m openings at z 0 and ±25. |
| **Circle** | A 65 m open disc around a 28 m rock column; Bases mid-ring at radius 47, evenly spaced, team 0 west. |

The lane arenas spread N Bases evenly along x between ±55. **Every Base cell gets a rock-free 8 m
disc** (a middle team on the Chokepoints wall line carves its own gap). The old `borderwall.pre` fence
and `spawnCorridorWalls` are GONE — `borderwall.pre` is now unused.

`spawnPvpNodes` places the arena's node table, **skipping any node whose cell landed in rock**: the
lane's 180°-symmetric 8-node table, an outer 6-node band on the wide arenas, and the Circle's 12 nodes
on two rings between the Bases.

`setPlacementBounds` covers the arena interior (`m_pvpInterior`).

## Teams

Everything is per-TEAM. `GameMaxTeams` is 8, but **the map spawns one Base per PLAYABLE team**, and a
player without a Base has no respawn anchor, mineral bank or healing — so the slot pool is exactly the
lobby's count (`m_numTeams`, default 2).

The server is team 0. A joining client gets its **lobby pick**, else the **least-populated team**
(`allocateClientTeam`), stamped onto its capsule's puppet `GameUnitComponent` and emitter at spawn.

**That component IS the record.** `clientTeam(clientId)` reads it back (−1 = no capsule → the `Gq*`
handlers refuse the request), and the OWNER learns its own team from the snapshot game blob.

> **NEVER derive a team from the clientId.** Ids are minted monotonically and never recycled, so a
> reconnect or a failed first attempt shifted the second connection onto a base-less team — the bug
> this replaced.

`GamePlayer` follows the component on a client (gated on `hasTarget`, so the prefab's authored team is
never latched) and stamps it everywhere else.

**Per-team specifics.** Players' and projectiles' Force teams are set at spawn/adopt; structures carry
the builder's team (GPl and place requests carry a team byte, and fields are re-teamed after spawn
since prefabs author team 0); demolish validates same-team; minerals and fuel are per-team arrays.

## Structure damage

* Every placeable prefab has `ContactEvents true`, and `placeStructure` installs an `onContact` hook:
  enemy-TEAM "Projectile" contacts chip "Projectile structure damage" health.
* **`tickDamage`'s rule: hostile = ANY other team's bubble owns the structure's query point.**
  *Push your field over their buildings to siege.*

**Known gaps:** player turrets target only units, not players; GSt caps at 79 structures.

---

# Multiplayer sync

The server runs ALL sim. Sync is three-layered.

### 1 — Player-structure MIRROR over game events

**GPl / GRm** reliable on change, plus a join replay. The `mirror*` appliers in StructureSystem are
idempotent, notified through its `onStructure*` hooks.

* A completed blueprint **RE-SENDS GPl** through `onStructureBuilt`, and `mirrorPlace` on a known id
  applies the built flag idempotently. **Cable segments sit outside GSt, so this IS their build
  notification.**
* Their build PROGRESS mirrors through **GCb**, sent right after every GSt: blueprint cable and
  crossing segments only, `[u16 count]{u32 id, u8 healthFrac}` over the segment's own `healthMax`, up
  to 190 records with `m_cableSyncCursor` rotating past that.
* **There is NO cable wire at all** — links derive locally on both sides.

Plus **GSt at 5 Hz**: u8-quantized health / charge / fuel / minerals / outputFrac / flow-utilization /
powered for the first 79 NON-CABLE structures, plus resource totals. **Mirrored emitters drive their
LOCAL ForceComponent from the synced outputFrac**, so client-side fields are real and the client
player's shield readbacks work.

Other broadcasts: **GRt** routes, **GBu** barracks unit type, **GLt** strike beams (kind byte:
turret lightning / melee hit, then from + to), **GWv** wave
index, **GDm** damage flush, **GMp** map, **GPz** the shared pause state (u8; the client request is
**GqZ** — any seated player may pause or resume; see the escape menu in `Code/App/CONTEXT.md`).

### 2 — Units and shots as network entities

`Component Network` in those prefabs plus `player.pre`.

### 3 — Client capsule adoption

Each client ADOPTS its server-spawned capsule (`GamePlayer::clientAdopt`, LocalOwner scan) and
owner-simulates through the claim stream, computing shield and health locally.

## The game blob

Replaced the old GSh / GqE shield-mirror events.

**Every snapshot or claim record for an entity WITH a `GameUnitComponent` carries `NetRecFlag_Game`
plus 5 bytes**: healthFrac and energyFrac as u8, emitter output over a fixed 0..8 range,
materialsFrac, and flags = `collapsed | team<<4`.

* Server-side it is CHANGE-DETECTED and **forces the record out even when the pose alone would skip**
  (asleep body, unmoved entity); a collapse edge flushes on the next snapshot tick.
* Receivers write the entity's GameUnitComponent and drive its LOCAL ForceComponent output and team,
  **so remote bubbles render true drain/collapse state on the right team** and the overhead labels read
  components directly on every instance.
* **The entity's OWNER keeps its self-computed shield** (`applyShield` off) and takes only
  server-authoritative materials. **Claims apply shield-only** — never materials or team, which are
  client-forgeable.

## Player capsules are puppets

`Puppet true` in `player.pre`: `update()` is inert, GamePlayer stamps health / energy / collapsed /
team each tick and reads materials back as LocalOwner. **Puppets are excluded from turret targeting
and from `NpcSystem` clear/save.**

**Carried MATERIALS live on the twin**: `materialsFrac` IS the server-authoritative store — there is
no `m_clientMaterials` map, and the state dies with the capsule. `m_clientPlayers` is the only
per-client container left.

## Client intents

`Gq*` request events — **GqP** place (cables included), **GqD** demolish, **GqW** routes, **GqU** unit
type — gated by a `setEventFilter` (only `Gq*` ≤ 64 B from clients, installed by `spawnWorld`; during
the LOBBY phase the LobbySystem's `Lb*`-only filter stands instead) and validated by the same
authority seams as local input.

> **The C++ dispatch hook `NetworkManager::setOnGameEvent` is owned by MAIN.CPP**, installed ONCE and
> never replaced: it routes "Lb*" to the LobbySystem and everything else to
> `GameMatch::handleNetEvent`. **GameMatch must NOT install its own hook** — a lobby client's
> GameMatch is constructed INSIDE a dispatch, and reassigning the `oc::function` there would destroy
> the executing lambda.

`GameMatch::onClientJoined` / `onClientLeft` (registered from main when game + server) spawn and
remove per-client players and replay world state.

Gameplay tweaks sync automatically through the `Core.Tweaks` `Synced` flag — all `Game/*` except
`Game/Camera`, `Game/Sim LOD` and `Game/Player/Detach camera (free fly)`.

**Shadow preset:** the `GameMatch` ctor writes the top-down "Shadows" values into the renderer's live
tweak block through `Renderer::setShadowParams` (Max distance 250 m from the player, Split lambda 0.5,
Caster pad 500 m, both biases 0) and the dtor restores whatever the sandbox had. They remain editable
in the tweak panel during the match.

**Off-screen casters** are not a game concern: the spatial `Shadow` pass (the view frustum swept
toward the sun by "Spatial/Culling/Shadow reach (m)") keeps walls and units outside the view casting
into it — see the Spatial CONTEXT.

**Scene focus:** `updateWindowed` calls `Renderer::setSceneFocus` every frame with the player's X/Z and
Y pinned at 1 m (a jump must not scroll the GI clipmap or slide the cascades), so
every distance-based quality falloff measures from the PLAYER: the sun cascades are nested spheres
around it ("Shadows/Max distance" is metres from the player) and the RTAO fade/early-out use the same
origin — the follow camera hanging in empty sky plays no part. With the camera detached the focus stays
on the player unless the local "Game/Player/Detach focus point" tweak is on too (`clearSceneFocus`, the
fly camera takes it); `~GameMatch` clears it. See "The scene focus" in the RendererVK CONTEXT.

**`Game/Player/Detach camera (free fly)`** (local, `GamePlayer::cameraDetached`): main runs the
testbed `FreeFlyCameraController` INSTEAD of the follow camera (seeded from the follow view on the
flip via `setPose`, WASD handed over), and `updateWindowed` skips `GameCamera::apply` and the whole
input half (`updateGameInput`: grid hotkeys, hotbar/popup clicks, mode clicks, the RMB move order)
— LMB-look and WASD belong to the fly camera. The debug draws, labels and HUD keep running; the
capsule keeps simulating (a standing move order still completes) but takes no new orders.

---

# Economy

A node produces NOTHING until an **extractor** is built on it — one per node; the ghost snaps to the
nearest free node within "Extractor snap radius", validated again at place time — AND that extractor
is fed energy.

## The cable transport (three media, one model)

**Energy, fuel and minerals move over the cables as WHOLE CELLS** (`Transport.cpp`). There are no
links, bands or per-pair transfers any more: every built cable segment and conducting crossing is a
NODE with an integer fill and an out-rate, and the flow is a local transport — a stencil over the
node graph — so **a run's bottleneck is literally its slowest segment**, producers and consumers can
sit anywhere along a line, loops and junctions need no routing, and the consumer nearest the
producer is served first (what a conveyor does).

| | |
|---|---|
| Node | a segment / crossing: `fill` cells (soft capacity per MEDIUM — "Cable / Pipeline / Conveyor cells per segment" 2 / 1 / 1: energy holds a burst, the slower media are pure transport), out-rate = the medium's "… throughput" (cells/s), `moved` gauge |
| Junction | one per (BUILT building, medium): the building's PORT. Every segment touching the building is its neighbour, and those segments are also connected to EACH OTHER, **so a line through an emitter carries through cable-to-cable** — the port itself accepts cells only while a slot on it still wants some (nothing parks in a producer's port, nothing relays through it); its out-rate scales with its degree so the cable stays the bottleneck |
| Slot | the building's port on its junction: role by type — **producer** (generator, solar; extractor fuel/minerals; fabricator minerals) pushes whole cells OUT of its float store, **consumer** pulls into its headroom, **storage** (battery and Base; fuel tank; Base and silo for MINERALS) reads the CABLE next to its port — the mean fill of its junction's neighbours, never the junction itself, which its own push fills — as the price signal, with TRUE hysteresis: it switches to PULLING when that fill reaches "Storage pulls above" (0.75) and keeps pulling until it drops to "Storage pushes below" (0.25), where it switches to PUSHING until the cable fills back up (a per-tick band idled a battery every other tick on 2-cell segments). **The Base's shield is FREE** (no per-second draw, no pressure surcharge — only the enemy siege load drains it), so the Base is a pure storage building: it banks for the grid and self-generates a trickle |
| Tick | fixed "Transport tick rate" 10 Hz, runs staggered over "Transport spread" 4 groups so a big base's work lands on several frames |

**Per tick, on main:** `kickTransport` — inject every due slot (supply capped at the port's free
space; a PRODUCER's taken cells simply come off its store at the join — the transport is that
store's only drain, so no reservation and no visible dip; STORAGE reserves, since the Base's siege
load drains it meanwhile, refunded at the join; metered machines — barracks "energy intake", turret
shot energy / fire interval — meter their demand with a carry), then ONE job.
**The job** runs "Transport substeps" (4) × three owner-only passes: OFFER (a node serves its own
slots' demand first, then FORWARDS everything it can into neighbours with free space that are
strictly DOWNHILL on THE DEMAND FIELD — `dist`, one BFS per run per tick from every port with
unserved demand, through cables only (ports never relay; an unseeded port sits one hop above its
best cable so it can still send) — and, with no such outlet, into cables (never ports) strictly
UPHILL on THE SUPPLY FIELD — `sdist`, the same BFS from every port pushing supply — so cells travel
toward demand when there is any and otherwise FILL the network outward from the producers until it
is full: no fill gradient, nothing drifts back toward a source or into another producer's dead end,
no ping-pong, loops are harmless — space-weighted with the integer remainder rotated, all inside
its out-budget, which
BANKS while idle, capped at two ticks, so a front crosses a segment per sub-step; then takes slot
supply into its free space),
APPLY (fill' = fill − out + the neighbours' out-slots aimed here), CLEAR. **The due runs tick IN
PARALLEL** (one job per run above a couple of runs; each run's BFS queue is its own slice of one
shared buffer — no `thread_local`, no `PerWorker`: the nested fan-out below is a wait, and a job
may resume on another thread after it); a run over 1024 nodes additionally fans its passes out per
node. No atomics, no entity walk, scheduling-independent. Scopes: `Transport inject` /
`Transport apply` (main), `Transport tick` → `Transport runs` → `Transport run` → `Transport
offer/apply/clear` (job).
**At the join** (`joinTransport`, top of the next tick) the cells land in the stores and
`flowUtil` = the served fraction of what each port asked.

Consumers still drain their internal float battery every production tick; empty = unpowered. A
full producer buffer still throttles production. Cells in the cables are real: a long run buffers
`segments × its medium's cells per segment`, and a cut line keeps what it held (fills survive rebuilds by structure id, save as
`Fill`, and mirror to clients by id — `GCf`, rotating, 6 B a segment: the fill plus the ~2 s
throughput average as a fraction of the rate, so clients draw the same bottleneck rings and label
numbers).

**Buffers:** extractor and other consumers "Internal buffer" 10; the shield emitters hold deeper
stores — "Emitter buffer" 50, "Bastion buffer" 150, "Lance buffer" 100 (the emitter restart charge
clamps to each) — generators 20, battery 200, **barracks =
the selected unit's energy cost** (stamped per instance in `stampTuning`; `energyCapacityOf` returns
a 1.0 placeholder just so power cables attach) — its store IS the BUILD BAR: its transport slot
meters its demand to "Barracks energy intake/s" 2 whatever feeds its junction, so the store fills
at the build rate and a unit is born the moment it is full (build time = cost / intake — Grunt
2.5 s, Brute 10 s; no timer). A barracks needs a power cable and holds no minerals. Powered
extractors and fabricators fill their OWN buffer and **stall when full**. Only
what sits in Mineral silos and the Base is SPENDABLE (a per-team cache recomputed per tick; spending
drains silos first, Base last).

## Physical cables

The link tools, point-to-point cables and Connector range links are all REMOVED. **`Connector` is a
RETIRED enum slot** — its table entries remain, placement refuses it, and `loadFrom` skips it.

Cables are **1-cell GRID STRUCTURES**: `CablePower` / `CablePipe` / `CableConveyor` (one per medium,
no tiers) plus the **1×3 oriented crossing** bridges — **ALSO one per medium**: `CrossingPower` /
`CrossingPipe` / `CrossingConveyor` (`isCrossingType`, `crossingMediumOf`, `crossingForMedium`).
There is no generic crossing. The three share `crossing.pre`; `applyStructureTint` hues it by
medium.

A perpendicular cable passes UNDER a crossing's middle cell. **A crossing conducts ONLY its own
medium** between its two END cells — a cable of another medium at an end is just in the way, and
the crossing never re-types on what touches it. **Each end accepts from THREE sides** (outward plus
the two laterals; only the middle is pass-through-only), and a run on one end plus a building
holding the medium on the other also counts.

### The networks are DERIVED, never authored

`StructureSystem::rebuildNetworks` — dirty-gated, main thread, in `tickAuthority` after the
request drain and in `tickMirror` (it joins the transport job first: the job indexes the graph).
**CLIENTS derive the same graph locally from the mirrored segments** and receive only the fills.

1. Union-find the **BUILT** segments over 4-neighbour equal-medium adjacency. **Blueprint segments
   break the path.** The under-cable participates through `CellEntry::underId`, which is also what
   keeps a crossing from unioning with the cable below it.
2. Attach every adjacent building with capacity > 0 in the run's medium; a blueprint building only
   gets `attachedMask` stamped (the "no cable" badge) — no node, no slot.
3. **BRIDGE through built buildings** — a building conducts every medium it holds: its junction
   node neighbours every segment touching it, so two same-medium runs touching it are one run and
   a line through it carries through. **Buildings NEVER connect by direct adjacency: cells always
   travel over cable.**
4. Lay the nodes out run-contiguous (segments, crossings, junctions), the edges as CSR with reverse
   indices, the slots per junction; stamp the rates from the tweaks; carry the old fills over by id.

### The cell hash

`m_cells`: cellKey → `{id, underId}`, maintained at the spawn and remove seams (a crossing demotes
what its middle bridges to `underId`; removal promotes it back).

It backs `cellsFree` — **per cell now: buildings refuse ANY occupied cell (no building on a cable); a
cable may slot under a free crossing middle; a crossing's middle may bridge exactly one plain cable
OR one END cell of another crossing (`isBridgeable` — an end counts as cable for bridging, so
crossings chain and stack); its ends must be free** — plus the derivation and the arm visuals.
`ignoreCables` is the unit-spawn probe, since cables are walk-through.

> **A CROSSING is validated by `planCrossing`, not `cellsFree`** (both share `footprintAreaClear`
> for the bounds / rock / reserved-node checks). One relaxation: an END cell may hold a plain cable
> of the crossing's OWN medium and OWN team, which the placement **REPLACES** — that segment is
> redundant under an end, which conducts that medium anyway, and refusing it meant a paint stroke
> could not cross a foreign line wherever its own run already stood. Anything else (another medium,
> a building, a second crossing, an enemy's cable) blocks exactly as before. `placeStructure`
> demolishes the named segments right before the spawn, so the cells are free when `insertCells`
> runs and the networks simply re-derive. **The replaced run is severed until the crossing is
> BUILT** — a blueprint conducts nothing.

**`isWalkThrough`** = cables, crossings AND the flat **Solar** slab: their prefab collider is `Layer
Cable, CollidesWith Projectile`, so players and units pass over them, and the code treats them alike
— no nav obstacle in `feedNav`, `actorInFootprint` never refuses them, `ignoreCables` lets unit
spawns land on them, and an RMB on one is a plain ground order. (The Solar keeps everything else a
building has: cells refuse other placements, labels, GSt, links.)

### Draw and lifecycle

Segments are real meshes with per-medium authored tints (power yellow, pipe orange, conveyor blue;
blueprint gray rides `applyStructureTint`). Each is a low `Cylinder2` hub with **4 render-only ARM
child entities** (`ArmPX/NX/PZ/NZ`, `Enabled false` in the `.pre`, cached on `Ref::arms` at spawn,
toggled by `updateArms` — toward a crossing only when its medium matches). A crossing is tinted to
its TYPE's medium hue always, conducting or not.

`drawDebug` adds only the FLOW pulse: per conducting run, a pulsing ring over every segment, with
brightness and speed from the busiest attached building's `flowUtil` — **which the server computes and
clients receive through GSt, so the reading is the same everywhere.**

Segments are full structures: a blueprint phase (**NON-conductive until built**), health ("Cable
health max" 40 — **`healthMax` is per-COMPONENT now**, so `investMaterials` heals against
`state->healthMax`), territory drain, projectile damage (`Layer Cable, CollidesWith Projectile` plus
`Layer Projectile` on the shot prefabs — **the first named physics layers in content**; everyone
else's Default/All masks are untouched, so players and units walk straight through), and death-sweep
destruction that re-derives the run broken.

**Cables are SKIPPED by:** GSt (the 79 cap), `feedNav` (no obstacle, no source), `actorInFootprint`,
the RMB building-face move order and route-click checks (treated as ground), and overhead labels
except blueprint or damaged on the authority.

## Construction

**Placing is FREE.** Structures spawn as BLUEPRINTS (gray tint, inert in every tick: no power, fields,
income or flows; links pre-wire but carry nothing).

> **HEALTH IS THE PROGRESS.** A ghost spawns at 1 hp, materials HEAL it at cost/healthMax per hp
> (`investMaterials`), and full health = built. The bar is blue while blueprint, and **damage
> literally undoes construction.** The SAME price repairs damaged built structures.

**Cheat:** "Game/Cheats/Free instant build" (a Synced checkbox — **the server's value rules all
players**) skips the blueprint phase.

**Materials come from:**

* **PLAYERS** — a carried inventory ("Game/Player/Materials max" 50; **server-authoritative for ALL
  players**, clients receive theirs through the game blob), refilled within "Refill radius" (6 m) of
  own-team Silos, Base or mineral Extractors at "Refill rate" (15/s) — `takeStoredMinerals`, so
  **hand-couriering a fresh dig site's buffer works before any conveyor exists** — and invested into
  nearby BLUEPRINTS only within "Build radius" (6 m) at "Player build rate" (8/s). Dropped on death.
* **The CONSTRUCTOR structure** (Production hotbar; conveyor-fed mineral stock plus powered; amber
  range ring in `drawDebug`), which funds the nearest own-team blueprint **OR damaged structure**
  within "Constructor range". With nothing to build or repair it idles.

Blueprint state mirrors through a GPl built flag plus a GSt status-bits byte (bit0 powered, bit1
blueprint).

**NPC emitter drain (`addEmitterLoad`) is SHIELD-INDEPENDENT** — collapsed units strain emitters like
shielded ones.

## Grid placement

Every placement — **extractors included, since the node's position snaps too** — aligns to a
`GridCellSize` **2 m** world grid (odd footprints centre on a cell, even on a corner) and occupies
`footprintExtent(type, rot)` cells.

| Footprint | Types |
|---|---|
| 1×1 (cables included) | most |
| 2×2 | FuelTank, Bastion, Turret, Solar, Fabricator, MineralSilo, House |
| 3×3 | Barracks, Base |
| 3×1 / 1×3 | **the three crossings** — the ONE non-square footprint, by its facing |

> A crossing's facing is quantized to ±X/±Z at `placeStructure` like the Lance, and carried on GPl
> and in the save's `Facing` — **otherwise client derivation diverges on which cells it covers.**
> Prefab `Scale` / `HalfExtents` = footprint half, so boxes fill their cells.

**Heights** use the baseshapes variants — `CubeHalf` / `CubeQuarter` / `Pillar2` / `Pillar3` are the
2 m cube at 0.5× / 0.25× / 2× / 3× height, origin centred: Wall and Mineral silo `Pillar2`, Battery
`CubeHalf`, Solar `CubeQuarter`. `structureSpawnHeights` is each prefab's box HALF height, **so
everything sits flush.**

**Refusals.** `cellsFree` refuses overlaps at aim (red ghost) AND at `placeStructure` (**the MP
seam**). FREE resource nodes reserve their extractor's future footprint, so buildings cannot block
extractor placement.

`actorInFootprint` (a spatial query for GameUnitComponents — units AND player capsules) refuses a
placement someone is STANDING in, at both seams. **It is deliberately NOT folded into `cellsFree`**,
which doubles as the unit-spawn-point probe and must not refuse a cell just because units stand near
it.

**The GHOST is the real box:** `drawStructureGhost` wireframes the exact footprint square × the
prefab's height plus the interior cell lines, green when clear and red when refused. **Every whitebox
building IS a box, so the preview is exact.**

---

# Hotbar and interaction modes

## The RTS grid hotbar

12 slots on the GRID HOTKEYS **QWER / ASDF / ZXCV**, row-major, always visible in game mode. **Every
slot is also CLICKABLE**: `slotAtScreenPos` → `activateSlot`, the ONE entry point for key and click.
Clicks over the hotbar never reach the world.

Every caption is a 3–5 char SHORTHAND from `c_structureShortNames`, indexed by `EStructureType` —
**the SAME table the world tag over a building uses, so a slot and the thing it builds read
identically.**

**HOVER CARDS.** Every slot carries a `HudSlot::tooltip` — `'\n'`-separated lines the overlay draws
in a box above the hotbar while the cursor is on that slot: the full type name, then
`StructureSystem::describeType` — **one sentence, the BUILD COST right under it, then the type's
exact flows, one per line.** Every metric shares one `<sign> <value> <unit>[ <note>]` column:

| Sign | Meaning | Colour |
|---|---|---|
| `-` | an input — a per-second draw, or the one-off `- 40 minerals to build` | orange |
| `+` | an output — a per-second yield | green |
| `=` | a capacity it banks (`= 200 energy`) | blue |

Anything else is a plain grey line, and **every kind of reach is ONE word, `Range N m`** — a
bubble's, a beam's, a heal radius, a build range, a house's link radius — so a card never makes a
player wonder whether "Reach" and "Radius" mean different things. **Every number is read off the
LIVE tweaks**, so a retuned economy retunes the cards; nothing there is a hand-written constant. The
page and mode slots get a one-liner instead.

> `refreshBuildHotbar` runs EVERY frame (the counts stay live), so the cards are cached per type and
> rebuilt once a REAL second (`Time::getElapsedSec` — frame-rate independent, and a paused game
> still refreshes), and `setSlotTooltip` early-outs on unchanged text — **a steady frame allocates
> nothing.** For the same reason the refresh clears only the slots this page did NOT fill.

| Slot | Root page | Category page |
|---|---|---|
| Q / W | **CMBT / PROD** | the category's items, in order |
| A / S / D | **CBL-P / CBL-F / CBL-M** — the cables arm straight from the root (a hidden `c_cableCategory`, still drawn as the root page: `isRootPage`) | items 4–6 |
| X (slot 9) | **DEL** — one demolish, then straight back to Select (the button releases itself) | **DEL** (category pages cap at 9 items so X stays Delete) |
| C (slot 10) | **CNCL** — leaves Delete mode / disarms a cable, back to Select | **CNCL** — straight back to Select |
| V (slot 11) | — | — |

**Esc / Tab = `cancelOneLevel`**: a two-click step drops → the armed item disarms → the page or
Delete mode returns to Select, **one per press**. **C = straight back to Select, whatever was
armed.**

`escWouldCancel()` is true while Esc still has an in-game meaning, and **main's escape MENU only opens
when it is false — so the cancel chain keeps first claim.**

## Q — Combat

**Emitter · Bastion · Lance · Wall · Turret · Barracks · House**

* **Bastion** — a big, hungry anchor bubble. **Lance** — a focused cone; **TWO-CLICK placement**: the
  first click anchors, the second aims the facing, with a fallback that auto-faces away from the Base.
* **Wall** — a DRAG line of square segments every 2 m (`c_wallSegmentSpacing`): the press anchors, the
  release places. Per-segment cost, `c_wallMaxSegments` 16.
  > **A BUILT wall is fed to Nav as a BREACHABLE obstacle** at "Wall breach cost" — one 2 m cell costs
  > (1+cost)× the walking distance, so enemies route through a wall rather than take a longer detour,
  > walk into it and bite it down. **Blueprint walls and every other building stay impassable.**
* **Turret** — HITSCAN lightning. The component picks the nearest enemy unit in "Turret range" and
  lands "Turret damage" on the spot; **it never misses.** The strike is a BUNDLE of jagged debug
  lines fading over "Turret beam lifetime" 0.5 s (`NpcSystem::addBeam` / `drawBeams`), broadcast to
  clients as GLt, plus a muzzle FLASH.
  > **BEAMS AND FLASHES** (`NpcSystem`): a beam has a kind — `Turret` (the lightning) or `MeleeHit`
  > (a unit's swing, see Targets). **The muzzle FLASH is not a separate record:** `drawBeams`
  > pushes one point light at every live Turret beam's `from` (`addPointLight`, a per-frame
  > record) with the intensity fading linearly over the beam's lifetime, scaled by "Muzzle flash
  > intensity" — so it lives and dies with the bolt. GLt carries the kind byte; melee hit beams
  > are relayed at most `c_maxHitBroadcast` 64 per frame, turret strikes always.
  > **There is NO impact flash: the VICTIM lights itself** — `GameUnitComponent::tickHurtLight`
  > (every role, before the client gate) compares health against the last tick's and pushes a red
  > point light over the collider's top while it drops, decaying over "Hurt light decay (s)" after
  > the last drop. So a melee hit, a turret strike, a shell AND force-field exposure all light the
  > unit, and clients see it off the replicated health. "Hurt light intensity" scales it.
  > **Flash counts never scale with the crowd:** "Light area (m)" buckets the world and "Hurt
  > flashes/s per area" caps the flashes in each bucket (see the Entity CONTEXT's
  > GameUnitComponent notes). The shield/emitter GLOW is the Force system's bubble light
  > ("Force/Glow/Bubble light …" tweaks): one light per bubble, sized by the bubble radius.
  > **THE ENERGY STORE IS THE RELOAD BAR** (the barracks rule, and there is NO fire timer any more):
  > `stampTuning` sets its energy capacity to "Turret shot energy" (**2 — keep it a WHOLE number:
  > the transport delivers whole cells and a consumer asks for `floor(capacity − store)`, so 1.5
  > stalled one cell short**) and its transport slot meters its intake to shotEnergy / "Turret fire
  > interval", so a fed turret fires at exactly the authored cadence, a starved one fires slower,
  > and a full turret with no target holds its charge and fires the instant one appears. Its bar
  > is progress-green and always shown.
* **Barracks** — ONE type, 3×3, cable-fed. The old Brute/Runner/Spitter variants are RETIRED enum
  slots; `loadFrom` maps them to a Barracks with that unit type. See below.
* **House** — 2×2, no grid role. Every built house links to the nearest built own-team barracks within
  "House link radius", **one barracks per house**, re-derived every `refresh()` on every instance by
  `linkHouses`. Drawn as a line to its barracks plus the radius ring while selected.

### Barracks detail

Selecting an OWN barracks shows a UNIT-TYPE PICKER popup above it (`HudPopup` in `Core.GameHud`, drawn
by GameHudOverlay; clicks resolve in `updateWindowed` through `popupButtonAtScreenPos` **before** the
hotbar). Options: **Grunt / Brute / Runner / Swarm**.

**The pick is an ORDER, like the route**: `queueUnitTypeRequest` → validated in `tickAuthority` →
`onUnitTypeChanged` → GBu broadcast plus join replay. Clients request through GqU; saved as
`UnitType`.

Each spawn pays the type's "\<Type\> spawn energy" from the energy store, whose capacity IS that
cost and which fills at the capped "Barracks energy intake/s" (a TOTAL over all its power links) —
**full store = a unit, so the bar
over the barracks is the build progress and the price IS the build time** (cost / intake). The
spawn check carries a 0.01 epsilon so a fill that lands a rounding step short of the cap still
counts.

It holds the type's "\<Type\> population" against the **POPULATION CAP = "Barracks population" +
"House population" per linked house**. A spawn only happens while `population + cost <= cap`
(`BarracksData`, tallied by the spawn and death edges — the unit's `popCost` rides its DeathRecord).
**The population rides GSt in the barracks' otherwise-unused output byte.** Spawns land at a free cell
around the building.

### Barracks routes

In Select mode with an own-team barracks selected, **RIGHT-click on ground sets a waypoint**; SHIFT+RMB
appends, max `MaxRouteWaypoints` — **bounded by the 64 B GqW request.**

Spawned units march the waypoints (advancing on touching each "Waypoint radius" circle, **no combat
mid-march** — except an enemy unit within "Route engage radius", which diverts the walk target and
resumes after) before their AI takes over. The barracks route is copied onto the unit at spawn AND
**re-pushed to its live units when the route changes** — the march index is kept and clamped, so
appended routes continue.

Server-validated (own-team barracks only), mirrored through GRt plus the join replay, drawn as a green
line chain with circles for the own team.

## W — Production

**Generator · Solar · Extractor · Fabricator · Constructor · Battery · Fuel tank · Mineral silo ·
Medic station**

Solar is a free trickle with no fuel (a flat `CubeQuarter` slab); the fabricator turns energy + fuel
into minerals; the mineral silo is the team's spendable bank.

* **Medic station** — 2×2, a plain powered consumer ("Medic energy/s" 1.5 from its internal buffer,
  band 0, unpowered = red ring). While built AND powered it heals every body inside "Medic heal
  radius" (12 m, green ring) at "Medic heal/s" (4) — **HEALTH and the SHIELD BATTERY both**, the
  same rate each. **UNITS: the station's own component update** (`EMachineKind::Medic`, the turret
  pattern — one spatial query of the radius per powered station inside the parallel pass, banking
  `GameUnitComponent::heal` into the unit's heal inbox; the unit's own tick applies it to health and
  battery and clears its permanent `collapsed` latch once the battery holds charge). Stations
  STACK on units (two radii = two heals). **The own player**: `GameMatch::tickMedicHealing` on every
  instance (`GamePlayer::heal` + `charge`; health/energy are owner-computed, `powered` arrives
  through GSt), once per tick however many overlap. The reach/rate tweaks bind
  `GameStructureParams::medicRange/medicHealRate`. Ghost shows the reach.

## A / S / D — Cables (root page)

**Power cable · Pipeline · Conveyor** (slots `CBL-P/F/M`). **The crossings are NOT on the hotbar** —
only the paint stroke places them (auto-crossing below).

Cable segments place by **PAINTING only** (`updateCablePlacement`): an LMB press starts a stroke at
its cell, holding and dragging keeps placing the cells the cursor crosses (L-filled between cursor
samples by `placeCableLine` — dominant leg first, **occupied cells skip so a stroke across an
existing run fills gaps**, so the run never breaks), and the release ends the stroke. The old
two-click L-line is gone.

**Auto-crossing.** The stroke HOLDS its newest cell back one step (`m_cablePending`). When the cell
after it is a bridgeable SOLE occupant of ANOTHER medium (`bridgeableAt` — a plain segment or another
crossing's end cell, with nothing bridging it), the stroke places **the stroke's OWN medium's crossing over that cell with its long
axis along the stroke** (`crossingForMedium`): the held cell and the cell beyond become the
crossing's two end cells and are never painted, so the painted run continues through the
crossing's outward ends. **An end cell that already holds this stroke's own medium is no obstacle:
`planCrossing` replaces it** (see the cell hash), so re-crossing a line where the run already
stands works. A crossing the cells still refuse (an end on another medium or a building, an actor
on it, an L-turn at the foreign cell) falls back to the plain skip. Same-medium cells still just
skip — the stroke merges into that run. The release AND the RMB cancel both land the held cell (it was
already shown painted); switching items or disarming drops it.

One placement burst caps at `c_cableMaxSegments` 32 GqP events.

## The Bases

They exist from game start only (`spawnBase`; `isPlaceableType` guards Base and the retired Connector
off placement), one per team.

* Respawn anchor.
* A passive trickle of Minerals at "Base income mult" of an extractor.
* MINERAL + ENERGY storage, **no fuel**: spawned full, self-generating solar-style; power cables
  attach.
* **Its SHIELD runs on EMITTER RULES, minus the bill** — `hasShieldEmitter` includes Base in
  `tickPower`, `strainable`, mirror and save: unit siege drain, latching dark when starved, output and
  reach from the "Base shield" tweaks — but **no per-second draw and no pressure surcharge** ("Base
  energy/s" is unused): the Base is a STORAGE building on the energy grid (a transport storage
  port, self-generating "Base energy gen/s"). Only enemies leaning on it drain it. `base.pre`'s
  authored Output no longer stands.
* DAMAGEABLE but **the death sweep never destroys it**: at 0 hp it stands dead until repaired.
  Constructors heal it at its `m_costs` entry — **never placed, so that entry only prices repairs**;
  `investMaterials` admits Base alongside placeables.

> **A destroyed Base would soft-lock the respawn.**

## Structure health and labels

Every structure taps the pressure bake at its position (`sampleBakedField`, no GPU query slot);
**territory owned by ANY other team's bubble drains health.**

**THE EMITTER GLOW is the Force system's bubble light** (no `Component Light` on the emitter
prefabs): one point light per bubble at its centre, sized by its iso radius, so it grows and fades
with the powered field and goes dark with it — see [`Code/Force/CONTEXT.md`](../Force/CONTEXT.md).

Emitters shrink out over "Emitter shrink time" when starved and **latch off until "Emitter restart
charge"**, pay a pressure surcharge ("Emitter energy/s @ pressure 1") plus a per-unit siege drain
(`addEmitterLoad`: a flat rate onto the NEAREST strainable enemy emitter within "Game/Enemies/Emitter
drain range" — 20 m, the Bastion's visible bubble radius at reach 45 / output 2.6, planar distance
to the STRUCTURE, shield state irrelevant; spitter shots deposit too, within their own
`EmitterDrainRadius`).

**World labels are CULLED before they are built** ("HUD" `Label max distance`, 120 m, not
Synced, **measured from the PLAYER entity** — the camera only when there is none): units come from
a frustum query bounded by that distance plus the camera-to-player distance and then test the exact
player distance, structures test it against the anchor (the selected one is exempt), and both drop
anything `worldToScreen` puts outside
the viewport plus the overlay's 100 px margin — `worldToScreen` itself only rejects what is BEHIND
the camera, so without these every structure on the map built a label the widget pass then clipped.

**World labels:** a health bar over every damageable structure, plus a second bar for storage (energy
yellow, fuel orange) and a third on the Base. **FULL health and shield bars stay hidden** — only
damage, blueprint progress, selection, or a prefab's `AlwaysDisplayHealth true` shows one. Undamaged
non-player units skip their label entirely; `player.pre` opts in so player tags persist. A SELECTED
cable segment or crossing adds its transport readout — "Energy 3 / 4 cells, 8.0 / 10.0 per s" plus
"Run 41 / 96 cells over 24 segments" (`StructureSystem::cableInfo`: the segment's fill, the cells
leaving it as a ~2 s average against its out-rate — also its second bar, hued by medium — and its
run's total; clients read both from the mirror). A segment at or above 90 % of its rate draws a
pulsing RED ring: the bottleneck marker, and the only cable ring.

## Unit selection

Select mode, **authority only** — units simulate on the server.

**LMB DRAG (> 8 px)** draws a box on the ground and on release selects every visible own-team unit
whose screen position is inside (`NpcSystem::queryVisibleUnits` — a camera FRUSTUM query — plus
`worldToScreen`). SHIFT adds; a plain click clears. `m_selectedUnits` are owning EntityPtrs pruned on
death, drawn with a green ring.

**EVERY RMB MOVE ORDER** — ground, held re-aim, or building face — **also goes to the selected
units**: `orderMove` = a LOCKED target with `moveOrder` set (dropping the route), auto-cleared within
`waypointRadius` so the AI resumes. A DSL `setTarget` lock never clears. **CTRL + RMB = units ONLY**
— the player stays put (`m_rmbUnitsOnly`, latched on the press for the whole hold).

A FRESH order can make the unit IGNORE the lane term for "Order flow blind" seconds so an old trail
cannot pull it back — **0 by default now that a fresh order SEEDS its own lane.**

## Interaction modes (`EPlayerMode`)

**Neutral / default = SELECT.** Click to inspect (highlight ring plus overhead info); RIGHT-click
GROUND with an own barracks selected = a waypoint; RIGHT-click a building = a MOVE ORDER to it.

**Q/W on the root page = Build** with that category, **A/S/D = Build with a cable armed** — **the
hotbar page doubles as the mode indicator.**

**Build mode ALSO click-selects** (`updateSelectionClick`, shared with Select mode): with nothing armed
every LMB inspects, and **with a ghost armed, a click the placement refuses — occupied cells, i.e. on a
building — selects it instead of doing nothing**, while any click that CAN place still places.

### RMB always moves the player

Cancelling rides along on the SAME press:

1. **Whatever step is pending is cancelled, one level at a time.** A half-finished two-click flow
   (Lance aim, Wall line, cable paint stroke, Crossing aim) drops first; the next RMB disarms the item.
2. **With an own barracks selected and the cursor on GROUND**, the press is instead a route waypoint
   (SHIFT appends). **This is the ONLY consumer that sets `m_rmbConsumed`**, because a rally point is a
   positive order, not a cancel, and pairing it with a move would drag the player to every waypoint.
3. **Everything else becomes a MOVE ORDER** (green destination ring). On GROUND the capsule walks to
   the clicked point and **HOLDING RMB keeps re-aiming at the cursor**. On a BUILDING the destination
   is the CLICKED ground point pushed out to the nearest face of the inflated footprint (half
   footprint + 1.2 m) when it fell inside — **so you walk to the side you clicked instead of grinding
   into the wall.** One-shot; it does not drag.

**Cancels deliberately do NOT consume** — a cancel that also ate the movement read as a dropped input.

A respawn drops a standing order. WASD movement no longer exists (the letter keys are grid hotkeys),
and **because RMB-hold steers, the camera yaw drag moved to MIDDLE-mouse**; LEFT/RIGHT arrows also
yaw, and the wheel zooms.

---

# Units

`Game:Npc`. **Barracks-produced, plus the co-op loose spawns.**

**Own-team units are TINTED GREEN** (`GameUnitComponent::applyTeamTint`: each render node's own
authored colour mixed 55 % toward the HUD's own-team green, children included — a tint, so the
unit types stay told apart): the game stamps the viewer's team into
`GameUnitParams::localTeam` (ctor, the server's lobby team, a client's adopted team), and the tint
applies at spawn and whenever the replicated team lands through the network blob — so clients tint
their own side too, and a team change re-tints as the next snapshot arrives. Other teams keep their
authored colours (restored per node if a unit ever leaves the local team).

## The split

* **The SIM is `GameUnitComponent`** in the entity pass: steering by the Nav flow fields (see Nav),
  shield battery on player rules minus regen with permanent collapse, exposure damage, field push,
  melee, the ranged stance, and death / void self-despawn.
* **`NpcSystem` is PRODUCTION + SERVICING.** It holds no entity lists at all — units and shots are
  World roots, walked when needed (see The architecture rule).
  `service(structures)` drains the components' static queues on the main thread: FireRequests, deaths
  (freeing the spawner's roster slot), SeedRequests, TurretFireRequests and SpawnRequests.
* **Shield, health and team need NO publish step** — they live on the component and ride the entity
  snapshot's game blob.

**BARRACKS and TURRETS are per-entity too.** `GameStructureComponent::update`'s machine block —
discriminated by `machineKind`, stamped with the union variant — runs the spawn clock or target query
ON the structure, pays from its own stores, claims the roster slot, and queues a request. `service()`
only performs the main-thread entity spawn, **refunding a failed one.**

Roster counts are spawn/death **EDGES** (`++aliveUnits` at the decision, `--` on the death event;
re-seeded by `loadUnits`).

## Targets

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
light (see the turret section for beams, flashes and the hurt light). Ranged units never swing.

**There is NO published player list.** Units find enemy players — puppet GameUnitComponents,
spatially registered like everything else — in the SAME queries as structures: the auto-target sweep
takes the nearest enemy puppet as fallback, and the short melee probe chews any puppet in reach.

## Damage

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

## Far tick

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

## Loose units spawn PARKED

`spawnLooseUnits` calls `PhysicsComponent::park(true)` right after the batch. **The body command
applies before the next step, so the body never simulates one.** The bubble needs nothing here:
every bubble spawns dark, and only the World's SIM LOD gate switches it on (see the Entity
CONTEXT's "Force bubbles").

> Wave blobs overlap, and a live unselected body took box3d's push-out and — frictionless and never
> steered — coasted away: the "flung" waves.

**The World's wake edge** (a fresh entity starts at `schedTier` 3, so its first stamped visit IS a
wake) zeroes the velocities and enables it once a player is near.

This is what makes a WAVE — spawned outside the barrier, far from everyone — reach the Base. A unit
that arrives at a shielded base enters its ZONE (see SIM LOD focus) and ticks at tier 1 there, so
the field pushes it. **One that arrives at an UNSHIELDED target while no player is near stands
frozen there** (known).

## `Friction 0` on every unit body

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

---

# Player

`Entities/Game/player.pre` = playerCapsule plus a `Component Force` shield.

* **MOUSE-ONLY movement** — RTS right-click move orders on the testbed's velocity-steering model,
  LShift sprint, Space jump off a ground raycast. **NO WASD** (those keys are grid hotkeys), and
  `tickMovement` ignores its camera-forward argument.
* `Accel` is **deliberately soft (30)**: steering force must lose the shoving match against bubble
  push.
* **NO player RANGED combat** — the old shot/projectile code is deleted. There IS a **melee AURA**:
  `GameMatch::tickPlayerMelee` (authority) grinds every adjacent enemy unit for "Melee dps" within
  "Melee radius" from every player capsule.

## The shield — a BATTERY model

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

---

# Save / load

**F9 save, F10 load — server and single-player only; clients refuse.**

A full sim snapshot to `Assets/Local/gamesave.txt` through AssetParser
(`StructureSystem::saveTo` / `loadFrom` / `clearAllStructures`, `NpcSystem::saveUnits` / `loadUnits`,
`GameMatch::saveGame` / `loadGame`): structures with id / type / pos / facing / node / team /
blueprint / stores / route — **cable segments are ordinary Structure entries, with NO Cable nodes** —
plus the map inputs and `PlayerPos`, the LOCAL player's body position. **Shots are NOT saved**;
projectiles are transient and a load clears them. **A structure entry writes only NON-DEFAULT
keys**: Id, Type and Position always; Facing, NodeIndex, Team, Blueprint, Health, the three
stores, Fill, OutputFrac and UnitType only when the `loadFrom` fallback (no facing, no node, team
0, built, full health, empty stores, no fill, dark, Grunt) would not restore them. A blueprint's
health is its build progress and is always below max, so it is always written.

**The PENDING TRICKLE** rides along too (`saveTrickle` / `loadTrickle`, co-op only). A wave is sized
in points at `queueWave` and its bodies materialize over the following frames, so a save mid-wave
used to drop everything not yet spawned — an F9 during a big wave quietly shrank it. `WaveTrickle`
carries the remaining points, WHERE they enter (`Origin`) and march (`Dest`), the blob `Radius`,
`LastArchetype`, and
the wave's rolled, JITTERED `Mix` — which cannot be re-derived from the archetype table.
`AmbientTrickle` carries the world-start scatter's remaining points plus the group it is mid-way
through placing. A resumed wave with points left **re-seeds its Nav lane**.

> `loadTrickle` MUST run after `rebuildCoopMap` — that voids the in-progress ambient group, whose
> anchor belongs to the old map. A save with NO trickle nodes (every save from before this existed)
> CLEARS both budgets: that file is the complete state, and letting the running session's own
> scatter continue on top of it would double-spawn. The spacing ring (`m_waveRecent`) is deliberately
> not saved — it only rejects spots for a handful of spawns.

Units save type / team / pos / health / energy / source / route index **plus `Order`, the standing
MOVE ORDER destination** — written only for a real move order, never for a transient `wanderOrder`
stroll. **Only NON-DEFAULT keys are written** (a co-op save holds tens of thousands of units):
Type and Position always, the rest only when the load fallback would not restore it — team 1, a
fresh spawn's full health and battery, source 0 and route index 0 are omitted. `loadUnits`'s
fallbacks ARE those defaults; keep the two in step. `loadUnits` replays it through `orderMove(pos, fresh=false)`, so a loaded co-op wave keeps
marching on the Base instead of parking (see **Targeting range gates**: the order is the only wave
marker). A save without the key loads AI-driven, as before.

`PlayerPos` is written only when the instance owns a capsule (headless writes no key), and a load
without the key leaves the player put — so old saves still load. The load runs
`GamePlayer::teleport`, which follows the same **teleport contract** as the death respawn: physics
teleport, zeroed velocity, both interpolation poses stomped and the step stamp refreshed, plus the
standing move order dropped. Both call sites — F10 in `updateWindowed` and the `--scenario` timer —
are main thread and PRE-`physics.update`, so those direct body setters are sanctioned.
**REMOTE players are not saved, and no player STATE is** (health, energy, materials): a load leaves
every player's pools as they are.

> **SAVE COMPAT: `Type` is the raw enum INT, so `EStructureType` values are never removed or
> reordered.** New types APPEND before Count; the retired Connector (index 2) keeps its dead slot
> (`loadFrom` skips it with a warning), the retired per-type barracks map to a Barracks with that unit
> type, and old saves' Cable nodes are silently ignored since links re-derive. `ENpcType` follows the
> same rule.

`loadFrom` clears first (removal hooks → GRm prune connected clients) preserving ids, then the server
re-broadcasts everything. Unit entities resync through normal network despawn and spawn.

## The profiling scenario

`runScenario(savePath)` — authority only, called once at `--scenario-at`. See **Profiling** in
[`Code/Core/CONTEXT.md`](../Core/CONTEXT.md) for the flags and the retry conditions.
