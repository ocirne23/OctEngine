# Economy: cables and construction

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.

## Economy

A node produces NOTHING until an **extractor** is built on it — one per node; the ghost snaps to the
nearest free node within "Extractor snap radius", validated again at place time — AND that extractor
is fed energy.

### The cable transport (three media, one model)

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

### Physical cables

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

#### The networks are DERIVED, never authored

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

#### The cell hash

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

#### Draw and lifecycle

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

### Construction

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

### Grid placement

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
