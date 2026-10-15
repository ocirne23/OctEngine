# Game modes: co-op PvE and PvP

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.

## Co-op PvE (`--coop`)

### The map

A big square — `c_coopHalfSize` 270 m on the 600 m ground plane — of RANDOM IMPASSABLE TERRAIN.
`rebuildCoopMap(seed, fill, lanes)` ("Game/Coop" "Map seed" (0 = random each run), "Terrain fill"
0.3, "Terrain lanes" 6):

1. Two-octave value noise thresholded to EXACTLY "Terrain fill" (a sorted-copy threshold).
2. A rock-free Base zone, `c_coopBaseClearRadius` 26 m.
3. "Terrain lanes" carves wobbling attack lanes from the Base ring out past the edge.
4. `finishGrid` flood-fills from the Base. **`CoopMap::depth` is the geodesic BFS distance**, which
   the ambient scatter gates on.

**Edge barrier at ±270:** a `barrier.pre` ring (20 m segments, E/W yaw'd 90°) whose collider is
`Layer Barrier, CollidesWith Player`. **It blocks ONLY player capsules** (`player.pre` carries
`Layer Player`) — units and shots walk through. Visuals are a post per segment plus
`drawCoopBarrier`'s pulsing energy lines on the real clock.

**Edge WALL at ±299:** an `edgewall.pre` ring just inside the 600 m ground rim — **invisible** (no
Render component) and, unlike the barrier, on the Default layer, so it is **solid for everyone**
exactly like `rock.pre`. Waves spawn in the band BETWEEN the barrier and the rim, where crowd
pressure can shove bodies off the world. 100 m segments — nothing sees it, so long boxes hold the
ring to 6 static bodies a side, each side spanning the WHOLE rim (±300) so the four seal the corners
between them. Co-op only: a PvP arena's rock border is already solid.

> **Spawn clearance is a `static_assert`.** A wave spawn point is clamped to ±`c_coopGroundEdge`
> (296) on BOTH axes — the push-out writes 272–282, also inside — so no spawn CENTRE can land in
> the wall. The body around it still has to fit: `c_edgeWallClearance` = wall − half-thickness −
> clamp = **2.5 m**, against the largest unit radius (Titan, 2.0 m in `enemyTitan.pre`). The assert
> (≥ 2 m) fails the build if anyone moves `c_coopGroundEdge` out or the wall in;
> `c_edgeWallHalfThick` must be kept in step with `edgewall.pre`'s `HalfExtents`.

**Nodes:** a STARTER mineral + fuel pair ~11 m from the Base, then up to 48 golden-angle-spiral
candidates SNAPPED to reachable open cells with a 13 m spacing floor.

### Teams

Every player is on **team 0** around ONE central Base (`allocateClientTeam` returns 0, and
`teamStartPos` is the shared start). The AI team is `GameMatch::CoopAiTeam` = **1, NOT a high slot**,
and fields UNITS ONLY.

> Co-op runs the Force system at **TWO LIVE TEAMS** (`ForceSystem::setNumTeams(2)` in the ctor; PvP
> sets 8 explicitly, since exit-to-menu can chain a co-op session into a PvP one), so every force
> shader recompiles with `NUM_FORCE_TEAMS=2` and the bake volume and buffers shrink to fit. **Every
> co-op team index must stay < 2.**

### Shield-less bodies

`ENpcType::Swarm` (`enemySwarm.pre`) is **the cheap horde body: health only, NO `Component Force`**,
so no bubble, battery or GPU readbacks. Built for many-thousand counts. Grunt and Runner are
health-only too.

**Shield-less units interact with emitters through the BAKED PRESSURE FIELD**
(`ForceSystem::sampleBakedField` in the entity pass — a CPU bilinear tap, no per-unit GPU slot,
unlimited counts):

* `opposing` / `opposingGradient` reproduce the SHIELDED units' push chain —
  `-grad × 0.35` (the 13-sample integral's mean self-weight) `× forceGain × pushGain × pressure ×
  tension`, speed-clamped — **so ONE "Field push gain" tweak rules both paths.**
* Exposure damage is GRADED by field depth (`smoothstep(0, iso, opposing) × fieldDps × mult`),
  **because the push equilibrium parks pressing units AT the surface** (`opposing ≈ iso`), where a
  binary inside test reads false and units grind on shells unharmed.
* The push itself ramps from "Field push starts (x iso)" 0.7 — **below it the field does NOT
  shove, so units walk into the damage band instead of being stopped out in the weak fringe.**
* ~3 frames latent.

**FALLBACK** with the bake disabled: `GameStructureComponent::bubbleRadius` (stamped next to
`strainable` in the death sweep as `reach × 0.5 × outputFrac`) gives the same exposure damage
CPU-side, with no push.

A shield-less body spawns with a **ZERO battery** (`energy = energyMax = 0` when `shieldOutput <= 0`),
so the label pass's shield-vs-health branch and the damage absorb never mistake it for shielded, and
damaged swarm show health bars like any unit. They still deposit `addLoad` strain.

### Ambient scatter

"Ambient budget" (500000) POINTS of units placed at world start, in small **GROUPS** (3–9 bodies in a
4–9 m disc): `AmbientSpawn` holds one archetype and a remaining count, so the trickle fills the
current group — a disc around a reachable anchor — before rolling the next. **Loose blobs, not a
lattice.**

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
order fields and reads the World's root list + the immutable map, so it runs during present, never in
front of the entity batch submit; its orders land in the next pass; its own RNG, never `linearRand`):
an IDLE AI unit — not hunting, locked or routing — now and then strolls 0.4–1× "Ambient wander
distance" (12 m) with its heading = a random unit vector + "Ambient wander base bias" (0.5) × toward
the Base, clamped to open ground. **Cheap and SMOOTH by design:** the expected strolls per frame =
SELECTED units / "Ambient wander interval (s)" (90) × dt (the unit count is
`GameUnitComponent::liveCount`, the selected fraction a smoothed estimate from the random probes —
sizing from every unit would dump every far unit's strolls on the few near a player), carried as a
fractional budget (`m_wanderBudget`), each costing at most 32 random probes into the root list (a
non-unit root is a wasted probe) to find an idle unit — no per-unit timer, no burst.

The order is a **wander order** (`GameUnitComponent::orderWander`): it never seeds a lane (not even
on the hunt-seed AI team — a stroll is a direction, not a route), **walks at "Wander speed mult"
(0.25) × the unit's move speed, capped at "Wander speed max (m/s)" (0.75) so runners stroll too**
(the far tick's teleport as well), and self-expires after "Ambient wander timeout (s)" (12 — ticked
by the far tick as well), so a target behind rock cannot pin the unit. **Only SELECTED units (inside
some player's outer SIM LOD tier) are candidates**: a far unit is invisible, would walk the order by
the far tick's teleport and then arrive in view mid-stroll — a whole patch visibly "starting to
wander" as a player approached.

### Waves

They-are-Billions style. `tickWaves` arms the clock ("First wave delay (s)" 40, then "Wave interval
(s)" 120).

**`queueWave`:**

1. Picks a RANDOM compass direction projected onto the barrier square and stepped 8 m OUTSIDE it —
   **the swarm spawns in the open ring between barrier and ground edge and walks in THROUGH the
   player-only barrier.**
2. **Sizes the wave in BUDGET POINTS**, not unit counts: "Wave budget" (20) plus "Wave budget growth"
   (40) per wave, where the growth itself climbs by "Wave growth growth" (5) every wave — wave i =
   base + growth·i + growthGrowth·i(i−1)/2 (`nextWaveBudget`). Each type spends its "Cost \<type\>"
   tweak, **so a brute-heavy archetype fields far fewer bodies than a swarm flood of the same
   budget.** The "Max enemy units" cap (100000, counting every live unit) converts at the cheapest
   cost, minus the points still queued; the clock and the wave index advance even at the cap.
3. Rolls the archetype, sizes the spawn blob, seeds ONE lane and fires "GWv" (u16 index) at clients.

**`tickCoopSpawns` TRICKLES the actual spawns** ("Spawns per frame" 100 — wave first, then ambient),
so a huge wave enters over seconds rather than one hitch. An unaffordable roll downgrades to the mix's
cheapest type; if even that does not fit, the wave ends. **The trickle is THREADED:** the frame's
rolls (budget math plus glm RNG, main-thread — `linearRand` is not thread-safe) collect into
`NpcSystem::spawnLooseUnits`, one `World::spawnBatch` fan-out per frame, with team/order fixup serial
after the join.

### The spawn blob

Bodies land in a disc around `m_waveOrigin` (the ring point), **sized ONCE per wave in `queueWave`
by `waveSpawnRadius`: the AREA scales with the wave's expected body count, so the areal density is
the same in wave 1 and wave 20.** Body count = the queued points / the MIX's weighted mean cost, and
`r = sqrt(bodies · "Wave spawn area per unit" (6 m²) / π)`, clamped to [8 m, `c_coopGroundEdge`]. It
rides the save (`WaveTrickle/Radius`), so a mid-wave load keeps the same blob.

> Do not size the radius off the REMAINING budget or cap it: parked bodies never push each other
> apart, so a stacked disc only resolves as box3d shoves them out once a player comes near, and a
> shrinking disc packs the wave's tail tightest.

The blob is clamped to the ground plane and any point inside the barrier square pushes back out
along the wave's dominant axis, **so an oversized disc becomes a wide BAND along the barrier face** —
the only direction with room, since the spawn band is just `c_coopGroundEdge − c_coopHalfSize` deep.

Spawn points then roll up to 6 times against the last `c_waveRecentSpawns` (32) spawns at 2 m
spacing, **so parked bodies rarely overlap in the first place.**

Each unit is `orderMove`d to the Base's near face on the incoming side (`m_waveDest`, ±4 m jitter);
the lock releases on arrival OR at the first enemy structure inside "Order break radius" (15 m —
`GameUnitParams::orderBreakRadius`, checked by the unit's combat probe), and the AI takes over,
hunting the NEAREST structure — without the break the whole wave marches past everything to the Base.
All spawns are loose units — **sourceId 0: no route, no population accounting.** The authority HUD
shows "Next wave (s)".

#### Unit types and costs

`ENpcType`: Grunt, Brute, Runner, Spitter, Swarm, Elite, Giant, Titan, Lobber, Spawner, Warrior.
**SAVE FILES store the type as an int — APPEND only.** Costs are the "Game/Coop" "Cost \<type\>"
defaults.

| Type | Cost | Notes |
|---|---|---|
| Grunt / Brute / Runner / Spitter / Swarm | 3 / 25 / 2 / 10 / 1 | The barracks tier. Spitter is enemy-only. Grunt, Runner and Swarm are HEALTH-ONLY bodies (no Component Force). |
| Warrior | 5 | `enemyWarrior.pre`: the SHIELDED grunt (bubble + battery, 80 hp, harder hits, a bit bigger and yellow) — a BARRACKS option too (12 energy, 3 pop); "warrior line" from wave 4, "shield wall" from 6. |
| Elite / Giant / Titan / Lobber / Spawner | 15 / 100 / 500 / 30 / 40 | **Enemy-only elite tier**, `enemyElite/Giant/Titan/Lobber/Spawner.pre`. |

* **The Lobber** is `Ranged` with `ShotKind 1` = the slow SPLASH shell `enemyLob.pre`
  (`SplashRadius 3`, 12 unit / 18 structure damage); "Lobber shot speed" 14.
* **The Spawner** is a tough ranged-stance hive: `ShotKind 2` makes each "shot" a LOOSE Swarm body
  born beside it toward its target every `FireInterval` while it holds at `StandoffRange`. **The
  births are loose units — no wave budget, no cap — so the hive itself is the thing to kill.**
* `isBarracksUnitType` allows only Grunt / Brute / Runner / Swarm / Warrior; **an old
  Spitter-barracks save loads as a Grunt barracks.**

#### Archetypes (`c_waveArchetypes`, [MatchCoop.cpp](../../../Code/Game/Private/MatchCoop.cpp))

ONE archetype per wave: a named recipe of weighted types (up to 5 — "endgame"), **gated by a minimum
wave index**, weights jittered ×0.6–1.4, and **never the same recipe twice in a row** when a choice
exists.

| Min wave | Recipes |
|---|---|
| 1–3 | swarm · swarm + runners (2) · grunt push (2) · runner rush (3) |
| 4–6 | warrior line (4) · spitter siege (4) · brute hammer (5) · shield wall (6) · combined arms (6) |
| 7–8 | brute wall · **elite guard** (7) · the works · **lobber barrage** (8) |
| 9–10 | **giant push** · **hive** (9) · siege column (10) |
| 11–13 | **titan** (11) · **hive siege** (12) · endgame (13, all four elites) |

`sampleMix` samples the wave's stored jittered copy per spawn; the ambient scatter samples the
archetype directly per spawn. The wave log names the recipe.

### Targeting range gates

EVERY unit's Nav team-field targeting is RANGE-GATED by "Target search radius" (15 m, geodesic). The
field rebuilds every Nav "Rebuild interval" with LIVE source positions, **so in-range units track a
moving player tightly while out-of-range units hold their patch instead of marching across the map.**

Out of range, `hasTarget` from an earlier tick is **DROPPED at once** — kept, the unit would march to
the last known spot for up to a whole retarget interval. Inside the "Nav follow radius" (40 m) band
the unit may still walk an existing crowd lane.

Wave units stay order-driven until arrival or the first enemy structure inside "Order break radius",
then the same gated AI takes over.

**That standing order is therefore the ONLY thing that marks a wave unit**, so the F9 save carries it
per unit (`Order`, see Save / load ([CONTEXT.md](CONTEXT.md))): without it a loaded wave stops where it
stands and holds its patch like ambient scatter.

### Known gaps

* **The Base takes damage but the death sweep never destroys it — no lose condition yet.**
* Thousands of dynamic bodies is the AIM, not yet a measured budget. Knobs: Max enemy units, Spawns
  per frame, `Physics/World/Worker count`.

## PvP

Place Force emitter pylons plus economy to siege the other team.

### The arenas (`EPvpMap`)

The lobby's "Map" pick calls `setPvpMap` before `spawnWorld`; the AUTHORITY builds it, CLIENTS build
whatever GMp names.

| Arena | Layout |
|---|---|
| **Lane** | The original corridor, x ±65 / z ±20. |
| **Wide lane** | z ±40. |
| **Chokepoints** | The wide lane plus a 10 m middle wall at \|x\| < 5, pierced by three 10 m openings at z 0 and ±25. |
| **Circle** | A 65 m open disc around a 28 m rock column; Bases mid-ring at radius 47, evenly spaced, team 0 west. |

The lane arenas spread N Bases evenly along x between ±55. **Every Base cell gets a rock-free 8 m
disc** (a middle team on the Chokepoints wall line carves its own gap). `borderwall.pre` is unused —
the rock border ring replaced it.

`spawnPvpNodes` places the arena's node table, **skipping any node whose cell landed in rock**: the
lane's 180°-symmetric 8-node table, plus an outer 6-node band on the wide arenas, and the Circle's 12
nodes on two rings (38 m / 56 m) between the Bases.

`setPlacementBounds` covers the arena interior (`m_pvpInterior`).

### Teams

Everything is per-TEAM. `GameMaxTeams` is 8, but **the map spawns one Base per PLAYABLE team**, and a
player without a Base has no respawn anchor, mineral bank or healing — so the slot pool is exactly the
lobby's count (`m_numTeams`, 2..8, default 2).

The server is team 0 (or its lobby pick). A joining client gets its **lobby pick**, else the
**least-populated team** (`allocateClientTeam`), stamped onto its capsule's puppet
`GameUnitComponent` and emitter at spawn.

**That component IS the record.** `clientTeam(clientId)` reads it back (−1 = no capsule → the `Gq*`
handlers refuse the request), and the OWNER learns its own team from the snapshot game blob.

> **NEVER derive a team from the clientId.** Ids are minted monotonically and never recycled, so a
> reconnect or a failed first attempt would shift the second connection onto a base-less team.

`GamePlayer` follows the component on a client (gated on `hasTarget`, so the prefab's authored team is
never latched) and stamps it everywhere else.

**Per-team specifics.** Players' and projectiles' Force teams are set at spawn/adopt; structures carry
the builder's team (GPl and place requests carry a team byte, and fields are re-teamed after spawn
since prefabs author team 0); demolish validates same-team; minerals and fuel are per-team arrays.

### Structure damage

* **Shots:** `GameProjectileComponent::onContact` (authority) lands the shot prefab's
  `StructureDamage` on an enemy-team structure it hits (a splash shell: on everything in
  `SplashRadius`). Every placeable prefab authors `ContactEvents true`.
* **Territory:** `GameStructureComponent::update` drains "Damage/s in enemy field" (6) health while
  ANY other team's bubble owns the structure's position (`sampleBakedField`). *Push your field over
  their buildings to siege.* `StructureSystem::tickDamage` is only the death sweep.

**Known gaps:** player turrets target only units, not players; GSt caps at 79 structures.
