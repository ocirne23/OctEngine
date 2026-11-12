# Economy: cables and construction

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.

## Economy

### The harvest: the Lumberyard and the Quarry

Two production buildings make the raw goods: the **Lumberyard** makes fuel (wood), the **Quarry** minerals (stone)
(`isHarvestType`). **Each stands ANYWHERE** a building fits. What is within "Harvest radius" (32 m) of it sets its
**YIELD** (0..1, `Ref::yield`): its rate is "Lumberyard fuel/s" (1) or "Quarry minerals/s" (2) x the yield x the
staffing. Neither needs energy: only workers (below).

| | The yield |
|---|---|
| Lumberyard | the TREES in the radius / "Trees for full lumberyard yield" (12). Dead wood (a log, a stump) counts half a tree; a bush counts nothing. |
| Quarry | the ROCKS in the radius, their sizes added (m, each rock's longest axis) / "Rock (m) for full quarry yield" (12). A cactus counts nothing. |
| The flat maps | no trees or rocks: a RESOURCE NODE of the yard's kind (fuel / mineral) in the radius is a full yield. Several yards can share one node. |
| Always | at least "Harvest minimum yield" (0.1), at most 1. |

* **YARDS SHARE: a tree or a rock works for ONE yard.** Yards of one kind whose circles overlap form a CLUSTER (centres
  under two radii apart, joined through any number of yards: `harvestCluster`) and divide what stands in their circles.
  `shareHarvest` is a MAXIMUM FLOW from the records to the yards, one yard after the other in id order (the older yard
  first). The records are grouped by the SET of yards that reach them (one group per region of the overlapping
  circles), so the flow runs on a few groups. A yard takes what is free in its groups, then it takes from an earlier
  yard what that yard can get back from another group (an augmenting path). **So nothing a yard could use stays
  unused**: the total is the most the cluster can get, an older yard works the trees only it can reach before the
  shared ones, and the order only says who is short. A yard's amount never drops when a younger one comes; it can rise
  when a yard goes. At most 64 yards share in one cluster. A flat map's node is not shared.
* **The yield is part of the structure**: on GPl (a `uint16`), in the save (`Yield`, written only below 1). Nothing
  removes a tree, so it changes only when a yard of its kind comes or goes: `spawnStructure` and
  `removeStructureBookkeeping` mark the yards of the kind (`Ref::harvestDirty`), and `resolveHarvestYields` (EVERY
  instance, from `GameMatch::surveyHarvest`) shares each dirty cluster again from this instance's own records - at
  once, then about once a second while the source has no answer for a circle. Each instance derives the same yields
  from the same roster and the same world, so nothing more is sent. With no source (the flat maps, a headless
  server) a yard keeps its placement yield.
* **The source** (`StructureSystem::HarvestSource`, set by the App through `GameMatch::setHarvestSource`; the sandbox
  world only): `TreeSystem::surveyHarvest` VISITS each record of the tree world's CPU records in a circle, with its
  world position (a record's identity across two surveys), its wood and its stone (Procedural, "World records"
  in [Trees.md](../Procedural/Trees.md); `TreeWorld::FloorType::harvest` says what a record type gives). Those records
  exist only within the tree world's CPU keep radius of THIS instance's camera, and they are readable at one point of
  the frame only: after the procedural updates, before `TreeSystem::kickWorldUpdate`.
* **So the GHOST asks, and the answer comes one frame later**: `updateBuildMode` leaves the ghost's cell in
  `m_harvestProbe`, main.cpp calls `GameMatch::surveyHarvest` at that point (`StructureSystem::harvestYield`: the
  ghost as the YOUNGEST yard of its cluster, so the preview is what is left for it), and
  `computeAim` reads the answer for that very cell. Until then - or while the source has no answer (the world's chunks
  are not generated yet) - the ghost cannot place and the HUD line "Yield" reads "surveying...".
* **The placement carries the REQUESTER's yield** (`queuePlaceRequest`, GqP): the authority may be too far from the
  place to hold its records. It only clamps the value (a co-op trust, like the client's ground height), and replaces
  it by its own share (`resolveHarvestYields`) when it holds the records.
* **Shown:** the ghost draws its harvest radius, hued from red (little) to green (full), and the HUD line "Yield" gives
  the percent, which a world label over the ghost repeats with a bar (`updateLabels`, from `m_shownYield` / `m_yieldGhostPos`); a selected yard's label has a "Yield" line; `drawDebug` rings the radius of the built yards of the kind ON THE GHOST only (its `armed` input: a Quarry ghost shows the quarries, a Lumberyard ghost the lumberyards).

### Light on beams

**With "Game/Light" "Beams" (the default) NOTHING rides the cable transport below while the carriers are on too**:
`transportInject` skips the energy slots as well, and the power cable is off the hotbar. Light moves along DERIVED
links (`Light.cpp`). No link is authored, saved or sent: every instance derives them from its roster.

| | |
|---|---|
| Hubs | The Crystal stores (Battery), the Beacon (Base) and the Lenses: the STORAGE role of the energy medium (`transportRoleOf`). |
| A producer's beam | A Mirror (Solar) links to the nearest hub within "Mirror beam range (m)" (60) that it has a clear line to; a Hearth (Generator) to the nearest within "Hub reach (m)". It empties its buffer into the hub at "Beam rate" (20/s). |
| A hub's feed | Every CONSUMER (fields, the turret, the barracks, the medic station) links to the nearest hub within "Hub reach (m)" (30) with a clear line, and fills from it at "Feed rate" (10/s). **THE LIGHT BALANCE** (defaults; the default sun cycle gives a Mirror ~780 s of full daylight and keeps a field awake ~640 s a night; all with enough storage between): an Emitter draws 0.75 light/s awake (480 a night), a Bastion 2 (1280 a night). **3 Mirrors carry 1 Emitter through the night, 8 a Bastion** ("Solar energy/s" 0.21: 3 x 0.21 x 780 = 491). **1 Hearth, burning day and night, carries 1 Emitter, 2 a Bastion** ("Gen energy/s" 0.45: 0.45 x 1440 = 648). **ONE full Crystal store ("Battery capacity" 1500) runs TWO Emitters through one night** (960, plus room for the pressure draw). A change of the sun cycle's speed or latitude moves all of these. **The barracks and the turret keep their METERED intake** (their store is the build / reload bar). The Beacon keeps a quarter of its store for its own field. |
| Hub to hub | A hub links to the nearest OTHER hub within "Hub link range (m)" (60); the two level their fill fractions at "Share rate" (10/s). So stores chain a network over the land. |
| **The Lens** | A RELAY (`lens.pre`, 1×1, a 3 m post; the Works page): a hub with a small store ("Lens buffer" 40). Nothing is aimed: **it links to the NEAREST hub within "Lens range (m)" (32) with a clear line - but not to a hub that links to IT already** (that pair is joined). Every structure has one link, so without that rule a lens between a store and the next lens would link back to the store, and the chain would end there. `rebuildLightLinks` runs the lenses in a second pass, so each sees the links of the others (and of the lenses before it). **A line of lenses 32 m apart, or less, is a chain.** A link with a lens at an end is never longer than the lens range, whatever the other end's own range (a store's 60 m link range, a mirror's beam range). The link levels the two fill fractions like any hub pair, at "Beam rate" (not the share rate), so light goes both ways: from a far mirror field to the base, or from the base to a far outpost. As a hub, a lens also takes the mirrors' and hearths' light and feeds the consumers in its reach. The ghost shows the lens range. Like a Crystal store, a lens never shows the light badges: the end of a chain has no link of its own (the others link to it). |
| Which hub | **The nearest USEFUL hub in range with a clear line, else the nearest one** (`rebuildLightLinks`). Useful: for a consumer a hub with light to give (over the Beacon's reserve), for a producer a hub with room. So a turret or a barracks next to a full Crystal store does not stay dark because an empty hub is nearer (the Beacon at its reserve, a lens with nothing behind it), and a mirror does not beam into a full store. The links are derived again about once a second, so a link follows the light. Hub to hub: the nearest. |
| A beam's ends | `beamPointOf`: the CENTRE OF THE BUILDING'S CRYSTAL (`beamHeightOf` in Light.cpp, per type; keep it in step with the `Crystal` child of the .pre files); a building with no crystal: 1 m over its top. |
| Shown while building | With a type that uses light on the ghost, `drawDebug` (its `armed` input) rings each built hub in gold at the distance that type links over: the same range choice as `rebuildLightLinks`, a Lens end never longer than "Lens range". |
| A clear line | `lightLineClear`: one physics ray between the two beam points against the Terrain and Vegetation layers. Buildings and units do not block. |
| When | `rebuildLightLinks` about once a second, and at once when the roster size changes (`tickLight` on the authority, `tickMirror` on a client: the beams and the badges). `tickLight` (authority, after the carriers) moves the light. |
| Drawn | `drawLightLinks`, called from `tickAuthority` / `tickMirror` (**never from `drawDebug`: that runs before the begin-frame job, which resets the frame's light records**): **a link shows only while it MOVES light** - `Ref::lightFlow` above 0.02 light/s, the link's MEASURED flow, smoothed over ~1/3 s in `tickLight` (a store that is topped up every tick never looks busy, so the stores cannot say it); on a mirror it is an estimate from what the building makes or draws - as a `BeamLight` between the two crystals ("Beam glow", "Beam radius (m)", "Beam glow range (m)"; **the glow is × flow / "Beam full flow (light/s)" (5), and the radius goes from half to full with it**, so a beam shows how much light it carries): the Beacon's beam in small. **A MIRROR's beam is LIGHT GLITTER, not a beam column** (within 400 m of the camera): golden sparks that fly from the mirror to its hub's crystal - `Effects/light_glitter.pfx` through `ParticleSystem::emitStream` (Particle's CONTEXT, "Streams"), "Mirror glitter (sparks/s)" (14) x the mirror's share of its full output. A field of mirrors is dozens of beams, and the fog apply draws the glow of only `RendererState::MAX_BEAM_COLUMNS` (32) beams a frame (every pixel walks them all) - as columns the mirrors took all of them. A stream has no such count, and it is no light. Tried debug lines, dropped because a 1 px line has no glow. Every other link keeps its column and is also a light (`addLightInfo`), the 14 nearest. No line is drawn. The 14 moving links nearest the camera get one (the renderer draws 16 columns a frame, and the Beacon's beam takes one at night). With the fog off no beam shows. |
| Badges | "No light store in reach" / "Light blocked" (a hub is in range, no clear line) on a building that makes or uses light and has no link. A Crystal store and the Beacon never show them. |
| Ghost | A Crystal store's ghost shows its hub reach, a Mirror's its beam range. |

**FIELDS SLEEP BY DAY (the sandbox world).** `GameStructureSettings::fieldNight` (runtime, `stampLightRules` on every
instance: 1 - the daylight; 1 in the other modes) scales a field's STANDING draw, and "Sleeping field alpha" (0.15)
its shell alpha (which scales the shell's glow and its bubble light too, see `ForceEmitter::setShellAlpha` in the
Force CONTEXT): by day a field draws no light and is faint, but its output and its area stay. **A demon on it wakes
it at once** (`engaged`: pressure, or the units' drain), with the full draw and the full shell. The mirror wakes on
its own emitter's pressure. The Beacon's field follows the same alpha (its draw is free anyway).

**Not built (the design in `Docs/GDD-Economy.md`):** a player-aimed beam (every link is the nearest hub), the Collector
tower, loss in fog, demons that block a beam, a volumetric draw of the beams.

### Workers: production runs on population, not on light

**Light runs the fields and the machines of defence** (emitters, the turret, the barracks, the medic station).
**Production draws none:** the Lumberyard, the Quarry, the Fabricator and the Constructor hold no energy store and have
no power port (`energyCapacityOf`, `consumerDraw`). They and the Generator need
WORKERS (`StructureSystem::workersNeeded`, "Game/Population" "Workers: ..." 2 / 3 / 2 / 1).

* A team's workers = "Base workers" (6) + "House workers" (4) for each BUILT House; its jobs = the sum over its built
  production buildings (`updateStaffing`: in `tickProduction` on the authority, in `tickMirror` on a client).
* `staffing(team)` = workers / jobs, at most 1. **Short of workers, every production rate of the team scales by it**:
  the lumberyard's and the quarry's output, the fabricator's output and its wood draw, the generator's output, the constructor's rate.
  No building stops alone.
* The HUD counter "Workers" shows jobs / workers (red when short); every production building then carries the
  "Short of workers" badge.
* `powered` on a production building means "it works": always for the lumberyard, the quarry, the constructor and the generator,
  and "it paid its wood" for the fabricator.
* A House ALSO still raises the population cap of the nearest barracks (see [Hotbar.md](Hotbar.md)).

### Carriers: wood and stone on the road

**With "Game/Carriers" "Enabled" (the default) only ENERGY rides the cable transport below.** Fuel (wood) and
minerals (stone) move by WALKERS (`Carriers.cpp`, `StructureSystem::tickCarriers`, authority, after the
constructors). `transportInject` skips every slot of medium 1 and 2, so those cables hold and move nothing. The
pipeline is off the hotbar; the conveyor is the **Road**.

| | |
|---|---|
| The look | `cableConveyor.pre` + `Models/roads.obj` (hand-written): cobblestones, 5 cm high with a bevel, a road 1.2 m wide. The hub is the centre patch of stones (`RoadHub`); each of the four arm children is a strip of stones from the patch to the cell edge (`RoadArmX` / `RoadArmZ`, centred meshes moved to ±0.83, no rotation), shown toward what the segment joins. So an end, a straight, a corner, a T and a crossing each get their shape, and two neighbour cells' strips meet in one joint on the cell edge. |
| The road | BUILT `CableConveyor` segments and `CrossingConveyor` pieces (`roadAt`: the cell's occupant or the cable under a crossing). A blueprint is no road. |
| Who walks | A walker (`m_walkers`, a `Walker`) is one FREE WORKER of its team: workers - production jobs (see Population). It comes out of a House or the Beacon that touches a road (`touchesRoad`), at most "House workers" / "Base workers" per building. The HUD's "Workers" reads `jobs + walkers / workers`. |
| The job | A TRANSPORT JOB is a building that SENDS goods (`carrierSendGood`, by `transportRoleOf`): a PRODUCER sends when its store holds a load ("Load", 10) or half its capacity; a STORAGE (Storehouse, Base) sends a load only to a CONSUMER that is under half full. A walker goes to the job's road cell (the PICKUP) and STAYS: load, deliver, come back, wait there. |
| Who gets a walker | `carrierJobTier`: 2 = the building has a load to send (`Ref::sendReady`) and no walker waits at it or is on its way (so a long road gets MORE walkers, up to "Max walkers per building" 6); 1 = a producer with no walker at all, or build work (below). `roadSearch` takes the nearest tier 2, else the nearest tier 1. |
| The ~1 s pass | `carrierHousePass`: derives `sendReady` (a storage: only with a hungry consumer on its road), then every house sends ONE worker per pass while a job wants one and the team has a free worker. When production takes the worker (free < walkers), the house DISMISSES walkers, the waiting ones first: a dismissed walker goes home when it is next idle. |
| Waiting | The first walker in line at a pickup (`Ref::waitRank`) loads; it looks for a load about once a second. A walker BEHIND another one looks for other work on its road network (tier 1 or 2). The first in line leaves only for a waiting load elsewhere (tier 2) and only after 8 s idle. After 20 s idle a walker goes home, except the one walker of a producer. |
| Where to | `findRoadTarget` = `roadSearch` with the takers as the goal: ONE breadth-first search over the road cells from the walker's cell (4096 cells at most). **The first TAKER with room that a road cell touches wins - a storage as much as a consumer**, so a load stops at the nearest storehouse and that storehouse's own carriers feed the consumers farther on (a consumer first, whatever the distance, sent every load past a near storehouse to a far one). Same team, built. **A taker counts only when the WHOLE load fits** (`carrierAccepts`): the load the source would send against the taker's room, less what other walkers already carry to it (`Ref::goodsIncoming`, counted fresh each tick) - a hearth that misses one wood is no target for a full load. That is why the buffers hold several loads ("Internal buffer", "Generator fuel tank", "Generator buffer": 30). The result is also the walking path (cell centres). |
| BUILD WORK | **A walker with no other load BUILDS**: it carries stone to the BLUEPRINTS on the road - buildings, and pieces of the road itself. `blueprintNeed` = the stone a blueprint still needs (`investMaterials`' price) less what the walkers already carry to it (`Ref::buildIncoming`, counted fresh each tick). The pass sets `Ref::buildReady` on a building that holds stone and is no consumer (`carrierHoldsStone`: a store, the Beacon, a quarry) with a blueprint on its road; that is tier 1 in `carrierJobTier`, so a waiting LOAD (tier 2) always gets the walker first. At the pickup the first in line tries the loads, and only then takes a load of stone to the nearest blueprint (`findRoadTarget` with `blueprints`: `carrierAccepts` then takes ONLY blueprints, never one otherwise). At the blueprint it calls `investMaterials`; what is left goes on to the NEXT blueprint from there (a road piece that was just built is road for that search, so one walker lays a painted road piece by piece), and the rest goes back to the pickup by a new search from there. A blueprint is found only when a BUILT road cell touches it: the first piece next to a building is the player's or the Builder's hut's. |
| REPAIR WORK | **The last work of all**: the same trip, to a DAMAGED built structure at the road (`repairNeed` over 0.25 stone; `ECarrierWork`: Goods, Build, Repair - `Walker::work` says what a load is for). `Ref::repairReady` is set only while the team has NO build work on any road (it shares the build work's tier 1, so that gate puts it behind), and at the pickup a walker tries the loads, then a blueprint, then a repair. The delivery is `investMaterials` again, and what is left goes on to the next damaged structure. |
| The trip | The load leaves the store at departure. The walker goes at "Walk speed (m/s)" (3.5), unloads what fits, walks the same path back to the pickup, and puts the rest back into the job's store. |
| The walker | `carrier.pre`: render-only, `Global true`, no physics and no unit sim; the tick writes its `pos` / `rot` (it faces its walking direction: an entity's forward is -Z) and shows its LOAD (the prefab's second child, held in front of the body) only while it carries one. Removed when it arrives home, when its house is gone, or when no road leads home. The per-building counters (`Ref::walkers`, `walkersIdle`, `homeWalkers`) are counted from `m_walkers` at the top of every tick, so a removed building needs no bookkeeping. |
| Badge | "No road": a building that moves wood or stone and touches no road (`attachedMask` bit 2), or a House that touches none (its workers cannot go out). |

So the delivery RATE of ONE walker is load x speed / (2 x road length), and a job takes walkers until its loads no
longer wait: throughput holds over a long road, for the price of workers. **Known gaps:** the walkers exist on
the authority only (clients see the stores change, and their HUD shows no walkers), a save does not hold a walker (a
load on the road at F9 is lost; the walkers come out again), and demons do not attack a walker.

### The cable transport (three media, one model)

**Energy, fuel and minerals move over the cables as WHOLE CELLS** (`Transport.cpp`). Every built
cable segment and conducting crossing is a NODE with an integer fill and an out-rate, and the flow is
a local transport — a stencil over the node graph — so **a run's bottleneck is literally its slowest
segment**, producers and consumers can sit anywhere along a line, loops and junctions need no routing,
and the consumer nearest the producer is served first (what a conveyor does).

| | |
|---|---|
| Node | a segment / crossing: `fill` cells (soft capacity per MEDIUM — "Cable / Pipeline / Conveyor cells per segment" 2 / 1 / 1: energy holds a burst, the slower media are pure transport), out-rate = the medium's "Cable / Pipeline / Conveyor throughput" (20 / 4 / 4 cells/s), `moved` gauge |
| Junction | one per (BUILT building, medium): the building's PORT. Every segment touching the building is its neighbour, and those segments are also connected to EACH OTHER, **so a line through an emitter carries through cable-to-cable** — the port itself accepts cells only while a slot on it still wants some (nothing parks in a producer's port, nothing relays through it); its out-rate scales with its degree so the cable stays the bottleneck |
| Slot | the building's port on its junction: role by type — **producer** (generator, solar; extractor fuel/minerals; fabricator minerals) pushes whole cells OUT of its float store, **consumer** pulls into its headroom, **storage** (battery and Base; fuel tank; Base and silo for MINERALS) reads the CABLE next to its port — the mean fill of its junction's neighbours, never the junction itself, which its own push fills — as the price signal, with TRUE hysteresis: it switches to PULLING when that fill reaches "Storage pulls above (fill)" (0.75) and keeps pulling until it drops to "Storage pushes below (fill)" (0.25), where it switches to PUSHING until the cable fills back up (a per-tick band idles a battery every other tick on 2-cell segments). **The Base's shield is FREE** (no per-second draw, no pressure surcharge — only the enemy siege load drains it), so the Base is a pure storage building: it banks for the grid and self-generates a trickle |
| Tick | fixed "Transport tick rate (Hz)" 10, staggered over "Transport spread (groups)" 4 so a big base's work lands on several frames |

**Per tick, on main:** `kickTransport` — inject every due slot (supply capped at the port's free
space; a PRODUCER's taken cells simply come off its store at the join — the transport is that
store's only drain, so no reservation and no visible dip; STORAGE reserves, since the Base's siege
load drains it meanwhile, refunded at the join; metered machines — barracks "energy intake", turret
shot energy / fire interval — meter their demand with a carry), then ONE job ("Transport tick").

**The job** runs "Transport substeps" (4) × three owner-only passes: OFFER (a node serves its own
slots' demand first, then FORWARDS everything it can into neighbours with free space that are
strictly DOWNHILL on THE DEMAND FIELD — `dist`, one BFS per run per tick from every port with
unserved demand, through cables only (ports never relay; an unseeded port sits one hop above its
best cable so it can still send) — and, with no such outlet, into cables (never ports) strictly
UPHILL on THE SUPPLY FIELD — `sdist`, the same BFS from every port pushing supply — so cells travel
toward demand when there is any and otherwise FILL the network outward from the producers until it
is full: no fill gradient, nothing drifts back toward a source or into another producer's dead end,
no ping-pong, loops are harmless — space-weighted with the integer remainder rotated, all inside
its out-budget, which BANKS while idle, capped at two ticks, so a front crosses a segment per
sub-step; then takes slot supply into its free space), APPLY (fill' = fill − out + the neighbours'
out-slots aimed here), CLEAR.

**The due runs tick IN PARALLEL** (a `parallelFor` over the runs once two or more are due; each run's
BFS queue is its own slice of one shared buffer — no `thread_local`, no `PerWorker`: the nested
fan-out below is a wait, and a job may resume on another thread after it); a run of 1024+ nodes
additionally fans its passes out per node. No atomics, no entity walk, scheduling-independent.
Scopes: `Transport inject` / `Transport apply` / `Transport join` (main), `Transport tick` →
`Transport runs` → `Transport run` → `Transport offer/apply/clear` (job).

**At the join** (`joinTransport`, top of the next tick) the cells land in the stores and
`flowUtil` = the served fraction of what each port asked.

Consumers still drain their internal float battery every production tick; empty = unpowered. A
full producer buffer still throttles production. Cells in the cables are real: a long run buffers
`segments × its medium's cells per segment`, and a cut line keeps what it held (fills survive rebuilds
by structure id, save as `Fill`, and mirror to clients by id — GCf, see [Sync.md](Sync.md): the fill
plus the ~2 s throughput average as a fraction of the rate, so clients draw the same bottleneck rings
and label numbers).

**Buffers:** the lumberyard (fuel), the quarry (minerals) and the other consumers "Internal buffer" 10; the shield emitters hold deeper
stores — "Emitter buffer" 50, "Bastion buffer" 150, "Lance buffer" 100 (the emitter restart charge
clamps to each) — "Generator buffer" 10, "Battery capacity" 200. **A barracks' energy capacity is the
selected unit's spawn energy** (stamped per instance in `stampTuning`; `energyCapacityOf` returns a
1.0 placeholder just so power cables attach) — its store IS the BUILD BAR: its transport slot meters
its demand to "Barracks energy intake/s" (2) whatever feeds its junction, so the store fills at the
build rate and a unit is born the moment it is full (build time = cost / intake — Grunt 2.5 s, Brute
10 s; no timer). A barracks needs a power cable and holds no minerals. Lumberyards, quarries and
fabricators fill their OWN buffer and **stall when full**. Only what sits in Mineral silos and the
Base is SPENDABLE (a per-team cache recomputed per tick; spending drains silos first, Base last).

### Physical cables

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

It backs `cellsFree` — **per cell: buildings refuse ANY occupied cell (no building on a cable); a
cable may slot under a free crossing middle; a crossing's middle may bridge exactly one plain cable
OR one END cell of another crossing (`isBridgeable` — an end counts as cable for bridging, so
crossings chain and stack); its ends must be free** — plus the derivation and the arm visuals.
`ignoreCables` is the unit-spawn probe, since cables are walk-through.

> **A CROSSING is validated by `planCrossing`, not `cellsFree`** (both share `footprintAreaClear`
> for the bounds / rock / resource-node checks). One relaxation: an END cell may hold a plain cable
> of the crossing's OWN medium and OWN team, which the placement **REPLACES** — that segment is
> redundant under an end, which conducts that medium anyway, and refusing it would stop a paint
> stroke from crossing a foreign line wherever its own run already stands. Anything else (another
> medium, a building, a second crossing, an enemy's cable) blocks. `placeStructure` demolishes the
> named segments right before the spawn, so the cells are free when `insertCells` runs and the
> networks simply re-derive. **The replaced run is severed until the crossing is BUILT** — a
> blueprint conducts nothing.

**`isWalkThrough`** = cables, crossings AND the flat **Solar** slab: their prefab collider is `Layer
Cable, CollidesWith Projectile`, so players and units pass over them, and the code treats them alike
— no nav obstacle in `gatherNavFeed`, `actorInFootprint` never refuses them, `ignoreCables` lets unit
spawns land on them, and an RMB on one is a plain ground order. (The Solar keeps everything else a
building has: cells refuse other placements, labels, GSt, links.)

#### Draw and lifecycle

Segments are real meshes with per-medium authored tints (power yellow, pipe orange, conveyor blue;
blueprint gray rides `applyStructureTint`). Each is a low `Cylinder2` hub with **4 render-only ARM
child entities** (`ArmPX/NX/PZ/NZ`, `Enabled false` in the `.pre`, cached on `Ref::arms` at spawn,
toggled by `updateArms` — toward a crossing only when its medium matches). A crossing is tinted to
its TYPE's medium hue always, conducting or not.

`drawDebug` draws the BOTTLENECK marker — a pulsing red ring over every segment whose ~2 s average
throughput sits at or above 90 % of its out-rate (clients read the mirrored GCf average, so they see
the same rings) — plus the node rings, the built yards' harvest radius, the constructor / medic reach rings and a red ring on every
unpowered consumer.

Segments are full structures: a blueprint phase (**NON-conductive until built**), health (40; a crossing 60 —
**every type has its own health**: `GameStructureSettings::health`, indexed by `EStructureType`, the "Game/Health"
tweaks, through `StructureSystem::healthMaxOf`: the Beacon 1000, a Wall 500, a Gate / Great lantern / Barracks 400,
a Light tower 300, most buildings 200-250, a House / Bridge 150, a Mirror / Lens 100. `stampTuning` stamps it into
the component's `healthMax` each tick, and `investMaterials` heals against that, so the build and repair price is
the type's cost whatever its health), territory drain, projectile damage (`Layer Cable, CollidesWith Projectile` plus
`Layer Projectile` on the shot prefabs; everyone else's Default/All masks are untouched, so players
and units walk straight through), and death-sweep destruction that re-derives the run broken.

**Cables are SKIPPED by:** GSt (the 79 cap), `gatherNavFeed` (no obstacle, no source),
`actorInFootprint`, the RMB building-face move order and route-click checks (treated as ground), the
problem badges, and overhead labels except blueprint or damaged on the authority.

### Construction

**Placing is FREE.** Structures spawn as BLUEPRINTS (gray tint, inert in every tick: no power, fields,
income or flows; links pre-wire but carry nothing).

> **HEALTH IS THE PROGRESS.** A ghost spawns at 1 hp, materials HEAL it at cost/healthMax per hp
> (`investMaterials`), and full health = built. The bar is blue while blueprint, and **damage
> literally undoes construction.** The SAME price repairs damaged built structures.

> **RUBBLE.** A BUILT building that the enemy destroys is not removed: `tickDamage` makes it RUBBLE - a blueprint
> again (inert: no production, no field, no light link, no walkers) with `GameStructureComponent::rubble` set, its
> stores lost, at the health that leaves "Rubble rebuild cost (x)" (0.5) of its cost to pay; 0 = no rubble. Everything
> that builds a blueprint builds it up again (the Builder's hut, the workers on the road, a player), and
> `investMaterials` clears the flag when it is built. `StructureSystem::setRubble` is the one seam for the flag: THE
> LOOK - the building's own meshes are hidden (the root node's pass mask to 0, which `EntityState::updateSelf` skips;
> the child pieces disabled) and a rubble PILE stands on its ground point (`rubble1.pre` for a 1x1 tile building,
> `rubble2.pre` for a 2x2 one, the same at scale 1.5 for a 3x3 one; `Models/rubble.obj`, hand-written; a root entity
> owned by `Ref::rubbleVisual`) - and the COLLIDER off (a `SetEnabled` body command) - **rubble is walked over and is no target**, or a
> wall's rubble would block the enemy for ever: `damage()` ignores it, the units' bite probe skips it, and it is no
> Nav obstacle and no Nav source. Its cells stay taken (Delete mode removes it). It
> **Rubble is not built up under attack** (`rubbleBlocked`): a living enemy unit within "Rubble safe radius (m)" (30,
> flat on the map) blocks every way to build it - `investMaterials` takes nothing, and `blueprintNeed` and
> `fundNearbyBlueprint` skip it, so no worker walks to it and the stone goes to the next blueprint. It
> rides GSt's status byte (bit 4) and the save key `Rubble`. A blueprint that never stood, and a cable or crossing
> piece (outside GSt), are removed as before; the Base never dies.

**Cheat:** "Game/Construction/Free instant build" (a Synced checkbox — **the server's value rules all
players**) skips the blueprint phase.

**Materials come from:**

* **PLAYERS** — a carried inventory ("Game/Player/Materials max" 50; **server-authoritative for ALL
  players**, clients receive theirs through the game blob), refilled within "Refill radius" (6 m) of
  own-team Silos, Base or Quarries at "Refill rate" (15/s) — `takeStoredMinerals`, so
  **hand-couriering a fresh dig site's buffer works before any conveyor exists** — and invested into
  nearby BLUEPRINTS only within "Player build radius" (6 m) at "Player build rate" (8/s), all
  "Game/Construction" tweaks.
* **The CONSTRUCTOR structure** (Production hotbar; conveyor-fed mineral stock plus powered; amber
  range ring in `drawDebug`, in build mode only - its `buildMode` input, which also rings the Light towers' and the
  Searchlights' ranges), which funds the nearest own-team blueprint **OR damaged structure**
  within "Constructor range" (27 m) at "Constructor build rate" (4/s). With nothing to build or
  repair it idles.

Blueprint state mirrors through a GPl built flag plus a GSt status-bits byte (bit0 powered, bit1
blueprint).

**Unit siege drain (`GameStructureComponent::addLoad`) is SHIELD-INDEPENDENT** — collapsed units
strain emitters like shielded ones.

### Grid placement

Every placement aligns to a
`GridCellSize` **2 m** world grid (odd footprints centre on a cell, even on a corner) and occupies
`footprintExtent(type, rot)` cells, within "Place range" (30 m) of the player.

| Footprint | Types |
|---|---|
| 1×1 (cables included; the Bridge is one) | Emitter, Lance, Wall, Gate, Lens, Constructor |
| 2×2 | FuelTank, Generator, Battery, Bastion, Turret, Searchlight, Solar, Lumberyard, Quarry, Fabricator, MineralSilo, House, MedicStation |
| 3×3 | Barracks, Base |
| 3×1 / 1×3 | **the three crossings** — the ONE non-square footprint, by its facing |

> A crossing's facing is quantized to ±X/±Z at `placeStructure` like the Lance, and carried on GPl
> and in the save's `Facing` — **otherwise client derivation diverges on which cells it covers.**
> Prefab `Scale` / `HalfExtents` = footprint half, so boxes fill their cells.

**Heights** use the baseshapes variants — `CubeHalf` / `CubeQuarter` / `Pillar2` / `Pillar3` are the
2 m cube at 0.5× / 0.25× / 2× / 3× height, origin centred: the Storehouse (Mineral silo) a two-storey warehouse of several pieces in a 6 m box, the Wall and the Gate two overlapping `Cube`s (3 m tall: no 3 m piece exists and
a scale is uniform), Battery
`CubeHalf`, Solar `CubeQuarter`. `structureSpawnHeights` is each prefab's box HALF height, **so
everything sits flush.**

**Refusals.** `cellsFree` refuses overlaps at aim (red ghost) AND at `placeStructure` (**the MP
seam**). A resource node (the flat maps) keeps its own 2×2 cells: nothing builds on the deposit.

**THE GROUND (the co-op sandbox world, `StructureSystem::setTerrainGround`).** `groundFit(type, pos, rot, outY)` runs
next to `cellsFree` at aim (the ghost, plus a "Can't build" HUD line naming the reason) AND in `placeStructure`. It
casts 9 rays down onto the terrain collider (layer "Terrain") over the footprint — corners, edge midpoints, center —
and refuses:

| Result | When |
|---|---|
| `NoGround` | a ray misses (outside the streamed collider ring) — at aim no ghost at all |
| `Steep` | highest − lowest sample > "Max ground step (m)" (twice that for cable pieces). **0 = no slope limit, the default**: a building stands on any slope |
| `Water` | the physics water surface (ocean, rivers, lakes) above the lowest sample - **never for the Bridge and the Gate** (`standsOnWater`, below) |
| `Blocked` | a tree or rock world collider (layer "Vegetation", `PhysicsWorld::overlapBoxAny`) cuts the box from the lowest ground to the structure's top |

`outY` — set on refusals too, so a red ghost stands on the ground — is the LOWEST sample for a building (the box sinks
into the high side, it never floats over the low one) and the center for a cable piece. The structure spawns at
`outY + spawnHeightOf`. **The MP seam:** `GqP` carries the client's ground height; when the SERVER has no collider
there (`NoGround` — the client builds far from the server's camera), that height stands. On the flat maps the ground
is the y = 0 plane and every test passes. Debug rings and routes draw at `structureGroundPos` / the terrain height.

**ON WATER (`standsOnWater`: the Bridge and the Gate).** Where the water surface (at the footprint's centre) is above
the lowest sample, **the SURFACE is the ground**: `outY`, the step test (the bed does not count) and the tree / rock
box all start there. So the structure's origin is `water + spawnHeightOf`, whatever the depth - a river, a lake or the
sea. Each prefab carries what is under the surface, since `Scale` is uniform and a piece cannot stretch to the bed:
* **Bridge** (`bridge.pre`, 1×1; the Logistics page): **A ROAD SEGMENT ON A DECK** - `isCableType`, the conveyor
  medium (`cableMediumOf` 2). So it is in the road's runs (`rebuildNetworks`), the carriers walk it (`roadAt`), a
  building next to it is "on the road", and everything a cable piece is holds for it: painted by a stroke, walk-through
  for Nav / `actorInFootprint`, cable health, no label, outside GSt. **The Road's stroke places it by
  itself**: `GameMatch::roadPieceAt` turns a road cell whose ground test says `Water` into a Bridge (the ghost and
  `placeCableLine` / `finishCableStroke`), so a road painted across a river bridges it. What differs from a road: its
  collider is REAL (no `Layer Cable`: players and units stand on the deck), it has no arm pieces, and its origin is
  0.25 over the WATER. The deck (`CubeQuarter`, 0.5 m) is moved down 0.35 in the prefab: its top is 0.15 m over the
  water, a step a capsule walks up. A render-only pier of three stacked pieces reaches 11 m under the deck. The
  carriers walk the path between the cells' entity heights, so they go up and down smoothly between road and deck.
  **A bank more than a step above the water has no ramp**: the road ends on the bank, the deck is at the water.
* **Gate** (`gate.pre`, 1×1, a drag line; the Defence page): a wall segment whose top is a wall's height (3 m) over the
  surface, with a foot of two pieces down to 12 m under it; the collider covers all of it. `Layer Gate`,
  `CollidesWith Default Projectile`: **the PLAYERS (layer Player) walk through it, every unit does not** (the layers
  cannot tell teams: own units are stopped too, and an enemy PLAYER passes). Nav takes it as a wall (`isBreachable`).
* Water deeper than the pier / the foot leaves a gap under it. The aim ray hits the BED (the terrain collider), so
  over deep water the ghost is a little past the cursor.

`actorInFootprint` (a spatial query for GameUnitComponents) refuses a placement a UNIT OR A PLAYER is standing in, at
both seams. **Nothing moves a body out of a building**, so no building may be made around one: the ghost is red
while a player stands in the footprint, and rubble with a body on the pile is not built up (`rubbleBlocked`). The
walk-through types are never refused, and a player does not refuse a Gate (its collider is not on the players'
layer). **It is deliberately NOT folded into `cellsFree`**,
which doubles as the unit-spawn-point probe and must not refuse a cell just because units stand near
it.

**The GHOST is the real box:** `drawStructureGhost` wireframes the exact footprint square × the
prefab's height plus the interior cell lines, green when clear and red when refused. **The box is the
COLLIDER**, which every building still is.

**THE SHAPES.** Ten buildings are placeholder models of the setting (stone, timber, bronze, light crystals):
the Base, Emitter, Bastion, Turret, Wall, Solar, Battery, Generator, Barracks and House. The ROOT's Render is
one piece (the pedestal or the body) and the rest are render-only child entities (`Component Scene` /
`Entity`), all inside the collider box (the Base's crystal stands out of it). **A Render `Scale` is ONE number**,
so every piece is a baseshapes variant at a uniform scale. `applyStructureTint` turns the children gray with a
blueprint and gives each its OWN authored colour once built. The other buildings are still plain boxes.
**The crystals GLOW**: the buildings' `Crystal` pieces are nodes of `gemsGlow` (the same model, unlit) with an
`Emissive` strength, so the bloom takes them (Emissive in [`Docs/Context/RendererVK/Scene.md`](../RendererVK/Scene.md));
a blueprint's crystal is gray and does not glow.
**The light crystals are the `gems` container's `Diamond`** (`Models/gems.obj`, hand-written: two pyramids joined at
their bases, the same above and below; scale 1 = 2 m tall, 1.4 m across, the origin at the middle): a large one
floats INSIDE the Base's beam (the beam starts at the crown under it), one floats high over the tall pedestal of the
Emitter and of the Bastion (above their collider boxes, to show from far), a large one stands in the Battery (a 4 m
building now), and small ones sit on the Turret, Solar and Barracks. The flat maps' two resource
nodes are models too: a rock outcrop with ore (`mineralNode.pre`) and a log stack (`fuelNode.pre`). The Lumberyard
(`lumberyard.pre`, brown) and the Quarry (`quarry.pre`, grey) are placeholder boxes.
