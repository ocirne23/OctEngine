# Game

> Library documentation for `Code/Game`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction.
>
> The game COMPONENTS (`GameUnitComponent` / `GameStructureComponent` / `GameProjectileComponent`)
> live in Entity — see [`Docs/Context/Entity/CONTEXT.md`](../Entity/CONTEXT.md). Unit steering over the flow
> fields is in [`Docs/Context/Nav/CONTEXT.md`](../Nav/CONTEXT.md).

A static lib linked by App; links Entity, Force, Physics and Input. Two modes on one codebase: a
top-down tactical **PvP** whitebox, and a **co-op PvE** survival mode.

```
App.exe --game                     single player sandbox
App.exe --game --server            WINDOWED listen server  (--headless refuses --game: GPU field
App.exe --game --connect <ip>      client                   readbacks drive the authority sim)
App.exe --game --coop [...]        PvE; clients pass --coop too — the world layout is built locally
```

Without `--game` the testbed is untouched.

## The documentation files

This file holds the architecture, the frame, the world and save / load. The rest of the Game
documentation lives in topic files in this folder — read the ones for the area you touch.

| File | Covers |
|---|---|
| [Modes.md](Modes.md) | Game modes: co-op PvE (the map, teams, the Swarm unit, ambient scatter, waves, the spawn blob, unit types and archetypes, targeting range gates) and PvP (the arenas, teams, structure damage) |
| [Sync.md](Sync.md) | Multiplayer sync: the three layers (structure mirror, network entities, capsule adoption), the game blob, puppets, client intents, settings, shadow preset, scene focus, detach camera |
| [Economy.md](Economy.md) | Economy: the cable transport, physical cables and crossings, derived networks, the cell hash, construction, grid placement |
| [Hotbar.md](Hotbar.md) | Hotbar and interaction modes: the grid hotbar and hover cards, Combat (turret, barracks, routes), Production, cable painting, the Bases, structure health and labels, unit selection, RMB move orders |
| [Units.md](Units.md) | Units and the player: the unit sim / NpcSystem split, targets, melee, damage, the far tick, parked loose units, friction; the player shield, problem bubbles, world labels, HUD |

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
`Game:Npc`, `Game:GameCamera`, `Game:Lobby` (the pre-game LOBBY model, `LobbySystem` — multiplayer, or LOCAL: offline, one player, Start launches at once —
ready checks, the start countdown, the host's team/map picks; the `Lb*` events) and `Game:Chat` (the
TEXT CHAT model, `ChatSystem` — the "ChM" event's log). **Both are plain stack locals in `main()`**,
serviced by main's menu loop and drawn by the UI's lobby page / `UI:ChatPanel`: see **Lobby** and
**Chat** in [`Docs/Context/App/CONTEXT.md`](../App/CONTEXT.md) for the flow; barrel `Public/Game.ixx`.

**`StructureSystem`'s bodies are split over six implementation units by topic** (all
`module Game;`, the list is repeated at the end of `Structures.ixx`): `Structures.cpp` (the
type tables behind `structureTypeName` / `spawnHeightOf`, `describeType`, the settings listeners,
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
| `updatePlayer(dt)` | After `networkManager.receive`, **PRE-PHYSICS** ([main.cpp:747](../../../Code/App/main.cpp#L747) region) | **The PLAYER/CAMERA hot path and nothing else**: capsule adoption on a client, velocity steering, and the shield's body push — the direct body setters that must land BEFORE this frame's physics step. Deliberately minimal so main reaches the spatial and begin-frame kicks as early as possible. |
| `update(dt)` | AFTER the spatial + begin-frame joins ([main.cpp:791](../../../Code/App/main.cpp#L791)) | The whole rest of the tick: structures authority/mirror, materials, unit production, base healing, nav staging, net flushes. **Spawns, destroys and spatial queries are legal again here.** Becomes the server tick in MP. |
| `updateWindowed(camera, dt)` | Right after `controls.applyPlayerCamera` | Camera overwrite, aim and placement input (`updateGameInput`), ghost + debug draw, HUD. **SKIPPED while the main menu or lobby is active**, so a lobby client's constructed GameMatch simulates but gets no input, camera or HUD. With the "Detach camera" tweak on, the camera overwrite and the input half are skipped (see the Multiplayer section ([Sync.md](Sync.md))). |

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
tints), plus generated rock terrain and per-mode resource nodes. **Every enemy unit's body is the
shared box-limb model `Debug/CubeGuy.pre`** as a Scene child — size by the reference-site `Scale`,
colour by `Component GameUnit` `Color`; the GameUnit turns it to the heading and feeds its walk (The
MODEL, in [`Docs/Context/Entity/CONTEXT.md`](../Entity/CONTEXT.md)). `player.pre` keeps its own shape.

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
marching on the Base instead of parking (see **Targeting range gates** ([Modes.md](Modes.md)): the order is the only wave
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
[`Docs/Context/Core/CONTEXT.md`](../Core/CONTEXT.md) for the flags and the retry conditions.
