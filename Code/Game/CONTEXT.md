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

* `NpcSystem` and `StructureSystem` keep **ROSTERS of owning `EntityPtr`s**, added at spawn and
  deregistered through `World::setOnRootEntityRemoved` — **the ONE notification every removal path
  funnels into** (death destroy request, network despawn, editor delete). No per-frame query
  revalidates them.
* The per-entity SIMULATION lives in the Entity components, inside the parallel pass. A unit
  **REPORTS** what the game needs — shots to spawn, its death, damage — through the components' static
  event queues, and `NpcSystem::service` drains them on the main thread.
* Machine state lives ON the structure (a union in `GameStructureComponent`), **so there are no
  id-keyed maps and state dies with its structure.**

## Partitions

`Game:Match` (the orchestrator), `Game:Player`, `Game:Structures`, `Game:Npc`, `Game:GameCamera`;
barrel `Public/Game.ixx`.

> **`GameMatch` MUST be a stack local in `main()`** — it holds EntityPtrs and Force handles, so a
> global would need an InitSeg slot.

---

# The frame

Three entry points, all main thread, at three different points of main.cpp's loop.

| Call | Where | What |
|---|---|---|
| `updatePlayer(dt)` | After `networkManager.receive`, **PRE-PHYSICS** ([main.cpp:747](../App/main.cpp#L747) region) | **The PLAYER/CAMERA hot path and nothing else**: capsule adoption on a client, velocity steering, and the shield's body push — the direct body setters that must land BEFORE this frame's physics step. Deliberately minimal so main reaches the spatial and begin-frame kicks as early as possible. |
| `update(dt)` | AFTER the spatial + begin-frame joins ([main.cpp:791](../App/main.cpp#L791)) | The whole rest of the tick: structures authority/mirror, materials, unit production, base healing, nav staging, net flushes. **Spawns, destroys and spatial queries are legal again here.** Becomes the server tick in MP. |
| `updateWindowed(camera, dt)` | Right after `controls.applyPlayerCamera` | Camera overwrite, aim and placement input, ghost + debug draw, HUD. **SKIPPED while the main menu or lobby is active**, so a lobby client's constructed GameMatch simulates but gets no input, camera or HUD. |

**Consequences of `update`'s placement:** freshly spawned actors link into the spatial index at the
NEXT commit (the spawn guard keeps them visible), new bodies' velocities integrate on the NEXT step,
and `feedNav`'s staging feeds the NEXT frame's `NavSystem::update`.

In game mode `InputControls::setGameMode(true)` mutes the testbed spawn and possess keys — script
event fires, hotbar routing, F5/F6 and T/R/G stay — and the sponza spawn is skipped.

## SIM LOD focus

`GameMatch::update` publishes the focus every frame through `World::setSimLodFocus`: its own capsule
plus every client twin, **so no unit throttles near any player.**

With the LOD active the World visits ONLY entities within the outer tier radius of a player, plus
`Global true` roots. A structure beyond that radius **does not tick at all** — no production, no
flows, no turret fire.

> **EVERY BUILDING prefab authors `Global true`** (emitter, generator, extractor, battery, fuel tank,
> solar, fabricator, bastion, lance, barracks, wall, turret, silo, constructor, base, house), so
> player structures are never LOD-culled. **Only the CABLE segments and the Crossing stay
> LOD-selected** — they carry no stores, and hundreds of them would sit in the always-visited list.

`terrainroot.pre` is Global too, so the rocks, markers and barrier children are selected individually
by their own position.

---

# The world

A flat 400 m plane (`Assets/Entities/Game/*.pre`, baseshapes solid meshes with per-prefab `Color`
tints), plus generated rock terrain and per-mode resource nodes.

**NO win condition yet.** The Force AMBIENT FIELD (the old PvE world field) is fully REMOVED.

## The shared terrain grid

`CoopMap` serves BOTH modes — it carries its own `cellsX` / `cellsZ` / `cellSize` / `origin`, and
everything downstream (cell math, flood fill, rect merging, rock spawn, `clampToOpenGround`) reads
those.

| Mode | Grid |
|---|---|
| Co-op | `c_coopCells` 36² at 10 m — **exactly `2 × c_coopHalfSize` (180 m)**, static_asserted |
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

The same inputs ride the F9 save (`MapSeed` / `MapFill` / `MapLanes`, or `PvpMap`), **because a
joiner's tweak sync lands after the replay — too late.** `loadGame` regenerates the exact map after
clearing structures.

---

# Co-op PvE (`--coop`)

## The map

A big square — `c_coopHalfSize` 180 m on the 400 m ground plane — of RANDOM IMPASSABLE TERRAIN.
`rebuildCoopMap(seed, fill, lanes)`:

1. Two-octave value noise thresholded to EXACTLY "Terrain fill" (a sorted-copy threshold).
2. A rock-free Base zone, `c_coopBaseClearRadius` 26 m.
3. "Terrain lanes" carves wobbling attack lanes from the Base ring out past the edge.
4. `finishGrid` flood-fills from the Base. **`CoopMap::depth` is the geodesic BFS distance**, which
   the ambient scatter gates on.

**Edge barrier at ±180:** a `barrier.pre` ring (20 m segments, E/W yaw'd 90°) whose collider is
`Layer Barrier, CollidesWith Player`. **It blocks ONLY player capsules** (`player.pre` carries
`Layer Player`, mask All) — units and shots walk through. Visuals are a post per segment plus
`drawCoopBarrier`'s pulsing energy lines on the real clock.

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
* Exposure damage is GRADED by field depth
  (`smoothstep(iso × "Field damage starts", iso, opposing) × fieldDps × mult`), **because the push
  equilibrium parks pressing units AT the surface** (`opposing ≈ iso`), where a binary inside test
  read false and units ground on shells unharmed.
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

"Ambient budget" (20000) POINTS of units placed at world start, in small **GROUPS**: `AmbientSpawn`
holds one archetype and a remaining count, so the trickle fills the current group — a disc around a
reachable anchor — before rolling the next. **Loose blobs, not a lattice.**

`ambientPointNear` rejection-samples open ground and slides bodies 1.2 m off rock edges, **so nothing
lines up on the cell lattice.**

**The archetype roll is gated by GEODESIC spawn depth**, the same `minWave` gate driven by depth
instead of time, as a **WINDOW**: only recipes whose gate lies within "Ambient recipe window" (3)
bands below the cell's depth band roll.

> So the near ring is swarm-grade and **the deep map holds ONLY the elite tier** — giants, titans and
> lobbers never sit near the Base.

Cells under "Ambient min depth" (0.2 of the max walking depth) are rejected outright, on top of the
planar "Ambient safe radius" (45 m).

## Waves

They-are-Billions style. `tickWaves` arms the clock ("First wave delay" 30 s, then "Wave interval"
90 s).

**`queueWave`:**

1. Picks a RANDOM compass direction projected onto the barrier square and stepped OUTSIDE it — **the
   swarm spawns in the open ring between barrier and ground edge and walks in THROUGH the
   player-only barrier.** Cluster points that drift inside push back out along the dominant axis,
   clamped to `c_coopGroundEdge` 196.
2. **Sizes the wave in BUDGET POINTS**, not unit counts: "Wave budget" (20) plus "Wave budget growth"
   (40) per wave. Each type spends its "Cost <type>" tweak, **so a brute-heavy archetype fields far
   fewer bodies than a swarm flood of the same budget.** The "Max enemy units" cap (15000) converts at
   the cheapest cost, and an unaffordable roll downgrades to the mix's cheapest type.
3. Seeds ONE lane and fires "GWv" (u16 index) at clients.

**`tickCoopSpawns` TRICKLES the actual spawns** ("Spawns per frame" 100 — wave first, then ambient),
so a huge wave enters over seconds rather than one hitch. **The trickle is THREADED:** the frame's
rolls (budget math plus glm RNG, main-thread — `linearRand` is not thread-safe) collect into
`NpcSystem::spawnLooseUnits`, one `World::spawnBatch` fan-out per frame, with team/order/roster fixup
serial after the join.

Wave spawn points roll up to 6 times against the last `c_waveRecentSpawns` (32) spawns at 2 m
spacing, **so parked bodies rarely overlap in the first place.**

Each unit is `orderMove`d to the Base's near face on the incoming side; the lock releases on arrival
and the AI takes over. All spawns go through `spawnLooseUnit` — **sourceId 0: no route, no death
accounting.** The authority HUD shows "Next wave (s)".

### Unit types and costs

`ENpcType`: Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant, Titan, Lobber, Spawner.
**SAVE FILES store the type as an int — APPEND only.**

| Type | Cost | Notes |
|---|---|---|
| Grunt / Brute / Runner / Spitter / Swarm | 3 / 10 / 2 / 5 / 1 | The barracks tier. Spitter is enemy-only. |
| Elite / Giant / Titan / Lobber / Spawner | 8 / 25 / 60 / 12 / 30 | **Enemy-only elite tier**, `enemyElite/Giant/Titan/Lobber.pre` + `enemySpawner.pre`. |

* **The Lobber** is `Ranged` with `ShotKind 1` = the slow SPLASH shell `enemyLob.pre`
  (`SplashRadius 5`); shot speed 14.
* **The Spawner** is a tough ranged-stance hive: `ShotKind 2` makes each "shot" a LOOSE Swarm body
  born beside it toward its target every `FireInterval` while it holds at `StandoffRange`. **The
  births are loose units — no wave budget, no roster cap — so the hive itself is the thing to kill.**
* `isBarracksUnitType` allows only Grunt / Brute / Runner / Swarm anywhere; **an old Spitter-barracks
  save loads as a Grunt barracks.**

### Archetypes (`c_waveArchetypes`, [Match.cpp:514](Private/Match.cpp#L514))

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

Wave units stay order-driven until arrival, then the same gated AI takes over.

## Known gaps

* **The Base takes damage but the death sweep never destroys it — no lose condition yet.**
* The wave clock and pending counts are not saved (F10 re-arms).
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

Other broadcasts: **GRt** routes, **GBu** barracks unit type, **GLt** turret beams, **GWv** wave
index, **GDm** damage flush, **GMp** map.

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
`Game/Camera` and `Game/Sim LOD`.

---

# Economy

A node produces NOTHING until an **extractor** is built on it — one per node; the ghost snaps to the
nearest free node within "Extractor snap radius", validated again at place time — AND that extractor
is fed energy.

## Three flow networks, one band model

**Energy** (`tickPower`), **fuel** and **minerals** all use the same gravity-fed potential bands.
Every structure holds LOCAL stores; each tick every derived link moves resource toward consumers,
capped by the MEDIUM's throughput.

| Network | Bands (high → low) | Throughput tweak |
|---|---|---|
| Energy | producers (generator, solar) always exporting → battery storage BALANCING by fill fraction → consumers, outranking everything until FULL | "Cable throughput" 5/s |
| Fuel | extractor tanks export → fuel tanks balance → generator/fabricator burners fill first | "Pipeline throughput" |
| Minerals | extractor/fabricator outputs (3) → **the Base (2)** → silos (1) → constructors (0) | "Conveyor throughput" |

> **The Base sits BETWEEN producers and silos on purpose**, so extractors still dump into it at full
> rate while its own trickle PREFERS flowing out over banking, keeping only what receivers cannot
> take. This replaced the old mineral-sink fiction.

* **Cross-band links run at full throughput downhill**: consumers fill first, and the surplus banks.
* **Same-band links move a DAMPED fraction** (`c_equalizeDamping` 0.25) of the exact equalizing
  transfer. Each source sizes its share without seeing the others, **so undamped, N sources feeding
  one receiver overshoot the balance point together and bounce back next tick** — wasted throughput
  and a flow direction that keeps reversing. Damping stays stable up to 1/damping simultaneous
  sources, **and removed the need for any direction hysteresis in the draw.**

Energy propagates hop by hop, thin lines starve, and a full generator buffer throttles production
(export-limited = no fuel burn). Consumers drain their internal battery; empty = unpowered.

**Buffers:** emitter and extractor "Internal buffer" 10, generators 20, battery 100, **barracks
"Barracks energy capacity" 20** — units are PAID from it, so a barracks needs a power cable and holds
no minerals. Powered extractors and fabricators fill their OWN buffer and **stall when full**. Only
what sits in Mineral silos and the Base is SPENDABLE (a per-team cache recomputed per tick; spending
drains silos first, Base last).

## Physical cables

The link tools, point-to-point cables and Connector range links are all REMOVED. **`Connector` is a
RETIRED enum slot** — its table entries remain, placement refuses it, and `loadFrom` skips it.

Cables are **1-cell GRID STRUCTURES**: `CablePower` / `CablePipe` / `CableConveyor` (one per medium,
no tiers) plus the **1×3 oriented Crossing** bridge.

A perpendicular cable passes UNDER the Crossing's middle cell. It conducts whichever ONE medium its
two END cells resolve to: **each end accepts from THREE sides** (outward plus the two laterals; only
the middle is pass-through-only), and a run on one end plus a capacity-holding building on the other
also counts.

### Links are DERIVED, never authored

`StructureSystem::rebuildDerivedLinks` — dirty-gated, main thread, in `tickAuthority` after the
request drain and in `tickMirror`. **CLIENTS derive locally from the mirrored segments.**

1. Union-find the **BUILT** segments over 4-neighbour equal-medium adjacency. **Blueprint segments
   break the path.** The under-cable participates through `CellEntry::underId`, which is also what
   keeps a crossing from unioning with the cable below it.
2. Attach every adjacent building with capacity > 0 in the run's medium (blueprint buildings
   pre-wire).
3. **BRIDGE through built buildings** — a building conducts every medium it holds, so two same-medium
   runs touching it merge into one. Blueprint buildings do not bridge, and **buildings NEVER connect
   by direct adjacency: a link always needs cable between them.**
4. Diff desired-vs-live: all attached pairs per run (a clique up to `c_runCliqueCap` **32 buildings =
   496 links**). Past that, a **STAR from a ROLE-picked hub** — storage band 1 first (it relays both
   directions, the old Connector's role), else a producer, lowest id as tie-break.
   > **NEVER a plain consumer when better exists:** a band-0 hub cannot send energy UP to a battery,
   > which starved batteries on big runs.
5. Owner = the lower structureId, **so rebuilds never flip flow state.**

A pair of buildings may still hold **ONE LINK PER MEDIUM**.

### The cell hash

`m_cells`: cellKey → `{id, underId}`, maintained at the spawn and remove seams (a crossing demotes its
under-cable to `underId`; removal promotes it back).

It backs `cellsFree` — **per cell now: buildings refuse ANY occupied cell (no building on a cable); a
cable may slot under a free crossing middle; a crossing may bridge exactly one cable** — plus the
derivation and the arm visuals. `ignoreCables` is the unit-spawn probe, since cables are walk-through.

### Draw and lifecycle

Segments are real meshes with per-medium authored tints (power yellow, pipe orange, conveyor blue;
blueprint gray rides `applyStructureTint`). Each is a low `Cylinder2` hub with **4 render-only ARM
child entities** (`ArmPX/NX/PZ/NZ`, `Enabled false` in the `.pre`, cached on `Ref::arms` at spawn,
toggled by `updateArms`). A conducting Crossing TINTS to its medium's hue (`Ref::conductMedium`).

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
| 3×1 / 1×3 | **Crossing** — the ONE non-square footprint, by its facing |

> The Crossing's facing is quantized to ±X/±Z at `placeStructure` like the Lance, and carried on GPl
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

| Slot | Root page | Category page |
|---|---|---|
| Q / W / E | **CMBT / PROD / CBLE** | the category's items, in order |
| X (slot 9) | **DEL** | **DEL** (category pages cap at 9 items so X stays Delete) |
| C (slot 10) | — | **CNCL** |
| V (slot 11) | — | **BACK** |

**C / Esc / Tab = `cancelOneLevel`**: a two-click step drops → the armed item disarms → the page or
Delete mode returns to Select, **one per press**. **V = straight back to Select.**

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
* **Turret** — HITSCAN lightning. The component picks the nearest enemy unit in "Turret range", pays
  "Turret shot energy" from its internal store and lands "Turret damage" on the spot; **it never
  misses.** The strike is a BUNDLE of jagged debug lines fading over "Turret beam lifetime" 0.5 s
  (`NpcSystem::addBeam` / `drawBeams`), broadcast to clients as GLt.
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

Each spawn pays the type's "\<Type\> spawn energy" from the energy store **and takes that cost ×
"Barracks seconds per energy" to produce — the price IS the build time.**

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

**Generator · Solar · Extractor · Fabricator · Constructor · Battery · Fuel tank · Mineral silo**

Solar is a free trickle with no fuel (a flat `CubeQuarter` slab); the fabricator turns energy + fuel
into minerals; the mineral silo is the team's spendable bank.

## E — Cables

**Power cable · Pipeline · Conveyor · Crossing**

Cable segments place by **PAINTING only** (`updateCablePlacement`): an LMB press places its cell,
holding and dragging keeps placing the cells the cursor crosses (L-filled between cursor samples by
`placeCableLine` — dominant leg first, **occupied cells skip so a stroke across an existing run fills
gaps**, so the run never breaks), and the release ends the stroke. The old two-click L-line is gone.

**The Crossing is single-click**: its long axis follows the CAMERA facing quantized to ±X/±Z, and
re-pressing or clicking the armed CRSS slot **rotates it 90°**. One placement burst caps at
`c_cableMaxSegments` 32 GqP events.

## The Bases

They exist from game start only (`spawnBase`; `isPlaceableType` guards Base and the retired Connector
off placement), one per team.

* Respawn anchor.
* A passive trickle of Minerals at "Base income mult" of an extractor.
* MINERAL + ENERGY storage, **no fuel**: spawned full, self-generating solar-style; power cables
  attach.
* **Its SHIELD runs on EMITTER RULES** — `hasShieldEmitter` includes Base in `tickPower`, `strainable`,
  mirror and save: energy draw, pressure surcharge, unit siege drain, latching dark when starved, with
  output and reach from the "Base shield" tweaks. `base.pre`'s authored Output no longer stands.
* DAMAGEABLE but **the death sweep never destroys it**: at 0 hp it stands dead until repaired.
  Constructors heal it at its `m_costs` entry — **never placed, so that entry only prices repairs**;
  `investMaterials` admits Base alongside placeables.

> **A destroyed Base would soft-lock the respawn.**

## Structure health and labels

Every structure has a `ForceQuery`; **territory owned by ANY other team's bubble drains health.**

Emitters shrink out over "Emitter shrink time" when starved and **latch off until "Emitter restart
charge"**, pay a pressure surcharge ("Emitter energy/s @ pressure 1") plus a per-unit siege drain
(`addEmitterLoad`: a flat rate onto the NEAREST ACTIVE bubble; spitter shots deposit too).

**World labels:** a health bar over every damageable structure, plus a second bar for storage (energy
yellow, fuel orange) and a third on the Base. **FULL health and shield bars stay hidden** — only
damage, blueprint progress, selection, or a prefab's `AlwaysDisplayHealth true` shows one. Undamaged
non-player units skip their label entirely; `player.pre` opts in so player tags persist.

## Unit selection

Select mode, **authority only** — units simulate on the server.

**LMB DRAG (> 8 px)** draws a box on the ground and on release selects every visible own-team unit
whose screen position is inside (`NpcSystem::queryVisibleUnits` — a camera FRUSTUM query — plus
`worldToScreen`). SHIFT adds; a plain click clears. `m_selectedUnits` are owning EntityPtrs pruned on
death, drawn with a green ring.

**EVERY RMB MOVE ORDER** — ground, held re-aim, or building face — **also goes to the selected
units**: `orderMove` = a LOCKED target with `moveOrder` set (dropping the route), auto-cleared within
`waypointRadius` so the AI resumes. A DSL `setTarget` lock never clears.

A FRESH order can make the unit IGNORE the lane term for "Order flow blind" seconds so an old trail
cannot pull it back — **0 by default now that a fresh order SEEDS its own lane.**

## Interaction modes (`EPlayerMode`)

**Neutral / default = SELECT.** Click to inspect (highlight ring plus overhead info); RIGHT-click
GROUND with an own barracks selected = a waypoint; RIGHT-click a building = a MOVE ORDER to it.

**Q/W/E on the root page = Build** with that category — **the hotbar page doubles as the mode
indicator.**

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

## The split

* **The SIM is `GameUnitComponent`** in the entity pass: steering by the Nav flow fields (see Nav),
  shield battery on player rules minus regen with permanent collapse, exposure damage, field push,
  melee, the ranged stance, and death / void self-despawn.
* **`NpcSystem` is PRODUCTION + SERVICING plus the rosters.** It holds no other unit state.
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

`GameMatch::feedNav` supplies obstacles (the border ring plus structure footprints) and per-team
sources (live non-invulnerable structures, player capsules, and **live units from the roster — no
sweep**).

**UNIT-VS-UNIT COMBAT:** non-ranged units melee ONE enemy unit — the nearest inside
`attackRange + victim.bodyRadius` — for `attackDps`, holding at that ring. Players in the swarm still
take the area damage from every adjacent unit.

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

`NpcSystem::service`, authority only. "Far tick interval (s)" 0.5, readout "Far ticked".

Units the SIM LOD did not select — **no tier stamp, body disabled, never visited by the entity pass** —
still walk their ROUTE or MOVE ORDER through `GameUnitComponent::updateFar`, **a parallelFor over the
roster** every interval of sim time:

* Straight at the target where the raster shows a clear line, else along the enemy team field's
  descent (geodesic, around rocks).
* **By TELEPORT**: the entity pos is the far truth; the body pose and prev/curr are stomped, and the
  spatial entry is refreshed so the selection query finds it as it approaches.
* Waypoints advance and the order clears with the full sim's radius rule. A step into rock holds.
* No combat, bubble, strain or health death check while far. **The ONE exception: a unit below
  `voidY` −3 — fallen through the floor — is killed by the far tick too**, so no unit escapes it.

## Loose units spawn PARKED

`spawnLooseUnits` calls `PhysicsComponent::park(true)` plus `ForceComponent::setActive(false)` right
after the batch. **The body command applies before the next step, so the body never simulates one**,
and the bubble stays dark until a tiered visit.

> Wave blobs overlap, and a live unselected body took box3d's push-out and — frictionless and never
> steered — coasted away: the "flung" waves.

**The World's wake edge** (a fresh entity starts at `schedTier` 3, so its first stamped visit IS a
wake) zeroes the velocities and enables it once a player is near.

This is what makes a WAVE — spawned outside the barrier, far from everyone — reach the Base. **A unit
that ARRIVES while no player is near stands frozen there**; structures are not focus points (known).

## `Friction 0` on every unit capsule

Every `enemy*.pre`; the player keeps 0.3.

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
  normalizes by the OUTPUT that produced it** (`m_outputHistory`), since the readback is ~2 frames
  latent and scales with output.
* **SURFACE TENSION**: push AND drain scale by `(1 + tension × pressure)` on BOTH sides
  ("Game/Shield/Surface tension", "Game/Enemies/Push tension", "Game/Structures/Pressure draw
  tension") — **leaning deep into a bubble stiffens superlinearly and burns both batteries.**
* "Cover drain reduction" cuts the drain by the FRIENDLY field surplus over the own output — standing
  inside a team emitter's bubble.
* **Health drains only while the equilibrium shield radius squishes below "Damage radius" AND enemy
  pressure is actually present**, and it REGENERATES at "Base heal/s" within "Base heal radius" of an
  own-team Base. `tickBaseHealing` is **own player only** — health is owner-computed, so every instance
  heals its own capsule against its local structure mirror, with no sync.
* "Spawn grace" gives post-respawn immunity, **because the GPU readbacks still carry the death position
  for ~2 frames.**
* Death teleport-respawns per the teleport contract.

**HUD** through `Globals::gameHud`: bars Health / Shield / Materials, counters Minerals / Fuel / Power,
and hotbar slot counts = affordable.

---

# Save / load

**F9 save, F10 load — server and single-player only; clients refuse.**

A full sim snapshot to `Assets/Local/gamesave.txt` through AssetParser
(`StructureSystem::saveTo` / `loadFrom` / `clearAllStructures`, `NpcSystem::saveUnits` / `loadUnits`,
`GameMatch::saveGame` / `loadGame`): structures with id / type / pos / facing / node / team /
blueprint / stores / route — **cable segments are ordinary Structure entries, with NO Cable nodes** —
plus the map inputs. **Players and shots are NOT saved**; projectiles are transient and a load clears
them.

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
