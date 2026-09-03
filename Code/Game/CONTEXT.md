# Game

> Library documentation for `Code/Game`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction.
>
> The game components (`GameUnitComponent` / `GameStructureComponent` / `GameProjectileComponent`)
> live in Entity — see [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md). Unit steering over the flow
> fields is in [`Code/Nav/CONTEXT.md`](../Nav/CONTEXT.md).

The game built on the engine: `Code/Game`, a static lib linked by App. A top-down tactical PvP
whitebox plus a co-op PvE mode.

* `App.exe --game` = single player sandbox.
* MULTIPLAYER = `--game --server` (a WINDOWED listen server — GPU readbacks drive the sim, so
  `--headless` refuses `--game`) plus `--game --connect <ip>`.
* Without `--game` the testbed is untouched.

The OLD PvE — ambient field, player combat — is REMOVED. The co-op mode below is the new PvE.

## Partitions and the frame

Partitions: `Game:Match` (the orchestrator — a STACK LOCAL in main, holding EntityPtrs and Force
handles), `Game:Player`, `Game:Structures`, `Game:GameCamera`. Barrel: `Public/Game.ixx`.

### main.cpp call order

| Call | When, and why |
|---|---|
| `game.updatePlayer(dt)` | After `networkManager.receive`, pre-physics. ONLY the player-body writes — capsule adopt, velocity steering, and the shield push. The camera hot path, kept minimal so the spatial and begin-frame kicks start ASAP. |
| `game.update(dt)` | AFTER the spatial and begin-frame joins. The whole rest of the game tick; spawns, destroys and queries are legal there. |
| `game.updateWindowed(camera, dt)` | Right after `controls.applyPlayerCamera`. Overwrites the frame camera. |

**Consequences of `game.update`'s position:** fresh actors link into the spatial index at the NEXT
commit under the spawn guard; new bodies' velocities integrate on the NEXT step; and `feedNav`'s
staging feeds the NEXT frame's `NavSystem::update`.

**`updateWindowed`** drives the angled follow cam, LEFT/RIGHT arrows and MIDDLE-drag yaw, and wheel
zoom. Q/E/WASD are grid hotkeys. It is SKIPPED while the main menu or lobby is active, so a lobby
client's constructed GameMatch simulates but gets no input, camera or HUD.

In game mode `InputControls::setGameMode(true)` mutes the testbed spawn and possess keys — script
event fires, hotbar routing, F5/F6 and T/R/G stay — and the sponza spawn is skipped.

### Sim LOD focus

`GameMatch::update` publishes the SIM LOD focus every frame through `World::setSimLodFocus`: its own
capsule plus every client twin, so no unit throttles near any player. See the SIM LOD section in
[`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md).

With the LOD active the World visits ONLY entities within the outer tier radius of a player, plus
`Global true` roots — `terrainroot.pre` is one, so the rocks, markers and barrier children are
selected individually by their own position.

A structure beyond that radius does not tick at all — no production, no flows, no turret fire —
unless its prefab authors `Global true`. **EVERY BUILDING prefab authors it** (emitter, generator,
extractor, battery, fuel tank, solar, fabricator, bastion, lance, barracks, wall, turret, silo,
constructor, base, house), so player structures are never LOD-culled. Only the CABLE segments and the
Crossing stay LOD-selected: they carry no stores, and hundreds of them would sit in the
always-visited list.

## World

A flat 400 m plane (`Assets/Entities/Game/*.pre`, baseshapes solid meshes with per-prefab `Color`
tints), plus rock arena terrain and per-arena resource nodes (below).

NO win condition yet. The Force AMBIENT FIELD — the old PvE world field — is fully REMOVED from the
engine.

---

# Co-op PvE

`--coop`, requires `--game`. CLIENTS pass both too: the world layout is built locally, and only the
sim mirrors.

## The generated map

No corridor. A big square — `c_coopHalfSize` 180 m half-size on the 400 m ground plane — of RANDOM
IMPASSABLE TERRAIN.

`GameMatch::rebuildCoopMap(seed, fill, lanes)`:

1. 10 m cells (`c_coopCells` 36², aligned 5:1 with the 2 m build grid). The SAME `CoopMap` grid plus
   `finishGrid` / `spawnTerrain` / `clampToOpenGround` machinery builds the PvP arenas at 5 m cells —
   see the PvP section.
2. Two-octave value noise thresholded to EXACTLY "Terrain fill" (a sorted-copy threshold).
3. A rock-free Base zone (`c_coopBaseClearRadius` 26).
4. "Terrain lanes" carves wobbling attack lanes from the Base ring out past the edge.
5. A 4-connected BFS FLOOD FILL from the Base turns every unreachable open pocket INTO rock — so all
   open ground (nodes, ambient spawns, move orders) is reachable by construction, and
   `CoopMap::depth` is the geodesic BFS distance.

Blocked cells spawn `rock.pre` — a 10 m cube, sunk to a 3.5–6 m exposed height — under ONE
`CoopTerrain` root through `spawnBatch`. Horizontal runs merge into `m_terrainRects`, which are the
Nav obstacles AND the `StructureSystem::setTerrainBlocked` rects: `cellsFree` refuses them (red
ghosts, no building in rock), and unit-spawn probes skip them.

Blocked cells also spawn `terrainmark.pre` — a 13 m dark-red ground plane, render-only — so the
impassable footprint outlines every rock.

**Edge barrier at ±180.** A `barrier.pre` ring (20 m segments, E/W yaw'd 90°) whose collider is
`Layer Barrier, CollidesWith Player`: it blocks ONLY player capsules (`player.pre` now carries
`Layer Player`, mask All). Units and shots walk through. Visuals are a post per segment plus
`drawCoopBarrier`'s pulsing energy lines (3 heights, real clock). Placement bounds = inside the
barrier.

### Determinism and sync

Generation is pure seeded math (`MapRng` / `mapHash` — **NEVER `glm::linearRand`**), so every instance
derives the identical map.

* The AUTHORITY rolls the seed ("Game/Coop/Map seed", 0 = random) and broadcasts seed + fill + lanes
  as the "GMp" event — FIRST in `onClientJoined`, before the GPl replay, in reliable ch1 order.
* Clients defer terrain and nodes until it arrives; `rebuildCoopMap` no-ops on repeats.
* fill and lanes ride GMp — and the F9 save as MapSeed / MapFill / MapLanes, where `loadGame`
  regenerates the exact map after clearing structures — because a joiner's tweak sync lands after the
  replay.

### Nodes and teams

* A STARTER mineral + fuel pair about 10 m from the Base, then golden-angle-spiral candidates SNAPPED
  to reachable open cells with a 13 m spacing floor.
* A move order or route waypoint clicked into rock clamps to the nearest open cell
  (`clampToOpenGround`).
* Every player is on team 0 around ONE central Base (`allocateClientTeam` returns 0, and
  `teamStartPos` is the shared start). The AI team is `GameMatch::CoopAiTeam` = 1, NOT a high slot,
  and fields UNITS ONLY, no structures.
* Co-op runs the Force system at TWO LIVE TEAMS (`ForceSystem::setNumTeams(2)` in the GameMatch
  ctor): every force shader recompiles with `NUM_FORCE_TEAMS=2` and the bake volume and buffers
  shrink to fit (one RGBA16F volume, one vec4 per sample readback). **Every co-op team index must
  stay < 2.**

## The Swarm unit

`ENpcType::Swarm`, `Entities/Game/swarmUnit.pre` — the cheap horde body: health only, NO
`Component Force`, so no bubble, battery or GPU readbacks. Built for many-thousand counts.

**Shield-less units interact with emitters through the BAKED PRESSURE FIELD**
(`ForceSystem::sampleBakedField(pos, team)` in the entity pass — a CPU bilinear tap, no per-unit GPU
slot, unlimited unit counts).

* `opposing` / `opposingGradient` reproduce the SHIELDED units' exact push chain:
  `-grad × 0.35` (the 13-sample integral's mean self-weight) `× forceGain × pushGain × pressure ×
  tension`, speed-clamped. So ONE "Field push gain" tweak rules both paths.
* Exposure damage is GRADED by field depth:
  `smoothstep(iso × "Field damage starts (x iso)", iso, opposing) × fieldDps × mult`.
  The push equilibrium parks pressing units AT the surface (opposing ≈ iso), where a binary inside
  test read false and units ground on shells unharmed. About 3 frames latent.
* **Fallback** when the bake is disabled ("Force/Bake/Enabled" off):
  `GameStructureComponent::bubbleRadius` — stamped next to `strainable` in the death sweep as
  `reach × 0.5 × outputFrac` — gives the same exposure damage CPU-side, with no push.

They still deposit `addLoad` strain. A shield-less body spawns with a ZERO battery
(`energy = energyMax = 0` when `shieldOutput ≤ 0`, in `GameUnitComponent::spawn`), so the label
pass's shield-vs-health branch and the damage absorb never mistake it for shielded. DAMAGED swarm
show health bars like any unit — full bars hide, so a horde only labels its damaged slice.

## Ambient spawns

"Ambient budget" POINTS of units scattered around the map at world start, in small GROUPS.

`AmbientSpawn`: 3–9 bodies of ONE archetype in a 4–9 m disc around an anchor on a random REACHABLE
map cell, uniform by area, outside "Ambient safe radius". `ambientPointNear` rejection-samples open
ground and slides bodies 1.2 m off rock edges, so nothing lines up on the 10 m lattice.

* Same per-type "Cost *" tweaks as waves.
* A group's archetype roll is gated by GEODESIC spawn depth (`CoopMap::depth` / `maxDepth`) the way
  waves gate by index, but as a WINDOW: only recipes whose wave gate lies within
  "Ambient recipe window" 3 bands below the depth band roll. Near ring = swarm-grade recipes only;
  deep map = ONLY the elite tier — giants, titans and lobbers hold the far map and never sit near the
  Base.
* Cells under "Ambient min depth" 0.2 of the max walking depth are rejected outright, on top of the
  planar "Ambient safe radius" 60.

## Targeting range gate

EVERY unit's Nav team-field targeting is RANGE-GATED by "Target search radius" (geodesic
`Sample.dist`). The field rebuilds about every 0.25 s with LIVE source positions, so in-range units
track a moving player tightly; out of range they hold their patch instead of marching across the map.
This replaced the old per-unit `ambient` flag.

Out of the search radius, a non-locked `hasTarget` from an earlier tick is DROPPED at once — it is
persistent component state, and kept, the unit marched to the last known spot for up to a retarget
interval and looked as if it ignored the follow radius. Then the "Nav follow radius" band may still
walk an existing crowd lane.

Units splat "Game/Enemies/Steer/Flow splat gain" (0.5) × their measured velocity into that lane per
tick.

## Waves

They-are-Billions style. Wave units stay order-driven until arrival, after which the same gated AI
takes over.

`tickWaves` arms the clock. `queueWave`:

1. Picks a RANDOM compass direction projected onto the barrier square and stepped OUTSIDE it — the
   swarm spawns in the open ring between barrier and ground edge and walks in THROUGH the
   player-only barrier. Cluster points that drift inside push back out along the dominant axis,
   clamped to `c_coopGroundEdge` 196.
2. Sizes the wave in BUDGET POINTS: "Wave budget" plus "Wave budget growth" per wave. Each type
   spends its "Cost <type>" tweak (swarm 1, brute 10), so heavy archetypes field fewer bodies. The
   "Max enemy units" cap converts at the cheapest cost, and an unaffordable roll downgrades to the
   mix's cheapest type.
3. Seeds ONE lane and fires "GWv" (u16 index) at clients.

`tickCoopSpawns` TRICKLES the actual spawns ("Spawns per frame" 24 — wave first, then ambient), so
huge waves enter over seconds rather than one hitch. The trickle is THREADED: the frame's rolls
(budget math plus glm RNG, main-thread — `linearRand` is not thread-safe) collect into
`NpcSystem::spawnLooseUnits`, one `World::spawnBatch` fan-out per frame, with team/order/roster fixup
serial after the join.

### Composition

One ARCHETYPE per wave. `c_waveArchetypes` in Match.cpp holds named recipes of at most 5 weighted
types — pure rushes AND combined-arms mixes — gated by wave index so heavies unlock over time, never
the same recipe twice in a row, with weights jittered ±40 %.

| Type | Prefab | Cost | Enters |
|---|---|---|---|
| Elite | `enemyElite.pre` | 8 | wave 7, "elite guard" |
| Lobber | `enemyLobber.pre` | 12 | wave 8, "lobber barrage" |
| Giant | `enemyGiant.pre` | 25 | wave 9 |
| Spawner | `enemySpawner.pre` | 30 | wave 9 "hive", wave 12 "hive siege" |
| Titan | `enemyTitan.pre` | 60 | wave 11 |

The elite tier (`ENpcType` Elite / Giant / Titan / Lobber) is enemy-only. "endgame" at wave 13 mixes
all four.

* The **Lobber** is `Ranged` with `ShotKind 1` = the slow SPLASH shell `enemyLob.pre`,
  `SplashRadius 5`.
* The **Spawner** is a tough ranged-stance hive: `ShotKind 2` makes each "shot" a LOOSE Swarm body
  born beside it toward its target every `FireInterval` 2.5 s while it holds at `StandoffRange` 18.
  The births are loose units — no wave budget, no roster cap — so the hive itself is the thing to
  kill.

`rollWaveType` samples the rolled mix per spawn, and the wave log names it. Each unit is `orderMove`d
to the Base's near face on the incoming side; the lock releases on arrival, and the AI takes over.

All spawns go through `NpcSystem::spawnLooseUnit` (sourceId 0 — no route, no death accounting). The
authority HUD shows "Next wave (s)".

### Known gaps

* The Base takes damage but the death sweep never destroys it — no lose condition yet.
* The wave clock and pending counts are not saved (F10 re-arms).
* Thousands of dynamic bodies is the AIM, not yet a measured budget. Knobs: Max enemy units, Spawns
  per frame, and box3d "Physics/World/Worker count".

---

# PvP

Place Force emitter pylons plus economy to siege the other team.

## Sync layers

The server runs ALL sim. Sync is three-layered.

### 1 — Player-structure MIRROR over game events

GPl / GRm reliable on change, plus a join replay. The `mirror*` appliers in StructureSystem are
idempotent, notified through its `onStructure*` hooks.

* A completed blueprint RE-SENDS GPl through `onStructureBuilt`, and `mirrorPlace` on a known id
  applies the built flag idempotently. Cable segments sit outside GSt, so this IS their build
  notification.
* Their build PROGRESS mirrors through GCb, sent right after every GSt: blueprint cable and crossing
  segments only, `[u16 count]{u32 id, u8 healthFrac over the segment's own healthMax}`, up to 190
  records with `m_cableSyncCursor` rotating past that. `mirrorCableProgress` writes health only while
  the segment is still a blueprint.
* **There is NO cable wire at all** — links derive locally on both sides. See Economy.

Plus **GSt at 5 Hz**: u8-quantized health / charge / fuel / minerals / outputFrac / flow-utilization /
powered for the first 79 NON-CABLE structures, plus resource totals. Mirrored emitters drive their
LOCAL ForceComponent from the synced outputFrac, so client-side fields are real and the client
player's shield readbacks work.

### 2 — Units and shots as network entities

`Component Network` in those prefabs plus `player.pre`.

### 3 — Client capsule adoption

Each client ADOPTS its server-spawned capsule (`GamePlayer::clientAdopt`, LocalOwner scan) and
owner-simulates through the claim stream, computing shield and health locally.

### The game blob

Replaced the old GSh / GqE shield-mirror events.

Every snapshot or claim record for an entity WITH a GameUnitComponent carries `NetRecFlag_Game` plus
5 bytes (`packGameStateBlob` / `applyGameStateBlob` in NetworkManager.cpp): healthFrac and energyFrac
as u8, emitter output over a fixed 0..8 range, materialsFrac, and flags = `collapsed | team<<4`.

* Server-side it is CHANGE-DETECTED (`ServerState::lastSentGameBlob`) and forces the record out even
  when the pose alone would skip (asleep body, unmoved entity); a collapse edge flushes on the next
  snapshot tick.
* Receivers write the entity's GameUnitComponent and drive its LOCAL ForceComponent output and team,
  so remote bubbles render true drain/collapse state on the right team, and the overhead labels read
  components directly on every instance.
* The entity's OWNER keeps its self-computed shield (`applyShield` off) and takes only
  server-authoritative materials. Claims apply shield-only — never materials or team, which are
  client-forgeable.

### Player capsules are puppets

`Puppet true` in `player.pre`: `update()` is inert, GamePlayer stamps health / energy / collapsed /
team each tick and reads materials back as LocalOwner, and the server writes `m_clientMaterials` into
the twins.

Puppets are excluded from turret targeting and from `NpcSystem` clear/save.

### Client intents

`Gq*` request events — GqP place (cables included), GqD demolish, GqW routes.

* Gated by a `setEventFilter` (only `Gq*` ≤ 64 B from clients, installed by `spawnWorld`; during the
  LOBBY phase the LobbySystem's `Lb*`-only filter stands instead).
* Validated by the same authority seams as local input.
* **The C++ dispatch hook `NetworkManager::setOnGameEvent` is owned by MAIN.CPP**, installed ONCE and
  never replaced: it routes "Lb*" to the LobbySystem and everything else to
  `GameMatch::handleNetEvent`, now public. GameMatch must NOT install its own hook — a lobby client's
  GameMatch is constructed INSIDE a dispatch, and reassigning the `oc::function` there would destroy
  the executing lambda.

`GameMatch::onClientJoined` / `onClientLeft` (registered from main when game + server) spawn and
remove per-client players and replay world state.

Gameplay tweaks sync automatically through the `Core.Tweaks` `Synced` flag — all `Game/*` except
`Game/Camera`.

## Teams

Everything is per-TEAM. `GameMaxTeams` is 8, but the corridor map spawns one Base per PLAYABLE team,
so `GameMatch::PlayableTeams` = 2 is the slot pool: a player without a Base has no respawn anchor,
mineral bank or healing.

* The server is team 0. A joining client gets the LOWEST FREE slot (`allocateClientTeam`; extras
  double up on the last), stamped onto its capsule's puppet GameUnitComponent and emitter at spawn.
* **That component IS the record.** `clientTeam(clientId)` reads it back (−1 = no capsule, so the
  `Gq*` handlers refuse the request), and the OWNER learns its own team from the snapshot game blob
  (`applyTeam` deliberately applies outside the `applyShield` gate, so an owner that computes its own
  shield still receives team and materials; claims never carry either — client-forgeable).
* **NEVER derive a team from the clientId.** Ids are minted monotonically and never recycled, so a
  reconnect or a failed first attempt shifted the second connection onto a base-less team — the bug
  this replaced.
* GamePlayer follows the component on a client (gated on `hasTarget`, so the prefab's authored team
  is never latched) and stamps it everywhere else.

**Per-team specifics.** Players' and projectiles' Force teams are set at spawn/adopt
(`GamePlayer::setTeam`). Structures carry the builder's team — GPl and place requests carry a team
byte, and fields are re-teamed after spawn since prefabs author team 0. Demolish validates same-team.
Minerals and fuel are per-team arrays (GSt sends all 8; the HUD and affordability read own team).

## The arenas

The map is one of the PVP ARENAS (`EPvpMap`). The lobby's "Map" pick calls `setPvpMap` before
`spawnWorld`; the AUTHORITY builds it in `spawnWorld` through `rebuildPvpMap`, and CLIENTS build
whatever the server's GMp event names.

> GMp now carries a mode byte: 0 = the co-op inputs, 1 = the arena index. So a client has NO terrain
> or nodes until its join replay lands, and extractor node indices agree by construction.

All ROCK TERRAIN on the co-op cell machinery — the `CoopMap` grid now carries its own cellsX / cellsZ
/ cellSize / origin, and PvP uses 5 m cells, so `rock.pre` and `terrainmark.pre` spawn at HALF scale
(`c_pvpCellSize`) — with a 10 m rock BORDER ring (`c_pvpBorderCells` 2). The old `borderwall.pre`
fence and `spawnCorridorWalls` are GONE; `borderwall.pre` is now unused.

`generatePvpGrid`:

| Arena | Layout |
|---|---|
| Lane | The original corridor (x ±65, z ±20) |
| Wide lane | z ±40 |
| Chokepoints | The wide lane plus a 10 m middle wall at \|x\| < 5, pierced by three 10 m openings at z 0 and ±25 |
| Circle | A 65 m open disc around a 28 m rock column; Bases mid-ring at radius 47 evenly around the circle, team 0 west |

`baseGroundPos` is per arena; the lane arenas spread N Bases evenly along x between ±55. Every Base
cell gets a rock-free 8 m disc (a middle team on the Chokepoints wall line carves its own gap).

Then the shared `finishGrid` flood-fills from team 0's Base, sealing cut-off pockets, and merges the
rects — Nav obstacles plus `setTerrainBlocked` plus placement bounds = the arena interior,
`m_pvpInterior`.

**Nodes per arena** (`spawnPvpNodes`; a node whose cell is rock is skipped): the lane's
180°-symmetric 8-node table, plus an outer 6-node band on the wide arenas, and the Circle's 12 nodes
on two rings between the Bases.

A PvP F9 save carries `PvpMap`; `loadGame` regenerates a different arena (clearing structures first)
like the co-op seed.

Everything spawns deterministically on every instance with a Base at each end — team 0 at (−55,0,0),
team 1 at (55,0,0). Clients spawn and respawn beside theirs; both are damageable but never destroyed.
`setPlacementBounds` keeps footprints inside the walls.

## Structure damage

* Every placeable prefab has `ContactEvents true`, and `placeStructure` installs an `onContact` hook:
  enemy-TEAM "Projectile" contacts chip "Projectile structure damage" health.
* `tickDamage`'s rule: hostile = ANY other team's bubble owns the structure's query point. **Push
  your field over their buildings to siege.**

**Known gaps:** player turrets target only units, not players; GSt caps at 79 structures.

## Save / load

F9 save, F10 load; server and single-player only, since clients refuse.

A full sim snapshot to `Assets/Local/gamesave.txt` through AssetParser
(`StructureSystem::saveTo` / `loadFrom` / `clearAllStructures`, `NpcSystem::saveUnits` / `loadUnits`,
`GameMatch::saveGame` / `loadGame`): structures with id / type / pos / facing / node / team /
blueprint / stores / route — cable segments are ordinary Structure entries, with NO Cable nodes.
Players and shots are NOT saved.

**Save compat.** `Type` is the raw enum INT, so `EStructureType` values are never removed or
reordered: new types APPEND before Count, and the retired Connector (index 2) keeps its dead slot
(`loadFrom` skips it with a warning; old saves' Cable nodes are silently ignored, and links
re-derive).

`loadFrom` clears first (removal hooks → GRm prune connected clients) preserving ids, then the server
re-broadcasts everything (GPl / GRt). Unit entities resync through normal network despawn and spawn.

---

# Economy

A node produces NOTHING until an extractor is built on it — one per node; the ghost snaps to the
nearest free node within "Extractor snap radius", validated again at place time — AND that extractor
is fed energy.

## Energy is a flow network (`tickPower`)

EVERY structure holds LOCAL charge:

| Structure | Buffer |
|---|---|
| Emitter, extractor | "Internal buffer" 10 |
| Generators | "Generator buffer" 20 |
| Battery | "Battery capacity" 100 |
| Barracks | "Barracks energy capacity" 20 |

Units are PAID from the barracks buffer ("<Unit> spawn energy" per type, stamped as the component's
`spawnCost`), so a barracks needs a power cable and holds no minerals.

Each tick every derived link moves energy GRAVITY-FED toward consumers, capped by the MEDIUM's
throughput ("Cable throughput" 5/s — tiers are gone; `m_cableThroughput[3]` by medium). Three
potential bands:

1. **Producers** (generator, solar) on top, always exporting.
2. **Battery storage** in the middle, BALANCING by fill fraction among themselves.
3. **Consumers** below, outranking everything until FULL.

* Cross-band links run at full throughput downhill: consumers fill first, and the surplus banks in
  batteries.
* Same-band links move a DAMPED fraction (`c_equalizeDamping` 0.25) of the exact equalizing transfer.
  Each source sizes its share without seeing the others, so undamped, N sources feeding one receiver
  overshoot the balance point together and bounce back next tick — wasted throughput and a flow
  direction that keeps reversing. Damping stays stable up to 1/damping simultaneous sources, and
  removed the need for any direction hysteresis in the draw.

Energy still propagates hop by hop, thin lines starve, and a full generator buffer throttles
production (export-limited = no fuel burn). Consumers drain their internal battery; empty =
unpowered.

## Fuel is a flow network too

The SAME gravity-fed bands: extractor tanks export, fuel tanks balance, and generator/fabricator
burners fill first.

Powered fuel extractors fill their OWN small tank (full = production stalls). PIPELINE runs
("Pipeline throughput") move it. Generators and fabricators burn from their OWN tank; fabricators
need energy AND fuel.

## Minerals are the third flow network

Same band model, four levels:

```
extractor / fabricator outputs (3) → the Base (2) → silos (1) → constructors (0)
```

The Base sits between producers and silos on purpose, so extractors still dump into it at full rate
while its own trickle PREFERS flowing out over banking, keeping only what receivers cannot take. This
replaced the old mineral-sink fiction.

Powered mineral extractors and fabricators fill their OWN buffer (full = stalls). CONVEYOR runs
("Conveyor throughput") move mineral stores. Only what sits in Mineral silos and the Base
("Mineral silo/base capacity" stores) is SPENDABLE — `m_minerals` is a per-team cache recomputed per
tick, and spending drains silos first, Base last.

## Physical cables

The link tools, point-to-point cables and Connector range links are all REMOVED.

Cables are 1-cell GRID STRUCTURES: `EStructureType::CablePower` / `CablePipe` / `CableConveyor` — one
per medium, no tiers — plus the 1×3 oriented **Crossing** bridge.

A perpendicular cable passes UNDER the Crossing's middle cell. It conducts whichever ONE medium its
two END cells resolve to: each end accepts from THREE sides (outward plus the two laterals; only the
middle is pass-through-only), and a run on one end plus a capacity-holding building on the other also
counts.

### Links are derived, never authored

`StructureSystem::rebuildDerivedLinks` — dirty-gated, main thread, in `tickAuthority` after the
request drain and in `tickMirror`. CLIENTS derive locally from the mirrored segments, with no cable
wire.

1. Union-find the BUILT segments over 4-neighbour equal-medium adjacency. Blueprint segments break the
   path. The under-cable participates through `CellEntry::underId`, which is also what keeps a
   crossing from unioning with the cable below it.
2. Attach every adjacent building with capacity > 0 in the run's medium (blueprint buildings
   pre-wire).
3. BRIDGE through built buildings — a building conducts every medium it holds, so two same-medium
   runs touching it merge into one. Blueprint buildings do not bridge, and buildings NEVER connect by
   direct adjacency: a link always needs cable between them.
4. Diff desired-vs-live links: all attached pairs per run (a clique up to `c_runCliqueCap` 32
   buildings = 496 links). Past that, a STAR from a ROLE-picked hub — storage band 1 first (it relays
   both directions, the old Connector's role), else a producer, lowest id as tie-break. **NEVER a
   plain consumer when better exists**, since a band-0 hub cannot send energy UP to a battery, which
   starved batteries on big runs.
5. Owner = the lower structureId, so rebuilds never flip flow state.

### The cell hash

`m_cells`: cellKey → {id, underId}, maintained at the spawn and remove seams (`insertCells` /
`eraseCells` — a crossing demotes its under-cable to `underId`, and removal promotes it back).

It backs `cellsFree` (per-cell now: buildings refuse ANY occupied cell — no building on a cable; a
cable may slot under a free crossing middle; a crossing may bridge exactly one cable; `ignoreCables`
is the unit-spawn probe, since cables are walk-through), the derivation, and the arm visuals.

A pair of buildings may still hold ONE LINK PER MEDIUM — a power run and a pipe run between the same
two derive side by side.

### Cable draw

The segments are real meshes with per-medium authored tints: power yellow, pipe orange, conveyor
blue; blueprint gray rides `applyStructureTint`.

`drawDebug` only adds the FLOW pulse: per conducting run, a pulsing ring over every segment, with
brightness and speed from the busiest attached building's `flowUtil` gauge — which the server
computes and clients receive through GSt, so the reading is the same everywhere. The run table
`m_runs` stores structure IDs, refreshed by each rebuild.

### Cable lifecycle

Segments are full structures:

* A blueprint phase, NON-conductive until built.
* Health ("Cable health max" 40, softer than buildings). **`healthMax` is per-COMPONENT now** —
  `investMaterials` / `fundNearbyBlueprint` heal against `state->healthMax`, never the global.
* Territory drain, and projectile damage (`Layer Cable, CollidesWith Projectile` in the cable
  `.pre`s, plus `Layer Projectile` on `projectile.pre` / `enemyShot.pre` — the first named physics
  layers in content; everyone else's Default/All masks are untouched, so players and units walk
  straight through).
* Death sweep destruction → the run re-derives broken.

Cables are SKIPPED by: GSt (the 79 cap), `feedNav` (no NavObstacle, no NavSource),
`actorInFootprint`, the RMB building-face move order and route-click checks (treated as ground), and
overhead labels except blueprint or damaged on the authority (cable health is not mirrored).

## Surface tension

Pushing into bubbles scales push AND energy costs by `(1 + tension × pressure)` on BOTH sides:
"Game/Shield/Surface tension", "Game/Enemies/Push tension", "Game/Structures/Pressure draw tension".

Player and unit `Accel` defaults are halved, so steering loses the shoving match.

## Construction

Placing is FREE. Structures spawn as BLUEPRINTS (`Structure::blueprint`, gray tint through
`applyStructureTint`), inert in every tick: no power, fields, income or flows; links pre-wire but
carry nothing.

**HEALTH IS THE PROGRESS.** A ghost spawns at 1 hp; materials HEAL it at cost/healthMax per hp
(`investMaterials`); full health = built. The health bar is blue while blueprint, and damage
literally undoes construction. The SAME price repairs damaged built structures.

**Cheat:** "Game/Cheats/Free instant build" (a Synced checkbox — the server's value rules all players)
skips the blueprint phase: placements spawn built at full health, with no materials.

### Where materials come from

**(a) Players.** A carried "Materials" inventory (HUD counter; "Game/Player/Materials max";
server-authoritative for ALL players — clients receive theirs through the snapshot game blob's
materials byte).

It refills near own-team Silos, Base or mineral Extractors (`takeStoredMinerals` — hand-couriering a
fresh dig site's buffer works before any conveyor exists), and is invested into nearby BLUEPRINTS
only (`fundNearbyBlueprint`, "Game/Construction" tweaks). Dropped on death.

**(b) The CONSTRUCTOR structure.** Production hotbar; conveyor-fed mineral stock plus powered; amber
range ring in `drawDebug`. It funds the nearest own-team blueprint OR damaged structure within
"Constructor range" (`fundNearbyBlueprint(..., includeRepairs)`). With NOTHING to build or repair it
idles — the old barracks spawn-clock boost is gone.

Blueprint state mirrors through a GPl built flag plus a GSt status-bits byte (bit0 powered, bit1
blueprint; caps GSt 79).

**NPC emitter drain** (`addEmitterLoad`) is SHIELD-INDEPENDENT: collapsed units strain emitters like
shielded ones.

## Grid placement

Every placement — extractors included, since the node's position snaps too — aligns to a 2 m world
grid (`StructureSystem::snapToGrid`; odd footprints center on a cell, even on a corner) and occupies
`footprintExtent(type, rot)` cells.

| Footprint | Types |
|---|---|
| 1×1 (default, cables included) | most |
| 2×2 | FuelTank, Bastion, Turret, Solar, Fabricator, MineralSilo |
| 3×3 | Barracks, Base |
| 3×1 / 1×3 | **Crossing** — the ONE non-square footprint, by its facing |

The Crossing's facing is quantized to ±X/±Z at `placeStructure` like the Lance, and carried on GPl and
in the save's `Facing` — otherwise client derivation diverges on which cells it covers. Prefab
`Scale` / `HalfExtents` = footprint half, so boxes fill their cells.

**Heights** vary through the baseshapes height variants. `CubeHalf` / `CubeQuarter` / `Pillar2` /
`Pillar3` are the 2 m cube with 0.5× / 0.25× / 2× / 3× height, origin centered:

* Wall and Mineral silo = `Pillar2`; Battery = `CubeHalf`; Solar = `CubeQuarter`.
* Cables are low `Cylinder2` hubs (baseshapes' 2-high center-origin cylinder) with 4 render-only ARM
  child entities (`ArmPX` / `NX` / `PZ` / `NZ`, `Enabled false` in the `.pre`, cached on `Ref::arms`
  at spawn, toggled by `updateArms` toward same-medium neighbours, crossings and capacity-holding
  buildings).
* The border wall renders a LOW `CubeHalf` but keeps a tall 10×20×10 collider — a deliberate
  mismatch: an invisible fence above the visible wall.
* `structureSpawnHeights` = each prefab's box HALF height, so everything sits flush.

**Refusals.** `cellsFree` refuses overlaps at aim (red ghost) AND at `placeStructure` (the MP seam).
FREE resource nodes reserve their extractor's future footprint, so buildings cannot block extractor
placement; extracted nodes rely on the standing extractor's own cells.

`StructureSystem::actorInFootprint` (a spatial query for GameUnitComponents — units AND player
capsules) refuses a placement someone is STANDING in, checked at both seams as well. It is
deliberately NOT folded into `cellsFree`, which doubles as the unit-spawn-point probe and must not
refuse a cell just because units stand near it.

**The ghost is the real box.** `drawStructureGhost` wireframes the exact footprint square × the
prefab's height (`spawnHeightOf` = box half height) plus the interior cell lines, green when clear and
red when refused. Every whitebox building IS a box, so the preview is exact; the Wall line and Lance
anchor draw the same ghost per segment.

---

# Hotbar and interaction modes

## The RTS grid hotbar

12 slots on the GRID HOTKEYS QWER / ASDF / ZXCV (`c_gridKeys`), always visible in game mode. Every
slot is also CLICKABLE: `slotAtScreenPos` → `activateSlot`, the ONE entry point for key and click.
Clicks over the hotbar never reach the world.

Every slot caption is a 3–5 char SHORTHAND from `c_structureShortNames`, indexed by `EStructureType`
— the SAME table the world tag over a building uses, so a slot and the thing it builds read
identically.

**The ROOT page** (Select/Delete modes) holds the three categories on Q/W/E (CMBT / PROD / CBLE) and
DEL on X. X is Delete on EVERY page, and category pages cap items at 9 so X stays Delete.

**A CATEGORY page** holds its items in slot order (max 10), plus CNCL on C and BACK on V.

**C / Esc / Tab** = `cancelOneLevel`: a two-click step drops → the armed item disarms → the page or
Delete mode returns to Select, one per press. **V** = straight back to Select.

## Q — Combat (`c_combatItems`)

* **Emitter.**
* **Bastion** — big anchor, hungry.
* **Lance** — focused cone. TWO-CLICK placement: the first click anchors, the second aims the facing;
  the fallback auto-faces away from the Base.
* **Wall** — a DRAG line of square segments every 2 m: the press anchors, the release places.
  Per-segment cost, `c_wallMaxSegments` 16. A BUILT wall is fed to Nav as a BREACHABLE obstacle at
  "Wall breach cost" 10 — one 2 m cell costs 22 m of walking, so enemies route through a wall rather
  than take a longer detour, walk into it and bite it down. Blueprint walls and every other building
  stay impassable.
* **Turret** — HITSCAN lightning. The component picks the nearest enemy unit in "Turret range", pays
  "Turret shot energy" from its internal store and lands "Turret damage" 25 on the spot; it never
  misses. The strike is a BUNDLE of jagged debug lines (6 tight core strands plus 4 wide forks,
  re-jittered per frame) fading over "Turret beam lifetime" 0.5 s (`NpcSystem::addBeam` /
  `drawBeams`), broadcast to clients as GLt.
* **Barracks** — ONE type, 3×3, cable-fed. The old Brute/Runner/Spitter variants are RETIRED enum
  slots; `loadFrom` maps them to a Barracks with that unit type. See below.
* **House** — 2×2, no grid role. Every built house links to the nearest built own-team barracks within
  "House link radius" 25 (one barracks per house), re-derived every `refresh()` on every instance by
  `linkHouses` (`Ref::linkedId`, `barracks.houses`). Drawn as a line to its barracks plus the radius
  ring while selected.

### Barracks detail

Selecting an OWN barracks shows a UNIT-TYPE PICKER popup above it (`HudPopup` in `Core.GameHud`,
drawn by GameHudOverlay, clicks resolved in `updateWindowed` through `popupButtonAtScreenPos` before
the hotbar).

The pick is an ORDER like the route: `queueUnitTypeRequest` → validated in `tickAuthority` →
`onUnitTypeChanged` → GBu broadcast plus join replay. Clients request through GqU; saved as
`UnitType`.

Each spawn pays the type's "\<Type\> spawn energy" from the energy store, and takes that cost ×
"Barracks seconds per energy" 0.5 to produce — **the price IS the build time.**

It holds the type's "\<Type\> population" (Grunt 2 / Brute 5 / Runner 2 / Swarm 1) against the
barracks' POPULATION CAP = "Barracks population" 20 plus "House population" 10 per linked house. A
spawn only happens while population + cost ≤ cap (`BarracksData` population / popCap / spawnPop,
tallied by the spawn and death edges — the unit's `popCost` rides its DeathRecord). The population
rides GSt in the barracks' otherwise-unused output byte.

The Spitter is NOT a barracks option: `isBarracksUnitType` refuses it everywhere and the popup lists
`c_barracksMenu` only. An old Spitter-barracks save loads as a Grunt barracks.

Spawns land at a free cell around the building (`freeSpawnPointAround`).

### Barracks routes

In Select mode with an own-team barracks selected, RIGHT-click on ground sets a waypoint; SHIFT+RMB
appends, max `MaxRouteWaypoints` 6 — bounded by the 64 B GqW request.

Spawned units march the waypoints, advancing on touching each "Waypoint radius" circle, with no
combat mid-march, before their AI takes over.

Server-validated (`queueRouteRequest`: own-team barracks only), mirrored through GRt plus the join
replay, and drawn as a green line chain with circles for the own team.

## W — Production

Everything that is not a weapon: generator, solar (free trickle, no fuel; a flat `CubeQuarter` slab),
extractor, fabricator (energy + fuel → minerals), constructor, battery, fuel tank, and mineral silo
(the team's spendable bank).

## E — Cables (`c_cableItems`)

Power cable, pipeline, conveyor, crossing.

Cable segments place by PAINTING only (`updateCablePlacement`): an LMB press places its cell, holding
and dragging keeps placing the cells the cursor crosses (L-filled between cursor samples through
`placeCableLine` — dominant leg first, and occupied cells skip so a stroke across an existing run
fills gaps, so the run never breaks), and the release ends the stroke. The old two-click L-line is
gone.

The Crossing is single-click: its long axis follows the CAMERA facing quantized to ±X/±Z, and
re-pressing or clicking the armed CRSS slot rotates it 90° (`m_crossingRotated`). Once conducting it
TINTS to its medium's hue (`Ref::conductMedium`, stamped by `rebuildDerivedLinks` →
`applyStructureTint`, which also tints cable and crossing CHILD pieces — that is what turns arms and
ends blueprint-gray and back).

One placement burst caps at `c_cableMaxSegments` 32 GqP events, inside the wall pattern.

## The Bases

They exist from game start only (`spawnBase`; `isPlaceableType` guards Base and the retired Connector
off placement), one per team at the corridor ends.

* Respawn anchor.
* A passive trickle of Minerals at "Base income mult" (0.25) of an extractor.
* MINERAL + ENERGY storage (no fuel): "Base energy capacity" 100, spawned full, self-generating
  "Base energy gen/s" 2 solar-style; power cables attach.
* Its SHIELD runs on EMITTER RULES — `hasShieldEmitter` includes Base in
  `tickPower` / `strainable` / mirror / save: "Base shield energy/s" draw plus a pressure surcharge
  plus unit siege drain, latching dark when starved, with output and reach from the "Base shield"
  tweaks. `base.pre`'s authored Output no longer stands.
* Conveyor- and power-linkable.
* DAMAGEABLE (health bar plus territory / melee / projectile damage) but the death sweep never
  destroys it: at 0 hp it stands dead until repaired. Constructors
  (`fundNearbyBlueprint includeRepairs`) heal it at its `m_costs` entry — 100; never placed, so that
  entry only prices repairs, and `investMaterials` admits Base alongside placeables.

> A destroyed Base would soft-lock the respawn.

## Structure health and labels

Every structure has a `ForceQuery`; territory owned by ANY other team's bubble drains health.

Emitters shrink out over "Emitter shrink time" when starved and latch off until
"Emitter restart charge". They pay a pressure surcharge ("Emitter energy/s @ pressure 1") plus a
per-unit siege drain (`addEmitterLoad`: a flat rate onto the NEAREST ACTIVE bubble; spitter shots
deposit too).

**World labels.** A health bar over every damageable structure, plus a second bar for storage (energy
yellow, fuel orange). FULL health and shield bars stay hidden — only damage, blueprint progress,
selection, or a prefab's `AlwaysDisplayHealth true` shows one. Undamaged non-player units skip their
label entirely; `player.pre` opts in so player tags persist.

## Unit selection

Select mode, authority only — units simulate on the server.

LMB DRAG (> 8 px) draws a box on the ground, and on release selects every visible own-team unit whose
screen position is inside (`updateUnitSelection`, `NpcSystem::queryVisibleUnits` plus
`worldToScreen`). SHIFT adds; a plain click clears. `m_selectedUnits` are owning EntityPtrs pruned on
death, drawn with a green ring.

EVERY RMB MOVE ORDER — ground, held re-aim, or building face — also goes to the selected units:
`GameUnitComponent::orderMove` = a LOCKED target with `moveOrder` set (dropping the route),
auto-cleared within `waypointRadius` so the AI resumes. A DSL `setTarget` lock never clears.

A FRESH order (the RMB press) can make the unit IGNORE the lane term for "Order flow blind" seconds,
so an old trail cannot pull it back the way it came. It is 0 by default now that a fresh order SEEDS
its own lane. (`FlowField::clearArea` exists but is unused.)

## Interaction modes (`GameMatch::EPlayerMode` — Build / Delete / Select)

**Neutral / default = SELECT.** Click to inspect (highlight ring plus overhead info); RIGHT-click
GROUND with an own barracks selected = a waypoint route; RIGHT-click a building = a MOVE ORDER to it.

**Q/W/E** (root page) = Build with that category. The hotbar page doubles as the mode indicator, and
V / Esc / C / Tab return to Select.

**Build mode ALSO click-selects** (`updateSelectionClick`, shared with Select mode): with nothing
armed every LMB inspects, and with a ghost armed, a click the placement refuses (occupied cells, i.e.
on a building) selects it instead of doing nothing — while any click that CAN place still places.

### RMB always moves the player

Cancelling rides along on the SAME press:

1. Whatever step is pending is cancelled, one level at a time. A half-finished two-click flow (Lance
   aim, Wall line, cable paint stroke, Crossing aim) drops first; the next RMB disarms the item
   (`disarmBuild`, the same end state as the C slot).
2. With an own barracks selected and the cursor on GROUND, the press is instead a route waypoint
   (SHIFT appends). This is the ONLY consumer that sets `m_rmbConsumed`, because a rally point is a
   positive order, not a cancel, and pairing it with a move would drag the player to every waypoint.
3. Everything else becomes a MOVE ORDER (`GamePlayer::setMoveTarget`, green destination ring). On
   GROUND the capsule walks to the clicked point, and HOLDING RMB keeps re-aiming at the cursor. On a
   BUILDING the destination is the CLICKED ground point, pushed out to the nearest face of the
   inflated footprint (half footprint + 1.2 m) when it fell inside — so you walk to the side you
   clicked instead of grinding into the wall. One-shot; it does not drag.

Cancels deliberately do NOT consume: a cancel that also ate the movement read as a dropped input.

A respawn drops a standing order. WASD movement no longer exists — the letter keys are grid hotkeys.
Because RMB-hold now steers, the camera yaw drag moved to MIDDLE-mouse (Q/E unchanged).

**X** (any page) = Delete: click a structure to demolish, no refund, Base refused.
**C / Esc / Tab** = one level back, or return to Select.

GqS / GqM requests and ALL of GamePlayer's combat code (`tickCombat` / `queueShot` / `queueMelee` /
`trackProjectile`, the `m_projectiles` list, the "Game/Combat" tweaks) are deleted.

---

# Units

`Game:Npc`, barracks-produced only.

## The sim

`GameUnitComponent` in the entity pass. Steering is by the Nav flow fields — see Nav: nearest enemy
by walking distance, routing around walls, own-team density spread.

`GameMatch::feedNav` supplies:

* **obstacles** = the border ring plus structure footprints
* **per-team sources** = live non-invulnerable structures, player capsules, and live units (kind 3,
  from NpcSystem's unit roster — no sweep)

**UNIT-VS-UNIT COMBAT:** non-ranged units melee ONE enemy unit — the nearest inside
`attackRange + victim.bodyRadius` — for `attackDps`, holding at that ring. Players in the swarm still
take the area damage from every adjacent unit.

**The pre-Nav local search is the fallback** where no field covers a unit: stuck-sidestep;
auto-targeting = a random pick among the 4 nearest enemy structures through a spatial query with a
nearest-enemy-player fallback, re-rolled on a jittered "Retarget interval"; melee gnaw against the
nearest enemy structure's `meleeRadius` ring; shield battery on player rules minus regen, with
permanent collapse; exposure damage; field push; the ranged spitter stance raising `wantsFire`; and
death / void self-despawn.

## `NpcSystem`

PRODUCTION plus SERVICING, plus the OWNING unit and projectile rosters (see the roster and
`setOnRootEntityRemoved` scheme in Entity). It holds no other unit state.

A unit REPORTS what the game needs through `GameUnitComponent`'s static event queues — FireRequests
for spitter shots, and its own death carrying `sourceId` — and `service(structures)` drains them on
the main thread.

Shield, health and team state need NO publish step: they live on the component and ride the entity
snapshot's game blob.

**BARRACKS and TURRETS are per-entity too.** `GameStructureComponent::update`'s machine block
(discriminated by `machineKind`, stamped with the union variant) runs the spawn clock or target query
ON the structure, pays minerals and energy from its own stores, claims the roster slot, and queues a
SpawnRequest or TurretFireRequest. `service()` only performs the main-thread entity spawns, refunding
a failed one.

Production tuning lives on `GameStructureComponent::params` plus the per-type `barracks.spawnCost`
stamp, registered from StructureSystem with tweak names unchanged. NpcSystem keeps only the shot
speeds.

**Roster counts are spawn/death EDGES** — `++aliveUnits` at the decision, `--` on the death event;
re-seeded by `loadUnits`.

The overhead labels use `queryVisibleUnits` (a camera FRUSTUM query — an off-screen unit would only
fail `worldToScreen` anyway; puppets label as players, and the own player is skipped). Save/load, the
scenario select-all and the route re-push all walk the roster (`queryAllUnits` is a roster copy, not a
query).

`tickBarracks` spawns the variant's unit prefab. Per-type stats are AUTHORED in the `.pre` GameUnit
block: Grunt baseline, Brute 180 hp slow, Runner 30 hp fast, Spitter ranged. The barracks route is
copied onto the unit at spawn AND re-pushed to its live units when the route changes; the march index
is kept and clamped, so appended routes continue and finished units march to the new tail.

`tickTurrets` fires team-tagged `projectile.pre` at the nearest enemy-team unit. Projectiles are
`GameProjectileComponent` (lifetime, field deflection, emitter sap, enemy-team contact damage — routed
from `World::handleContactEvent`, and the first touch spends).

## No published player list

Units find enemy players — puppet GameUnitComponents, spatially registered like everything else — in
the SAME queries as structures.

* The auto-target sweep ("Target search radius", default 50: LOCAL harassment, since the barracks
  route does the long-distance delivery) takes the nearest enemy puppet as a fallback.
* The short melee probe chews any puppet in reach.
* The server stamps team onto client twins' puppets in the materials loop; claims never apply team —
  client-forgeable.

## Damage

**PLAYER DAMAGE IS UNIFIED under `GameUnitComponent::damage()`** — the ONE entry point for every
victim.

* On a unit it CAS-subtracts health.
* On a PUPPET it atomically banks into `pendingDamage` — health is owner-computed, so a direct write
  would be stomped.

Unit melee and enemy projectile contacts both just call `damage()` on whatever they hit; projectiles
now hurt players through the same call, closing an old known gap.

The inbox drains main-thread through `takePendingDamage()`: `GamePlayer::tickShieldAndHealth` drains
its OWN capsule, and the server's GDm flush (0.1 s) drains the client twins' inboxes directly. The
component accumulates between flushes, so there is no clientId-keyed damage map.

Carried MATERIALS live the same way: the twin's `materialsFrac` IS the server-authoritative store —
no `m_clientMaterials` map, and the state dies with the capsule. `m_clientPlayers` is the only
per-client container left.

`GamePlayer::applyDamage` ABSORBS into the shield battery first
("Game/Shield/Damage absorb (energy per hp)"); overflow, or a collapsed hit, reaches health. It
respects spawn grace.

## Far tick

`NpcSystem::service`, authority only. "Game/Sim LOD/Far tick interval (s)" 0.5, readout "Far ticked".

Units the SIM LOD did not select — no tier stamp on their spatial entry (`getPassMaskExact`), i.e.
beyond the outer radius of every player, body disabled, never visited by the entity pass — still walk
their ROUTE or MOVE ORDER through `GameUnitComponent::updateFar`, a parallelFor over the roster every
interval of sim time.

* Straight at the target where the raster shows a clear line, else along the enemy team field's
  descent (geodesic, around rocks).
* By TELEPORT: the entity pos is the far truth; the body pose and prev/curr are stomped, and the
  spatial entry is refreshed so the selection query finds it as it approaches.
* Waypoints advance and the order clears with the full sim's radius rule. A step into rock holds.
* No combat, no bubble, no strain, and no health death check while far — all resume when a player
  comes within the outer radius. The ONE exception: a unit below `voidY` −3, i.e. fallen through the
  floor, is killed by the far tick too, through `GameUnitComponent::kill`, so no unit escapes it.

### Loose units spawn parked

`spawnLooseUnits` calls `PhysicsComponent::park(true)` plus `ForceComponent::setActive(false)` right
after the batch. The body command applies before the next step, so the body never simulates one, and
the bubble stays dark until a tiered visit.

> Wave blobs overlap, and a live unselected body took box3d's push-out and — frictionless and never
> steered — coasted away: the "flung" waves.

The World's wake edge (a fresh entity starts at `schedTier` 3, so its first stamped visit IS a wake)
zeroes the velocities and enables it once a player is near. Wave spawn points also roll up to 6 times
against the last 32 wave spawns (`m_waveRecent`, 2 m spacing), so parked bodies rarely overlap in the
first place.

This is what makes a WAVE — spawned outside the barrier, far from everyone — reach the Base. A unit
that ARRIVES while no player is near stands frozen there; structures are not focus points (known).

## Unit capsules have `Friction 0`

Every `enemy*.pre`; the player keeps 0.3.

The steering SETS the body velocity each tick, and the SIM LOD ticks tier-1/2 units only every 0.25 /
1 s, so ground friction (unit 0.3 × ground 0.6, about 4 m/s² of deceleration) bled the commanded speed
away between ticks — tier-2 units stopped dead inside their interval. **Velocity-driven bodies need
no friction; do not put it back.**

The flip side: STOPPING is an explicit command too. `GameUnitComponent::update` BRAKES — planar
velocity to zero at the steering accel — whenever it has no walk target or has arrived. Without it a
coasting unit never stopped, kept splatting its velocity into the crowd lane, and the pack followed
the ghost trail. There is no lane splat while braking.

---

# Player

`Entities/Game/player.pre` = playerCapsule plus a `Component Force` shield.

* **MOUSE-ONLY movement** — RTS right-click move orders (see the RMB chain above:
  `setMoveTarget` / `hasMoveTarget`, "Game/Player/Move arrive radius") on the testbed's
  velocity-steering model. LShift sprint, Space jump. NO WASD — those keys are grid hotkeys, and
  `tickMovement` ignores its camera-forward argument now.
* **NO player combat** — the combat code is fully deleted.
* **Shield** — output shrinks under `getPressure()` and regens; `getAppliedForce()` pushes the
  capsule.
* **Health** — drains only when the shield is COLLAPSED and the query says enemy-owned. It
  REGENERATES at "Game/Player/Base heal/s" within "Base heal radius" of an own-team Base
  (`GameMatch::tickBaseHealing`, own player only — health is owner-computed, so every instance heals
  its own capsule against its local structure mirror, with no sync).
* **Death** teleport-respawns, per the teleport contract (prev/curr pose stomp).
* All tunables under "Game/*" tweaks. HUD through `Globals::gameHud`: bars Health / Shield /
  Materials, counters Minerals / Fuel / Power, and hotbar slot counts = affordable.
